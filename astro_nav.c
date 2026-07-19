/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * astro_nav.c -- integer-only celestial sight-reduction primitives.
 */

#include "astro_nav.h"
#include "fp_math.h"

/* All 128-bit intermediates go through fp_math.h's fp_w128 working
 * type, so this file compiles unchanged on targets without compiler
 * __int128 (Cortex-M, RV32). The two helpers below carry the file's
 * one recurring wide idiom. */

/* Round-to-nearest signed division, positive divisor (half away from
 * zero, matching the (num >= 0 ? num + den/2 : num - den/2) / den
 * pattern everywhere a scaled product comes back to a narrow unit). */
static fp_w128 div_round_w128(fp_w128 num, fp_w128 den)
{
    fp_w128 half = fp_w_divs_pow2(den, 1);
    fp_w128 adj  = (fp_w_cmp(num, fp_w_from_i64(0)) >= 0)
                   ? fp_w_add(num, half) : fp_w_sub(num, half);
    return fp_w_divs(adj, den);
}

/* (a * b) / den with that same rounding, for int64 operands whose
 * product needs 128 bits; den > 0, result known to fit int64. */
static int64_t muldiv_round(int64_t a, int64_t b, int64_t den)
{
    return fp_w_to_i64(div_round_w128(fp_w_muls(a, b),
                                      fp_w_from_i64(den)));
}

/* ================================================================== */
/*  Unit conversions: centidegrees <-> Q16.48 radians                  */
/* ================================================================== */

static fixed_t cdeg_to_rad(int32_t cdeg)
{
    fp_math_init(); /* FP_PI is computed at runtime */
    return (fixed_t)muldiv_round(cdeg, FP_PI,
                                 ASTRO_NAV_CDEG_PER_HALFTURN);
}

static int32_t rad_to_cdeg(fixed_t t)
{
    fp_math_init();
    int neg = (t < 0);
    if (neg) t = -t;
    /* Scale t/pi (Q16.48, at most ~2 for any angle up to a full turn)
     * to centidegrees in 128-bit: an angle near a full turn is 36000
     * cdeg, past Q16.48's +-32768 integer range, so an fp_mul here
     * would wrap its int64 result (it did, for GMST > 327.68 deg). */
    fp_w128 cdeg = fp_w_muls(fp_div(t, FP_PI),
                             ASTRO_NAV_CDEG_PER_HALFTURN);
    int32_t r = (int32_t)fp_w_to_i64(
        fp_w_asr(fp_w_add(cdeg, fp_w_from_i64(FP_HALF)), FP_PRECISION));
    return neg ? -r : r;
}

/* ================================================================== */
/*  Inverse trig: CORDIC vectoring mode                                */
/* ================================================================== */

/* CORDIC vectoring: rotate (x, y) onto the +x axis, accumulating the
 * rotation angle. Returns atan2(y, x) for the first quadrant (x, y >= 0)
 * in [0, pi/2]. One 48-iteration shift/add pass over the same
 * fp_cordic_angles[] table fp_sincos's rotation mode uses -- versus the
 * old binary search, which cost 50 full fp_sincos calls per inverse.
 * Convergence range is |angle| <= sum(atan 2^-i) ~ 1.743 rad > pi/2.
 * Overflow: inputs are <= 1.0; CORDIC gain K ~ 1.647 keeps every
 * intermediate below 2.4, far inside Q16.48 range. */
static fixed_t fp_atan2_acute(fixed_t y, fixed_t x)
{
    if (x == 0 && y == 0) return 0; /* callers gate this; belt-and-braces */

    fixed_t z = 0;
    for (int i = 0; i < FP_CORDIC_N; i++) {
        fixed_t xs = x >> i;
        fixed_t ys = y >> i;
        if (y >= 0) {
            x += ys; y -= xs; z += fp_cordic_angles[i];
        } else {
            x -= ys; y += xs; z -= fp_cordic_angles[i];
        }
    }
    return z;
}

/* asin(s) = atan2(|s|, sqrt(1 - s^2)), sign restored afterward.
 * (1 - s)(1 + s) avoids squaring: both factors are in [0, 2], so the
 * product is <= 1 and fp_mul cannot overflow. */
static fixed_t fp_asin_local(fixed_t s)
{
    if (s > FP_ONE)  s = FP_ONE;
    if (s < -FP_ONE) s = -FP_ONE;

    fixed_t as = s < 0 ? -s : s;
    fixed_t c = fp_sqrt(fp_mul(FP_ONE - as, FP_ONE + as));
    fixed_t a = fp_atan2_acute(as, c);
    return s < 0 ? -a : a;
}

static fixed_t fp_atan2_local(fixed_t y, fixed_t x)
{
    fixed_t ax = x < 0 ? -x : x;
    fixed_t ay = y < 0 ? -y : y;
    fixed_t a = fp_atan2_acute(ay, ax);
    if (x >= 0) return y >= 0 ? a : -a;
    return y >= 0 ? FP_PI - a : -FP_PI + a;
}

/* ================================================================== */
/*  Shared sight terms                                                 */
/* ================================================================== */

typedef struct {
    fixed_t sphi, cphi;
    fixed_t sdec, cdec;
    fixed_t slha, clha;
} sight_terms_t;

int32_t astro_nav_lha_cdeg(int32_t gha_cdeg, int32_t lon_east_cdeg)
{
    /* Sum in 64-bit: in-domain inputs can't overflow int32, but this
     * helper shouldn't be the one place an out-of-domain call turns
     * into UB rather than a wrong answer. */
    int32_t lha = (int32_t)(((int64_t)gha_cdeg + lon_east_cdeg)
                            % ASTRO_NAV_CDEG_PER_TURN);
    if (lha < 0) lha += ASTRO_NAV_CDEG_PER_TURN;
    return lha;
}

static void compute_sight_terms(int32_t phi_cdeg, int32_t lon_east_cdeg,
                                int32_t gha_cdeg, int32_t dec_cdeg,
                                sight_terms_t *terms)
{
    fp_math_init();
    int32_t lha_cdeg = astro_nav_lha_cdeg(gha_cdeg, lon_east_cdeg);
    fp_sincos(cdeg_to_rad(phi_cdeg), &terms->cphi, &terms->sphi);
    fp_sincos(cdeg_to_rad(dec_cdeg), &terms->cdec, &terms->sdec);
    fp_sincos(cdeg_to_rad(lha_cdeg), &terms->clha, &terms->slha);
}

static int32_t sight_altitude_from_terms(const sight_terms_t *t)
{
    fixed_t sin_hc = fp_mul(t->sphi, t->sdec)
                   + fp_mul(fp_mul(t->cphi, t->cdec), t->clha);
    if (sin_hc > FP_ONE)  sin_hc = FP_ONE;
    if (sin_hc < -FP_ONE) sin_hc = -FP_ONE;

    return rad_to_cdeg(fp_asin_local(sin_hc));
}

static void sight_horizontal_from_terms(const sight_terms_t *t,
                                        fixed_t *north, fixed_t *east)
{
    *north = fp_mul(t->cphi, t->sdec)
           - fp_mul(fp_mul(t->sphi, t->cdec), t->clha);
    *east = -fp_mul(t->cdec, t->slha);
}

/* CORDIC residue at exact zenith/nadir is a handful of Q48 ulps. */
#define AZIMUTH_EPS ((fixed_t)1 << 20)

/* ================================================================== */
/*  Method A -- classic spherical trig in Q16.48                       */
/* ================================================================== */

void astro_nav_reduce_method_a(int32_t lat_cdeg, int32_t lon_east_cdeg,
                               int32_t gha_cdeg, int32_t dec_cdeg,
                               astro_nav_sight_result_t *out)
{
    sight_terms_t t;
    compute_sight_terms(lat_cdeg, lon_east_cdeg, gha_cdeg, dec_cdeg, &t);

    fixed_t north, east;
    sight_horizontal_from_terms(&t, &north, &east);

    out->hc_cdeg = sight_altitude_from_terms(&t);
    out->zn_valid = !((north < AZIMUTH_EPS && north > -AZIMUTH_EPS)
                   && (east  < AZIMUTH_EPS && east  > -AZIMUTH_EPS));
    if (out->zn_valid) {
        int32_t signed_zn = rad_to_cdeg(fp_atan2_local(east, north));
        out->zn_cdeg = signed_zn < 0
            ? signed_zn + ASTRO_NAV_CDEG_PER_TURN : signed_zn;
        if (out->zn_cdeg == ASTRO_NAV_CDEG_PER_TURN) out->zn_cdeg = 0;
    } else {
        out->zn_cdeg = 0;
    }
}

/* ================================================================== */
/*  Method B -- square-ray azimuth                                     */
/* ================================================================== */

static const uint16_t atan_ratio_mdeg[33] = {
        0,  1790,  3576,  5356,  7125,  8881, 10620, 12339, 14036,
    15709, 17354, 18970, 20556, 22109, 23629, 25115, 26565, 27979,
    29358, 30700, 32005, 33275, 34509, 35707, 36870, 37999, 39094,
    40156, 41186, 42184, 43152, 44091, 45000,
};

static uint64_t fixed_mag(fixed_t v)
{
    return v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
}

/* Interpolate the 33-entry table: acute angle in centidegrees for a
 * square-perimeter ratio in Q0.16 ([0, 65536] = [0, 1]). Shared by the
 * Method B path and the square_key -> Zn human-boundary converter. */
static int32_t atan_cdeg_from_ratio_q16(uint32_t ratio_q16)
{
    uint32_t pos = ratio_q16 * 32u;
    uint32_t index = pos >> 16;
    uint32_t mdeg;
    if (index >= 32u) {
        mdeg = atan_ratio_mdeg[32];
    } else {
        uint32_t fraction = pos & 0xffffu;
        uint64_t weighted = (uint64_t)atan_ratio_mdeg[index] *
                            (65536u - fraction)
                          + (uint64_t)atan_ratio_mdeg[index + 1] * fraction;
        mdeg = (uint32_t)((weighted + 32768u) >> 16);
    }
    return (int32_t)((mdeg + 5u) / 10u);
}

static int32_t square_ratio_angle_cdeg(uint64_t minor, uint64_t major,
                                       uint32_t *ratio_q16_out)
{
    uint32_t ratio_q16 = major
        ? (uint32_t)fp_w_to_i64(fp_w_divs(
              fp_w_shl(fp_w_from_u64(minor), 16),
              fp_w_from_u64(major))) : 0;
    if (ratio_q16 > 65536u) ratio_q16 = 65536u;
    *ratio_q16_out = ratio_q16;
    return atan_cdeg_from_ratio_q16(ratio_q16);
}

static void square_ray_azimuth(fixed_t north, fixed_t east,
                               astro_nav_square_result_t *out)
{
    uint64_t an = fixed_mag(north);
    uint64_t ae = fixed_mag(east);
    out->sight.zn_valid = !((north < AZIMUTH_EPS && north > -AZIMUTH_EPS)
                         && (east  < AZIMUTH_EPS && east  > -AZIMUTH_EPS));
    if (!out->sight.zn_valid) {
        out->sight.zn_cdeg = 0;
        out->square_key = 0;
        return;
    }

    uint32_t ratio_q16;
    int32_t acute_cdeg;
    uint32_t acute_key;
    if (ae <= an) {
        acute_cdeg = square_ratio_angle_cdeg(ae, an, &ratio_q16);
        acute_key = (ratio_q16 * 8192u + 32768u) >> 16;
    } else {
        int32_t complement = square_ratio_angle_cdeg(an, ae, &ratio_q16);
        acute_cdeg = ASTRO_NAV_CDEG_PER_QUARTER - complement;
        acute_key = 16384u - ((ratio_q16 * 8192u + 32768u) >> 16);
    }

    uint32_t key;
    int32_t zn;
    if (north >= 0 && east >= 0) {
        key = acute_key;
        zn = acute_cdeg;
    } else if (north < 0 && east >= 0) {
        key = 32768u - acute_key;
        zn = ASTRO_NAV_CDEG_PER_HALFTURN - acute_cdeg;
    } else if (north < 0) {
        key = 32768u + acute_key;
        zn = ASTRO_NAV_CDEG_PER_HALFTURN + acute_cdeg;
    } else {
        key = 65536u - acute_key;
        zn = ASTRO_NAV_CDEG_PER_TURN - acute_cdeg;
    }

    out->square_key = (uint16_t)(key & 0xffffu);
    out->sight.zn_cdeg = zn == ASTRO_NAV_CDEG_PER_TURN ? 0 : zn;
}

void astro_nav_reduce_method_b(int32_t lat_cdeg, int32_t lon_east_cdeg,
                               int32_t gha_cdeg, int32_t dec_cdeg,
                               astro_nav_square_result_t *out)
{
    sight_terms_t t;
    compute_sight_terms(lat_cdeg, lon_east_cdeg, gha_cdeg, dec_cdeg, &t);
    out->sight.hc_cdeg = sight_altitude_from_terms(&t);

    fixed_t north, east;
    sight_horizontal_from_terms(&t, &north, &east);
    square_ray_azimuth(north, east, out);
}

/* ================================================================== */
/*  Method C -- machine-native almanac (unit vectors in, keys out)     */
/* ================================================================== */

/* Method C is what the reduction looks like when the almanac itself is
 * machine-native: bodies published as Q2.30 earth-fixed unit vectors
 * instead of GHA/declination angles. With O = observer and B = body,
 *
 *   sin Hc = O . B
 *   e = Ox*By - Oy*Bx          ( = cos(phi) * E_classical )
 *   n = Bz  - Oz*(O . B)       ( = cos(phi) * N_classical )
 *
 * Both horizontal components carry the same cos(phi) >= 0 factor, so
 * in real arithmetic the direction -- and therefore the square key --
 * is identical to the classical azimuth; with rounded Q2.30 inputs it
 * agrees to within quantization (<= 1 cdeg vs Method A in the sweep).
 * No sin/cos, no CORDIC, no table: the hot path is
 * three dot/cross products and one ratio division. Angles appear only
 * in the boundary converters below, where a human asks for them. */

#define Q30_ONE ((int64_t)1 << 30)

/* Round Q16.48 to Q2.30 and clamp to +-1.0. Add-half-then-arithmetic-
 * shift is round-to-nearest for either sign (ties toward +inf, the one
 * asymmetry). Right-shifting a negative is implementation-defined in
 * C99, not UB; it is arithmetic on every compiler with __int128 (a hard
 * requirement here), this file already relies on that in fp_mul and the
 * CORDIC loops, and the cross-platform golden gate pins the bits. */
static int32_t q48_to_q30(fixed_t v)
{
    fixed_t r = (v + ((fixed_t)1 << 17)) >> 18;
    if (r >  Q30_ONE) r =  Q30_ONE;
    if (r < -Q30_ONE) r = -Q30_ONE;
    return (int32_t)r;
}

void astro_nav_unitvec_from_cdeg(int32_t lat_cdeg, int32_t lon_east_cdeg,
                                 astro_nav_unitvec_t *out)
{
    fixed_t clat, slat, clon, slon;
    fp_math_init();
    fp_sincos(cdeg_to_rad(lat_cdeg), &clat, &slat);
    fp_sincos(cdeg_to_rad(lon_east_cdeg), &clon, &slon);
    out->x = q48_to_q30(fp_mul(clat, clon));
    out->y = q48_to_q30(fp_mul(clat, slon));
    out->z = q48_to_q30(slat);
}

/* Zenith/pole guard in the Q2.60 horizontal products: ~2^-28 of a unit
 * vector, the same real magnitude as AZIMUTH_EPS in Q16.48 and safely
 * above the ~2^-29 noise floor of rounded Q2.30 inputs. */
#define AZIMUTH_EPS_Q60 ((int64_t)1 << 32)

/* Acute square key in [0, 8192] for minor/major, minor <= major.
 * Same rounding as the Method B key path. */
static uint32_t square_acute_key(uint64_t minor, uint64_t major)
{
    uint32_t ratio_q16 = major
        ? (uint32_t)fp_w_to_i64(fp_w_divs(
              fp_w_shl(fp_w_from_u64(minor), 16),
              fp_w_from_u64(major))) : 0;
    if (ratio_q16 > 65536u) ratio_q16 = 65536u;
    return (ratio_q16 * 8192u + 32768u) >> 16;
}

void astro_nav_reduce_method_c(const astro_nav_unitvec_t *observer,
                               const astro_nav_unitvec_t *body,
                               astro_nav_machine_sight_t *out)
{
    int64_t ox = observer->x, oy = observer->y, oz = observer->z;
    int64_t bx = body->x,     by = body->y,     bz = body->z;

    /* sin Hc = O . B: Q2.60 products, round to Q2.30 (round-to-nearest;
     * see q48_to_q30 on the shift idiom). Each product is <= 2^60 and
     * the sum <= 3 * 2^60, well inside int64. */
    int64_t s_q60 = ox * bx + oy * by + oz * bz;
    int64_t s = (s_q60 + ((int64_t)1 << 29)) >> 30;
    if (s >  Q30_ONE) s =  Q30_ONE;
    if (s < -Q30_ONE) s = -Q30_ONE;
    out->sin_hc_q30 = (int32_t)s;

    /* Horizontal components, both scaled by cos(phi) >= 0 (Q2.60).
     * bz * Q30_ONE instead of bz << 30: left-shifting a negative is UB. */
    int64_t e_q60 = ox * by - oy * bx;
    int64_t n_q60 = bz * Q30_ONE - oz * s;

    int64_t ae = e_q60 < 0 ? -e_q60 : e_q60;
    int64_t an = n_q60 < 0 ? -n_q60 : n_q60;

    /* Degenerate when the body is at zenith/nadir OR the observer is at
     * a pole (cos(phi) = 0 kills both components). Methods A/B, fed
     * angles, still resolve the pole; a bare pole vector carries no
     * meridian, so C reports zn_valid = 0 there. */
    out->zn_valid = !(ae < AZIMUTH_EPS_Q60 && an < AZIMUTH_EPS_Q60);
    if (!out->zn_valid) {
        out->square_key = 0;
        return;
    }

    uint32_t acute_key = (ae <= an)
        ? square_acute_key((uint64_t)ae, (uint64_t)an)
        : 16384u - square_acute_key((uint64_t)an, (uint64_t)ae);

    uint32_t key;
    if (n_q60 >= 0 && e_q60 >= 0)     key = acute_key;
    else if (n_q60 < 0 && e_q60 >= 0) key = 32768u - acute_key;
    else if (n_q60 < 0)               key = 32768u + acute_key;
    else                              key = 65536u - acute_key;
    out->square_key = (uint16_t)(key & 0xffffu);
}

int32_t astro_nav_hc_cdeg_from_sin_q30(int32_t sin_hc_q30)
{
    fp_math_init();
    fixed_t s = (fixed_t)sin_hc_q30 * ((fixed_t)1 << 18); /* Q2.30 -> Q16.48 */
    if (s >  FP_ONE) s =  FP_ONE;
    if (s < -FP_ONE) s = -FP_ONE;
    return rad_to_cdeg(fp_asin_local(s));
}

int32_t astro_nav_zn_cdeg_from_square_key(uint16_t square_key)
{
    /* Invert the key construction: top 2 bits = quadrant, low 14 bits =
     * position along the quadrant edge. t <= 8192 is the minor/major
     * half (ratio = t * 8 in Q0.16); t > 8192 is the swapped half. */
    uint32_t quadrant = square_key >> 14;
    uint32_t t = square_key & 0x3fffu;
    int32_t acute = (t <= 8192u)
        ? atan_cdeg_from_ratio_q16(t * 8u)
        : ASTRO_NAV_CDEG_PER_QUARTER
          - atan_cdeg_from_ratio_q16((16384u - t) * 8u);
    return (int32_t)quadrant * ASTRO_NAV_CDEG_PER_QUARTER + acute;
}

/* ================================================================== */
/*  Two-body fix -- circle-of-equal-altitude intersection              */
/* ================================================================== */

/* Each sight is exactly the constraint O . B = sin Ho: a plane cutting
 * the unit sphere in a circle of equal altitude. Two sights plus
 * |O| = 1 have a closed-form solution. With g = B1 . B2:
 *
 *   O   = a B1 + b B2 +- c (B1 x B2)
 *   a   = (s1 - g s2) / (1 - g^2)
 *   b   = (s2 - g s1) / (1 - g^2)
 *   c^2 = (1 - a s1 - b s2) / (1 - g^2)
 *
 * The Marcq St. Hilaire intercept/LOP construction is the hand-plotting
 * linearization of these same two circles; in vector form the exact
 * answer is simpler than the approximation. Conditioning is geometric,
 * not arithmetic: position error scales as observation error divided by
 * sin(cut angle). The gates below reject only the hopeless geometries;
 * quality of cut remains the navigator's judgment, as it always was.
 *
 * Everything runs in Q16.48 via fp_mul/fp_div/fp_sqrt -- integer only,
 * zero CORDIC iterations, no angle in any unit. Range audit: the
 * parallel gate guarantees 1 - g^2 >= 2^-12, so |a|, |b| <= 2^13 and
 * c^2 <= 2^12, all far inside fp_mul's |product| < 32768 domain, and
 * a s1 + b s2 >= 0 (positive-definite form) bounds the c^2 numerator. */

/* 1 - g^2 below this (~cut angle under 0.9 deg): circles effectively
 * concentric, no usable intersection. Also the overflow guard above. */
#define FIX_EPS_PARALLEL ((fixed_t)FP_ONE >> 12)

/* c^2 within -2^-16 of zero snaps to a tangent solution (observation
 * noise around genuine tangency); anything more negative means the
 * circles simply do not intersect. */
#define FIX_EPS_TANGENT  ((fixed_t)FP_ONE >> 16)

static fixed_t q30_to_q48(int32_t v)
{
    return (fixed_t)v * ((fixed_t)1 << 18);
}

static fixed_t sin_q30_to_q48_clamped(int32_t v)
{
    fixed_t s = q30_to_q48(v);
    if (s >  FP_ONE) s =  FP_ONE;
    if (s < -FP_ONE) s = -FP_ONE;
    return s;
}

void astro_nav_fix_two_body(const astro_nav_unitvec_t *body1,
                            int32_t sin_ho1_q30,
                            const astro_nav_unitvec_t *body2,
                            int32_t sin_ho2_q30,
                            const astro_nav_unitvec_t *dr_hint,
                            astro_nav_fix_result_t *out)
{
    out->position.x = 0; out->position.y = 0; out->position.z = 0;
    out->alternate = out->position;
    out->valid = 0;

    fixed_t b1x = q30_to_q48(body1->x);
    fixed_t b1y = q30_to_q48(body1->y);
    fixed_t b1z = q30_to_q48(body1->z);
    fixed_t b2x = q30_to_q48(body2->x);
    fixed_t b2y = q30_to_q48(body2->y);
    fixed_t b2z = q30_to_q48(body2->z);
    fixed_t s1  = sin_q30_to_q48_clamped(sin_ho1_q30);
    fixed_t s2  = sin_q30_to_q48_clamped(sin_ho2_q30);

    fixed_t g = fp_mul(b1x, b2x) + fp_mul(b1y, b2y) + fp_mul(b1z, b2z);
    fixed_t omg2 = FP_ONE - fp_mul(g, g);
    if (omg2 < FIX_EPS_PARALLEL)
        return;

    fixed_t a = fp_div(s1 - fp_mul(g, s2), omg2);
    fixed_t b = fp_div(s2 - fp_mul(g, s1), omg2);

    /* c^2 numerator; a s1 + b s2 = |in-plane part|^2 >= 0. */
    fixed_t num = FP_ONE - fp_mul(a, s1) - fp_mul(b, s2);
    if (num < -fp_mul(omg2, FIX_EPS_TANGENT))
        return;
    fixed_t c = num <= 0 ? 0 : fp_sqrt(fp_div(num, omg2));

    /* W = B1 x B2, |W|^2 = 1 - g^2. */
    fixed_t wx = fp_mul(b1y, b2z) - fp_mul(b1z, b2y);
    fixed_t wy = fp_mul(b1z, b2x) - fp_mul(b1x, b2z);
    fixed_t wz = fp_mul(b1x, b2y) - fp_mul(b1y, b2x);

    /* In-plane part; |a B1 + b B2|^2 = a s1 + b s2 <= 1 once the
     * tangency gate passes, so components are unit-bounded even when
     * a, b individually are large. */
    fixed_t px = fp_mul(a, b1x) + fp_mul(b, b2x);
    fixed_t py = fp_mul(a, b1y) + fp_mul(b, b2y);
    fixed_t pz = fp_mul(a, b1z) + fp_mul(b, b2z);

    /* The two intersections differ by +-c W; the hint (any positive
     * scale of a rough direction) picks the nearer hemisphere. */
    fixed_t hdot = fp_mul(wx, q30_to_q48(dr_hint->x))
                 + fp_mul(wy, q30_to_q48(dr_hint->y))
                 + fp_mul(wz, q30_to_q48(dr_hint->z));
    fixed_t cs = hdot >= 0 ? c : -c;

    out->position.x  = q48_to_q30(px + fp_mul(cs, wx));
    out->position.y  = q48_to_q30(py + fp_mul(cs, wy));
    out->position.z  = q48_to_q30(pz + fp_mul(cs, wz));
    out->alternate.x = q48_to_q30(px - fp_mul(cs, wx));
    out->alternate.y = q48_to_q30(py - fp_mul(cs, wy));
    out->alternate.z = q48_to_q30(pz - fp_mul(cs, wz));
    out->valid = 1;
}

/* ================================================================== */
/*  Running-fix advancement -- vessel run as a rotation                 */
/* ================================================================== */

/* Tenths of a nautical mile to radians of arc: 0.1 nm = 0.1 arcmin =
 * 1/6 cdeg, so rad = tenths * pi / 108000, rounded to nearest. */
static fixed_t run_tenths_to_rad(int32_t tenths)
{
    fp_math_init();
    return (fixed_t)muldiv_round(tenths, FP_PI, 108000);
}

void astro_nav_advance_body_for_run(const astro_nav_unitvec_t *body,
                                    const astro_nav_unitvec_t *dr,
                                    int32_t course_cdeg,
                                    int32_t run_tenths_nm,
                                    astro_nav_unitvec_t *out)
{
    fp_math_init();

    /* Course and run are periodic (a run is a rotation angle; one
     * full turn = 216000 tenths-nm of great circle), so reduce them
     * exactly -- extreme int32 arguments would otherwise overflow the
     * Q16.48 radian conversions. */
    course_cdeg   = course_cdeg % ASTRO_NAV_CDEG_PER_TURN;
    run_tenths_nm = run_tenths_nm % 216000;

    fixed_t px = q30_to_q48(dr->x);
    fixed_t py = q30_to_q48(dr->y);
    fixed_t pz = q30_to_q48(dr->z);

    /* Local east at the DR: normalize (z-hat x P). Degenerate at a
     * pole, where a course has no direction -- return B unchanged. */
    fixed_t h2 = fp_mul(px, px) + fp_mul(py, py);
    if (h2 < (FP_ONE >> 24)) {
        *out = *body;
        return;
    }
    fixed_t inv_h = fp_inv_sqrt(h2);
    fixed_t ex = fp_mul(-py, inv_h);
    fixed_t ey = fp_mul(px, inv_h);
    /* Local north = P x east (unit, since east is unit and normal
     * to P); east has no z component, so this simplifies. */
    fixed_t nx = -fp_mul(pz, ey);
    fixed_t ny = fp_mul(pz, ex);
    fixed_t nz = fp_mul(px, ey) - fp_mul(py, ex);

    /* Track direction t = cos(course) north + sin(course) east, and
     * rotation axis a = P x t: rotating about a by the run angle
     * carries P along the great-circle track (R P = P cos + t sin). */
    fixed_t cc, sc;
    fp_sincos(cdeg_to_rad(course_cdeg), &cc, &sc);
    fixed_t tx = fp_mul(cc, nx) + fp_mul(sc, ex);
    fixed_t ty = fp_mul(cc, ny) + fp_mul(sc, ey);
    fixed_t tz = fp_mul(cc, nz);
    fixed_t ax = fp_mul(py, tz) - fp_mul(pz, ty);
    fixed_t ay = fp_mul(pz, tx) - fp_mul(px, tz);
    fixed_t az = fp_mul(px, ty) - fp_mul(py, tx);

    fixed_t ct, st;
    fp_sincos(run_tenths_to_rad(run_tenths_nm), &ct, &st);

    /* Rodrigues: B' = B cos + (a x B) sin + a (a . B)(1 - cos). */
    fixed_t bx = q30_to_q48(body->x);
    fixed_t by = q30_to_q48(body->y);
    fixed_t bz = q30_to_q48(body->z);
    fixed_t adb = fp_mul(ax, bx) + fp_mul(ay, by) + fp_mul(az, bz);
    fixed_t k = fp_mul(adb, FP_ONE - ct);
    fixed_t xx = fp_mul(ay, bz) - fp_mul(az, by);
    fixed_t xy = fp_mul(az, bx) - fp_mul(ax, bz);
    fixed_t xz = fp_mul(ax, by) - fp_mul(ay, bx);

    out->x = q48_to_q30(fp_mul(bx, ct) + fp_mul(xx, st) + fp_mul(ax, k));
    out->y = q48_to_q30(fp_mul(by, ct) + fp_mul(xy, st) + fp_mul(ay, k));
    out->z = q48_to_q30(fp_mul(bz, ct) + fp_mul(xz, st) + fp_mul(az, k));
}

/* ================================================================== */
/*  N-body fix -- least squares over the same circle constraints       */
/* ================================================================== */

/* Gauss-Newton on the unit sphere. Each iteration linearizes
 * sin Hc_i = O . B_i on the tangent plane at the current estimate:
 * the derivative along a tangent direction t is just t . B_i, so one
 * step is a 2x2 normal-equation solve. Residuals are weighted by
 * 1/cos Hc_i, which converts the sin-domain misfit to an angular one --
 * every sight then counts in arcminutes (= nautical miles), instead of
 * high sights quietly counting less.
 *
 * Range audit: weights are pre-scaled by 1/16 and capped at 1.0 (the
 * cap engages above ~86 deg altitude), so with n <= 32 every
 * normal-equation accumulator stays below 64 and every fp_mul operand
 * far inside range; steps are clamped to 0.25 rad, and the guarded
 * division below cannot overflow fp_div. */

#define FIX_N_MAX_SIGHTS 32
#define FIX_N_MAX_ITER    8
#define FIX_N_STEP_CLAMP ((fixed_t)FP_ONE >> 2)   /* 0.25 rad per step */
#define FIX_N_CONVERGED  ((fixed_t)1 << 22)       /* 2^-26 rad step    */
#define FIX_N_DET_SHIFT  10   /* reject det/(a11 a22) < 2^-10, ~1.8 deg */

/* num / den clamped to +-clampv, without letting fp_div overflow when
 * den is small: compare against den * clampv first. */
static fixed_t fp_div_clamped(fixed_t num, fixed_t den, fixed_t clampv)
{
    fixed_t lim = fp_mul(den, clampv);
    if (num >= lim)  return clampv;
    if (num <= -lim) return -clampv;
    return fp_div(num, den);
}

/* Q16.48 radians to milli-arcminutes (10800000 marcmin per pi rad). */
static int64_t rad_to_marcmin(fixed_t rad)
{
    return muldiv_round(rad, 10800000, FP_PI);
}

void astro_nav_fix_n_body(const astro_nav_unitvec_t *bodies,
                          const int32_t *sin_ho_q30,
                          int n,
                          const astro_nav_unitvec_t *seed,
                          astro_nav_fixn_result_t *out)
{
    out->position.x = 0; out->position.y = 0; out->position.z = 0;
    out->valid = 0;
    out->iterations = 0;
    out->max_residual_marcmin = 0;
    if (n < 2 || n > FIX_N_MAX_SIGHTS)
        return;

    fixed_t bx[FIX_N_MAX_SIGHTS], by[FIX_N_MAX_SIGHTS];
    fixed_t bz[FIX_N_MAX_SIGHTS], s[FIX_N_MAX_SIGHTS];
    fixed_t w[FIX_N_MAX_SIGHTS];
    for (int i = 0; i < n; i++) {
        bx[i] = q30_to_q48(bodies[i].x);
        by[i] = q30_to_q48(bodies[i].y);
        bz[i] = q30_to_q48(bodies[i].z);
        s[i]  = sin_q30_to_q48_clamped(sin_ho_q30[i]);
        /* Angle-domain weight 1/cos Ho, pre-scaled by 1/16, capped at
         * 1.0; cos^2 = (1 - s)(1 + s) cannot overflow fp_mul. */
        fixed_t c2 = fp_mul(FP_ONE - s[i], FP_ONE + s[i]);
        fixed_t wi = (c2 < (FP_ONE >> 8)) ? FP_ONE
                                          : (fp_inv_sqrt(c2) >> 4);
        if (wi > FP_ONE) wi = FP_ONE;
        w[i] = wi;
    }

    /* Normalized seed. */
    fixed_t ox = q30_to_q48(seed->x);
    fixed_t oy = q30_to_q48(seed->y);
    fixed_t oz = q30_to_q48(seed->z);
    fixed_t norm2 = fp_mul(ox, ox) + fp_mul(oy, oy) + fp_mul(oz, oz);
    if (norm2 < (FP_ONE >> 8))
        return;
    fixed_t inv = fp_inv_sqrt(norm2);
    ox = fp_mul(ox, inv); oy = fp_mul(oy, inv); oz = fp_mul(oz, inv);

    int converged = 0;
    int iter;
    for (iter = 0; iter < FIX_N_MAX_ITER && !converged; iter++) {
        /* Orthonormal tangent basis at O, built from the smallest
         * component of O (never degenerate, poles included). */
        fixed_t aox = ox < 0 ? -ox : ox;
        fixed_t aoy = oy < 0 ? -oy : oy;
        fixed_t aoz = oz < 0 ? -oz : oz;
        fixed_t t1x, t1y, t1z;
        if (aox <= aoy && aox <= aoz) {
            t1x = 0;   t1y = -oz; t1z = oy;    /* x-hat x O */
        } else if (aoy <= aoz) {
            t1x = oz;  t1y = 0;   t1z = -ox;   /* y-hat x O */
        } else {
            t1x = -oy; t1y = ox;  t1z = 0;     /* z-hat x O */
        }
        /* |t1|^2 = 1 - O_k^2 >= 2/3 for the smallest component. */
        fixed_t it1 = fp_inv_sqrt(fp_mul(t1x, t1x) + fp_mul(t1y, t1y)
                                  + fp_mul(t1z, t1z));
        t1x = fp_mul(t1x, it1); t1y = fp_mul(t1y, it1);
        t1z = fp_mul(t1z, it1);
        fixed_t t2x = fp_mul(oy, t1z) - fp_mul(oz, t1y);
        fixed_t t2y = fp_mul(oz, t1x) - fp_mul(ox, t1z);
        fixed_t t2z = fp_mul(ox, t1y) - fp_mul(oy, t1x);

        fixed_t a11 = 0, a12 = 0, a22 = 0, g1 = 0, g2 = 0;
        for (int i = 0; i < n; i++) {
            fixed_t sh = fp_mul(ox, bx[i]) + fp_mul(oy, by[i])
                       + fp_mul(oz, bz[i]);
            fixed_t r  = fp_mul(w[i], s[i] - sh);
            fixed_t j1 = fp_mul(w[i], fp_mul(t1x, bx[i])
                       + fp_mul(t1y, by[i]) + fp_mul(t1z, bz[i]));
            fixed_t j2 = fp_mul(w[i], fp_mul(t2x, bx[i])
                       + fp_mul(t2y, by[i]) + fp_mul(t2z, bz[i]));
            a11 += fp_mul(j1, j1);
            a12 += fp_mul(j1, j2);
            a22 += fp_mul(j2, j2);
            g1  += fp_mul(j1, r);
            g2  += fp_mul(j2, r);
        }

        /* det / (a11 a22) = sin^2 of the effective azimuth-spread
         * angle, so a relative gate is scale-free: it rejects the same
         * near-parallel geometry whether the sights are high (weight
         * capped) or low (weight ~1/16). ~2^-10 is a ~1.8 deg spread,
         * matching the two-body parallel gate. */
        fixed_t tr = fp_mul(a11, a22);
        fixed_t det = tr - fp_mul(a12, a12);
        if (det <= 0 || det < (tr >> FIX_N_DET_SHIFT))
            return;   /* all circles near-parallel here: no cut */

        fixed_t u = fp_div_clamped(fp_mul(a22, g1) - fp_mul(a12, g2),
                                   det, FIX_N_STEP_CLAMP);
        fixed_t v = fp_div_clamped(fp_mul(a11, g2) - fp_mul(a12, g1),
                                   det, FIX_N_STEP_CLAMP);

        ox += fp_mul(u, t1x) + fp_mul(v, t2x);
        oy += fp_mul(u, t1y) + fp_mul(v, t2y);
        oz += fp_mul(u, t1z) + fp_mul(v, t2z);
        inv = fp_inv_sqrt(fp_mul(ox, ox) + fp_mul(oy, oy)
                          + fp_mul(oz, oz));
        ox = fp_mul(ox, inv); oy = fp_mul(oy, inv); oz = fp_mul(oz, inv);

        fixed_t au = u < 0 ? -u : u;
        fixed_t av = v < 0 ? -v : v;
        if (au < FIX_N_CONVERGED && av < FIX_N_CONVERGED)
            converged = 1;
    }
    out->iterations = iter;
    if (!converged)
        return;

    /* Worst single-sight angular miss (w restores 1/cos, the << 4
     * undoes the pre-scale; small-angle is exact at these residuals). */
    int64_t worst = 0;
    for (int i = 0; i < n; i++) {
        fixed_t sh = fp_mul(ox, bx[i]) + fp_mul(oy, by[i])
                   + fp_mul(oz, bz[i]);
        fixed_t r = fp_mul(w[i], s[i] - sh) * 16;
        int64_t m = rad_to_marcmin(r);
        if (m < 0) m = -m;
        if (m > worst) worst = m;
    }
    out->max_residual_marcmin = worst;

    out->position.x = q48_to_q30(ox);
    out->position.y = q48_to_q30(oy);
    out->position.z = q48_to_q30(oz);
    out->valid = 1;
}

int32_t astro_nav_sin_q30_from_cdeg(int32_t angle_cdeg)
{
    fixed_t c, s;
    fp_math_init();
    fp_sincos(cdeg_to_rad(angle_cdeg), &c, &s);
    return q48_to_q30(s);
}

void astro_nav_latlon_cdeg_from_unitvec(const astro_nav_unitvec_t *v,
                                        int32_t *lat_cdeg,
                                        int32_t *lon_east_cdeg)
{
    fp_math_init();
    *lat_cdeg = rad_to_cdeg(fp_asin_local(sin_q30_to_q48_clamped(v->z)));
    /* CORDIC vectoring is scale-invariant, so the Q2.30 components can
     * be fed to atan2 directly. At an exact pole (x = y = 0) longitude
     * is undefined; report 0. */
    *lon_east_cdeg = (v->x == 0 && v->y == 0)
        ? 0
        : rad_to_cdeg(fp_atan2_local((fixed_t)v->y, (fixed_t)v->x));
}

/* ================================================================== */
/*  Altitude corrections -- sextant Hs to observed Ho                  */
/* ================================================================== */

/* Milli-arcminutes to Q16.48 radians: rad = marcmin * pi / 10800000,
 * rounded to nearest (the exact mirror of cdeg_to_rad, x600 finer). */
static fixed_t marcmin_to_rad(int64_t marcmin)
{
    fp_math_init();
    return (fixed_t)muldiv_round(marcmin, FP_PI, 10800000);
}

int64_t astro_nav_dip_marcmin(int32_t eye_height_cm)
{
    if (eye_height_cm <= 0)
        return 0;
    if (eye_height_cm > 10000)
        eye_height_cm = 10000;
    fp_math_init();
    /* dip = 1.76' sqrt(h m) = 176 sqrt(h cm) milli-arcmin exactly. */
    fixed_t root = fp_sqrt((fixed_t)eye_height_cm << 48);
    fp_w128 num = fp_w_add(fp_w_muls(root, 176),
                           fp_w_from_i64((int64_t)1 << 47));
    return fp_w_to_i64(fp_w_asr(num, 48));
}

int64_t astro_nav_refraction_marcmin(int64_t ha_marcmin)
{
    fp_math_init();
    if (ha_marcmin < -60000)  ha_marcmin = -60000;
    if (ha_marcmin > 5400000) ha_marcmin = 5400000;

    /* Bennett's argument in degrees, Q16.48: h + 7.31 / (h + 4.4).
     * With h >= -1 the divisor is >= 3.4, comfortably positive.
     * Multiply, don't shift: ha_marcmin is negative below the horizon,
     * and left-shifting a negative signed value is UB. */
    fixed_t h_deg = (fixed_t)muldiv_round(ha_marcmin,
                                          (int64_t)1 << 48, 60000);
    fixed_t c731 = (fixed_t)(((int64_t)731 << 48) / 100);
    fixed_t c44  = (fixed_t)(((int64_t)44 << 48) / 10);
    fixed_t arg_deg = h_deg + fp_div(c731, h_deg + c44);

    /* Degrees to radians (arg is ~[1.15, 90.1], no overflow), then
     * R = cot(arg) arcmin = 1000 cot(arg) milli-arcmin. */
    fixed_t arg_rad = (fixed_t)muldiv_round(arg_deg, FP_PI,
                                            (int64_t)180 << 48);
    fixed_t c, s;
    fp_sincos(arg_rad, &c, &s);
    fixed_t cot = fp_div(c, s);
    if (cot <= 0)
        return 0;   /* Bennett dips microscopically below 0 at 90 deg */
    fp_w128 num = fp_w_add(fp_w_muls(cot, 1000),
                           fp_w_from_i64((int64_t)1 << 47));
    return fp_w_to_i64(fp_w_asr(num, 48));
}

int64_t astro_nav_refraction_tp_marcmin(int64_t ha_marcmin,
                                        int32_t temp_c,
                                        int32_t pressure_mb)
{
    /* Standard density scaling of the Bennett value:
     *   R_tp = R * (P / 1010) * (283 / (273 + T)),
     * one exactly-rounded integer division. At (10, 1010) the
     * numerator is r * 1010 * 283 = r * den, so the result is r
     * bit-for-bit and the standard function stays this one's special
     * case. r <= 34478 and P <= 1100 keep the numerator far inside
     * int64_t; den > 0 for all temp_c > -273. */
    int64_t r = astro_nav_refraction_marcmin(ha_marcmin);
    int64_t num = r * pressure_mb * 283;
    int64_t den = 1010LL * (273 + temp_c);
    return (num + den / 2) / den;   /* r >= 0, so num >= 0 */
}

int64_t astro_nav_parallax_marcmin(int64_t ha_marcmin, int64_t hp_marcmin)
{
    fixed_t c, s;
    fp_math_init();
    fp_sincos(marcmin_to_rad(ha_marcmin), &c, &s);
    /* Divide, don't shift: num < 0 (Ha past the zenith, or negative
     * HP) with the subtract-half idiom needs truncating division --
     * an arithmetic shift floors, landing one low for negatives. */
    return muldiv_round(hp_marcmin, c, (int64_t)1 << 48);
}

int64_t astro_nav_correct_altitude_tp_marcmin(int64_t hs_marcmin,
                                              int64_t index_error_marcmin,
                                              int32_t eye_height_cm,
                                              int64_t hp_marcmin,
                                              int64_t sd_marcmin,
                                              int limb,
                                              int32_t temp_c,
                                              int32_t pressure_mb)
{
    int64_t ha = hs_marcmin + index_error_marcmin
               - astro_nav_dip_marcmin(eye_height_cm);
    int64_t ho = ha - astro_nav_refraction_tp_marcmin(ha, temp_c,
                                                      pressure_mb)
               + astro_nav_parallax_marcmin(ha, hp_marcmin);
    if (limb > 0) ho += sd_marcmin;   /* lower limb */
    if (limb < 0) ho -= sd_marcmin;   /* upper limb */
    return ho;
}

int64_t astro_nav_correct_altitude_marcmin(int64_t hs_marcmin,
                                           int64_t index_error_marcmin,
                                           int32_t eye_height_cm,
                                           int64_t hp_marcmin,
                                           int64_t sd_marcmin,
                                           int limb)
{
    return astro_nav_correct_altitude_tp_marcmin(hs_marcmin,
                                                 index_error_marcmin,
                                                 eye_height_cm,
                                                 hp_marcmin, sd_marcmin,
                                                 limb, 10, 1010);
}

int64_t astro_nav_moon_augmentation_marcmin(int64_t h_marcmin,
                                            int64_t hp_marcmin,
                                            int64_t sd_marcmin)
{
    fp_math_init();
    if (h_marcmin < -5400000)  h_marcmin = -5400000;
    if (h_marcmin >  5400000)  h_marcmin =  5400000;
    if (hp_marcmin < 0)        hp_marcmin = 0;
    if (hp_marcmin > 120000)   hp_marcmin = 120000;

    fixed_t c, sh, sp;
    fp_sincos(marcmin_to_rad(h_marcmin), &c, &sh);
    fp_sincos(marcmin_to_rad(hp_marcmin), &c, &sp);

    /* Topocentric-to-geocentric distance ratio, law of cosines with
     * the observer one equatorial radius (= sin HP of the distance)
     * from the geocenter and the Moon's center at geocentric apparent
     * altitude h:  d = sqrt(1 - 2 sin(HP) sin(h) + sin^2(HP)).
     * The HP clamp keeps sin(HP) <= 0.035, so d is in [0.96, 1.04]
     * and the square root's argument is never near zero. */
    fixed_t one = (fixed_t)1 << FP_PRECISION;
    fixed_t d = fp_sqrt(one - 2 * fp_mul(sp, sh) + fp_mul(sp, sp));

    /* SD scales as 1/distance: SD_topo - SD = SD (1 - d) / d,
     * rounded to nearest (truncating division: the increment is
     * negative below the horizon, where d > 1). */
    return muldiv_round(sd_marcmin, fp_div(one - d, d),
                        (int64_t)1 << FP_PRECISION);
}

int64_t astro_nav_correct_altitude_moon_tp_marcmin(int64_t hs_marcmin,
                                                   int64_t index_error_marcmin,
                                                   int32_t eye_height_cm,
                                                   int64_t hp_marcmin,
                                                   int64_t sd_marcmin,
                                                   int limb,
                                                   int32_t temp_c,
                                                   int32_t pressure_mb)
{
    /* The generic chain at limb 0 is dip + refraction + parallax: the
     * altitude of the Moon's CENTER, which after parallax is the
     * geocentric apparent altitude -- exactly the angle the
     * augmentation wants. The limb step then applies the topocentric
     * semidiameter instead of the geocentric one. */
    int64_t hc = astro_nav_correct_altitude_tp_marcmin(hs_marcmin,
                     index_error_marcmin, eye_height_cm, hp_marcmin,
                     sd_marcmin, 0, temp_c, pressure_mb);
    int64_t sd_topo = sd_marcmin
        + astro_nav_moon_augmentation_marcmin(hc, hp_marcmin,
                                              sd_marcmin);
    if (limb > 0) return hc + sd_topo;   /* lower limb */
    if (limb < 0) return hc - sd_topo;   /* upper limb */
    return hc;
}

int64_t astro_nav_correct_altitude_moon_marcmin(int64_t hs_marcmin,
                                                int64_t index_error_marcmin,
                                                int32_t eye_height_cm,
                                                int64_t hp_marcmin,
                                                int64_t sd_marcmin,
                                                int limb)
{
    return astro_nav_correct_altitude_moon_tp_marcmin(hs_marcmin,
                                                      index_error_marcmin,
                                                      eye_height_cm,
                                                      hp_marcmin,
                                                      sd_marcmin,
                                                      limb, 10, 1010);
}

int32_t astro_nav_sin_q30_from_marcmin(int64_t angle_marcmin)
{
    fixed_t c, s;
    fp_math_init();
    fp_sincos(marcmin_to_rad(angle_marcmin), &c, &s);
    return q48_to_q30(s);
}

/* ================================================================== */
/*  Sight averaging -- a run of shots of one body into one Ho          */
/* ================================================================== */

/* Least-squares line over the kept shots, evaluated at one instant.
 * All values are recentred on the first kept shot (dt = t - t0,
 * dh = h - h0), which with the documented input ranges (|dt| < 2^40,
 * |dh| <= 2 * 5400000 < 2^24, n <= 32) bounds every 128-bit
 * intermediate below 2^121 -- and keeps dt, dh and their plain sums
 * inside int64, so only the products and A, B are wide:
 *   B     = n Sum(dt^2) - (Sum dt)^2        (>= 0; 0 iff all t equal)
 *   A     = n Sum(dt dh) - Sum dt Sum dh    (slope numerator)
 *   h(t)  = h0 + [Sum dh * B + A (n dt - Sum dt)] / (n B)
 * B = 0 collapses the line to the mean, defined only at the shots'
 * own (single) timestamp. Returns 0 and sets *fit_ok = 0 there when
 * asked for any other instant. */
typedef struct {
    fp_w128 a, b;        /* slope numerator / denominator          */
    int64_t sum_dh;
    int64_t sum_dt;
    int64_t t0, h0;
    int     n;
} avg_fit_t;

static void avg_fit(const int64_t *ho, const int64_t *t,
                    const int *keep, int n, avg_fit_t *f)
{
    int64_t sum_dt = 0, sum_dh = 0;
    fp_w128 sum_dtdt = fp_w_from_i64(0), sum_dtdh = fp_w_from_i64(0);
    int kept = 0;
    f->t0 = 0;
    f->h0 = 0;
    for (int i = 0; i < n; i++) {
        if (!keep[i]) continue;
        if (kept == 0) { f->t0 = t[i]; f->h0 = ho[i]; }
        int64_t dt = t[i] - f->t0;   /* range-guarded by the caller */
        int64_t dh = ho[i] - f->h0;
        sum_dt   += dt;
        sum_dh   += dh;
        sum_dtdt = fp_w_add(sum_dtdt, fp_w_muls(dt, dt));
        sum_dtdh = fp_w_add(sum_dtdh, fp_w_muls(dt, dh));
        kept++;
    }
    f->n = kept;
    f->a = fp_w_sub(fp_w_mul(fp_w_from_i64(kept), sum_dtdh),
                    fp_w_muls(sum_dt, sum_dh));
    f->b = fp_w_sub(fp_w_mul(fp_w_from_i64(kept), sum_dtdt),
                    fp_w_muls(sum_dt, sum_dt));
    f->sum_dt = sum_dt;
    f->sum_dh = sum_dh;
}

static fp_w128 avg_eval(const avg_fit_t *f, int64_t t_ms, int *fit_ok)
{
    int64_t dt = t_ms - f->t0;   /* range-guarded by the caller */
    *fit_ok = 1;
    if (fp_w_cmp(f->b, fp_w_from_i64(0)) == 0) {
        /* All kept shots share one timestamp: the line is just their
         * mean, and only at that instant -- no rate is observable. */
        if (dt != 0) { *fit_ok = 0; return fp_w_from_i64(0); }
        return fp_w_from_i64(f->h0 + fp_w_to_i64(div_round_w128(
                   fp_w_from_i64(f->sum_dh), fp_w_from_i64(f->n))));
    }
    fp_w128 num = fp_w_add(fp_w_mul(fp_w_from_i64(f->sum_dh), f->b),
                           fp_w_mul(f->a, fp_w_from_i64(
                               (int64_t)f->n * dt - f->sum_dt)));
    fp_w128 den = fp_w_mul(fp_w_from_i64(f->n), f->b);
    return fp_w_add(fp_w_from_i64(f->h0), div_round_w128(num, den));
}

void astro_nav_average_sights(const int64_t *ho_marcmin,
                              const int64_t *t_ms,
                              int n,
                              int64_t t_ref_ms,
                              int64_t reject_marcmin,
                              astro_nav_avg_result_t *out)
{
    out->ho_marcmin = 0;
    out->rate_marcmin_per_min = 0;
    out->max_residual_marcmin = 0;
    out->used = 0;
    out->valid = 0;
    if (n < 2 || n > 32) return;

    /* Range guards double as overflow guards for the 128-bit fit.
     * The guard subtraction itself must not overflow, so it widens
     * first -- extreme int64 timestamps are inputs to reject, not UB. */
    fp_w128 dt_lim = fp_w_from_i64((int64_t)1 << 40);
    for (int i = 0; i < n; i++) {
        if (ho_marcmin[i] < -5400000 || ho_marcmin[i] > 5400000) return;
        fp_w128 dt = fp_w_sub(fp_w_from_i64(t_ms[i]),
                              fp_w_from_i64(t_ms[0]));
        if (fp_w_cmp(dt, fp_w_neg(dt_lim)) < 0 ||
            fp_w_cmp(dt, dt_lim) > 0) return;
    }
    {
        fp_w128 dt = fp_w_sub(fp_w_from_i64(t_ref_ms),
                              fp_w_from_i64(t_ms[0]));
        if (fp_w_cmp(dt, fp_w_neg(dt_lim)) < 0 ||
            fp_w_cmp(dt, dt_lim) > 0) return;
    }

    int keep[32];
    for (int i = 0; i < n; i++) keep[i] = 1;

    avg_fit_t f;
    fp_w128 worst;
    int worst_i;
    for (;;) {
        avg_fit(ho_marcmin, t_ms, keep, n, &f);
        worst = fp_w_from_i64(-1);
        worst_i = -1;
        for (int i = 0; i < n; i++) {
            if (!keep[i]) continue;
            int fit_ok;
            fp_w128 r = fp_w_sub(fp_w_from_i64(ho_marcmin[i]),
                                 avg_eval(&f, t_ms[i], &fit_ok));
            if (fp_w_cmp(r, fp_w_from_i64(0)) < 0) r = fp_w_neg(r);
            if (fp_w_cmp(r, worst) > 0) { worst = r; worst_i = i; }
        }
        if (reject_marcmin <= 0 ||
            fp_w_cmp(worst, fp_w_from_i64(reject_marcmin)) <= 0 ||
            f.n <= 2)
            break;
        keep[worst_i] = 0;   /* drop the worst shot, refit */
    }

    int fit_ok;
    fp_w128 ho_ref = avg_eval(&f, t_ref_ms, &fit_ok);
    if (!fit_ok) return;
    /* The fitted line at t_ref must still be an altitude: the guards
     * above bound slope and span separately, but not slope * span, so
     * a steep run extrapolated to a distant t_ref can leave +-90 deg
     * (and int64). Refuse, don't clamp. The narrowings below are then
     * safe: fitted values at the kept shots are a Euclidean projection
     * of the shots, so residuals stay under sqrt(32) * 5400000 < 2^26,
     * and the least-squares slope is bounded the same way, keeping the
     * rate under sqrt(32) * 10800000 * 60000 < 2^42. */
    if (fp_w_cmp(ho_ref, fp_w_from_i64(-5400000)) < 0 ||
        fp_w_cmp(ho_ref, fp_w_from_i64(5400000)) > 0) return;
    out->ho_marcmin = fp_w_to_i64(ho_ref);
    out->rate_marcmin_per_min =
        fp_w_cmp(f.b, fp_w_from_i64(0)) == 0 ? 0
        : fp_w_to_i64(div_round_w128(fp_w_mul(f.a, fp_w_from_i64(60000)),
                                     f.b));
    out->max_residual_marcmin = fp_w_to_i64(worst);
    out->used = f.n;
    out->valid = 1;
}

/* ================================================================== */
/*  Vector ephemeris -- generating the machine-native star almanac     */
/* ================================================================== */

/* The catalog: J2000/ICRS equatorial unit vectors in Q2.30 for 18
 * bright navigational stars (17 of the almanac's 57 selected stars,
 * plus Polaris), generated offline (double precision) from their
 * Hipparcos-era J2000 RA/dec and committed as integers -- the almanac
 * stores what the machine consumes. The committed values are
 * validated rather than trusted: --ephemeris-check and
 * --external-check gate them against Skyfield+Hipparcos apparent
 * places and printed almanac pages on every `make check`. The
 * comments give the classical SHA (= 360 - RA) and declination in
 * degrees for cross-checking against a nautical almanac's star pages
 * (almanac values drift from these J2000 epoch numbers by precession,
 * ~0.84'/year along the ecliptic). */
const astro_nav_star_t astro_nav_stars[ASTRO_NAV_STAR_COUNT] = {
    { "Sirius",     {  -201278724,  1008477128,  -308840190 } }, /* SHA 258.713 dec -16.716 */
    { "Canopus",    {   -67884803,   647189149,  -854083933 } }, /* SHA 264.012 dec -52.696 */
    { "Arcturus",   {  -841584992,  -565847736,   352806668 } }, /* SHA 146.085 dec +19.182 */
    { "Vega",       {   134321514,  -826150956,   672572549 } }, /* SHA  80.765 dec +38.784 */
    { "Capella",    {   140123514,   732631001,   772359193 } }, /* SHA 280.828 dec +45.998 */
    { "Rigel",      {   209435683,  1041918898,  -153177051 } }, /* SHA 281.366 dec  -8.202 */
    { "Procyon",    {  -448943753,   970468743,    97782563 } }, /* SHA 245.174 dec  +5.225 */
    { "Betelgeuse", {    22429940,  1064545492,   138424340 } }, /* SHA 271.207 dec  +7.407 */
    { "Achernar",   {   529056537,   240308539,  -902924411 } }, /* SHA 335.571 dec -57.237 */
    { "Altair",     {   493085162,  -939354809,   165532688 } }, /* SHA  62.304 dec  +8.868 */
    { "Aldebaran",  {   369263624,   960970232,   305126358 } }, /* SHA 291.020 dec +16.509 */
    { "Spica",      {  -981486052,  -382630887,  -207846673 } }, /* SHA 158.702 dec -11.161 */
    { "Antares",    {  -370243372,  -887352887,  -477960464 } }, /* SHA 112.648 dec -26.432 */
    { "Pollux",     {  -420384333,   849501861,   504524632 } }, /* SHA 243.671 dec +28.026 */
    { "Fomalhaut",  {   899078374,  -250812243,  -530728556 } }, /* SHA  15.587 dec -29.622 */
    { "Deneb",      {   489249697,  -575720967,   762955836 } }, /* SHA  49.642 dec +45.280 */
    { "Regulus",    {  -928251986,   491630201,   222642089 } }, /* SHA 207.907 dec +11.967 */
    { "Polaris",    {    10873733,     8481607,  1073653263 } }, /* SHA 322.045 dec +89.264 */
};

/* Julian centuries since J2000.0 in Q16.48.
 * 36525 days * 86400000 ms = 3155760000000 ms per Julian century;
 * plain integer division truncates by < 2^-48 century -- harmless and
 * deterministic. */
static fixed_t centuries_from_ms(int64_t ut1_ms)
{
    /* multiply, not shift: ut1_ms may be negative (pre-J2000), and
     * left-shifting a negative value is undefined in C99 */
    return (fixed_t)fp_w_to_i64(
        fp_w_divs(fp_w_muls(ut1_ms, (int64_t)1 << FP_PRECISION),
                  fp_w_from_i64(3155760000000LL)));
}

/* Micro-arcseconds to Q16.48 radians (648000 arcsec per half-turn). */
static fixed_t uas_to_rad(int64_t uas)
{
    return (fixed_t)muldiv_round(uas, FP_PI, 648000000000LL);
}

/* Greenwich mean sidereal time as a Q16.48 angle in [0, 2 pi).
 *
 * ERA (earth rotation angle) in turns is linear in UT1:
 *   ERA = 0.7790572732640 + 1.00273781191135448 * D,  D = days from J2000
 * and GMST adds the accumulated precession of the equinox in right
 * ascension (IERS 2003, linear term):
 *   GMST = ERA + 0.014506" + 4612.156534" * T,  T = Julian centuries.
 *
 * Both ERA constants are stored as Q0.48 turns (error ~1.5e-15 turns,
 * i.e. microarcseconds over a century). The whole-turn part is dropped
 * by a 2^48 modulus BEFORE converting to radians, so precision does
 * not decay with |D| the way a radians-first accumulation would. */
static fixed_t gmst_rad_from_ms(int64_t ut1_ms)
{
    static const int64_t ERA_C0_Q48   = 219285127848252LL;
    static const int64_t ERA_RATE_Q48 = 282245602254643LL;
    static const int64_t Q48_ONE      = (int64_t)1 << 48;

    fp_math_init();

    /* turns * 86400000 in Q48, then one divide back to Q48 turns.
     * The % Q48_ONE is C truncated remainder on a power of two,
     * spelled as subtract-the-truncated-quotient in fp_w128. */
    fp_w128 scaled = fp_w_add(fp_w_muls(ERA_C0_Q48, 86400000),
                              fp_w_muls(ut1_ms, ERA_RATE_Q48));
    fp_w128 tq48 = fp_w_divs(scaled, fp_w_from_i64(86400000));
    int64_t rem = fp_w_to_i64(
        fp_w_sub(tq48, fp_w_shl(fp_w_divs_pow2(tq48, 48), 48)));
    int64_t turns = (rem % Q48_ONE + Q48_ONE) % Q48_ONE;

    /* fractional turn -> radians: * 2 pi = * FP_PI >> 47, rounded */
    fixed_t era = (fixed_t)fp_w_to_i64(
        fp_w_asr(fp_w_add(fp_w_muls(turns, FP_PI),
                          fp_w_from_i64((int64_t)1 << 46)), 47));

    fixed_t t = centuries_from_ms(ut1_ms);
    fixed_t g = era + uas_to_rad(14506)
              + fp_mul(t, uas_to_rad(4612156534LL));

    fixed_t two_pi = FP_PI << 1;
    while (g < 0)       g += two_pi;
    while (g >= two_pi) g -= two_pi;
    return g;
}

/* Coordinate (frame) rotations about z and y, in Q16.48. */
static void rot_z(fixed_t *x, fixed_t *y, fixed_t angle)
{
    fixed_t c, s;
    fp_sincos(angle, &c, &s);
    fixed_t nx = fp_mul(*x, c) + fp_mul(*y, s);
    fixed_t ny = fp_mul(*y, c) - fp_mul(*x, s);
    *x = nx;
    *y = ny;
}

static void rot_y(fixed_t *x, fixed_t *z, fixed_t angle)
{
    fixed_t c, s;
    fp_sincos(angle, &c, &s);
    fixed_t nx = fp_mul(*x, c) - fp_mul(*z, s);
    fixed_t nz = fp_mul(*z, c) + fp_mul(*x, s);
    *x = nx;
    *z = nz;
}

/* IAU 1976 precession angles (linear + T^2 terms; the T^3 terms are
 * < 0.02" per century, far below this catalog's budget):
 *   zeta  = 2306.2181" T + 0.30188" T^2
 *   z     = 2306.2181" T + 1.09468" T^2
 *   theta = 2004.3109" T - 0.42665" T^2 */
static void precession_angles(fixed_t t, fixed_t *zeta, fixed_t *zp,
                              fixed_t *theta)
{
    fixed_t t2 = fp_mul(t, t);
    *zeta  = fp_mul(t, uas_to_rad(2306218100LL))
           + fp_mul(t2, uas_to_rad(301880LL));
    *zp    = fp_mul(t, uas_to_rad(2306218100LL))
           + fp_mul(t2, uas_to_rad(1094680LL));
    *theta = fp_mul(t, uas_to_rad(2004310900LL))
           - fp_mul(t2, uas_to_rad(426650LL));
}

void astro_nav_celestial_to_earthfixed(const astro_nav_unitvec_t *celestial,
                                       int64_t ut1_ms_from_j2000,
                                       astro_nav_unitvec_t *out)
{
    fp_math_init();

    /* Earth-fixed = Rz(GMST - z) Ry(theta) Rz(-zeta) . v_J2000: the
     * classical Rz(-z) of the precession matrix and the Rz(GMST) of
     * earth rotation fold into one z-rotation. */
    fixed_t zeta, zp, theta;
    precession_angles(centuries_from_ms(ut1_ms_from_j2000),
                      &zeta, &zp, &theta);

    fixed_t x = q30_to_q48(celestial->x);
    fixed_t y = q30_to_q48(celestial->y);
    fixed_t z = q30_to_q48(celestial->z);

    rot_z(&x, &y, -zeta);
    rot_y(&x, &z, theta);
    rot_z(&x, &y, gmst_rad_from_ms(ut1_ms_from_j2000) - zp);

    out->x = q48_to_q30(x);
    out->y = q48_to_q30(y);
    out->z = q48_to_q30(z);
}

int32_t astro_nav_gha_aries_cdeg(int64_t ut1_ms_from_j2000)
{
    /* gmst is in [0, 2 pi) so the cdeg value is in [0, 36000]; the
     * single 36000 case (rounding up from just under a full turn)
     * folds to 0. */
    return rad_to_cdeg(gmst_rad_from_ms(ut1_ms_from_j2000))
           % ASTRO_NAV_CDEG_PER_TURN;
}

/* ================================================================== */
/*  Sun ephemeris -- dynamics on TT, rotation on UT1                   */
/* ================================================================== */

/* Angles for the solar formula are accumulated in integer
 * micro-arcseconds (uas; 1.296e12 per turn), reduced modulo one turn
 * BEFORE converting to radians -- same precision discipline as the
 * GMST path: the mean longitude wraps ~360 turns over the +-100 year
 * domain and must not carry those turns into Q16.48 radians. */
#define TURN_UAS 1296000000000LL

/* c0 + rate * t, t in Julian centuries (Q16.48), everything in uas,
 * reduced to [0, TURN_UAS). */
static int64_t sun_poly_uas(int64_t c0_uas, int64_t rate_uas, fixed_t t)
{
    int64_t uas = c0_uas
                + muldiv_round(rate_uas, t, (int64_t)1 << FP_PRECISION);
    return ((uas % TURN_UAS) + TURN_UAS) % TURN_UAS;
}

/* amp * trig(angle), trig in Q16.48, rounded back to amp's own unit
 * (uas for the longitude terms, micro-AU for the distance terms). */
static int64_t sun_trig_term(int64_t amp, fixed_t trig)
{
    return muldiv_round(amp, trig, (int64_t)1 << FP_PRECISION);
}

void astro_nav_sun_inertial(int64_t tt_ms_from_j2000,
                            astro_nav_unitvec_t *out)
{
    fp_math_init();

    /* USNO low-precision solar coordinates (aa.usno.navy.mil, "Approx-
     * imate Solar Coordinates"), D = days from J2000.0 (TT), stated
     * accuracy ~1' within two centuries of J2000:
     *   g       = 357.529 + 0.98560028 D     (mean anomaly)
     *   q       = 280.459 + 0.98564736 D     (mean longitude)
     *   lambda  = q + 1.915 sin g + 0.020 sin 2g
     *   epsilon = 23.439 - 0.00000036 D
     * lambda includes annual aberration (folded into the constants)
     * but NOT nutation -- deliberately, like the star pipeline: a
     * mean-equinox longitude is the consistent partner of the GMST
     * (mean equinox) earth rotation. Rates below are per Julian
     * century (x 36525), constants in uas (deg x 3.6e9). */
    fixed_t t = centuries_from_ms(tt_ms_from_j2000);

    int64_t g_uas = sun_poly_uas(1287104400000LL, 129596580817200LL, t);
    int64_t q_uas = sun_poly_uas(1009652400000LL, 129602771366400LL, t);

    fixed_t cg, sg, cg2, sg2;
    fp_sincos(uas_to_rad(g_uas), &cg, &sg);
    fp_sincos(uas_to_rad((2 * g_uas) % TURN_UAS), &cg2, &sg2);

    int64_t lam_uas = q_uas
                    + sun_trig_term(6894000000LL, sg)       /* 1.915 deg */
                    + sun_trig_term(72000000LL, sg2);       /* 0.020 deg */
    lam_uas = ((lam_uas % TURN_UAS) + TURN_UAS) % TURN_UAS;

    /* Obliquity: 23.439 deg - 0.0131490 deg per century, in uas. */
    fixed_t eps = uas_to_rad(84380400000LL)
                - fp_mul(t, uas_to_rad(47336400LL));

    /* Ecliptic-of-date longitude to an equatorial-of-date vector... */
    fixed_t cl, sl, ce, se;
    fp_sincos(uas_to_rad(lam_uas), &cl, &sl);
    fp_sincos(eps, &ce, &se);
    fixed_t x = cl;
    fixed_t y = fp_mul(sl, ce);
    fixed_t z = fp_mul(sl, se);

    /* ...then back to J2000 with the same IAU 1976 angles the forward
     * pipeline uses, so astro_nav_celestial_to_earthfixed() undoes
     * this rotation exactly (to rounding) when it re-applies it. The
     * angles are evaluated at TT rather than UT1 centuries; the
     * difference (~70 s in 3.156e9 s) is ~5e-5 arcsec of precession.
     * Inverse of Rz(-zp) Ry(theta) Rz(-zeta) is Rz(zp)-first. */
    fixed_t zeta, zp, theta;
    precession_angles(t, &zeta, &zp, &theta);
    rot_z(&x, &y, zp);
    rot_y(&x, &z, -theta);
    rot_z(&x, &y, zeta);

    out->x = q48_to_q30(x);
    out->y = q48_to_q30(y);
    out->z = q48_to_q30(z);
}

void astro_nav_sun_earthfixed(int64_t ut1_ms_from_j2000,
                              int64_t tt_minus_ut1_ms,
                              astro_nav_unitvec_t *out)
{
    astro_nav_unitvec_t inertial;
    astro_nav_sun_inertial(ut1_ms_from_j2000 + tt_minus_ut1_ms,
                           &inertial);
    astro_nav_celestial_to_earthfixed(&inertial, ut1_ms_from_j2000, out);
}

void astro_nav_sun_distance(int64_t ut1_ms_from_j2000,
                            int64_t tt_minus_ut1_ms,
                            int32_t *distance_uau,
                            int32_t *sd_marcmin,
                            int32_t *hp_marcmin)
{
    fp_math_init();

    /* Same source model as astro_nav_sun_inertial(): USNO low-
     * precision solar coordinates, distance term
     *   R = 1.00014 - 0.01671 cos g - 0.00014 cos 2g   (AU),
     * evaluated in integer micro-AU off the same mean anomaly g. */
    fixed_t t = centuries_from_ms(ut1_ms_from_j2000 + tt_minus_ut1_ms);
    int64_t g_uas = sun_poly_uas(1287104400000LL, 129596580817200LL, t);

    fixed_t cg, sg, cg2, sg2;
    fp_sincos(uas_to_rad(g_uas), &cg, &sg);
    fp_sincos(uas_to_rad((2 * g_uas) % TURN_UAS), &cg2, &sg2);

    int64_t r_uau = 1000140
                  - sun_trig_term(16710, cg)    /* 0.01671 AU */
                  - sun_trig_term(140, cg2);    /* 0.00014 AU */

    /* SD = 0.2666 deg / R (the source page's semidiameter term):
     * 0.2666 deg = 15996 milli-arcmin, so SD = 15996e6 / R_uau.
     * HP = 8.794" / R (standard equatorial solar parallax):
     * 8.794" = 146.5667 milli-arcmin, so HP = 146566667 / R_uau.
     * Both angles are small enough to equal their sines at this
     * resolution; divisions round to nearest (R is always > 0). */
    *distance_uau = (int32_t)r_uau;
    *sd_marcmin   = (int32_t)((15996000000LL + r_uau / 2) / r_uau);
    *hp_marcmin   = (int32_t)((146566667LL + r_uau / 2) / r_uau);
}

/* ================================================================== */
/*  Moon ephemeris -- dynamics on TT, rotation on UT1                  */
/* ================================================================== */

/* Meeus, Astronomical Algorithms, 2nd ed., ch. 47 ("Position of the
 * Moon"): the abridged ELP-2000/82 series. Fundamental arguments
 * (eqns 47.1-47.6) accumulate in integer micro-arcseconds like the
 * Sun's mean longitude; Tables 47.A and 47.B supply the 60 + 60
 * periodic terms. The series is evaluated per term -- one fp_sincos
 * per table row (~135 total, ~7 us) -- so a reviewer can check each
 * row against the printed book line by line; the angle-addition
 * recurrence that would share those sines is a deliberate later
 * optimization, not taken here (correctness and reviewability first). */

/* Table 47.A: periodic terms of the Moon's longitude and distance.
 * Columns are the multipliers (k_D, k_M, k_M', k_F) of the four
 * fundamentals; the argument of each term is k_D D + k_M M + k_M' M'
 * + k_F F. Sigma_l (sin) is in 1e-6 deg, Sigma_r (cos) in 1e-3 km
 * (one 1e-3 km IS one metre). One row per book line, in book order. */
static const int8_t moon_lon_arg[60][4] = {
    {  0,  0,  1,  0 }, {  2,  0, -1,  0 }, {  2,  0,  0,  0 },
    {  0,  0,  2,  0 }, {  0,  1,  0,  0 }, {  0,  0,  0,  2 },
    {  2,  0, -2,  0 }, {  2, -1, -1,  0 }, {  2,  0,  1,  0 },
    {  2, -1,  0,  0 }, {  0,  1, -1,  0 }, {  1,  0,  0,  0 },
    {  0,  1,  1,  0 }, {  2,  0,  0, -2 }, {  0,  0,  1,  2 },
    {  0,  0,  1, -2 }, {  4,  0, -1,  0 }, {  0,  0,  3,  0 },
    {  4,  0, -2,  0 }, {  2,  1, -1,  0 }, {  2,  1,  0,  0 },
    {  1,  0, -1,  0 }, {  1,  1,  0,  0 }, {  2, -1,  1,  0 },
    {  2,  0,  2,  0 }, {  4,  0,  0,  0 }, {  2,  0, -3,  0 },
    {  0,  1, -2,  0 }, {  2,  0, -1,  2 }, {  2, -1, -2,  0 },
    {  1,  0,  1,  0 }, {  2, -2,  0,  0 }, {  0,  1,  2,  0 },
    {  0,  2,  0,  0 }, {  2, -2, -1,  0 }, {  2,  0,  1, -2 },
    {  2,  0,  0,  2 }, {  4, -1, -1,  0 }, {  0,  0,  2,  2 },
    {  3,  0, -1,  0 }, {  2,  1,  1,  0 }, {  4, -1, -2,  0 },
    {  0,  2, -1,  0 }, {  2,  2, -1,  0 }, {  2,  1, -2,  0 },
    {  2, -1,  0, -2 }, {  4,  0,  1,  0 }, {  0,  0,  4,  0 },
    {  4, -1,  0,  0 }, {  1,  0, -2,  0 }, {  2,  1,  0, -2 },
    {  0,  0,  2, -2 }, {  1,  1,  1,  0 }, {  3,  0, -2,  0 },
    {  4,  0, -3,  0 }, {  2, -1,  2,  0 }, {  0,  2,  1,  0 },
    {  1,  1, -1,  0 }, {  2,  0,  3,  0 }, {  2,  0, -1, -2 },
};
static const int32_t moon_lon_l[60] = {   /* Sigma_l, 1e-6 deg (sin) */
      6288774,   1274027,    658314,    213618,   -185116,   -114332,
        58793,     57066,     53322,     45758,    -40923,    -34720,
       -30383,     15327,    -12528,     10980,     10675,     10034,
         8548,     -7888,     -6766,     -5163,      4987,      4036,
         3994,      3861,      3665,     -2689,     -2602,      2390,
        -2348,      2236,     -2120,     -2069,      2048,     -1773,
        -1595,      1215,     -1110,      -892,      -810,       759,
         -713,      -700,       691,       596,       549,       537,
          520,      -487,      -399,      -381,       351,      -340,
          330,       327,      -323,       299,       294,         0,
};
static const int32_t moon_lon_r[60] = {   /* Sigma_r, 1e-3 km (cos) */
    -20905355,  -3699111,  -2955968,   -569925,     48888,     -3149,
       246158,   -152138,   -170733,   -204586,   -129620,    108743,
       104755,     10321,         0,     79661,    -34782,    -23210,
       -21636,     24208,     30824,     -8379,    -16675,    -12831,
       -10445,    -11650,     14403,     -7003,         0,     10056,
         6322,     -9884,      5751,         0,     -4950,      4130,
            0,     -3958,         0,      3258,      2616,     -1897,
        -2117,      2354,         0,         0,     -1423,     -1117,
        -1571,     -1739,         0,     -4421,         0,         0,
            0,         0,      1165,         0,         0,      8752,
};

/* Table 47.B: periodic terms of the Moon's latitude. Same argument
 * layout; Sigma_b (sin) is in 1e-6 deg. One row per book line. */
static const int8_t moon_lat_arg[60][4] = {
    {  0,  0,  0,  1 }, {  0,  0,  1,  1 }, {  0,  0,  1, -1 },
    {  2,  0,  0, -1 }, {  2,  0, -1,  1 }, {  2,  0, -1, -1 },
    {  2,  0,  0,  1 }, {  0,  0,  2,  1 }, {  2,  0,  1, -1 },
    {  0,  0,  2, -1 }, {  2, -1,  0, -1 }, {  2,  0, -2, -1 },
    {  2,  0,  1,  1 }, {  2,  1,  0, -1 }, {  2, -1, -1,  1 },
    {  2, -1,  0,  1 }, {  2, -1, -1, -1 }, {  0,  1, -1, -1 },
    {  4,  0, -1, -1 }, {  0,  1,  0,  1 }, {  0,  0,  0,  3 },
    {  0,  1, -1,  1 }, {  1,  0,  0,  1 }, {  0,  1,  1,  1 },
    {  0,  1,  1, -1 }, {  0,  1,  0, -1 }, {  1,  0,  0, -1 },
    {  0,  0,  3,  1 }, {  4,  0,  0, -1 }, {  4,  0, -1,  1 },
    {  0,  0,  1, -3 }, {  4,  0, -2,  1 }, {  2,  0,  0, -3 },
    {  2,  0,  2, -1 }, {  2, -1,  1, -1 }, {  2,  0, -2,  1 },
    {  0,  0,  3, -1 }, {  2,  0,  2,  1 }, {  2,  0, -3, -1 },
    {  2,  1, -1,  1 }, {  2,  1,  0,  1 }, {  4,  0,  0,  1 },
    {  2, -1,  1,  1 }, {  2, -2,  0, -1 }, {  0,  0,  1,  3 },
    {  2,  1,  1, -1 }, {  1,  1,  0, -1 }, {  1,  1,  0,  1 },
    {  0,  1, -2, -1 }, {  2,  1, -1, -1 }, {  1,  0,  1,  1 },
    {  2, -1, -2, -1 }, {  0,  1,  2,  1 }, {  4,  0, -2, -1 },
    {  4, -1, -1, -1 }, {  1,  0,  1, -1 }, {  4,  0,  1, -1 },
    {  1,  0, -1, -1 }, {  4, -1,  0, -1 }, {  2, -2,  0,  1 },
};
static const int32_t moon_lat_b[60] = {   /* Sigma_b, 1e-6 deg (sin) */
      5128122,    280602,    277693,    173237,     55413,     46271,
        32573,     17198,      9266,      8822,      8216,      4324,
         4200,     -3359,      2463,      2211,      2065,     -1870,
         1828,     -1794,     -1749,     -1565,     -1491,     -1475,
        -1410,     -1344,     -1335,      1107,      1021,       833,
          777,       671,       607,       596,       491,      -451,
          439,       422,       421,      -366,      -351,       331,
          315,       302,      -283,      -229,       223,       223,
         -220,      -220,      -185,       181,      -177,       176,
          166,      -164,       132,      -119,       115,       107,
};

/* One rational polynomial term sign/den * t_pow, given in deg, as
 * uas: t_pow (Q16.48 power of t) * 3.6e9 / den, rounded to nearest.
 * These carry only the t^3 and t^4 terms of the fundamental arguments;
 * the largest (M', den 69699) is ~0.05 arcsec (51651 uas) at the +-1
 * century domain edge, and the Q16.48 rounding here is well under a
 * uas -- nano-arcsec noise against a 274-milli-arcmin model. den == 0
 * marks an absent term (M has no t^4). */
static int64_t moon_rational_uas(int sign, int64_t den, fixed_t t_pow)
{
    if (den == 0) return 0;
    fp_w128 n  = fp_w_muls(t_pow, 3600000000LL);  /* deg-rational -> uas */
    int neg    = fp_w_cmp(n, fp_w_from_i64(0)) < 0;
    fp_w128 nm = neg ? fp_w_neg(n) : n;
    fp_w128 dm = fp_w_shl(fp_w_from_i64(den), FP_PRECISION);
    int64_t q  = fp_w_to_i64(
        fp_w_divs(fp_w_add(nm, fp_w_divs_pow2(dm, 1)), dm));
    return ((sign < 0) ^ neg) ? -q : q;
}

/* A lunar fundamental argument (Meeus 47.1-47.5) in uas, reduced to
 * [0, TURN_UAS). c0 + rate*t + c2*t^2 stay on the exact integer path
 * (Meeus' <= 8-decimal coefficients times 3.6e9 uas/deg are exact
 * integers, and sun_poly_uas already carries the constant + linear
 * part like the Sun's); the small t^3, t^4 rationals are added on top.
 * t3s/t3d and t4s/t4d are the signs and Meeus denominators (deg). */
static int64_t moon_arg_uas(int64_t c0, int64_t rate, int64_t c2,
                            int t3s, int64_t t3d, int t4s, int64_t t4d,
                            fixed_t t, fixed_t t2, fixed_t t3, fixed_t t4)
{
    int64_t uas = sun_poly_uas(c0, rate, t)      /* c0 + rate*t (reduced) */
                + sun_trig_term(c2, t2)          /* c2 * t^2 (exact uas)  */
                + moon_rational_uas(t3s, t3d, t3)
                + moon_rational_uas(t4s, t4d, t4);
    return ((uas % TURN_UAS) + TURN_UAS) % TURN_UAS;
}

/* Reduce a raw uas angle (a per-row multiplier combination, or an
 * additive-term sum or difference of two fundamentals) to one turn.
 * The worst 47.A/47.B combination is |k_D|+|k_M|+|k_M'|+|k_F| <=
 * 4+2+4+3 = 13 reduced fundamentals, ~1.7e13 uas -- int64 headroom
 * ~5e5, comfortably inside the type. */
static int64_t moon_mod_turn(int64_t uas)
{
    return ((uas % TURN_UAS) + TURN_UAS) % TURN_UAS;
}

/* Meeus eqn 47.6 eccentricity damping: terms with |k_M| = 1 are
 * scaled by E, |k_M| = 2 by E^2, the rest unchanged. */
static fixed_t moon_efac(int k_m, fixed_t e, fixed_t e2)
{
    int a = k_m < 0 ? -k_m : k_m;
    return a == 1 ? e : a == 2 ? e2 : FP_ONE;
}

/* arcsin(s) in milli-arcmin for a small positive Q16.48 ratio s (the
 * Moon's HP peaks near s = 0.018, i.e. 61'). At that size the arcsine
 * is NOT its argument at milli-arcmin resolution -- the cubic term is
 * ~3 milli-arcmin -- so evaluate s + s^3/6; the dropped s^5/40 term is
 * ~1e-5 milli-arcmin, far below the almanac's resolution. Radians ->
 * milli-arcmin is * 10800000 / pi (180 * 60 * 1000 milli-arcmin per
 * half-turn, over pi), i128-rounded. */
static int64_t moon_arcsin_marcmin(fixed_t s)
{
    fixed_t s3  = fp_mul(fp_mul(s, s), s);
    fixed_t ang = s + s3 / 6;                     /* Q16.48 radians */
    return muldiv_round(ang, 10800000LL, FP_PI);
}

/* The four fundamentals the periodic tables combine, plus L' (also an
 * additive-term argument), each reduced mod one turn. Shared by the
 * position and distance entry points so the two never drift. */
static void moon_fundamentals(fixed_t t, fixed_t t2, fixed_t t3,
                              fixed_t t4, int64_t *Lp, int64_t *D,
                              int64_t *M, int64_t *Mp, int64_t *F)
{
    *Lp = moon_arg_uas(785939211720LL, 1732564372443156LL, -5682960LL,
                       +1, 538841LL, -1, 65194000LL, t, t2, t3, t4);
    *D  = moon_arg_uas(1072260691560LL, 1602961601052240LL, -6774840LL,
                       +1, 545868LL, -1, 113065000LL, t, t2, t3, t4);
    *M  = moon_arg_uas(1287104793120LL, 129596581047240LL, -552960LL,
                       +1, 24490000LL, 0, 0LL, t, t2, t3, t4);
    *Mp = moon_arg_uas(485868227040LL, 1717915923019800LL, 31469040LL,
                       +1, 69699LL, -1, 14712000LL, t, t2, t3, t4);
    *F  = moon_arg_uas(335779542000LL, 1739527263083880LL, -13154040LL,
                       -1, 3526000LL, +1, 863310000LL, t, t2, t3, t4);
}

void astro_nav_moon_inertial(int64_t tt_ms_from_j2000,
                             astro_nav_unitvec_t *out)
{
    fp_math_init();

    /* Meeus ch. 47: geocentric ecliptic-of-date longitude/latitude
     * from the fundamental arguments and Tables 47.A/47.B. Angles are
     * accumulated in micro-arcseconds and reduced mod one turn before
     * each fp_sincos, the same precision discipline the Sun and GMST
     * paths use -- the mean longitude wraps hundreds of turns over the
     * +-100 year domain and must not carry them into Q16.48 radians.
     * Rates below are per Julian century, constants in uas (deg x
     * 3.6e9); the T^2 coefficients are exact integer uas as well. */
    fixed_t t  = centuries_from_ms(tt_ms_from_j2000);
    fixed_t t2 = fp_mul(t, t);
    fixed_t t3 = fp_mul(t2, t);
    fixed_t t4 = fp_mul(t3, t);

    int64_t Lp, D, M, Mp, F;
    moon_fundamentals(t, t2, t3, t4, &Lp, &D, &M, &Mp, &F);

    /* A1, A2, A3 (Meeus, text after Table 47.B) are linear: A1 the
     * Venus term, A2 the Jupiter term, A3 a term in the Moon's mean
     * longitude; all in uas. */
    int64_t A1 = sun_poly_uas(431100000000LL, 474656400000LL, t);
    int64_t A2 = sun_poly_uas(191124000000LL, 1725351444000000LL, t);
    int64_t A3 = sun_poly_uas(1128420000000LL, 1732559342400000LL, t);

    /* Eccentricity of Earth's orbit, Meeus eqn 47.6, in Q16.48:
     *   E = 1 - 0.002516 T - 0.0000074 T^2. */
    fixed_t e  = FP_ONE - fp_mul(t, 708191041404LL)
                        - fp_mul(t2, 2082914828LL);
    fixed_t e2 = fp_mul(e, e);

    /* Sigma_l and Sigma_b accumulate in uas (amplitude x 3600 uas per
     * 1e-6 deg); the E-factor multiplies the sine once, then
     * sun_trig_term rounds the amplitude product. Position needs only
     * the sine series (Sigma_l, Sigma_b); the cosine series (Sigma_r)
     * belongs to astro_nav_moon_distance(). */
    int64_t sig_l = 0, sig_b = 0;
    int i;
    for (i = 0; i < 60; i++) {
        int64_t a = (int64_t)moon_lon_arg[i][0] * D
                  + (int64_t)moon_lon_arg[i][1] * M
                  + (int64_t)moon_lon_arg[i][2] * Mp
                  + (int64_t)moon_lon_arg[i][3] * F;
        fixed_t c, s;
        fp_sincos(uas_to_rad(moon_mod_turn(a)), &c, &s);
        (void)c;
        sig_l += sun_trig_term((int64_t)moon_lon_l[i] * 3600,
                               fp_mul(s, moon_efac(moon_lon_arg[i][1],
                                                   e, e2)));
    }
    for (i = 0; i < 60; i++) {
        int64_t a = (int64_t)moon_lat_arg[i][0] * D
                  + (int64_t)moon_lat_arg[i][1] * M
                  + (int64_t)moon_lat_arg[i][2] * Mp
                  + (int64_t)moon_lat_arg[i][3] * F;
        fixed_t c, s;
        fp_sincos(uas_to_rad(moon_mod_turn(a)), &c, &s);
        fixed_t ef = moon_efac(moon_lat_arg[i][1], e, e2);
        sig_b += sun_trig_term((int64_t)moon_lat_b[i] * 3600,
                               fp_mul(s, ef));
    }

    /* Additive terms (Meeus, text after Table 47.B), units 1e-6 deg,
     * no E damping: Sigma_l gains 3958 sin A1 + 1962 sin(L' - F) +
     * 318 sin A2; Sigma_b gains -2235 sin L' + 382 sin A3 + 175
     * sin(A1 - F) + 175 sin(A1 + F) + 127 sin(L' - M') - 115
     * sin(L' + M'). */
    fixed_t c, s;
    fp_sincos(uas_to_rad(A1), &c, &s);
    sig_l += sun_trig_term(3958LL * 3600, s);
    fp_sincos(uas_to_rad(moon_mod_turn(Lp - F)), &c, &s);
    sig_l += sun_trig_term(1962LL * 3600, s);
    fp_sincos(uas_to_rad(A2), &c, &s);
    sig_l += sun_trig_term(318LL * 3600, s);

    fp_sincos(uas_to_rad(Lp), &c, &s);
    sig_b += sun_trig_term(-2235LL * 3600, s);
    fp_sincos(uas_to_rad(A3), &c, &s);
    sig_b += sun_trig_term(382LL * 3600, s);
    fp_sincos(uas_to_rad(moon_mod_turn(A1 - F)), &c, &s);
    sig_b += sun_trig_term(175LL * 3600, s);
    fp_sincos(uas_to_rad(moon_mod_turn(A1 + F)), &c, &s);
    sig_b += sun_trig_term(175LL * 3600, s);
    fp_sincos(uas_to_rad(moon_mod_turn(Lp - Mp)), &c, &s);
    sig_b += sun_trig_term(127LL * 3600, s);
    fp_sincos(uas_to_rad(moon_mod_turn(Lp + Mp)), &c, &s);
    sig_b += sun_trig_term(-115LL * 3600, s);

    /* lambda = L' + Sigma_l (mod one turn); beta = Sigma_b is signed
     * and never exceeds +-5.3 deg, so it is left unreduced. */
    int64_t lam_uas  = moon_mod_turn(Lp + sig_l);
    int64_t beta_uas = sig_b;

    /* Obliquity: the SAME expression the Sun uses (23.439 deg -
     * 0.0131490 deg per century, in uas), so the Moon rides the
     * identical ecliptic->equatorial->J2000 machinery -- the
     * ~1 arcsec against a full IAU 1976 obliquity costs under 1
     * milli-arcmin against DE421, measured, versus the model's 274. */
    fixed_t clam, slam, cbet, sbet, ce, se;
    fp_sincos(uas_to_rad(lam_uas), &clam, &slam);
    fp_sincos(uas_to_rad(beta_uas), &cbet, &sbet);
    fixed_t eps = uas_to_rad(84380400000LL)
                - fp_mul(t, uas_to_rad(47336400LL));
    fp_sincos(eps, &ce, &se);

    /* Ecliptic-of-date (lambda, beta) unit vector rotated about x by
     * +eps to an equatorial-of-date vector. */
    fixed_t cbeta_slam = fp_mul(cbet, slam);
    fixed_t x = fp_mul(cbet, clam);
    fixed_t y = fp_mul(ce, cbeta_slam) - fp_mul(se, sbet);
    fixed_t z = fp_mul(se, cbeta_slam) + fp_mul(ce, sbet);

    /* ...then back to J2000 with the same IAU 1976 angles (TT
     * centuries) the Sun uses, so astro_nav_celestial_to_earthfixed()
     * undoes this rotation exactly when it re-applies it. Inverse of
     * Rz(-zp) Ry(theta) Rz(-zeta) is Rz(zp)-first. */
    fixed_t zeta, zp, theta;
    precession_angles(t, &zeta, &zp, &theta);
    rot_z(&x, &y, zp);
    rot_y(&x, &z, -theta);
    rot_z(&x, &y, zeta);

    out->x = q48_to_q30(x);
    out->y = q48_to_q30(y);
    out->z = q48_to_q30(z);
}

void astro_nav_moon_earthfixed(int64_t ut1_ms_from_j2000,
                               int64_t tt_minus_ut1_ms,
                               astro_nav_unitvec_t *out)
{
    astro_nav_unitvec_t inertial;
    astro_nav_moon_inertial(ut1_ms_from_j2000 + tt_minus_ut1_ms,
                            &inertial);
    astro_nav_celestial_to_earthfixed(&inertial, ut1_ms_from_j2000, out);
}

void astro_nav_moon_distance(int64_t ut1_ms_from_j2000,
                             int64_t tt_minus_ut1_ms,
                             int32_t *distance_km,
                             int32_t *sd_marcmin,
                             int32_t *hp_marcmin)
{
    fp_math_init();

    /* Same Meeus ch. 47 model as astro_nav_moon_inertial(): the
     * distance needs only Table 47.A's cosine series (Sigma_r), so the
     * latitude table and the frame rotations are skipped.
     *   Delta = 385000.56 km + Sigma_r,  Sigma_r in metres. */
    fixed_t t  = centuries_from_ms(ut1_ms_from_j2000 + tt_minus_ut1_ms);
    fixed_t t2 = fp_mul(t, t);
    fixed_t t3 = fp_mul(t2, t);
    fixed_t t4 = fp_mul(t3, t);

    int64_t Lp, D, M, Mp, F;
    moon_fundamentals(t, t2, t3, t4, &Lp, &D, &M, &Mp, &F);
    (void)Lp;

    fixed_t e  = FP_ONE - fp_mul(t, 708191041404LL)
                        - fp_mul(t2, 2082914828LL);
    fixed_t e2 = fp_mul(e, e);

    int64_t sig_r = 0;
    int i;
    for (i = 0; i < 60; i++) {
        int64_t a = (int64_t)moon_lon_arg[i][0] * D
                  + (int64_t)moon_lon_arg[i][1] * M
                  + (int64_t)moon_lon_arg[i][2] * Mp
                  + (int64_t)moon_lon_arg[i][3] * F;
        fixed_t c, s;
        fp_sincos(uas_to_rad(moon_mod_turn(a)), &c, &s);
        sig_r += sun_trig_term((int64_t)moon_lon_r[i],
                               fp_mul(c, moon_efac(moon_lon_arg[i][1],
                                                   e, e2)));
    }

    /* Delta in metres (385000.56 km = 385000560 m), then rounded to
     * the nearest km; Delta is always positive (~356400-406700 km). */
    int64_t delta_m = 385000560LL + sig_r;
    *distance_km = (int32_t)((delta_m + 500) / 1000);

    /* HP = arcsin(6378.137 km / Delta), SD = arcsin(1737.4 km / Delta),
     * with s the Q16.48 ratio. The Nautical Almanac tabulates the
     * Moon's semidiameter from a fixed augmentation ratio k = SD/HP =
     * 0.2725; this computes SD and HP independently from the lunar
     * radius 1737.4 km and the equatorial radius 6378.137 km, whose
     * ratio is 0.27239 -- a few tenths of a milli-arcmin different, and
     * either is well inside the model's own budget. */
    fixed_t s_hp = (fixed_t)fp_w_to_i64(fp_w_divs(
        fp_w_add(fp_w_shl(fp_w_from_i64(6378137LL), FP_PRECISION),
                 fp_w_from_i64(delta_m / 2)),
        fp_w_from_i64(delta_m)));
    fixed_t s_sd = (fixed_t)fp_w_to_i64(fp_w_divs(
        fp_w_add(fp_w_shl(fp_w_from_i64(1737400LL), FP_PRECISION),
                 fp_w_from_i64(delta_m / 2)),
        fp_w_from_i64(delta_m)));
    *hp_marcmin = (int32_t)moon_arcsin_marcmin(s_hp);
    *sd_marcmin = (int32_t)moon_arcsin_marcmin(s_sd);
}

void astro_nav_civil_to_times(int32_t year, int32_t month, int32_t day,
                              int32_t hour, int32_t minute,
                              int32_t second, int32_t millisecond,
                              int32_t dut1_ms, int32_t tai_minus_utc_s,
                              int64_t *ut1_ms_from_j2000,
                              int64_t *tt_minus_ut1_ms)
{
    /* Fliegel-Van Flandern Gregorian Julian day number. The original
     * is Fortran truncating division; a is hoisted so every remaining
     * division here acts on nonnegative operands, where C matches. */
    int32_t a = (month - 14) / 12;    /* -1 for Jan/Feb, else 0 */
    int64_t jdn = (1461LL * (year + 4800 + a)) / 4
                + (367LL * (month - 2 - 12 * a)) / 12
                - (3LL * ((year + 4900 + a) / 100)) / 4
                + day - 32075;

    /* JDN labels the NOON of the civil date, and J2000.0 is JD
     * 2451545.0 = 2000-01-01 12:00, hence the half-day shift. A
     * second of 60 (leap second) just counts one more elapsed
     * second here, spilling into the next civil day -- exact when
     * dut1_ms is the value in effect AT that instant (the pre-leap
     * one); the next day's 00:00:00 label carries a DUT1 stepped
     * +1 s, landing one SI second later in UT1, as it must. */
    int64_t ms_of_day = ((int64_t)hour * 3600 + (int64_t)minute * 60
                         + second) * 1000 + millisecond;
    int64_t utc_ms = (jdn - 2451545LL) * 86400000LL
                   + ms_of_day - 43200000LL;

    *ut1_ms_from_j2000 = utc_ms + dut1_ms;
    *tt_minus_ut1_ms   = 32184LL + 1000LL * tai_minus_utc_s - dut1_ms;
}

void astro_nav_position_from_celestial_zenith(
        const astro_nav_unitvec_t *zenith_j2000,
        int64_t ut1_ms_from_j2000,
        astro_nav_unitvec_t *out_position)
{
    /* The observer's zenith, expressed earth-fixed, IS the observer's
     * position unit vector -- so the camera fix is exactly the almanac
     * rotation applied to the zenith instead of to a star. */
    astro_nav_celestial_to_earthfixed(zenith_j2000, ut1_ms_from_j2000,
                                      out_position);
}

int32_t astro_nav_circular_diff_cdeg(int32_t a_cdeg, int32_t b_cdeg)
{
    int32_t d = a_cdeg - b_cdeg;
    if (d > ASTRO_NAV_CDEG_PER_HALFTURN)  d -= ASTRO_NAV_CDEG_PER_TURN;
    if (d < -ASTRO_NAV_CDEG_PER_HALFTURN) d += ASTRO_NAV_CDEG_PER_TURN;
    return d < 0 ? -d : d;
}

int64_t astro_nav_cdeg_to_marcmin(int32_t cdeg)
{
    return (int64_t)cdeg * ASTRO_NAV_MARCMIN_PER_CDEG;
}

int64_t astro_nav_intercept_tenths_nm(int32_t ho_cdeg, int32_t hc_cdeg)
{
    return (int64_t)(ho_cdeg - hc_cdeg) * 6;
}
