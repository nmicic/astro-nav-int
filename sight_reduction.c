/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * sight_reduction.c -- celestial navigation sight reduction in PURE
 * INTEGER math.  No float, no double, no libm anywhere in the
 * computation (printf formats integers only).
 *
 * THE PROBLEM (the core calculation of celestial navigation):
 *   Given an assumed position (latitude phi, longitude lambda) and a
 *   celestial body's coordinates from the almanac (Greenwich hour angle
 *   GHA, declination dec), compute:
 *     Hc = computed altitude  (how high the body SHOULD appear)
 *     Zn = true azimuth       (which way to look)
 *   Comparing Hc against the sextant-observed altitude Ho gives the
 *   intercept (1 arcminute = 1 nautical mile) -- the Marcq St. Hilaire
 *   method every navigator since 1875 has used.
 *
 * THREE COMPUTATION PATHS, all integer-only:
 *
 *   PATH A -- classic spherical trigonometry in Q16.48 fixed point
 *     (fp_math.h: CORDIC sin/cos; CORDIC vectoring-mode asin/atan2):
 *        sin Hc = sin phi sin dec + cos phi cos dec cos LHA
 *        N = cos phi sin dec - sin phi cos dec cos LHA
 *        E = -cos dec sin LHA;  Zn = atan2(E, N)
 *
 *   PATH B -- square-ray azimuth key:
 *     reuse Path A's Hc, then map the local (north,east) direction to the
 *     L-infinity square perimeter.  A 33-entry integer atan correction table
 *     converts the monotone square key to true centidegrees.  The raw square
 *     key is also suitable for machine-to-machine pointing without ever
 *     converting to an angle.
 *
 *   PATH C -- machine-native almanac:
 *     observer and body enter as Q2.30 earth-fixed unit vectors (what a
 *     machine-native almanac would publish instead of GHA/dec angles) and
 *     the reduction is three dot/cross products plus one ratio divide --
 *     no CORDIC, no table, no angle in any unit.  Outputs are sin(Hc) in
 *     Q2.30 and the same 16-bit square key as Path B; converting either to
 *     centidegrees is a separate human-boundary call.
 *
 * UNITS: external angles are int32 CENTIDEGREES (1 cdeg = 0.01 deg =
 * 0.6 arcmin).  Longitude is signed EAST-positive.  Internally Q16.48
 * radians (fixed_t, 48 fractional bits, ~15 signed integer bits).
 *
 * OVERFLOW DISCIPLINE (the Q16.48 trap): the integer part is only 15
 * bits signed, so fp_mul(a,b) overflows once |a*b| > 32767.  Every
 * multiply below either has both factors <= 1 in magnitude, or divides
 * first.  Each site carries its bound.
 *
 * DISCLAIMER: toy / side project.  Do NOT navigate a vessel with this.
 */

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef ASTRO_NAV_NATIVE_REFERENCE
#include <math.h>
#include <time.h>
#endif

#include "astro_nav.h"
#include "fp_math.h"          /* --fuzz-w128 drives the fp_w128 op layer */
#include "ephemeris_reference.h"
#include "external_reference.h"
#include "cross_reference.h"
#include "sight_scenarios.h"

typedef astro_nav_sight_result_t sight_result_t;
typedef astro_nav_square_result_t square_result_t;
typedef astro_nav_machine_sight_t machine_sight_t;

#define CDEG_PER_TURN     ASTRO_NAV_CDEG_PER_TURN
#define CDEG_PER_HALFTURN ASTRO_NAV_CDEG_PER_HALFTURN
#define CDEG_PER_QUARTER  ASTRO_NAV_CDEG_PER_QUARTER

#define lha_cdeg_from      astro_nav_lha_cdeg
#define circular_diff_cdeg astro_nav_circular_diff_cdeg
#define sight_reduce_trig  astro_nav_reduce_method_a
#define sight_reduce_square astro_nav_reduce_method_b

/* Path C wrapper for the angle-based test tables: builds the unit
 * vectors a machine-native almanac would have published directly.
 * A body's east-longitude is MINUS its GHA. */
static void sight_reduce_machine(int32_t phi_cdeg, int32_t lon_east_cdeg,
                                 int32_t gha_cdeg, int32_t dec_cdeg,
                                 machine_sight_t *out)
{
    astro_nav_unitvec_t observer, body;
    astro_nav_unitvec_from_cdeg(phi_cdeg, lon_east_cdeg, &observer);
    astro_nav_unitvec_from_cdeg(dec_cdeg, -gha_cdeg, &body);
    astro_nav_reduce_method_c(&observer, &body, out);
}

/* ================================================================== */
/*  Deterministic LCG for the sweep sim (integer-only, seeded)         */
/* ================================================================== */

static uint64_t lcg_state = 0x243F6A8885A308D3ULL;   /* pi mantissa bits */

static uint64_t lcg_next(void)
{
    /* Knuth MMIX multiplier; top bits are the good ones. */
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state;
}

/* Uniform in [lo, hi) using the top 31 bits (modulo bias < 2^-15 of a
 * centidegree here -- irrelevant for a sweep). */
static int32_t lcg_range(int32_t lo, int32_t hi)
{
    return lo + (int32_t)((lcg_next() >> 33) % (uint64_t)(hi - lo));
}

/* ================================================================== */
/*  Pretty-printing helpers (integer digit extraction only)            */
/* ================================================================== */

/* Print a centidegree value as "DDD.DD deg". */
static void print_cdeg(int32_t cdeg)
{
    int32_t mag = cdeg < 0 ? -cdeg : cdeg;
    printf("%s%ld.%02ld", cdeg < 0 ? "-" : "",
           (long)(mag / 100), (long)(mag % 100));
}

/* Print milli-arcminutes as "A.BBB'". */
static void print_marcmin(int64_t marcmin)
{
    int64_t mag = marcmin < 0 ? -marcmin : marcmin;
    printf("%s%lld.%03lld'", marcmin < 0 ? "-" : "",
           (long long)(mag / 1000), (long long)(mag % 1000));
}

/* ================================================================== */
/*  Validation cases (double-precision references computed offline)    */
/* ================================================================== */

typedef struct {
    const char *name;
    int32_t phi_cdeg;        /* assumed latitude,  N positive  */
    int32_t lon_east_cdeg;   /* assumed longitude, E positive  */
    int32_t gha_cdeg;        /* Greenwich hour angle           */
    int32_t dec_cdeg;        /* declination, N positive        */
    int32_t exp_hc_cdeg;     /* reference Hc (rounded cdeg)    */
    int32_t exp_zn_cdeg;     /* reference Zn (rounded cdeg)    */
    int exp_zn_valid;        /* zero at exact zenith/nadir     */
} truth_case_t;

/* References computed offline with IEEE double sin/cos/asin/atan2 and
 * rounded to the nearest centidegree (so they carry +-0.5 cdeg =
 * +-0.3 arcmin of representation error of their own). */
static const truth_case_t truth_cases[] = {
    /* worked example: 40N 74W, GHA 60, dec 20N -> LHA 346 (body east) */
    { "40N 74W  GHA=60  dec=20N ",  4000, -7400,  6000,  2000,  6668, 14495, 1 },
    { "33.5S 18.42E GHA=310 -16.7", -3350,  1842, 31000, -1670,  5704,  6721, 1 },
    { "51.5N 0.13W GHA=330 23.4N ",  5150,   -13, 33000,  2340,  5360, 12907, 1 },
    { "0N 100E  GHA=290 dec=10S  ",     0, 10000, 29000, -1000,  5853, 25057, 1 },
    { "55S 67.3W GHA=40  dec=5N  ", -5500, -6730,  4000,   500,  2587,  3052, 1 },

    /* Cardinal and singular cases mirrored by the celestial-navigator
     * reference tests (repository pinned at the mirrored rows below). */
    { "45N dec=0 on meridian      ",  4500,     0,     0,     0,  4500, 18000, 1 },
    { "45S dec=0 on meridian      ", -4500,     0,     0,     0,  4500,     0, 1 },
    { "equator dec=23N meridian   ",     0,     0,     0,  2300,  6700,     0, 1 },
    { "equator body due west      ",     0,     0,  9000,     0,     0, 27000, 1 },
    { "equator body due east      ",     0,     0, 27000,     0,     0,  9000, 1 },
    { "45N anti-meridian          ",  4500,     0, 18000,     0, -4500,     0, 1 },
    { "north pole, selected merid.",  9000,     0,     0,  4500,  4500, 18000, 1 },
    { "0.01 deg from zenith        ",  4500,     0,     1,  4500,  8999, 27000, 1 },
    { "near pole and near zenith   ",  8999,     0,     1,  8998,  8999, 18002, 1 },
    { "exact zenith (Zn undefined)",  4500,     0,     0,  4500,  9000,     0, 0 },
    { "exact nadir (Zn undefined) ",  4500,     0, 18000, -4500, -9000,     0, 0 },

    /* Wide-latitude (|lat| > 60 deg) absolute Zn coverage: the A-vs-B sweep
     * below is differential, so it can't see a bug shared by both paths
     * (cdeg_to_rad / fp_sincos / rad_to_cdeg). These three pin Path A's Zn
     * against an independent IEEE-double oracle evaluating the same
     * closed-form spherical-trig identity this file documents up top
     * (sin Hc = sin phi sin dec + cos phi cos dec cos LHA; N = cos phi sin
     * dec - sin phi cos dec cos LHA; E = -cos dec sin LHA; Zn = atan2(E,N)),
     * same generation method as the rest of this table. */
    { "70N 45E  GHA=200 dec=30S  ",  7000,  4500, 20000, -3000, -3651,  7758, 1 },
    { "75S 10W  GHA=100 dec=15N  ", -7500, -1000, 10000,  1500, -1448, 27397, 1 },
    { "68.25N 179.9E GHA=350.5 60N", 6825, 17990, 35050,  6000,  3844, 35389, 1 },

    /* Sight-reduction case geometries mirrored from the test suite of
     * https://github.com/alejandrozarco/celestial-navigator (MIT),
     * revision c09c04dcb950ed1ae27f08dd1c2992c48b85f990, and from classic
     * Pub. 229 entries (US government publication). Its test harness
     * evaluates the IEEE-double formula at runtime; the rows here are the
     * same results rounded once to this program's centidegree API. */
    { "c-navigator 34N 20N LHA45  ",  3400, 0,  4500,  2000,  4791, 26245, 1 },
    { "c-navigator 35S 45S LHA30  ", -3500, 0,  3000, -4500,  6512, 23718, 1 },
    { "Pub229 30N 20N LHA45       ",  3000, 0,  4500,  2000,  4828, 26690, 1 },
    { "Pub229 30N 20N LHA315      ",  3000, 0, 31500,  2000,  4828,  9310, 1 },
    { "Pub229 45N 30N LHA60       ",  4500, 0,  6000,  3000,  4128, 27361, 1 },
    { "Pub229 52N 23N LHA330      ",  5200, 0, 33000,  2300,  5301, 13010, 1 },
    { "Pub229 10N 15S LHA90       ",  1000, 0,  9000, -1500,  -258, 25522, 1 },
    { "Pub229 33S 45S LHA20       ", -3300, 0,  2000, -4500,  7046, 22630, 1 },
    { "c-navigator Polaris equator",     0, 0,     0,  8935,    65,     0, 1 },
    { "Pub229 horizon dec23.44N   ",     0, 0,  9000,  2344,     0, 29344, 1 },
};
#define N_TRUTH ((int)(sizeof truth_cases / sizeof truth_cases[0]))

/* Tolerances (measured headroom noted in README):
 *   Hc vs double truth: +-2 arcmin = 3 cdeg (measured: <= 1 cdeg)
 *   Path A Zn vs truth: +-0.05 deg = 5 cdeg
 *   Path B square-corrected Zn vs truth: +-0.02 deg = 2 cdeg
 *   Path C Zn (key -> cdeg boundary conversion) vs truth: +-3 cdeg
 *     (key round-trip quantization adds ~0.6 cdeg over Path B) */
#define TOL_HC_CDEG      3
#define TOL_ZN_A_CDEG    5
#define TOL_ZN_B_CDEG    2
#define TOL_ZN_C_CDEG    3

/* ================================================================== */
/*  Sweep sim parameters                                               */
/* ================================================================== */

#define SWEEP_TARGET     2000    /* accepted geometries                */
#define SWEEP_MAX_TRIES 20000    /* hard cap on rejection sampling     */
#define PHI_MAX_CDEG     9000    /* |phi| <= 90 deg (full domain)       */
#define DEC_MAX_CDEG     9000    /* |dec| <= 90 deg (full domain)       */
#define FIX_N_MAX_SIGHTS   32    /* astro_nav_fix_n_body()'s own bound  */
#define HC_MIN_CDEG       500    /* skip |Hc| <  5 deg (refraction hell) */
#define HC_MAX_CDEG      8500    /* skip |Hc| > 85 deg (azimuth degenerate) */

/* Two-body fix round trip: position in, position out. Everything in the
 * chain is quantized (Q2.30 vectors, Q2.30 sines, cdeg output rounding),
 * so the gate allows 2 cdeg of cross-platform headroom; measured max on
 * the reference host is 0 cdeg across the cases and the sweep. */
#define TOL_FIX_CDEG        2
#define FIX_SWEEP_TARGET  500

/* Independent calendar oracle for the civil-time A/B check: Howard
 * Hinnant's days_from_civil (public domain construction), a different
 * decomposition of the Gregorian 400-year cycle than the library's
 * Fliegel-Van Flandern form. Returns days since 1970-01-01. */
static int64_t ref_days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* ------------------------------------------------------------------ */
/*  --predict helpers: the public surface inverted at milli-arcminute   */
/*  resolution (CLI-side composition only -- no new core code)         */
/* ------------------------------------------------------------------ */

/* Arcsine in milli-arcminutes: the angle whose Q2.30 sine lands
 * nearest the machine value. Bisection over the public
 * astro_nav_sin_q30_from_marcmin() -- 23 evaluations over a monotone
 * domain, so the result is bit-determined by the public function
 * itself. Near +-90 deg the Q2.30 sine plateaus (the last ~0.1 deg
 * spans a handful of sine LSBs); every plateau angle is equally
 * consistent with the machine value and the smallest is returned. */
static int64_t predict_asin_marcmin(int32_t sin_q30)
{
    int64_t lo = -5400000, hi = 5400000;
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        if (astro_nav_sin_q30_from_marcmin(mid) < sin_q30) lo = mid;
        else hi = mid;
    }
    int64_t dlo = (int64_t)sin_q30 - astro_nav_sin_q30_from_marcmin(lo);
    int64_t dhi = (int64_t)astro_nav_sin_q30_from_marcmin(hi) - sin_q30;
    if (dlo < 0) dlo = -dlo;
    if (dhi < 0) dhi = -dhi;
    return dlo <= dhi ? lo : hi;
}

/* The correction chain run backward: the sextant reading Hs whose
 * corrected Ho equals the target -- what a zero-index-error instrument
 * would read under the chain's idealized natural-sea-horizon model.
 * Fixed-point iteration hs += (target - chain(hs)): the chain is Hs
 * plus corrections that move slowly with Hs (the steepest, refraction
 * near the horizon, changes under 0.2' per 1' of altitude), so each
 * step cuts the error by 5x or better. The loop keeps the best
 * iterate; *err_out reports how far chain(returned Hs) still sits
 * from the target -- 0 or 1 milli-arcminute everywhere the target is
 * reachable, larger only if the answer would clamp at +-90 deg. */
static int64_t predict_hs_marcmin(int64_t target_ho, int32_t eye_cm,
                                  int64_t hp, int64_t sd, int32_t limb,
                                  int32_t temp_c, int32_t pressure_mb,
                                  int moon, int64_t *err_out)
{
    int64_t hs = target_ho, best_hs = hs, best_err = INT64_MAX;
    for (int i = 0; i < 40; i++) {
        if (hs < -5400000) hs = -5400000;
        if (hs >  5400000) hs =  5400000;
        int64_t ho = moon
            ? astro_nav_correct_altitude_moon_tp_marcmin(hs, 0, eye_cm,
                                                         hp, sd, limb,
                                                         temp_c,
                                                         pressure_mb)
            : astro_nav_correct_altitude_tp_marcmin(hs, 0, eye_cm, hp,
                                                    sd, limb, temp_c,
                                                    pressure_mb);
        int64_t err = target_ho - ho;
        int64_t aerr = err < 0 ? -err : err;
        if (aerr < best_err) { best_err = aerr; best_hs = hs; }
        if (err == 0) break;
        hs += err;
    }
    *err_out = best_err;
    return best_hs;
}

static int run_self_tests(void)
{
    printf("astro-nav-int: integer-only sight reduction (Q16.48 + square rays)\n");
    printf("================================================================\n\n");

    int failures = 0;

    /* ---------------- 1. Truth-case validation ---------------- */
    printf("[1] Validation against double-precision references\n");
    printf("    %-28s %8s %8s %8s %8s %8s %8s %7s\n",
           "case", "Hc(A)", "Hc(B)", "Hc(C)", "Zn(A)", "Zn(B)", "Zn(C)",
           "status");

    for (int i = 0; i < N_TRUTH; i++) {
        const truth_case_t *tc = &truth_cases[i];

        sight_result_t a;
        sight_reduce_trig(tc->phi_cdeg, tc->lon_east_cdeg,
                          tc->gha_cdeg, tc->dec_cdeg, &a);
        square_result_t b;
        sight_reduce_square(tc->phi_cdeg, tc->lon_east_cdeg,
                            tc->gha_cdeg, tc->dec_cdeg, &b);
        machine_sight_t c;
        sight_reduce_machine(tc->phi_cdeg, tc->lon_east_cdeg,
                             tc->gha_cdeg, tc->dec_cdeg, &c);
        int32_t hc_c = astro_nav_hc_cdeg_from_sin_q30(c.sin_hc_q30);
        int32_t zn_c = astro_nav_zn_cdeg_from_square_key(c.square_key);

        /* Path C's one documented behavioral difference: at an exact pole
         * the observer unit vector carries no meridian, so C must report
         * Zn undefined even where the angle-fed paths still resolve it. */
        int at_pole = tc->phi_cdeg == CDEG_PER_QUARTER
                   || tc->phi_cdeg == -CDEG_PER_QUARTER;
        int exp_zn_valid_c = tc->exp_zn_valid && !at_pole;

        int32_t err_a = a.hc_cdeg - tc->exp_hc_cdeg;
        int32_t err_b = b.sight.hc_cdeg - tc->exp_hc_cdeg;
        int32_t err_c = hc_c - tc->exp_hc_cdeg;
        int32_t err_zn_a = tc->exp_zn_valid
            ? circular_diff_cdeg(a.zn_cdeg, tc->exp_zn_cdeg) : 0;
        int32_t err_zn_b = tc->exp_zn_valid
            ? circular_diff_cdeg(b.sight.zn_cdeg, tc->exp_zn_cdeg) : 0;
        int32_t err_zn_c = exp_zn_valid_c
            ? circular_diff_cdeg(zn_c, tc->exp_zn_cdeg) : 0;

        int ok = (err_a <= TOL_HC_CDEG && err_a >= -TOL_HC_CDEG)
              && (err_b <= TOL_HC_CDEG && err_b >= -TOL_HC_CDEG)
              && (err_c <= TOL_HC_CDEG && err_c >= -TOL_HC_CDEG)
              && (a.hc_cdeg == b.sight.hc_cdeg)
              && (a.zn_valid == tc->exp_zn_valid)
              && (b.sight.zn_valid == tc->exp_zn_valid)
              && (c.zn_valid == exp_zn_valid_c)
              && (!tc->exp_zn_valid || (err_zn_a <= TOL_ZN_A_CDEG
                                      && err_zn_b <= TOL_ZN_B_CDEG))
              && (!exp_zn_valid_c || err_zn_c <= TOL_ZN_C_CDEG);
        if (!ok) failures++;

        printf("    %-28s ", tc->name);
        print_cdeg(a.hc_cdeg);             printf(" ");
        print_cdeg(b.sight.hc_cdeg);       printf(" ");
        print_cdeg(hc_c);                  printf(" ");
        if (a.zn_valid) {
            print_cdeg(a.zn_cdeg);         printf(" ");
            print_cdeg(b.sight.zn_cdeg);   printf(" ");
        } else {
            printf("     N/A      N/A ");
        }
        if (c.zn_valid) print_cdeg(zn_c);
        else            printf("    N/A");
        printf(" %s\n", ok ? "PASS" : "FAIL");
    }
    printf("    tolerances: Hc(A/B/C) +-%d, Zn(A) +-%d, Zn(B) +-%d, "
           "Zn(C) +-%d cdeg\n"
           "    (Zn(C) is N/A at exact poles: a pole unit vector carries "
           "no meridian)\n\n",
           TOL_HC_CDEG, TOL_ZN_A_CDEG, TOL_ZN_B_CDEG, TOL_ZN_C_CDEG);

    /* ---------------- 2. Sweep: Paths A/B/C ---------------- */
    printf("[2] Sweep sim: %d random geometries, Paths A/B/C (Hc, Zn)\n",
           SWEEP_TARGET);
    printf("    domain: phi, dec in [-90, 90] deg (full), LHA in [0, 360)\n");

    int accepted = 0;
    int32_t max_hc_diff_cdeg = 0;
    int32_t max_hc_c_diff_cdeg = 0;
    int zn_c_compared = 0;
    int32_t max_zn_c_diff_cdeg = 0;

    /* Azimuth: only accumulated when Path A itself reports Zn as defined
     * (exact zenith/nadir is excluded already by the Hc-band filter below,
     * but the gate stays explicit -- never assert Zn where the code
     * itself declares it undefined). */
    int zn_compared = 0;
    int64_t sum_zn_diff_cdeg = 0;
    int32_t max_zn_diff_cdeg = 0;
    int32_t max_zn_diff_hc   = 0;

    for (int tries = 0; tries < SWEEP_MAX_TRIES && accepted < SWEEP_TARGET;
         tries++) {
        int32_t phi = lcg_range(-PHI_MAX_CDEG, PHI_MAX_CDEG + 1);
        int32_t dec = lcg_range(-DEC_MAX_CDEG, DEC_MAX_CDEG + 1);
        int32_t lha = lcg_range(0, CDEG_PER_TURN);   /* fold via GHA, lon=0 */

        sight_result_t a;
        sight_reduce_trig(phi, 0, lha, dec, &a);

        int32_t abs_hc = a.hc_cdeg < 0 ? -a.hc_cdeg : a.hc_cdeg;
        if (abs_hc < HC_MIN_CDEG || abs_hc > HC_MAX_CDEG)
            continue;   /* near-degenerate: navigators avoid these too */

        square_result_t b;
        sight_reduce_square(phi, 0, lha, dec, &b);
        int32_t hc_diff = a.hc_cdeg - b.sight.hc_cdeg;
        if (hc_diff < 0) hc_diff = -hc_diff;
        if (hc_diff > max_hc_diff_cdeg) max_hc_diff_cdeg = hc_diff;

        machine_sight_t c;
        sight_reduce_machine(phi, 0, lha, dec, &c);
        int32_t hc_c_diff = astro_nav_hc_cdeg_from_sin_q30(c.sin_hc_q30)
                          - a.hc_cdeg;
        if (hc_c_diff < 0) hc_c_diff = -hc_c_diff;
        if (hc_c_diff > max_hc_c_diff_cdeg) max_hc_c_diff_cdeg = hc_c_diff;

        accepted++;

        if (a.zn_valid && b.sight.zn_valid) {
            int32_t zdiff = circular_diff_cdeg(b.sight.zn_cdeg, a.zn_cdeg);
            sum_zn_diff_cdeg += zdiff;
            if (zdiff > max_zn_diff_cdeg) {
                max_zn_diff_cdeg = zdiff;
                max_zn_diff_hc = a.hc_cdeg;
            }
            zn_compared++;
        }

        /* C skips exact poles (its zn_valid = 0 there by design). */
        if (a.zn_valid && c.zn_valid) {
            int32_t zdiff = circular_diff_cdeg(
                astro_nav_zn_cdeg_from_square_key(c.square_key), a.zn_cdeg);
            if (zdiff > max_zn_c_diff_cdeg) max_zn_c_diff_cdeg = zdiff;
            zn_c_compared++;
        }
    }

    if (accepted < SWEEP_TARGET) {
        printf("    FAIL: only %d geometries accepted\n", accepted);
        failures++;
    }

    printf("    accepted: %d   |Hc(A) - Hc(B)| max ", accepted);
    print_marcmin(astro_nav_cdeg_to_marcmin(max_hc_diff_cdeg));
    printf("\n");
    if (max_hc_diff_cdeg != 0) {
        printf("    FAIL: Path B altitude must reuse Path A exactly\n");
        failures++;
    }

    int64_t mean_zn_cdeg_x100 = zn_compared
        ? (sum_zn_diff_cdeg * 100) / zn_compared : 0;   /* 2 decimal places */
    printf("    |Zn(A) - Zn(B)|: compared %d   mean ", zn_compared);
    printf("%lld.%02lld", (long long)(mean_zn_cdeg_x100 / 100),
           (long long)(mean_zn_cdeg_x100 % 100));
    printf(" cdeg   max ");
    print_cdeg(max_zn_diff_cdeg);
    printf(" deg (at Hc=");
    print_cdeg(max_zn_diff_hc);
    printf(" deg)\n\n");

    if (zn_compared > 0 && max_zn_diff_cdeg > TOL_ZN_B_CDEG) {
        printf("    FAIL: max A-vs-B Zn disagreement > %d cdeg tolerance\n",
               TOL_ZN_B_CDEG);
        failures++;
    }

    printf("    Path C vs A: |Hc| max %ld cdeg   |Zn| compared %d   max %ld cdeg\n\n",
           (long)max_hc_c_diff_cdeg, zn_c_compared,
           (long)max_zn_c_diff_cdeg);
    if (max_hc_c_diff_cdeg > 1) {
        printf("    FAIL: Path C altitude differs from Path A by > 1 cdeg\n");
        failures++;
    }
    if (zn_c_compared > 0 && max_zn_c_diff_cdeg > TOL_ZN_C_CDEG) {
        printf("    FAIL: max A-vs-C Zn disagreement > %d cdeg tolerance\n",
               TOL_ZN_C_CDEG);
        failures++;
    }

    /* ---------------- 3. Intercept demo ---------------- */
    printf("[3] Intercept demo (Marcq St. Hilaire)\n");
    {
        const truth_case_t *tc = &truth_cases[0];
        sight_result_t a;
        sight_reduce_trig(tc->phi_cdeg, tc->lon_east_cdeg,
                          tc->gha_cdeg, tc->dec_cdeg, &a);

        /* Fabricated observation: Ho = Hc + 12' (12' = 20 cdeg). */
        int32_t ho_cdeg = a.hc_cdeg + 20;
        int64_t signed_tenths = astro_nav_intercept_tenths_nm(ho_cdeg,
                                                              a.hc_cdeg);
        int toward = signed_tenths >= 0;
        int64_t tenths = toward ? signed_tenths : -signed_tenths;

        printf("    assumed position 40 00.0'N  74 00.0'W\n");
        printf("    Hc "); print_cdeg(a.hc_cdeg);
        printf(" deg   Ho "); print_cdeg(ho_cdeg);
        printf(" deg   Zn "); print_cdeg(a.zn_cdeg);
        printf(" deg\n");
        printf("    intercept: %ld.%ld nm %s %03ld deg -- \"",
               (long)(tenths / 10), (long)(tenths % 10),
               toward ? "TOWARD" : "AWAY",
               (long)((a.zn_cdeg + 50) / 100));
        printf("%ld.%ld miles %s, azimuth %03ld\"\n\n",
               (long)(tenths / 10), (long)(tenths % 10),
               toward ? "toward" : "away",
               (long)((a.zn_cdeg + 50) / 100));
    }

    /* ---------------- 4. Two-body fix ---------------- */
    printf("[4] Two-body fix: exact circle-of-equal-altitude intersection\n");
    {
        /* Round trip: generate the two "observed" sines with Method C
         * from a known true position, then invert with the fix from a
         * deliberately-offset DR hint. The recovered position must match
         * the generator. */
        typedef struct {
            const char *name;
            int32_t lat, lon;                 /* true position         */
            int32_t gha1, dec1, gha2, dec2;   /* the two bodies        */
            int32_t dr_dlat, dr_dlon;         /* DR hint offset, cdeg  */
        } fix_case_t;
        static const fix_case_t fix_cases[] = {
            { "40N 74W, morning pair    ",  4000,  -7400,  6000,  2000,
                                           31000,  -1000,   500,   -700 },
            { "33.5S 18.4E, two stars   ", -3350,   1842, 31000, -1670,
                                           10000,   1500,  -600,    800 },
            { "equator mid-Pacific      ",     0, -15000, 33000,   800,
                                           26000,  -1200,   700,    900 },
            { "65N, high latitude       ",  6500,   1500, 20000,  4500,
                                            5000,   1000,  -500,   -500 },
            { "45N Greenwich, Pub229 cut",  4500,      0,     0,     0,
                                            6000,   3000,   400,    600 },
        };
        enum { N_FIX = (int)(sizeof fix_cases / sizeof fix_cases[0]) };

        printf("    %-26s   recovered            true     "
               "        |dLat| |dLon|\n", "case");
        for (int i = 0; i < N_FIX; i++) {
            const fix_case_t *fc = &fix_cases[i];
            astro_nav_unitvec_t b1, b2, hint;
            machine_sight_t m1, m2;
            astro_nav_unitvec_from_cdeg(fc->dec1, -fc->gha1, &b1);
            astro_nav_unitvec_from_cdeg(fc->dec2, -fc->gha2, &b2);
            sight_reduce_machine(fc->lat, fc->lon, fc->gha1, fc->dec1, &m1);
            sight_reduce_machine(fc->lat, fc->lon, fc->gha2, fc->dec2, &m2);
            astro_nav_unitvec_from_cdeg(fc->lat + fc->dr_dlat,
                                        fc->lon + fc->dr_dlon, &hint);

            astro_nav_fix_result_t fix;
            astro_nav_fix_two_body(&b1, m1.sin_hc_q30, &b2, m2.sin_hc_q30,
                                   &hint, &fix);
            int32_t rlat = 0, rlon = 0;
            if (fix.valid)
                astro_nav_latlon_cdeg_from_unitvec(&fix.position,
                                                   &rlat, &rlon);
            int32_t err_lat = circular_diff_cdeg(rlat, fc->lat);
            int32_t err_lon = circular_diff_cdeg(rlon, fc->lon);
            int ok = fix.valid && err_lat <= TOL_FIX_CDEG
                               && err_lon <= TOL_FIX_CDEG;
            if (!ok) failures++;

            printf("    %-26s   ", fc->name);
            print_cdeg(rlat); printf(" "); print_cdeg(rlon);
            printf("   "); print_cdeg(fc->lat); printf(" ");
            print_cdeg(fc->lon);
            printf("   %2ld    %2ld   %s\n", (long)err_lat, (long)err_lon,
                   ok ? "ok" : (fix.valid ? "FAIL" : "FAIL (invalid)"));
        }

        /* Ambiguity: re-running the fix with the hint moved to the
         * alternate intersection must return exactly the two points
         * swapped -- bit-for-bit, since the arithmetic is identical. */
        {
            const fix_case_t *fc = &fix_cases[0];
            astro_nav_unitvec_t b1, b2, hint;
            machine_sight_t m1, m2;
            astro_nav_unitvec_from_cdeg(fc->dec1, -fc->gha1, &b1);
            astro_nav_unitvec_from_cdeg(fc->dec2, -fc->gha2, &b2);
            sight_reduce_machine(fc->lat, fc->lon, fc->gha1, fc->dec1, &m1);
            sight_reduce_machine(fc->lat, fc->lon, fc->gha2, fc->dec2, &m2);
            astro_nav_unitvec_from_cdeg(fc->lat, fc->lon, &hint);

            astro_nav_fix_result_t first, swapped;
            astro_nav_fix_two_body(&b1, m1.sin_hc_q30, &b2, m2.sin_hc_q30,
                                   &hint, &first);
            astro_nav_fix_two_body(&b1, m1.sin_hc_q30, &b2, m2.sin_hc_q30,
                                   &first.alternate, &swapped);
            int ok = first.valid && swapped.valid
                && swapped.position.x  == first.alternate.x
                && swapped.position.y  == first.alternate.y
                && swapped.position.z  == first.alternate.z
                && swapped.alternate.x == first.position.x
                && swapped.alternate.y == first.position.y
                && swapped.alternate.z == first.position.z;
            if (!ok) failures++;
            printf("    hint at alternate point swaps the two solutions"
                   " bit-for-bit: %s\n", ok ? "ok" : "FAIL");
        }

        /* Degenerate geometry must be rejected, not mis-answered. */
        {
            astro_nav_unitvec_t b1, anti, east, up, hint;
            astro_nav_fix_result_t fix;
            int32_t s = astro_nav_sin_q30_from_cdeg(4000);
            astro_nav_unitvec_from_cdeg(2000, -6000, &b1);
            anti.x = -b1.x; anti.y = -b1.y; anti.z = -b1.z;
            astro_nav_unitvec_from_cdeg(0, 0, &east);
            astro_nav_unitvec_from_cdeg(9000, 0, &up);
            astro_nav_unitvec_from_cdeg(3000, -4000, &hint);

            int bad = 0;
            astro_nav_fix_two_body(&b1, s, &b1, s, &hint, &fix);
            bad |= fix.valid;                 /* same body twice        */
            astro_nav_fix_two_body(&b1, s, &anti, -s, &hint, &fix);
            bad |= fix.valid;                 /* antipodal body         */
            astro_nav_fix_two_body(&up, (int32_t)1 << 30,
                                   &east, (int32_t)1 << 30, &hint, &fix);
            bad |= fix.valid;                 /* two disjoint zeniths   */
            if (bad) failures++;
            printf("    degenerate pairs (same body / antipode / disjoint"
                   " circles) rejected: %s\n", bad ? "FAIL" : "ok");
        }

        /* Round-trip sweep: random positions and body pairs with a
         * usable cut (>= 15 deg) and both altitudes in the operational
         * band, hint at the true position. */
        {
            int fixes = 0, tries = 0, invalid = 0;
            int32_t max_err_lat = 0, max_err_lon = 0;
            lcg_state = 0x9E3779B97F4A7C15ULL;   /* distinct seed */
            while (fixes < FIX_SWEEP_TARGET && tries < SWEEP_MAX_TRIES) {
                tries++;
                int32_t lat  = lcg_range(-8500, 8501);
                int32_t lon  = lcg_range(-18000, 18001);
                int32_t gha1 = lcg_range(0, CDEG_PER_TURN);
                int32_t dec1 = lcg_range(-8500, 8501);
                int32_t gha2 = lcg_range(0, CDEG_PER_TURN);
                int32_t dec2 = lcg_range(-8500, 8501);

                astro_nav_unitvec_t obs, b1, b2;
                astro_nav_unitvec_from_cdeg(lat, lon, &obs);
                astro_nav_unitvec_from_cdeg(dec1, -gha1, &b1);
                astro_nav_unitvec_from_cdeg(dec2, -gha2, &b2);

                /* Cut-angle filter: |B1 . B2| <= ~cos 15 deg. */
                int64_t g30 = ((int64_t)b1.x * b2.x + (int64_t)b1.y * b2.y
                             + (int64_t)b1.z * b2.z) >> 30;
                if (g30 > 1037000000 || g30 < -1037000000) continue;

                machine_sight_t m1, m2;
                astro_nav_reduce_method_c(&obs, &b1, &m1);
                astro_nav_reduce_method_c(&obs, &b2, &m2);
                /* Both altitudes in ~[5, 85] deg (sin bounds in Q2.30),
                 * matching the main sweep's operational band. */
                if (m1.sin_hc_q30 < 93583000 || m1.sin_hc_q30 > 1069650000)
                    continue;
                if (m2.sin_hc_q30 < 93583000 || m2.sin_hc_q30 > 1069650000)
                    continue;

                astro_nav_fix_result_t fix;
                astro_nav_fix_two_body(&b1, m1.sin_hc_q30,
                                       &b2, m2.sin_hc_q30, &obs, &fix);
                if (!fix.valid) { invalid++; continue; }

                int32_t rlat, rlon;
                astro_nav_latlon_cdeg_from_unitvec(&fix.position,
                                                   &rlat, &rlon);
                int32_t err_lat = circular_diff_cdeg(rlat, lat);
                int32_t err_lon = circular_diff_cdeg(rlon, lon);
                if (err_lat > max_err_lat) max_err_lat = err_lat;
                if (err_lon > max_err_lon) max_err_lon = err_lon;
                fixes++;
            }
            printf("    sweep: %d fixes   max |dLat| %ld cdeg   "
                   "max |dLon| %ld cdeg   rejected-invalid %d\n\n",
                   fixes, (long)max_err_lat, (long)max_err_lon, invalid);
            if (fixes < FIX_SWEEP_TARGET) {
                printf("    FAIL: sweep did not reach %d accepted fixes\n",
                       FIX_SWEEP_TARGET);
                failures++;
            }
            if (invalid != 0) {
                printf("    FAIL: %d well-conditioned pairs reported"
                       " invalid\n", invalid);
                failures++;
            }
            if (max_err_lat > TOL_FIX_CDEG || max_err_lon > TOL_FIX_CDEG) {
                printf("    FAIL: fix round-trip error > %d cdeg\n",
                       TOL_FIX_CDEG);
                failures++;
            }
        }
    }

    /* ------- 5. Running-fix advancement + n-body least squares ------ */
    printf("[5] Running-fix advancement and n-body least-squares fix\n");
    {
        /* Zero run is the exact identity (Q2.30 -> Q16.48 -> Q2.30
         * round-trips bit-for-bit), and a DR at an exact pole -- where
         * a course has no direction -- returns the body unrotated. */
        {
            astro_nav_unitvec_t b, dr, adv;
            astro_nav_unitvec_from_cdeg(2000, -6000, &b);
            astro_nav_unitvec_from_cdeg(4000, -7400, &dr);
            astro_nav_advance_body_for_run(&b, &dr, 4500, 0, &adv);
            int ok = adv.x == b.x && adv.y == b.y && adv.z == b.z;
            astro_nav_unitvec_from_cdeg(9000, 0, &dr);
            astro_nav_advance_body_for_run(&b, &dr, 0, 600, &adv);
            ok = ok && adv.x == b.x && adv.y == b.y && adv.z == b.z;
            if (!ok) failures++;
            printf("    zero run bitwise identity; pole DR unrotated:"
                   " %s\n", ok ? "ok" : "FAIL");
        }

        /* The run rotation itself: advancing the DR position advances
         * it along the track. 30 nm due north = 50 cdeg of latitude;
         * 60 nm due east on the equator = 100 cdeg of longitude. */
        {
            astro_nav_unitvec_t p, adv;
            int32_t rlat, rlon;
            astro_nav_unitvec_from_cdeg(4000, -7400, &p);
            astro_nav_advance_body_for_run(&p, &p, 0, 300, &adv);
            astro_nav_latlon_cdeg_from_unitvec(&adv, &rlat, &rlon);
            int ok = circular_diff_cdeg(rlat, 4050) <= 1
                  && circular_diff_cdeg(rlon, -7400) <= 1;
            astro_nav_unitvec_from_cdeg(0, 0, &p);
            astro_nav_advance_body_for_run(&p, &p, 9000, 600, &adv);
            astro_nav_latlon_cdeg_from_unitvec(&adv, &rlat, &rlon);
            ok = ok && circular_diff_cdeg(rlat, 0) <= 1
                    && circular_diff_cdeg(rlon, 100) <= 1;
            if (!ok) failures++;
            printf("    run rotation carries the DR along the track"
                   " (N and E checks): %s\n", ok ? "ok" : "FAIL");
        }

        /* Course and run are periodic: extreme int32 arguments reduce
         * exactly instead of overflowing the radian conversion, so
         * INT32_MAX/INT32_MIN match their reduced twins bit-for-bit. */
        {
            astro_nav_unitvec_t b, dr, a1, a2;
            astro_nav_unitvec_from_cdeg(2000, -6000, &b);
            astro_nav_unitvec_from_cdeg(4000, -7400, &dr);
            astro_nav_advance_body_for_run(&b, &dr, INT32_MAX,
                                           INT32_MAX, &a1);
            astro_nav_advance_body_for_run(&b, &dr, INT32_MAX % 36000,
                                           INT32_MAX % 216000, &a2);
            int ok = a1.x == a2.x && a1.y == a2.y && a1.z == a2.z;
            astro_nav_advance_body_for_run(&b, &dr, INT32_MIN,
                                           INT32_MIN, &a1);
            astro_nav_advance_body_for_run(&b, &dr, INT32_MIN % 36000,
                                           INT32_MIN % 216000, &a2);
            ok = ok && a1.x == a2.x && a1.y == a2.y && a1.z == a2.z;
            if (!ok) failures++;
            printf("    periodic course/run: extreme int32 == reduced,"
                   " bitwise: %s\n", ok ? "ok" : "FAIL");
        }

        /* Classic running fix, done as rotation: sight at 40N 74W,
         * steam true north 30 nm (end truth 40.5N 74W), fresh sight at
         * the end, advance the first BODY by the run, intersect. The
         * fix must recover the END position. */
        {
            const int32_t lat1 = 4000, lat2 = 4050, lon = -7400;
            astro_nav_unitvec_t b1, b1a, b2, dr1, hint;
            machine_sight_t m1, m2;
            astro_nav_fix_result_t fix;
            astro_nav_unitvec_from_cdeg(2000, -31000, &b1);
            astro_nav_unitvec_from_cdeg(-1000, -6000, &b2);
            sight_reduce_machine(lat1, lon, 31000, 2000, &m1);
            sight_reduce_machine(lat2, lon, 6000, -1000, &m2);
            astro_nav_unitvec_from_cdeg(lat1, lon, &dr1);
            astro_nav_advance_body_for_run(&b1, &dr1, 0, 300, &b1a);
            astro_nav_unitvec_from_cdeg(lat2 + 40, lon - 60, &hint);
            astro_nav_fix_two_body(&b1a, m1.sin_hc_q30,
                                   &b2, m2.sin_hc_q30, &hint, &fix);
            int32_t rlat = 0, rlon = 0;
            if (fix.valid)
                astro_nav_latlon_cdeg_from_unitvec(&fix.position,
                                                   &rlat, &rlon);
            int ok = fix.valid
                  && circular_diff_cdeg(rlat, lat2) <= TOL_FIX_CDEG
                  && circular_diff_cdeg(rlon, lon) <= TOL_FIX_CDEG;
            if (!ok) failures++;
            printf("    running fix (advance first sight 30 nm N,"
                   " recover end position): %s\n", ok ? "ok" : "FAIL");
        }

        /* n-body on exact data: 3, 4, and 5 sights generated from one
         * true position must recover it; n = 2 must also agree with
         * the closed-form two-body intersection near the seed. */
        {
            const int32_t lat = 4000, lon = -7400;
            static const int32_t nb_gha[5] = { 31000, 6000, 20000,
                                               1000, 34000 };
            static const int32_t nb_dec[5] = { 2000, -1000, 4500,
                                               3000, 500 };
            astro_nav_unitvec_t bodies[5], seed;
            int32_t sines[5];
            machine_sight_t m;
            for (int k = 0; k < 5; k++) {
                astro_nav_unitvec_from_cdeg(nb_dec[k], -nb_gha[k],
                                            &bodies[k]);
                sight_reduce_machine(lat, lon, nb_gha[k], nb_dec[k], &m);
                sines[k] = m.sin_hc_q30;
            }
            astro_nav_unitvec_from_cdeg(lat + 80, lon - 120, &seed);

            int ok = 1;
            for (int n = 2; n <= 5; n++) {
                astro_nav_fixn_result_t r;
                astro_nav_fix_n_body(bodies, sines, n, &seed, &r);
                int32_t rlat = 0, rlon = 0;
                if (r.valid)
                    astro_nav_latlon_cdeg_from_unitvec(&r.position,
                                                       &rlat, &rlon);
                int good = r.valid
                    && circular_diff_cdeg(rlat, lat) <= TOL_FIX_CDEG
                    && circular_diff_cdeg(rlon, lon) <= TOL_FIX_CDEG
                    && r.max_residual_marcmin <= 20;
                if (!good) ok = 0;
                printf("    n=%d exact sights: ", n);
                print_cdeg(rlat); printf(" "); print_cdeg(rlon);
                printf("   iters %d   worst residual %ld marcmin   %s\n",
                       r.iterations, (long)r.max_residual_marcmin,
                       good ? "ok" : "FAIL");
            }

            /* n = 2 equals the closed form (same two circles). */
            {
                astro_nav_fixn_result_t r;
                astro_nav_fix_result_t fix2;
                astro_nav_fix_n_body(bodies, sines, 2, &seed, &r);
                astro_nav_fix_two_body(&bodies[0], sines[0],
                                       &bodies[1], sines[1],
                                       &seed, &fix2);
                int32_t nlat = 0, nlon = 0, tlat = 0, tlon = 0;
                astro_nav_latlon_cdeg_from_unitvec(&r.position,
                                                   &nlat, &nlon);
                astro_nav_latlon_cdeg_from_unitvec(&fix2.position,
                                                   &tlat, &tlon);
                int good = r.valid && fix2.valid
                    && circular_diff_cdeg(nlat, tlat) <= 1
                    && circular_diff_cdeg(nlon, tlon) <= 1;
                if (!good) ok = 0;
                printf("    n=2 agrees with closed-form two-body fix:"
                       " %s\n", good ? "ok" : "FAIL");
            }

            /* Perturbed sines: push each observation off its circle by
             * a fraction of an arcminute (sextant-scale scatter). The
             * least-squares position must stay near truth and the
             * reported worst residual must land in the scatter band,
             * not at zero and not wildly amplified. */
            {
                int32_t noisy[3];
                noisy[0] = sines[0] + 200000;   /* ~ +0.6' of arc */
                noisy[1] = sines[1] - 200000;
                noisy[2] = sines[2] + 100000;
                astro_nav_fixn_result_t r;
                astro_nav_fix_n_body(bodies, noisy, 3, &seed, &r);
                int32_t rlat = 0, rlon = 0;
                if (r.valid)
                    astro_nav_latlon_cdeg_from_unitvec(&r.position,
                                                       &rlat, &rlon);
                int good = r.valid
                    && circular_diff_cdeg(rlat, lat) <= 8
                    && circular_diff_cdeg(rlon, lon) <= 8
                    && r.max_residual_marcmin >= 100
                    && r.max_residual_marcmin <= 2000;
                if (!good) ok = 0;
                printf("    n=3 perturbed sines: ");
                print_cdeg(rlat); printf(" "); print_cdeg(rlon);
                printf("   worst residual %ld marcmin   %s\n",
                       (long)r.max_residual_marcmin,
                       good ? "ok" : "FAIL");
            }
            if (!ok) failures++;
        }

        /* Degenerate and out-of-range inputs must be rejected. */
        {
            astro_nav_unitvec_t bodies[3], seed;
            int32_t sines[3];
            machine_sight_t m;
            /* All three bodies due north of an equatorial observer:
             * every circle runs east-west here -- no azimuth spread,
             * no cut. */
            static const int32_t col_dec[3] = { 2000, 4000, 6000 };
            for (int k = 0; k < 3; k++) {
                astro_nav_unitvec_from_cdeg(col_dec[k], 0, &bodies[k]);
                sight_reduce_machine(0, 0, 0, col_dec[k], &m);
                sines[k] = m.sin_hc_q30;
            }
            astro_nav_unitvec_from_cdeg(0, 0, &seed);
            astro_nav_fixn_result_t r;
            int bad = 0;
            astro_nav_fix_n_body(bodies, sines, 3, &seed, &r);
            bad |= r.valid;                    /* colinear azimuths */
            astro_nav_fix_n_body(bodies, sines, 1, &seed, &r);
            bad |= r.valid;                    /* n too small       */
            astro_nav_fix_n_body(bodies, sines, 33, &seed, &r);
            bad |= r.valid;                    /* n too large       */
            if (bad) failures++;
            printf("    degenerate n-body inputs (colinear azimuths,"
                   " n out of range) rejected: %s\n",
                   bad ? "FAIL" : "ok");
        }

        /* Round-trip sweep, n = 3: random positions and body triples
         * with operational altitudes and a usable pairwise azimuth
         * spread, seeded ~1.5 deg off truth. */
        {
            enum { FIXN_SWEEP_TARGET = 200 };
            int fixes = 0, tries = 0, invalid = 0;
            int32_t max_err_lat = 0, max_err_lon = 0;
            int64_t max_resid = 0;
            int max_iters = 0;
            lcg_state = 0xC2B2AE3D27D4EB4FULL;   /* distinct seed */
            while (fixes < FIXN_SWEEP_TARGET && tries < SWEEP_MAX_TRIES) {
                tries++;
                int32_t lat = lcg_range(-8500, 8501);
                int32_t lon = lcg_range(-18000, 18001);
                int32_t gha[3], dec[3], zn[3];
                sight_result_t sr;
                int usable = 1;
                for (int k = 0; k < 3; k++) {
                    gha[k] = lcg_range(0, CDEG_PER_TURN);
                    dec[k] = lcg_range(-8500, 8501);
                    sight_reduce_trig(lat, lon, gha[k], dec[k], &sr);
                    /* Altitude ~[5, 85] deg, azimuth defined. */
                    if (!sr.zn_valid || sr.hc_cdeg < 500
                                     || sr.hc_cdeg > 8500)
                        usable = 0;
                    zn[k] = sr.zn_cdeg;
                }
                if (!usable) continue;
                /* Pairwise azimuth spread >= 15 deg modulo a half
                 * turn (circles with opposite azimuths are parallel
                 * too, so the spread folds at 180 deg). */
                for (int a = 0; a < 3 && usable; a++)
                    for (int b = a + 1; b < 3; b++) {
                        int32_t d = zn[a] - zn[b];
                        d %= CDEG_PER_HALFTURN;
                        if (d < 0) d += CDEG_PER_HALFTURN;
                        if (d > CDEG_PER_HALFTURN - d)
                            d = CDEG_PER_HALFTURN - d;
                        if (d < 1500) { usable = 0; break; }
                    }
                if (!usable) continue;

                astro_nav_unitvec_t obs, bodies[3], seed;
                int32_t sines[3];
                machine_sight_t m;
                astro_nav_unitvec_from_cdeg(lat, lon, &obs);
                for (int k = 0; k < 3; k++) {
                    astro_nav_unitvec_from_cdeg(dec[k], -gha[k],
                                                &bodies[k]);
                    astro_nav_reduce_method_c(&obs, &bodies[k], &m);
                    sines[k] = m.sin_hc_q30;
                }
                astro_nav_unitvec_from_cdeg(lat + 100, lon - 150, &seed);

                astro_nav_fixn_result_t r;
                astro_nav_fix_n_body(bodies, sines, 3, &seed, &r);
                if (!r.valid) { invalid++; continue; }

                int32_t rlat, rlon;
                astro_nav_latlon_cdeg_from_unitvec(&r.position,
                                                   &rlat, &rlon);
                int32_t err_lat = circular_diff_cdeg(rlat, lat);
                int32_t err_lon = circular_diff_cdeg(rlon, lon);
                if (err_lat > max_err_lat) max_err_lat = err_lat;
                if (err_lon > max_err_lon) max_err_lon = err_lon;
                if (r.max_residual_marcmin > max_resid)
                    max_resid = r.max_residual_marcmin;
                if (r.iterations > max_iters) max_iters = r.iterations;
                fixes++;
            }
            printf("    sweep: %d 3-body fixes   max |dLat| %ld   max"
                   " |dLon| %ld cdeg   worst residual %ld marcmin  "
                   " max iters %d   rejected-invalid %d\n\n",
                   fixes, (long)max_err_lat, (long)max_err_lon,
                   (long)max_resid, max_iters, invalid);
            if (fixes < FIXN_SWEEP_TARGET) {
                printf("    FAIL: sweep did not reach %d accepted"
                       " fixes\n", FIXN_SWEEP_TARGET);
                failures++;
            }
            if (invalid != 0) {
                printf("    FAIL: %d well-conditioned triples reported"
                       " invalid\n", invalid);
                failures++;
            }
            if (max_err_lat > TOL_FIX_CDEG || max_err_lon > TOL_FIX_CDEG) {
                printf("    FAIL: n-body round-trip error > %d cdeg\n",
                       TOL_FIX_CDEG);
                failures++;
            }
            if (max_resid > 20) {
                printf("    FAIL: exact-data residual > 20 marcmin\n");
                failures++;
            }
        }
    }

    /* ------------- 6. Altitude corrections, Hs -> Ho ---------------- */
    printf("[6] Altitude corrections (dip, refraction, parallax, Hs -> Ho)\n");
    {
        /* Truth rows from a separate double-precision oracle of the
         * same formulas (dip 1.76' sqrt(m); Bennett refraction;
         * HP cos Ha). Values in milli-arcminutes. */
        {
            static const struct { int32_t cm; int64_t want; } dip[] = {
                {    0,    0 }, {  200, 2489 }, {  300, 3048 },
                {  550, 4128 }, { 1200, 6097 },
            };
            static const struct { int64_t ha, want; } refr[] = {
                {       0, 34478 }, {   60000, 24329 }, {  300000, 9883 },
                {  600000,  5392 }, { 1200000,  2703 }, { 2700000,  995 },
                { 4800000,   175 }, { 5400000,     0 },
            };
            int ok = 1;
            for (int i = 0; i < (int)(sizeof dip / sizeof dip[0]); i++) {
                int64_t got = astro_nav_dip_marcmin(dip[i].cm);
                int64_t d = got - dip[i].want;
                if (d < -2 || d > 2) ok = 0;
            }
            for (int i = 0; i < (int)(sizeof refr / sizeof refr[0]); i++) {
                int64_t got = astro_nav_refraction_marcmin(refr[i].ha);
                int64_t d = got - refr[i].want;
                if (d < -20 || d > 20) ok = 0;
            }
            {
                int64_t d = astro_nav_parallax_marcmin(1800000, 54000)
                          - 46765;
                if (d < -5 || d > 5) ok = 0;
                d = astro_nav_parallax_marcmin(3600000, 58600) - 29300;
                if (d < -5 || d > 5) ok = 0;
            }
            if (!ok) failures++;
            printf("    dip / Bennett refraction / parallax truth rows"
                   " vs double oracle: %s\n", ok ? "ok" : "FAIL");
        }

        /* Full chain Hs -> Ho, three classic sights (oracle values):
         * Moon LL 45 deg 30.5', Sun LL 25 deg 14.3', star 52 deg 07.8'. */
        {
            int ok = 1;
            int64_t ho, d;
            ho = astro_nav_correct_altitude_marcmin(2730500, -300, 300,
                                                    55000, 15000, 1);
            d = ho - 2779755;
            if (d < -30 || d > 30) ok = 0;
            ho = astro_nav_correct_altitude_marcmin(1514300, 100, 200,
                                                    150, 16100, 1);
            d = ho - 1526045;
            if (d < -30 || d > 30) ok = 0;
            ho = astro_nav_correct_altitude_marcmin(3127800, -200, 250,
                                                    0, 0, 0);
            d = ho - 3124042;
            if (d < -30 || d > 30) ok = 0;
            /* Upper limb subtracts exactly 2 SD versus lower limb. */
            int64_t ll = astro_nav_correct_altitude_marcmin(2730500, 0,
                                                    300, 55000, 15000, 1);
            int64_t ul = astro_nav_correct_altitude_marcmin(2730500, 0,
                                                    300, 55000, 15000, -1);
            if (ll - ul != 30000) ok = 0;
            if (!ok) failures++;
            printf("    Hs -> Ho chain (Moon LL / Sun LL / star, oracle"
                   " rows +- 0.03'): %s\n", ok ? "ok" : "FAIL");
        }

        /* Below-horizon and past-zenith arguments are in-domain:
         * refraction is finite and larger than at the horizon for
         * negative Ha (and clamped below -1 deg), and parallax rounds
         * half-away-from-zero symmetrically in the sign of HP cos Ha. */
        {
            int ok = 1;
            int64_t r = astro_nav_refraction_marcmin(-30000);
            if (r < 34478 || r > 60000) ok = 0;
            if (astro_nav_refraction_marcmin(-90000)
                != astro_nav_refraction_marcmin(-60000)) ok = 0;
            if (astro_nav_parallax_marcmin(6000000, 54000)
                != -astro_nav_parallax_marcmin(6000000, -54000)) ok = 0;
            if (!ok) failures++;
            printf("    below-horizon refraction / signed parallax"
                   " rounding symmetry: %s\n", ok ? "ok" : "FAIL");
        }

        /* Structure: refraction never increases with altitude, dip
         * never decreases with eye height, and the milli-arcminute
         * sine boundary agrees bit-for-bit with the centidegree one
         * on shared angles (600 marcmin = 1 cdeg). */
        {
            int ok = 1;
            int64_t prev = astro_nav_refraction_marcmin(0);
            for (int64_t ha = 10000; ha <= 5400000; ha += 10000) {
                int64_t r = astro_nav_refraction_marcmin(ha);
                if (r > prev) ok = 0;
                prev = r;
            }
            prev = 0;
            for (int32_t cm = 0; cm <= 2000; cm += 10) {
                int64_t d = astro_nav_dip_marcmin(cm);
                if (d < prev) ok = 0;
                prev = d;
            }
            for (int32_t cdeg = -9000; cdeg <= 9000; cdeg += 375) {
                if (astro_nav_sin_q30_from_marcmin((int64_t)cdeg * 600)
                    != astro_nav_sin_q30_from_cdeg(cdeg)) ok = 0;
            }
            if (!ok) failures++;
            printf("    monotone refraction/dip; marcmin sine boundary"
                   " == cdeg boundary bitwise: %s\n",
                   ok ? "ok" : "FAIL");
        }

        /* Temperature/pressure refraction. At the standard atmosphere
         * (10 C, 1010 mb) the tp functions must reproduce the standard
         * ones bit-for-bit -- the scale factor's numerator equals its
         * denominator exactly, so this is an identity, not a
         * tolerance. */
        {
            int ok = 1;
            for (int64_t ha = -90000; ha <= 5400000; ha += 10000) {
                if (astro_nav_refraction_tp_marcmin(ha, 10, 1010)
                    != astro_nav_refraction_marcmin(ha)) ok = 0;
            }
            if (astro_nav_correct_altitude_tp_marcmin(2730500, -300, 300,
                                                      55000, 15000, 1,
                                                      10, 1010)
                != astro_nav_correct_altitude_marcmin(2730500, -300, 300,
                                                      55000, 15000, 1))
                ok = 0;
            if (astro_nav_correct_altitude_tp_marcmin(1514300, 100, 200,
                                                      150, 16100, 1,
                                                      10, 1010)
                != astro_nav_correct_altitude_marcmin(1514300, 100, 200,
                                                      150, 16100, 1))
                ok = 0;
            if (!ok) failures++;
            printf("    tp refraction/chain at (10 C, 1010 mb) =="
                   " standard, bitwise: %s\n", ok ? "ok" : "FAIL");
        }

        /* Moon semidiameter augmentation, truth rows from a double
         * oracle of the exact form sd (1 - d) / d with
         * d = sqrt(1 - 2 sin(HP) sin(h) + sin^2(HP)): slightly
         * negative at h = 0 (the observer is sqrt(1 + sin^2 HP)
         * geocentric distances from the Moon there), ~0.3' near the
         * zenith. */
        {
            static const struct { int64_t h, hp, sd, want; } aug[] = {
                {       0, 60880, 16583,   -3 },
                {  600000, 54100, 14740,   39 },
                { 1800000, 56500, 15400,  126 },
                { 3448000, 60880, 16583,  251 },
                { 5400000, 61500, 16750,  305 },
                { -600000, 58000, 15800,  -48 },
            };
            int ok = 1;
            for (int i = 0; i < (int)(sizeof aug / sizeof aug[0]); i++) {
                int64_t got = astro_nav_moon_augmentation_marcmin(
                    aug[i].h, aug[i].hp, aug[i].sd);
                int64_t d = got - aug[i].want;
                if (d < -2 || d > 2) ok = 0;
            }
            /* Monotone in altitude (closer to the Moon as h rises),
             * and both clamps: past-zenith h and absurd HP pin to the
             * domain edge instead of leaving it. */
            int64_t prev = astro_nav_moon_augmentation_marcmin(
                -5400000, 60000, 16000);
            for (int64_t hm = -5300000; hm <= 5400000; hm += 100000) {
                int64_t a = astro_nav_moon_augmentation_marcmin(
                    hm, 60000, 16000);
                if (a < prev) ok = 0;
                prev = a;
            }
            if (astro_nav_moon_augmentation_marcmin(6000000, 60000, 16000)
                != astro_nav_moon_augmentation_marcmin(5400000, 60000,
                                                       16000)) ok = 0;
            if (astro_nav_moon_augmentation_marcmin(1800000, 150000, 16000)
                != astro_nav_moon_augmentation_marcmin(1800000, 120000,
                                                       16000)) ok = 0;
            if (!ok) failures++;
            printf("    Moon SD augmentation vs double oracle;"
                   " monotone in h; clamps: %s\n", ok ? "ok" : "FAIL");
        }

        /* The Moon chain against the generic one: identical at limb 0
         * (augmentation only enters through the limb step), offset by
         * exactly sd + augmentation(center altitude) with a limb, and
         * the tp variant at the standard atmosphere reproduces the
         * standard variant bit-for-bit -- the same three contracts the
         * generic pair keeps. */
        {
            int ok = 1;
            int64_t hc = astro_nav_correct_altitude_marcmin(
                2730500, -300, 300, 55000, 15000, 0);
            if (astro_nav_correct_altitude_moon_marcmin(
                    2730500, -300, 300, 55000, 15000, 0) != hc) ok = 0;
            int64_t a = astro_nav_moon_augmentation_marcmin(hc, 55000,
                                                            15000);
            int64_t ll = astro_nav_correct_altitude_moon_marcmin(
                2730500, -300, 300, 55000, 15000, 1);
            int64_t ul = astro_nav_correct_altitude_moon_marcmin(
                2730500, -300, 300, 55000, 15000, -1);
            if (ll != hc + 15000 + a) ok = 0;
            if (ll - ul != 2 * (15000 + a)) ok = 0;
            if (a < 100 || a > 200) ok = 0;   /* ~0.15' at 45 deg */
            if (astro_nav_correct_altitude_moon_tp_marcmin(
                    2730500, -300, 300, 55000, 15000, 1, 10, 1010)
                != ll) ok = 0;
            if (astro_nav_correct_altitude_moon_tp_marcmin(
                    1514300, 100, 200, 61000, 16600, -1, -40, 1030)
                != astro_nav_correct_altitude_tp_marcmin(
                       1514300, 100, 200, 61000, 16600, 0, -40, 1030)
                   - 16600
                   - astro_nav_moon_augmentation_marcmin(
                         astro_nav_correct_altitude_tp_marcmin(
                             1514300, 100, 200, 61000, 16600, 0,
                             -40, 1030), 61000, 16600)) ok = 0;
            if (!ok) failures++;
            printf("    Moon chain: limb 0 == generic; limb offset =="
                   " sd + augmentation; tp identity: %s\n",
                   ok ? "ok" : "FAIL");
        }

        /* Non-standard atmospheres against the double oracle
         * (Bennett times (P/1010)(283/(273+T))): a cold high-pressure
         * winter (-40 C, 1030 mb, factor ~1.239) and a warm
         * low-pressure summer (+35 C, 980 mb, factor ~0.892). */
        {
            static const int64_t ha_rows[] = {
                      0,   60000,  300000,  600000,
                1200000, 2700000, 4800000, 5400000,
            };
            static const int64_t cold[] = {   /* -40 C, 1030 mb */
                42705, 30135, 12242, 6678, 3349, 1232, 216, 0,
            };
            static const int64_t warm[] = {   /* +35 C,  980 mb */
                30738, 21690,  8811, 4807, 2410,  887, 156, 0,
            };
            int ok = 1;
            for (int i = 0; i < (int)(sizeof ha_rows / sizeof ha_rows[0]);
                 i++) {
                int64_t rc = astro_nav_refraction_tp_marcmin(ha_rows[i],
                                                             -40, 1030);
                int64_t rw = astro_nav_refraction_tp_marcmin(ha_rows[i],
                                                             35, 980);
                int64_t rs = astro_nav_refraction_marcmin(ha_rows[i]);
                int64_t d = rc - cold[i];
                if (d < -25 || d > 25) ok = 0;
                d = rw - warm[i];
                if (d < -25 || d > 25) ok = 0;
                /* Cold/dense raises refraction, warm/thin lowers it. */
                if (rs > 0 && !(rc > rs && rw < rs)) ok = 0;
            }
            /* Monotone in each knob at a fixed low altitude. */
            {
                int64_t prev = astro_nav_refraction_tp_marcmin(60000,
                                                               -60, 1010);
                for (int32_t t = -55; t <= 60; t += 5) {
                    int64_t r = astro_nav_refraction_tp_marcmin(60000, t,
                                                                1010);
                    if (r > prev) ok = 0;
                    prev = r;
                }
                prev = astro_nav_refraction_tp_marcmin(60000, 10, 800);
                for (int32_t p = 810; p <= 1100; p += 10) {
                    int64_t r = astro_nav_refraction_tp_marcmin(60000, 10,
                                                                p);
                    if (r < prev) ok = 0;
                    prev = r;
                }
            }
            if (!ok) failures++;
            printf("    tp refraction truth rows (-40 C/1030 mb,"
                   " +35 C/980 mb) and monotonicity: %s\n\n",
                   ok ? "ok" : "FAIL");
        }
    }

    printf("[7] Vector ephemeris (star catalog, GHA Aries, earth"
           " rotation)\n");
    {
        const int64_t ms_2026 = 836136000000LL; /* 2026-07-01 00:00 UT1 */

        /* GHA Aries truth rows from the double oracle of the same
         * model. The first two are also external anchors: the ERA
         * definition puts Aries at 280.4606 deg at the J2000 epoch,
         * and the 2000 almanac's daily page lists 99 deg 58.1' for
         * 2000-01-01 00:00 UT. */
        {
            static const struct { int64_t ms; int32_t want; } rows[] = {
                {            0LL, 28046 }, /* J2000.0 epoch          */
                {    -43200000LL,  9997 }, /* 2000-01-01 00:00 UT1   */
                { 820497600000LL, 10066 }, /* 2026-01-01 00:00 UT1   */
                { 836136000000LL, 27906 }, /* 2026-07-01 00:00 UT1   */
                {     11340000LL, 32784 }, /* GMST > 327.68 deg: used
                                            * to wrap Q16.48 negative */
            };
            int ok = 1;
            for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]);
                 i++) {
                int32_t got = astro_nav_gha_aries_cdeg(rows[i].ms);
                if (got < 0 || got >= 36000) ok = 0;
                if (astro_nav_circular_diff_cdeg(got, rows[i].want) > 1)
                    ok = 0;
                /* one sidereal day (86164.091 s) later, same sky */
                int32_t next = astro_nav_gha_aries_cdeg(rows[i].ms
                                                        + 86164091LL);
                if (astro_nav_circular_diff_cdeg(got, next) > 1) ok = 0;
            }
            if (!ok) failures++;
            printf("    GHA Aries truth rows (incl. J2000 anchor,"
                   " almanac 2000-01-01) +- 1 cdeg: %s\n",
                   ok ? "ok" : "FAIL");
        }

        /* Documented range holds around the whole turn: a full day in
         * 10-minute steps stays inside [0, 36000). The window above
         * 327.68 deg (~77 min of every day) once overflowed Q16.48 in
         * the radian-to-centidegree scaling and came back negative. */
        {
            int ok = 1;
            for (int64_t ms = 0; ms < 86400000LL; ms += 600000) {
                int32_t g = astro_nav_gha_aries_cdeg(ms);
                if (g < 0 || g >= 36000) ok = 0;
            }
            if (!ok) failures++;
            printf("    GHA Aries in [0, 36000) across a full day:"
                   " %s\n", ok ? "ok" : "FAIL");
        }

        /* Catalog invariants: every stored vector is unit-norm in
         * Q2.30, stays unit-norm through the three integer rotations,
         * and at the epoch itself (T = 0, precession angles vanish)
         * the rotation is pure earth spin: declination is preserved
         * and longitude shifts by exactly GHA Aries. */
        {
            const int64_t one = (int64_t)1 << 60;
            const int64_t tol = (int64_t)1 << 34;
            int32_t aries0 = astro_nav_gha_aries_cdeg(0);
            int ok = 1;
            for (int i = 0; i < ASTRO_NAV_STAR_COUNT; i++) {
                const astro_nav_unitvec_t *v = &astro_nav_stars[i].j2000;
                int64_t n2 = (int64_t)v->x * v->x + (int64_t)v->y * v->y
                           + (int64_t)v->z * v->z;
                if (n2 - one < -tol || n2 - one > tol) ok = 0;

                astro_nav_unitvec_t ef;
                astro_nav_celestial_to_earthfixed(v, 0, &ef);
                int64_t m2 = (int64_t)ef.x * ef.x + (int64_t)ef.y * ef.y
                           + (int64_t)ef.z * ef.z;
                if (m2 - one < -tol || m2 - one > tol) ok = 0;

                int32_t dlat, dlon, elat, elon;
                astro_nav_latlon_cdeg_from_unitvec(v, &dlat, &dlon);
                astro_nav_latlon_cdeg_from_unitvec(&ef, &elat, &elon);
                if (astro_nav_circular_diff_cdeg(elat, dlat) > 1) ok = 0;
                int32_t want_lon = dlon - aries0;
                while (want_lon < -18000) want_lon += 36000;
                while (want_lon > 18000)  want_lon -= 36000;
                if (astro_nav_circular_diff_cdeg(elon, want_lon) > 1)
                    ok = 0;
            }
            if (!ok) failures++;
            printf("    catalog unit norms; rotation preserves norm;"
                   " epoch rotation is pure spin: %s\n",
                   ok ? "ok" : "FAIL");
        }

        /* Earth-fixed truth rows: every catalog star rotated to
         * 2026-07-01 00:00 UT1, substellar point vs the double oracle
         * of the same model (Polaris row doubles as the precession
         * check: dec of date 89.37 deg, i.e. polar distance shrunk
         * from J2000's 44.2' to 37.6', matching the printed almanac's
         * 2026 Polaris pages). */
        {
            static const struct { int32_t lat, lon; } rows[] = {
                { -1675, -17748 }, /* Sirius     */
                { -5271,  17707 }, /* Canopus    */
                {  1906,  -6484 }, /* Arcturus   */
                {  3881,     39 }, /* Vega       */
                {  4603,  16060 }, /* Capella    */
                {  -817,  15989 }, /* Rigel      */
                {   516, -16389 }, /* Procyon    */
                {   741,  17009 }, /* Betelgeuse */
                { -5710,  10561 }, /* Achernar   */
                {   894,   1895 }, /* Altair     */
                {  1656,  15030 }, /* Aldebaran  */
                { -1130,  -7741 }, /* Spica      */
                { -2649,  -3130 }, /* Antares    */
                {  2796, -16232 }, /* Pollux     */
                { -2948,   6571 }, /* Fomalhaut  */
                {  4538,   3152 }, /* Deneb      */
                {  1184, -12662 }, /* Regulus    */
                {  8937,  12759 }, /* Polaris    */
            };
            int ok = 1;
            for (int i = 0; i < ASTRO_NAV_STAR_COUNT; i++) {
                astro_nav_unitvec_t ef;
                int32_t lat, lon;
                astro_nav_celestial_to_earthfixed(
                    &astro_nav_stars[i].j2000, ms_2026, &ef);
                astro_nav_latlon_cdeg_from_unitvec(&ef, &lat, &lon);
                if (astro_nav_circular_diff_cdeg(lat, rows[i].lat) > 2)
                    ok = 0;
                if (astro_nav_circular_diff_cdeg(lon, rows[i].lon) > 2)
                    ok = 0;
            }
            if (!ok) failures++;
            printf("    18 substellar points at 2026-07-01 vs double"
                   " oracle +- 2 cdeg (incl. Polaris precession): %s\n",
                   ok ? "ok" : "FAIL");
        }

        /* End to end: generate the almanac entry for Vega, take a
         * synthetic sight of it from a known position through Method
         * C, and recover that position with a two-star fix -- time,
         * catalog, and observation, no external almanac anywhere. */
        {
            astro_nav_unitvec_t obs, b1, b2;
            machine_sight_t s1, s2;
            astro_nav_fix_result_t fix;
            int ok = 1;
            astro_nav_unitvec_from_cdeg(4000, -7400, &obs);
            astro_nav_celestial_to_earthfixed(&astro_nav_stars[3].j2000,
                                              ms_2026, &b1); /* Vega */
            astro_nav_celestial_to_earthfixed(&astro_nav_stars[2].j2000,
                                              ms_2026, &b2); /* Arcturus */
            astro_nav_reduce_method_c(&obs, &b1, &s1);
            astro_nav_reduce_method_c(&obs, &b2, &s2);
            astro_nav_fix_two_body(&b1, s1.sin_hc_q30, &b2, s2.sin_hc_q30,
                                   &obs, &fix);
            if (!fix.valid) ok = 0;
            else {
                int32_t lat, lon;
                astro_nav_latlon_cdeg_from_unitvec(&fix.position,
                                                   &lat, &lon);
                if (astro_nav_circular_diff_cdeg(lat, 4000) > TOL_FIX_CDEG)
                    ok = 0;
                if (astro_nav_circular_diff_cdeg(lon, -7400) > TOL_FIX_CDEG)
                    ok = 0;
            }
            if (!ok) failures++;
            printf("    time + catalog + two sights -> fix, no external"
                   " almanac (Vega/Arcturus): %s\n\n",
                   ok ? "ok" : "FAIL");
        }

        /* Camera zenith fix: an observer whose zenith direction among
         * the stars coincides with a catalog star stands at that
         * star's substellar point -- so feeding each star's J2000
         * vector in as a plate-solved zenith must land on the truth
         * rows above (bit-identical to the almanac rotation, by
         * construction; this pins the semantics). */
        {
            static const struct { int32_t lat, lon; } rows[] = {
                { -1675, -17748 }, /* Sirius     */
                { -5271,  17707 }, /* Canopus    */
                {  1906,  -6484 }, /* Arcturus   */
                {  3881,     39 }, /* Vega       */
                {  4603,  16060 }, /* Capella    */
                {  -817,  15989 }, /* Rigel      */
                {   516, -16389 }, /* Procyon    */
                {   741,  17009 }, /* Betelgeuse */
                { -5710,  10561 }, /* Achernar   */
                {   894,   1895 }, /* Altair     */
                {  1656,  15030 }, /* Aldebaran  */
                { -1130,  -7741 }, /* Spica      */
                { -2649,  -3130 }, /* Antares    */
                {  2796, -16232 }, /* Pollux     */
                { -2948,   6571 }, /* Fomalhaut  */
                {  4538,   3152 }, /* Deneb      */
                {  1184, -12662 }, /* Regulus    */
                {  8937,  12759 }, /* Polaris    */
            };
            int ok = 1;
            for (int i = 0; i < ASTRO_NAV_STAR_COUNT; i++) {
                astro_nav_unitvec_t pos;
                int32_t lat, lon;
                astro_nav_position_from_celestial_zenith(
                    &astro_nav_stars[i].j2000, ms_2026, &pos);
                astro_nav_latlon_cdeg_from_unitvec(&pos, &lat, &lon);
                if (astro_nav_circular_diff_cdeg(lat, rows[i].lat) > 2)
                    ok = 0;
                if (astro_nav_circular_diff_cdeg(lon, rows[i].lon) > 2)
                    ok = 0;
            }
            if (!ok) failures++;
            printf("    camera zenith fix: star-as-zenith lands on its"
                   " substellar point, all 18: %s\n\n", ok ? "ok" : "FAIL");
        }
    }

    printf("[8] Sight averaging (line fit, outlier rejection)\n");
    {
        /* A perfectly linear run is recovered exactly: 5 shots a
         * minute apart, rising 3'/min (a brisk but realistic rate). */
        {
            static const int64_t ho[5] = { 1500000, 1503000, 1506000,
                                           1509000, 1512000 };
            static const int64_t t[5]  = { 0, 60000, 120000,
                                           180000, 240000 };
            astro_nav_avg_result_t r;
            int ok = 1;
            astro_nav_average_sights(ho, t, 5, 120000, 500, &r);
            if (!r.valid || r.used != 5) ok = 0;
            if (r.ho_marcmin != 1506000) ok = 0;
            if (r.rate_marcmin_per_min != 3000) ok = 0;
            if (r.max_residual_marcmin != 0) ok = 0;
            if (!ok) failures++;
            printf("    exact linear run recovered exactly (Ho, rate,"
                   " zero residual): %s\n", ok ? "ok" : "FAIL");
        }

        /* One bad shot (+5' -- a misread drum) is rejected and the
         * remaining shots, exactly linear again, fit exactly; with
         * rejection disabled the same shot stays in and shows up as
         * the scatter number. */
        {
            static const int64_t ho[5] = { 1500000, 1503000, 1511000,
                                           1509000, 1512000 };
            static const int64_t t[5]  = { 0, 60000, 120000,
                                           180000, 240000 };
            astro_nav_avg_result_t r, raw;
            int ok = 1;
            astro_nav_average_sights(ho, t, 5, 120000, 1000, &r);
            if (!r.valid || r.used != 4) ok = 0;
            if (r.ho_marcmin != 1506000) ok = 0;
            if (r.rate_marcmin_per_min != 3000) ok = 0;
            if (r.max_residual_marcmin != 0) ok = 0;
            astro_nav_average_sights(ho, t, 5, 120000, 0, &raw);
            if (!raw.valid || raw.used != 5) ok = 0;
            if (raw.max_residual_marcmin < 3000) ok = 0;
            if (!ok) failures++;
            printf("    +5' outlier: rejected worst-first, clean refit;"
                   " kept when rejection is off: %s\n", ok ? "ok" : "FAIL");
        }

        /* Shots at one identical instant average to their mean at that
         * instant, and are honestly refused at any other (no rate is
         * observable from a zero-length run). */
        {
            static const int64_t ho[3] = { 1500100, 1499900, 1500000 };
            static const int64_t t[3]  = { 60000, 60000, 60000 };
            astro_nav_avg_result_t r;
            int ok = 1;
            astro_nav_average_sights(ho, t, 3, 60000, 0, &r);
            if (!r.valid || r.ho_marcmin != 1500000) ok = 0;
            if (r.rate_marcmin_per_min != 0) ok = 0;
            astro_nav_average_sights(ho, t, 3, 60001, 0, &r);
            if (r.valid) ok = 0;
            if (!ok) failures++;
            printf("    one-instant run: mean at that instant, invalid"
                   " anywhere else: %s\n", ok ? "ok" : "FAIL");
        }

        /* Domain guards: n outside [2, 32], altitude outside +-90 deg,
         * a run spanning more than 2^40 ms. */
        {
            static const int64_t ho2[2] = { 1500000, 1503000 };
            static const int64_t t2[2]  = { 0, 60000 };
            int64_t ho_bad[2] = { 5400001, 1500000 };
            int64_t t_bad[2]  = { 0, ((int64_t)1 << 40) + 1 };
            astro_nav_avg_result_t r;
            int ok = 1;
            astro_nav_average_sights(ho2, t2, 1, 0, 0, &r);
            if (r.valid) ok = 0;
            astro_nav_average_sights(ho2, t2, 33, 0, 0, &r);
            if (r.valid) ok = 0;
            astro_nav_average_sights(ho_bad, t2, 2, 0, 0, &r);
            if (r.valid) ok = 0;
            astro_nav_average_sights(ho2, t_bad, 2, 0, 0, &r);
            if (r.valid) ok = 0;
            /* Extreme int64 timestamps: the guard subtraction itself
             * must widen, not overflow -- these must be *rejected*,
             * cleanly, under UBSan too. */
            int64_t t_extreme[2] = { INT64_MIN, INT64_MAX };
            astro_nav_average_sights(ho2, t_extreme, 2, 0, 0, &r);
            if (r.valid) ok = 0;
            astro_nav_average_sights(ho2, t2, 2, INT64_MIN, 0, &r);
            if (r.valid) ok = 0;
            /* Slope * span: each is guarded separately, so the fitted
             * line at a distant t_ref can leave +-90 deg (and int64).
             * Both must come back refused, never wrapped -- the second
             * run's exact line value at t_ref is 2^63, the historical
             * INT64_MIN print-negation trigger. */
            int64_t ho_steep[2] = { -5400000, 5400000 };
            int64_t t_steep[2]  = { 0, 1 };
            astro_nav_average_sights(ho_steep, t_steep, 2,
                                     (int64_t)1 << 40, 0, &r);
            if (r.valid) ok = 0;
            int64_t ho_wrap[2] = { -5111813, 3276800 };
            astro_nav_average_sights(ho_wrap, t_steep, 2,
                                     1099510972417LL, 0, &r);
            if (r.valid) ok = 0;
            if (!ok) failures++;
            printf("    domain guards (n, altitude range, time span)"
                   " refuse instead of answering: %s\n\n",
                   ok ? "ok" : "FAIL");
        }
    }

    printf("[9] Civil-time boundary (calendar UTC -> UT1_MS,"
           " TT_MINUS_UT1_MS)\n");
    {
        /* Pins: the epoch itself, and the repo's worked instant.
         * DUT1 = -16 ms is ILLUSTRATIVE, chosen so 2026-07-01 lands
         * on the round 69200 ms used throughout; the actual IERS
         * value for that date is UT1 - UTC = +0.014514 s (+15 ms),
         * i.e. a true TT - UT1 of 69169 ms. The pin checks the
         * arithmetic, not the policy numbers. */
        {
            int64_t ut1, ttmut1;
            int ok = 1;
            astro_nav_civil_to_times(2000, 1, 1, 12, 0, 0, 0,
                                     0, 32, &ut1, &ttmut1);
            if (ut1 != 0 || ttmut1 != 64184) ok = 0;
            astro_nav_civil_to_times(2026, 7, 1, 0, 0, 0, 0,
                                     -16, 37, &ut1, &ttmut1);
            if (ut1 != 836135999984LL || ttmut1 != 69200) ok = 0;
            if (!ok) failures++;
            printf("    J2000 epoch and the 2026-07-01/69200 worked"
                   " instant pinned: %s\n", ok ? "ok" : "FAIL");
        }

        /* Gregorian leap rule at its three corners (2000 leap, 1900
         * and 2100 not), and a leap second: 23:59:60.500 and the
         * next day's 00:00:00.500 are CONSECUTIVE instants, one SI
         * second apart -- the +1 s step lives in the policy numbers
         * (across the 2016-12-31 insertion IERS UT1 - UTC went
         * -0.4087 s -> +0.5913 s and TAI - UTC 36 -> 37). Fed each
         * instant's own DUT1 they land 1000 ms apart in UT1, and
         * TT - UT1 comes out IDENTICAL: the two +1 s steps cancel,
         * and that continuity across the leap is the invariant. */
        {
            int64_t a, b, dt;
            int ok = 1;
            astro_nav_civil_to_times(2000, 2, 29, 12, 0, 0, 0, 0, 0,
                                     &a, &dt);
            astro_nav_civil_to_times(2000, 3, 1, 12, 0, 0, 0, 0, 0,
                                     &b, &dt);
            if (b - a != 86400000LL) ok = 0;
            astro_nav_civil_to_times(1900, 2, 28, 12, 0, 0, 0, 0, 0,
                                     &a, &dt);
            astro_nav_civil_to_times(1900, 3, 1, 12, 0, 0, 0, 0, 0,
                                     &b, &dt);
            if (b - a != 86400000LL) ok = 0;
            astro_nav_civil_to_times(2100, 2, 28, 12, 0, 0, 0, 0, 0,
                                     &a, &dt);
            astro_nav_civil_to_times(2100, 3, 1, 12, 0, 0, 0, 0, 0,
                                     &b, &dt);
            if (b - a != 86400000LL) ok = 0;
            int64_t dta, dtb;
            astro_nav_civil_to_times(2016, 12, 31, 23, 59, 60, 500,
                                     -409, 36, &a, &dta);
            astro_nav_civil_to_times(2017, 1, 1, 0, 0, 0, 500,
                                     591, 37, &b, &dtb);
            if (b - a != 1000 || dta != dtb) ok = 0;
            if (!ok) failures++;
            printf("    Gregorian leap corners (2000/1900/2100) and"
                   " the 23:59:60 leap second: %s\n", ok ? "ok" : "FAIL");
        }

        /* A/B against an independent calendar algorithm: every civil
         * day 1899-01-01 .. 2101-12-31 through the library vs Howard
         * Hinnant's days_from_civil -- different decomposition of the
         * Gregorian cycle, must agree EXACTLY. */
        {
            static const int32_t mlen[12] = { 31, 28, 31, 30, 31, 30,
                                              31, 31, 30, 31, 30, 31 };
            int64_t days_checked = 0, mismatches = 0;
            for (int32_t y = 1899; y <= 2101; y++) {
                for (int32_t m = 1; m <= 12; m++) {
                    int32_t dmax = mlen[m - 1];
                    if (m == 2 && (y % 4 == 0
                                   && (y % 100 != 0 || y % 400 == 0)))
                        dmax = 29;
                    for (int32_t d = 1; d <= dmax; d++) {
                        int64_t ut1, dt;
                        astro_nav_civil_to_times(y, m, d, 12, 0, 0, 0,
                                                 0, 0, &ut1, &dt);
                        if (ut1 != (ref_days_from_civil(y, m, d)
                                    - 10957) * 86400000LL)
                            mismatches++;
                        days_checked++;
                    }
                }
            }
            if (mismatches) failures++;
            printf("    %lld civil days 1899-2101 vs independent"
                   " days_from_civil: %s\n\n", (long long)days_checked,
                   mismatches ? "FAIL" : "ok");
        }
    }

    printf("[10] Moon ephemeris (Meeus ch. 47 Example 47.a as the"
           " transcription gate)\n");
    {
        /* Meeus, Astronomical Algorithms 2nd ed., Example 47.a:
         * 1992 April 12.0 TD = JDE 2448724.5, which is 2448724.5 -
         * 2451545.0 = -2820.5 days from J2000.0, i.e. -2820.5 *
         * 86400000 = -243691200000 ms TT. The book prints lambda =
         * 133.162655 deg, beta = -3.229126 deg, Delta = 368409.7 km,
         * pi = 0.991990 deg -- every digit downstream of the full
         * 60 + 60 term tables, so reproducing them IS the
         * transcription gate for the coefficient arrays. */
        const int64_t tt_ms = -243691200000LL;

        /* Distance triple. Delta rounds to exactly 368410 km (printed
         * 368409.7). The printed parallax pi = 0.991990 deg is 0.991990
         * * 60000 = 59519.4 milli-arcmin; the double oracle's arcsine
         * of 6378.137/368409.7 gives 59519.376, and the semidiameter
         * arcsine of 1737.4/368409.7 gives 16212.286. +-2 milli-arcmin
         * absorbs the printed Delta's 0.05 km quantization and this
         * side's integer rounding. */
        {
            int32_t km, sd, hp;
            int ok = 1;
            astro_nav_moon_distance(tt_ms, 0, &km, &sd, &hp);
            if (km != 368410) ok = 0;
            if (hp < 59517 || hp > 59521) ok = 0;
            if (sd < 16210 || sd > 16214) ok = 0;
            if (!ok) failures++;
            printf("    Delta exactly 368410 km; HP/SD within +- 2"
                   " milli-arcmin of printed pi: %s\n", ok ? "ok" : "FAIL");
        }

        /* Inertial vector. Expected components derive OFFLINE from the
         * PRINTED lambda/beta -- mean equinox of date, geometric (the
         * book's apparent RA/dec add nutation; this library must not)
         * -- pushed through this library's own conventions: the Sun's
         * obliquity expression at T = -0.077221081451, then the IAU
         * 1976 T,T^2 inverse precession to J2000. That chain in double
         * precision gives (-0.68435552, 0.68938517, 0.23749863), i.e.
         * Q2.30 (-734821146, 740221695, 255012217). Tolerance: +-50
         * LSB. Sensitivity: the smallest table amplitude (107 x 1e-6
         * deg = 0.385 arcsec) is ~2000 Q2.30 LSB of arc, so a single
         * dropped, mis-signed, or mis-damped row shows up ~40x past
         * this gate at full swing; the actual integer-vs-oracle
         * residual is ~6 LSB (printed-digit quantization + CORDIC). */
        {
            astro_nav_unitvec_t v;
            int ok = 1;
            astro_nav_moon_inertial(tt_ms, &v);
            if (v.x < -734821146 - 50 || v.x > -734821146 + 50) ok = 0;
            if (v.y <  740221695 - 50 || v.y >  740221695 + 50) ok = 0;
            if (v.z <  255012217 - 50 || v.z >  255012217 + 50) ok = 0;
            if (!ok) failures++;
            printf("    J2000 vector from printed lambda/beta within"
                   " +- 50 LSB per component: %s\n\n", ok ? "ok" : "FAIL");
        }
    }

    printf("[11] Known-position prediction inverses (--predict)\n");
    {
        /* Arcsine identity: the milli-arcminute arcsine of the sine
         * of m must come back within 1 milli-arcminute across the
         * sextant range. The sweep stops short of +-90 deg, where the
         * Q2.30 sine plateaus and any plateau angle is a correct
         * answer; the stride is odd so the sweep never phase-locks to
         * the CORDIC quantization. */
        {
            int ok = 1;
            for (int64_t m = -5390000; m <= 5390000; m += 61873) {
                int64_t back = predict_asin_marcmin(
                    astro_nav_sin_q30_from_marcmin(m));
                if (back - m < -1 || back - m > 1) ok = 0;
            }
            if (!ok) failures++;
            printf("    asin(sin m) = m within +- 1 milli-arcmin,"
                   " |m| <= 5390000: %s\n", ok ? "ok" : "FAIL");
        }

        /* Chain-inversion identity: correcting the predicted sextant
         * reading must reproduce the target Ho within 1
         * milli-arcminute -- star, Sun, and Moon parameter sets, both
         * limbs, three heights of eye, standard and off-standard
         * atmospheres, from 0.5 deg to 85 deg. Every case must also
         * report a settled iteration. */
        {
            static const struct {
                int64_t hp, sd; int32_t limb; int moon;
            } bodies[] = {
                {     0,     0,  0, 0 },   /* star                */
                {   147, 15900,  1, 0 },   /* Sun lower limb      */
                {   147, 15900, -1, 0 },   /* Sun upper limb      */
                { 57000, 15600,  1, 1 },   /* Moon lower limb     */
                { 61000, 16700, -1, 1 },   /* Moon upper, perigee */
            };
            static const int64_t hcs[] =
                { 30000, 300000, 1200000, 2700000, 5100000 };
            static const int32_t eyes[] = { 0, 200, 1200 };
            static const int32_t atmos[][2] =
                { { 10, 1010 }, { -30, 1030 }, { 45, 990 } };
            int ok = 1, cases = 0;
            for (int b = 0; b < 5; b++)
             for (int h = 0; h < 5; h++)
              for (int e = 0; e < 3; e++)
               for (int a = 0; a < 3; a++) {
                int64_t err;
                int64_t hs = predict_hs_marcmin(hcs[h], eyes[e],
                                                bodies[b].hp,
                                                bodies[b].sd,
                                                bodies[b].limb,
                                                atmos[a][0],
                                                atmos[a][1],
                                                bodies[b].moon, &err);
                int64_t ho = bodies[b].moon
                    ? astro_nav_correct_altitude_moon_tp_marcmin(hs, 0,
                          eyes[e], bodies[b].hp, bodies[b].sd,
                          bodies[b].limb, atmos[a][0], atmos[a][1])
                    : astro_nav_correct_altitude_tp_marcmin(hs, 0,
                          eyes[e], bodies[b].hp, bodies[b].sd,
                          bodies[b].limb, atmos[a][0], atmos[a][1]);
                int64_t diff = ho - hcs[h];
                if (diff < -1 || diff > 1 || err > 1) ok = 0;
                cases++;
               }
            if (!ok) failures++;
            printf("    chain(predict_hs(Ho)) = Ho within +- 1"
                   " milli-arcmin, %d cases: %s\n", cases,
                   ok ? "ok" : "FAIL");
        }

        /* Index-error linearity: the chain sees hs and ie only as
         * their sum (ha = hs + ie - dip before anything else), so
         * chain(hs, ie) == chain(hs + ie, 0) EXACTLY. --predict's
         * implied aggregate correction (predicted Hs minus observed Hs)
         * rests on this identity; pin it so a chain change that
         * breaks it fails loudly. */
        {
            int ok = 1;
            static const int64_t ies[] =
                { -52000, -3000, 0, 2500, 41000 };
            for (int i = 0; i < 5; i++) {
                int64_t a = astro_nav_correct_altitude_moon_tp_marcmin(
                    2000000, ies[i], 300, 57000, 15600, 1, -5, 1020);
                int64_t b = astro_nav_correct_altitude_moon_tp_marcmin(
                    2000000 + ies[i], 0, 300, 57000, 15600, 1, -5,
                    1020);
                if (a != b) ok = 0;
                int64_t c = astro_nav_correct_altitude_tp_marcmin(
                    900000, ies[i], 550, 147, 15900, -1, 10, 1010);
                int64_t d = astro_nav_correct_altitude_tp_marcmin(
                    900000 + ies[i], 0, 550, 147, 15900, -1, 10, 1010);
                if (c != d) ok = 0;
            }
            if (!ok) failures++;
            printf("    chain(hs, ie) == chain(hs + ie, 0) exactly:"
                   " %s\n", ok ? "ok" : "FAIL");
        }

        /* The +-90 deg domain boundary, both sides. With no dip a
         * star's chain inverts essentially to the zenith; with 12 m
         * of eye height the reading a zenith-grazing altitude needs
         * lies past 90 deg, and the inversion must report the miss
         * (the CLI then refuses) instead of returning a nearest fit
         * as if it had converged. */
        {
            int64_t err_ok, err_miss;
            (void)predict_hs_marcmin(5399000, 0, 0, 0, 0,
                                     10, 1010, 0, &err_ok);
            (void)predict_hs_marcmin(5399000, 1200, 0, 0, 0,
                                     10, 1010, 0, &err_miss);
            int ok = (err_ok <= 1 && err_miss > 1);
            if (!ok) failures++;
            printf("    zenith boundary: eye 0 inverts, 12 m of dip"
                   " reports the miss: %s\n\n", ok ? "ok" : "FAIL");
        }
    }

    if (failures == 0) printf("ALL TESTS PASS\n");
    else               printf("%d TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}

/* ================================================================== */
/*  Golden determinism gate                                            */
/* ================================================================== */

/* Bit-exactness is the core claim of this repo, and the sweep above only
 * checks A-vs-B agreement within tolerance -- it would not notice both
 * paths drifting together (compiler, architecture, or libm-free math
 * change). This gate folds every output bit of a fixed input schedule
 * into one FNV-1a hash and compares it against a committed constant, so
 * any cross-platform or cross-compiler divergence is a hard failure.
 *
 * The schedule deliberately includes the degenerate geometries the sweep
 * rejects (zenith, nadir, poles, horizon) to pin edge behavior too. */

#define GOLDEN_CASES 4096
/* History: 0xe56b6546ac855970 before the Sun ephemeris was added to
 * the stream, 0xc7f055f1b2095c1a before the Sun distance/SD/HP were
 * appended, 0x63912332a1127691 before the civil-time adapter was
 * appended, 0x4d1a024ab55cfdfd before temperature/pressure refraction
 * was appended, 0x6d8e24cc0c7d19ad before the Moon ephemeris was
 * appended, 0xe2e42dff39f15cce before the Moon correction chain (SD
 * augmentation) was appended. Each extension added new fields inside
 * every case without touching the existing ones: recomputing the hash
 * with the newly added fields omitted reproduced the prior value,
 * verified at each update. */
#define GOLDEN_HASH  0x3d6ddb7b86f2c7d2ULL

static uint64_t golden_mix(uint64_t h, int64_t v)
{
    h ^= (uint64_t)v;
    h *= 0x100000001b3ULL;   /* FNV-1a 64 prime */
    return h;
}

static uint64_t golden_mix_case(uint64_t h, int32_t phi, int32_t lon,
                                int32_t gha, int32_t dec)
{
    sight_result_t a;
    square_result_t b;
    machine_sight_t c;
    sight_reduce_trig(phi, lon, gha, dec, &a);
    sight_reduce_square(phi, lon, gha, dec, &b);
    sight_reduce_machine(phi, lon, gha, dec, &c);
    h = golden_mix(h, a.hc_cdeg);
    h = golden_mix(h, a.zn_cdeg);
    h = golden_mix(h, a.zn_valid);
    h = golden_mix(h, b.sight.hc_cdeg);
    h = golden_mix(h, b.sight.zn_cdeg);
    h = golden_mix(h, b.sight.zn_valid);
    h = golden_mix(h, b.square_key);
    /* Path C machine outputs AND both human-boundary converters, so the
     * gate pins the whole surface bit-for-bit. */
    h = golden_mix(h, c.sin_hc_q30);
    h = golden_mix(h, c.square_key);
    h = golden_mix(h, c.zn_valid);
    h = golden_mix(h, astro_nav_hc_cdeg_from_sin_q30(c.sin_hc_q30));
    h = golden_mix(h, astro_nav_zn_cdeg_from_square_key(c.square_key));

    /* Two-body fix over a derived second body, plus its boundary
     * converters, so the whole fix surface is pinned bit-for-bit too.
     * Degenerate pairs in the schedule pin the valid = 0 path (the
     * result struct is fully zeroed there). */
    astro_nav_unitvec_t fobs, fb1, fb2;
    machine_sight_t c2;
    astro_nav_fix_result_t fix;
    astro_nav_unitvec_from_cdeg(phi, lon, &fobs);
    astro_nav_unitvec_from_cdeg(dec, -gha, &fb1);
    astro_nav_unitvec_from_cdeg(-(dec / 2),
                                -((gha + 11000) % CDEG_PER_TURN), &fb2);
    astro_nav_reduce_method_c(&fobs, &fb2, &c2);
    astro_nav_fix_two_body(&fb1, c.sin_hc_q30, &fb2, c2.sin_hc_q30,
                           &fobs, &fix);
    h = golden_mix(h, fix.valid);
    h = golden_mix(h, fix.position.x);
    h = golden_mix(h, fix.position.y);
    h = golden_mix(h, fix.position.z);
    h = golden_mix(h, fix.alternate.x);
    h = golden_mix(h, fix.alternate.y);
    h = golden_mix(h, fix.alternate.z);
    int32_t fix_lat, fix_lon;
    astro_nav_latlon_cdeg_from_unitvec(&fix.position, &fix_lat, &fix_lon);
    h = golden_mix(h, fix_lat);
    h = golden_mix(h, fix_lon);
    h = golden_mix(h, astro_nav_sin_q30_from_cdeg(phi));

    /* Running-fix advancement (course/run derived from the case, so
     * negative runs and the pole path are exercised) and a 3-body fix
     * over a third derived body, pinning the milestone-1 surface. */
    astro_nav_unitvec_t fadv, fb3;
    astro_nav_advance_body_for_run(&fb1, &fobs, gha, dec / 4, &fadv);
    h = golden_mix(h, fadv.x);
    h = golden_mix(h, fadv.y);
    h = golden_mix(h, fadv.z);
    machine_sight_t c3;
    astro_nav_unitvec_from_cdeg(dec / 3,
                                -((gha + 23000) % CDEG_PER_TURN), &fb3);
    astro_nav_reduce_method_c(&fobs, &fb3, &c3);
    const astro_nav_unitvec_t nb[3] = { fb1, fb2, fb3 };
    const int32_t ns[3] = { c.sin_hc_q30, c2.sin_hc_q30, c3.sin_hc_q30 };
    astro_nav_fixn_result_t fnr;
    astro_nav_fix_n_body(nb, ns, 3, &fobs, &fnr);
    h = golden_mix(h, fnr.valid);
    h = golden_mix(h, fnr.position.x);
    h = golden_mix(h, fnr.position.y);
    h = golden_mix(h, fnr.position.z);
    h = golden_mix(h, fnr.iterations);
    h = golden_mix(h, fnr.max_residual_marcmin);

    /* Altitude corrections, arguments derived from the case: an
     * apparent altitude spanning [0, 90] deg, eye heights to 12 m,
     * signed index errors, all three limbs. */
    int64_t ham = (int64_t)(phi < 0 ? -phi : phi) * 600;
    h = golden_mix(h, astro_nav_dip_marcmin(gha % 1200));
    h = golden_mix(h, astro_nav_refraction_marcmin(ham));
    h = golden_mix(h, astro_nav_parallax_marcmin(ham, 54000 + dec / 10));
    h = golden_mix(h, astro_nav_correct_altitude_marcmin(ham, lon / 100,
                       gha % 1200, 55000, 15000, dec % 3));
    h = golden_mix(h, astro_nav_sin_q30_from_marcmin(ham));
    int32_t tp_c  = (int32_t)((gha + phi + 9000) % 121) - 60;
    int32_t tp_mb = 800 + (int32_t)((dec + lon + 27000) % 301);
    h = golden_mix(h, astro_nav_refraction_tp_marcmin(ham, tp_c, tp_mb));
    h = golden_mix(h, astro_nav_correct_altitude_tp_marcmin(ham,
                       lon / 100, gha % 1200, 55000, 15000, dec % 3,
                       tp_c, tp_mb));
    h = golden_mix(h, astro_nav_moon_augmentation_marcmin(ham,
                       54000 + dec / 10, 14700 + gha % 2100));
    h = golden_mix(h, astro_nav_correct_altitude_moon_tp_marcmin(ham,
                       lon / 100, gha % 1200, 54000 + dec / 10,
                       14700 + gha % 2100, dec % 3, tp_c, tp_mb));

    /* Vector ephemeris: GHA Aries and one catalog star rotated to
     * earth-fixed, the instant derived from the case and folded into
     * +- one Julian century of J2000. */
    int64_t ms = ((int64_t)gha * 2718281829LL
                  + (int64_t)dec * 314159265358LL
                  + (int64_t)phi * 161803398874LL + lon)
                 % 3155760000000LL;
    astro_nav_unitvec_t ef;
    astro_nav_celestial_to_earthfixed(
        &astro_nav_stars[(uint32_t)(gha + dec + 9000)
                         % ASTRO_NAV_STAR_COUNT].j2000, ms, &ef);
    h = golden_mix(h, astro_nav_gha_aries_cdeg(ms));
    h = golden_mix(h, ef.x);
    h = golden_mix(h, ef.y);
    h = golden_mix(h, ef.z);

    /* Sight averaging over a 4-shot run derived from the case (slope,
     * scatter, and threshold all case-dependent, so the exact-fit,
     * rejection, and rejection-off paths are each exercised), and the
     * camera zenith fix over the case's observer vector. */
    int64_t avg_ho[4], avg_t[4];
    for (int k = 0; k < 4; k++) {
        avg_t[k]  = (int64_t)k * 60000 + phi % 997;
        avg_ho[k] = ham + (int64_t)k * (dec % 5000)
                  + (gha >> k) % 700 - 350;
        /* Clamp into the callee's +-90 deg domain: near-pole cases
         * would otherwise be refused whole (valid = 0), leaving
         * exactly the edge geometries unexercised through the fit.
         * The refusal path itself is pinned by the self-test. */
        if (avg_ho[k] >  5400000) avg_ho[k] =  5400000;
        if (avg_ho[k] < -5400000) avg_ho[k] = -5400000;
    }
    astro_nav_avg_result_t avg;
    astro_nav_average_sights(avg_ho, avg_t, 4, avg_t[1] + 30000,
                             lon % 2 ? 500 : 0, &avg);
    h = golden_mix(h, avg.valid);
    h = golden_mix(h, avg.ho_marcmin);
    h = golden_mix(h, avg.rate_marcmin_per_min);
    h = golden_mix(h, avg.max_residual_marcmin);
    h = golden_mix(h, avg.used);

    astro_nav_unitvec_t zpos;
    astro_nav_position_from_celestial_zenith(&fobs, ms, &zpos);
    h = golden_mix(h, zpos.x);
    h = golden_mix(h, zpos.y);
    h = golden_mix(h, zpos.z);
    int32_t zlat, zlon;
    astro_nav_latlon_cdeg_from_unitvec(&zpos, &zlat, &zlon);
    h = golden_mix(h, zlat);
    h = golden_mix(h, zlon);

    /* Sun ephemeris, both entry points: the inertial J2000 vector at
     * the case's instant, and the composed earth-fixed entry with a
     * case-derived TT - UT1 (spanning +- ~100 s around the present-day
     * ~69 s value, negatives included; the real quantity runs from
     * about -3 s to a forecast ~+200 s across the +-100 year domain,
     * and the API accepts +-600 s). */
    astro_nav_unitvec_t sun_in, sun_ef;
    astro_nav_sun_inertial(ms, &sun_in);
    h = golden_mix(h, sun_in.x);
    h = golden_mix(h, sun_in.y);
    h = golden_mix(h, sun_in.z);
    astro_nav_sun_earthfixed(ms, 63800 + phi * 11 + dec % 997, &sun_ef);
    h = golden_mix(h, sun_ef.x);
    h = golden_mix(h, sun_ef.y);
    h = golden_mix(h, sun_ef.z);

    /* Sun distance / semidiameter / horizontal parallax at the same
     * derived instant and TT - UT1 as the earth-fixed entry above. */
    int32_t sun_r_uau, sun_sd, sun_hp;
    astro_nav_sun_distance(ms, 63800 + phi * 11 + dec % 997,
                           &sun_r_uau, &sun_sd, &sun_hp);
    h = golden_mix(h, sun_r_uau);
    h = golden_mix(h, sun_sd);
    h = golden_mix(h, sun_hp);

    /* Civil-time boundary adapter over a case-derived calendar
     * timestamp: full field ranges including second = 60 (the leap
     * second) and negative DUT1. */
    int64_t civ_ut1, civ_ttmut1;
    astro_nav_civil_to_times(1900 + (gha + dec + 9000) % 200,
                             1 + (phi + 9000) % 12,
                             1 + (lon + 18000) % 28,
                             (phi + 9000) % 24,
                             (lon + 18000) % 60,
                             (dec + 9000) % 61,
                             gha % 1000,
                             dec % 900,
                             (phi + 9000) % 100,
                             &civ_ut1, &civ_ttmut1);
    h = golden_mix(h, civ_ut1);
    h = golden_mix(h, civ_ttmut1);

    /* Moon ephemeris, all three entry points, at the same derived
     * instant and the same case-derived TT - UT1 the Sun block uses --
     * for the Moon that offset moves the answer by up to ~1', so the
     * hash pins the two-timescale plumbing, not just the series. */
    astro_nav_unitvec_t moon_in, moon_ef;
    astro_nav_moon_inertial(ms, &moon_in);
    h = golden_mix(h, moon_in.x);
    h = golden_mix(h, moon_in.y);
    h = golden_mix(h, moon_in.z);
    astro_nav_moon_earthfixed(ms, 63800 + phi * 11 + dec % 997, &moon_ef);
    h = golden_mix(h, moon_ef.x);
    h = golden_mix(h, moon_ef.y);
    h = golden_mix(h, moon_ef.z);
    int32_t moon_km, moon_sd, moon_hp;
    astro_nav_moon_distance(ms, 63800 + phi * 11 + dec % 997,
                            &moon_km, &moon_sd, &moon_hp);
    h = golden_mix(h, moon_km);
    h = golden_mix(h, moon_sd);
    h = golden_mix(h, moon_hp);
    return h;
}

static int run_golden(void)
{
    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a 64 offset basis */

    /* Degenerate/edge geometries first (excluded from the sweep). */
    static const int32_t edges[][4] = {
        {  4500,     0,     0,  4500 },   /* exact zenith            */
        {  4500,     0, 18000, -4500 },   /* exact nadir             */
        {  9000,     0,     0,  4500 },   /* north pole              */
        { -9000,     0,     0, -4500 },   /* south pole              */
        {     0,     0,  9000,     0 },   /* equator, body due west  */
        {     0,     0, 27000,     0 },   /* equator, body due east  */
        {  8999,     0,     1,  8998 },   /* near pole + near zenith */
        {     0, 18000, 35999, -9000 },   /* domain corners          */
        {     0,-18000,     0,  9000 },
    };
    for (int i = 0; i < (int)(sizeof edges / sizeof edges[0]); i++)
        h = golden_mix_case(h, edges[i][0], edges[i][1],
                            edges[i][2], edges[i][3]);

    /* Full-domain LCG schedule, fixed seed, no rejection filter. */
    lcg_state = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < GOLDEN_CASES; i++) {
        int32_t phi = lcg_range(-9000, 9001);
        int32_t dec = lcg_range(-9000, 9001);
        int32_t lon = lcg_range(-18000, 18001);
        int32_t gha = lcg_range(0, CDEG_PER_TURN);
        h = golden_mix_case(h, phi, lon, gha, dec);
    }

    printf("golden determinism hash: 0x%016llx (%d cases + %d edges)\n",
           (unsigned long long)h, GOLDEN_CASES,
           (int)(sizeof edges / sizeof edges[0]));
    if (h != GOLDEN_HASH) {
        printf("FAIL: expected 0x%016llx -- output bits diverged from the\n"
               "committed golden value (platform/compiler nondeterminism or\n"
               "an intentional math change; if intentional, update GOLDEN_HASH)\n",
               (unsigned long long)GOLDEN_HASH);
        return 1;
    }
    printf("bit-exact against committed golden value: PASS\n");
    return 0;
}

/* ================================================================== */
/*  --fuzz-w128: differential fuzz of the fp_w128 op layer             */
/* ================================================================== */

/* One FNV-1a hash over every fp_w128 operation applied to a
 * deterministic, edge-biased operand schedule. The gate is
 * differential, not golden: `make check-backend` builds this binary
 * against BOTH backends (native __int128 and the two-limb portable
 * struct), runs both under UBSan, and compares the printed lines byte
 * for byte. Equal hashes prove the backends agree bit for bit on
 * modulo-2^128 wrap, the whole [0, 127] shift domain, every sign
 * combination through the division routines, and the -2^127
 * boundaries the contract in fp_math.h calls out; the UBSan wrapper
 * proves every exercised operation is defined on both sides. No hash
 * constant is committed on purpose -- backend agreement is the
 * contract, not any particular value. */

static const uint64_t fuzz_limbs[] = {
    0, 1, 2, 3,
    0x7fffffffffffffffULL,            /* INT64_MAX / sign seam        */
    0x8000000000000000ULL,            /* INT64_MIN bit pattern        */
    0x8000000000000001ULL,
    0xffffffffffffffffULL,            /* all-ones: carry propagation  */
    0xfffffffffffffffeULL,
    0x00000000ffffffffULL,            /* 32-bit halves: the partial-  */
    0x0000000100000000ULL,            /*   product seams in the       */
    0x00000001000000ffULL,            /*   portable 64x64 multiply    */
    0x5555555555555555ULL,
    0xaaaaaaaaaaaaaaaaULL,
    0x0123456789abcdefULL,
    0xfedcba9876543210ULL,
    (uint64_t)1 << 47,                /* FP_PRECISION-related seams   */
    ((uint64_t)1 << 48) - 1,
    (uint64_t)1 << 48,
};
#define FUZZ_NLIMBS ((int)(sizeof fuzz_limbs / sizeof fuzz_limbs[0]))

/* Edge-biased 64-bit limb: half the draws from the seam table, half
 * raw LCG output. */
static uint64_t fuzz_limb(void)
{
    uint64_t r = lcg_next();
    if (r & 1)
        return fuzz_limbs[(r >> 8) % FUZZ_NLIMBS];
    return lcg_next();
}

/* Assemble hi:lo through the op layer itself, so arbitrary 128-bit
 * patterns are built the same way on either backend. */
static fp_w128 fuzz_w128(void)
{
    uint64_t hi = fuzz_limb();
    uint64_t lo = fuzz_limb();
    return fp_w_add(fp_w_shl(fp_w_from_u64(hi), 64), fp_w_from_u64(lo));
}

/* Hash both limbs of a 128-bit value. */
static uint64_t fuzz_mix_w(uint64_t h, fp_w128 v)
{
    h = golden_mix(h, fp_w_to_i64(v));
    h = golden_mix(h, fp_w_to_i64(fp_w_asr(v, 64)));
    return h;
}

static int run_fuzz_w128(int32_t iters)
{
    /* Every shift-behavior boundary: word seams (63/64/65), the
     * portable branch points (31/32/33, 95/96/97), FP_PRECISION
     * (47/48/49), and the domain edge (126/127). */
    static const int shifts[] = { 0, 1, 2, 31, 32, 33, 47, 48, 49,
                                  63, 64, 65, 95, 96, 97, 126, 127 };
    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a 64 offset basis */
    const fp_w128 zero = fp_w_from_i64(0);
    const fp_w128 min128 = fp_w_shl(fp_w_from_u64(1), 127);  /* -2^127 */

    lcg_state = 0x9E3779B97F4A7C15ULL;   /* own seed, decoupled from
                                          * the --golden schedule     */

    /* Fixed boundary block first: exactly the cases the fp_math.h
     * contract calls out. */
    h = fuzz_mix_w(h, fp_w_divs(min128, fp_w_from_i64(-1)));  /* wraps */
    h = fuzz_mix_w(h, fp_w_neg(min128));                      /* wraps */
    h = fuzz_mix_w(h, fp_w_add(min128, min128));              /* wraps */
    h = fuzz_mix_w(h, fp_w_mul(min128, min128));              /* wraps */
    h = fuzz_mix_w(h, fp_w_sub(zero, min128));                /* wraps */
    h = fuzz_mix_w(h, fp_w_divs_pow2(min128, 127));           /* -1    */
    h = fuzz_mix_w(h, fp_w_shl(min128, 127));                 /* 0     */
    h = fuzz_mix_w(h, fp_w_asr(min128, 127));                 /* -1    */
    h = fuzz_mix_w(h, fp_w_muls(INT64_MIN, INT64_MIN));
    h = fuzz_mix_w(h, fp_w_muls(INT64_MIN, -1));
    h = golden_mix(h, fp_w_cmp(min128, zero));
    h = golden_mix(h, fp_w_to_i64(min128));

    for (int32_t i = 0; i < iters; i++) {
        fp_w128 a = fuzz_w128();
        fp_w128 b = fuzz_w128();
        int64_t sa = (int64_t)fuzz_limb();
        int64_t sb = (int64_t)fuzz_limb();
        int ks = shifts[lcg_next() % (sizeof shifts / sizeof shifts[0])];
        int kr = (int)(lcg_next() % 128);   /* full [0, 127] domain */

        h = fuzz_mix_w(h, fp_w_add(a, b));
        h = fuzz_mix_w(h, fp_w_sub(a, b));
        h = fuzz_mix_w(h, fp_w_neg(a));
        h = fuzz_mix_w(h, fp_w_mul(a, b));
        h = fuzz_mix_w(h, fp_w_muls(sa, sb));
        h = fuzz_mix_w(h, fp_w_shl(a, ks));
        h = fuzz_mix_w(h, fp_w_asr(a, ks));
        h = fuzz_mix_w(h, fp_w_shl(b, kr));
        h = fuzz_mix_w(h, fp_w_asr(b, kr));
        h = fuzz_mix_w(h, fp_w_divs_pow2(a, ks));
        h = golden_mix(h, fp_w_cmp(a, b));
        h = golden_mix(h, fp_w_to_i64(a));
        /* fp_w_bits is documented for non-negative values, but both
         * backends compute the same limb scan on any input -- hash
         * the agreement on the whole domain. */
        h = golden_mix(h, fp_w_bits(a));
        if (fp_w_cmp(b, zero) != 0)
            h = fuzz_mix_w(h, fp_w_divs(a, b));
        /* Small-magnitude divisor: quotients with many significant
         * bits, every sign combination. (sb|1 is odd, and 1000 is
         * even, so the remainder is never 0.) */
        h = fuzz_mix_w(h, fp_w_divs(a, fp_w_from_i64((sb | 1) % 1000)));

        /* Identities that hold modulo 2^128 on ANY input. */
        if (fp_w_cmp(fp_w_add(a, fp_w_neg(a)), zero) != 0
            || fp_w_cmp(fp_w_sub(a, b),
                        fp_w_add(a, fp_w_neg(b))) != 0
            || fp_w_to_i64(fp_w_from_i64(sa)) != sa) {
            printf("fp_w128 fuzz: FAIL (identity broke at iteration"
                   " %ld)\n", (long)i);
            return 1;
        }
    }

    printf("fp_w128 fuzz: %ld iterations, hash 0x%016llx\n",
           (long)iters, (unsigned long long)h);
    return 0;
}

/* ================================================================== */
/*  Command-line reduction (centidegree input; still integer-only)     */
/* ================================================================== */

static int parse_i32(const char *text, int32_t *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0'
        || value < INT32_MIN || value > INT32_MAX)
        return 0;
    *out = (int32_t)value;
    return 1;
}

static int parse_i64(const char *text, int64_t *out)
{
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return 0;
    *out = (int64_t)value;
    return 1;
}

static void print_usage(const char *program)
{
    printf("usage:\n");
    printf("  %s                         run self-tests\n", program);
    printf("  %s --self-test             run self-tests\n", program);
    printf("  %s --reduce LAT LON GHA DEC [HO]\n", program);
    printf("  %s --reduce-star LAT LON STAR UT1_MS [HO]\n", program);
    printf("  %s --fix DRLAT DRLON GHA1 DEC1 HO1 GHA2 DEC2 HO2\n", program);
    printf("  %s --fix-stars DRLAT DRLON STAR1 UT1_1 HO1 STAR2 UT1_2 HO2\n", program);
    printf("  %s --correct HS IE EYE_CM HP SD LIMB"
           " [TEMP_C PRESSURE_MB]\n", program);
    printf("  %s --correct-sun HS IE EYE_CM UT1_MS TT_MINUS_UT1_MS"
           " LIMB [TEMP_C PRESSURE_MB]\n", program);
    printf("  %s --correct-moon HS IE EYE_CM UT1_MS TT_MINUS_UT1_MS"
           " LIMB [TEMP_C PRESSURE_MB]\n", program);
    printf("  %s --predict LAT LON BODY UT1_MS EYE_CM LIMB [HS]"
           " [TEMP_C PRESSURE_MB]\n", program);
    printf("  %s --average TREF_MS REJECT HO T [HO T ...]\n", program);
    printf("  %s --time YEAR MONTH DAY HOUR MIN SEC MS DUT1_MS"
           " TAI_MINUS_UTC_S\n", program);
    printf("  %s --star INDEX UT1_MS     almanac entry from the catalog\n", program);
    printf("  %s --sun UT1_MS TT_MINUS_UT1_MS   Sun almanac entry\n", program);
    printf("  %s --moon UT1_MS TT_MINUS_UT1_MS  Moon almanac entry\n", program);
    printf("  %s --reduce-sun LAT LON UT1_MS TT_MINUS_UT1_MS [HO]\n", program);
    printf("  %s --reduce-moon LAT LON UT1_MS TT_MINUS_UT1_MS [HO]\n", program);
    printf("  %s --fix-sun DRLAT DRLON TT_MINUS_UT1_MS UT1_1 HO1 UT1_2 HO2\n", program);
    printf("  %s --fix-n DRLAT DRLON BODY UT1_MS HO [BODY UT1_MS HO ...]\n", program);
    printf("  %s --running-fix DRLAT DRLON COURSE SPEED BODY UT1_MS HO [...]\n", program);
    printf("  %s --zenith X Y Z UT1_MS   camera fix from a zenith vector\n", program);
    printf("  %s --golden                bit-exact determinism gate\n", program);
    printf("  %s --fuzz-w128 [N]         128-bit backend differential fuzz\n", program);
    printf("  %s --ephemeris-check       almanac vs independent truth rows\n", program);
    printf("  %s --external-check        almanac + corrections vs printed almanacs\n", program);
    printf("  %s --cross-check           almanac + geometry vs independent implementations\n", program);
    printf("  %s --scenario-check        end-to-end position recovery vs external answers\n\n", program);
#ifdef ASTRO_NAV_NATIVE_REFERENCE
    printf("  %s --native-reference      validate A/B against native double\n", program);
    printf("  %s --benchmark             benchmark A/B against native double\n\n", program);
#endif
    printf("All angles are integer centidegrees (100 = 1 degree), except\n");
    printf("sextant altitudes, which use milli-arcminutes (1000 = 1'); a\n");
    printf("typical 0.1' drum reading is 100 units. --correct HS/IE (with\n");
    printf("height of eye in cm, HP horizontal parallax, SD semidiameter,\n");
    printf("LIMB 1=lower -1=upper 0=center), --average HO shots (with\n");
    printf("T/TREF_MS in ms, REJECT the outlier threshold in\n");
    printf("milli-arcmin, 0 = off), --reduce HO, and --fix HO1/HO2, so\n");
    printf("the corrected Ho feeds the intercept and the fix with no\n");
    printf("centidegree rounding. --correct, --correct-sun, and\n");
    printf("--correct-moon take an optional TEMP_C (deg C, -60..60) and\n");
    printf("PRESSURE_MB (millibars, 800..1100) pair that rescales\n");
    printf("refraction from the 10 C / 1010 mb standard atmosphere;\n");
    printf("omitted means standard.\n");
    printf("--zenith X Y Z is a J2000 celestial-frame zenith direction\n");
    printf("as a Q2.30 unit vector (plate-solved star photo + gravity).\n");
    printf("--star INDEX is 0..%d, UT1_MS is UT1 milliseconds since the\n",
           ASTRO_NAV_STAR_COUNT - 1);
    printf("J2000.0 epoch (2000-01-01 12:00:00 UT1), +- 100 years.\n");
    printf("--fix-stars is --fix fed from timestamps: STAR catalog index\n");
    printf("plus UT1 instant per sight, full-precision body vectors, no\n");
    printf("centidegree almanac rounding (the Bris timed-crossing fix).\n");
    printf("--reduce-star is the one-sight version (Method C only); with\n");
    printf("HO it also prints the raw Q2.30 sine residual.\n");
    printf("Sun and Moon modes take TT - UT1 explicitly (= 32.184 s +\n");
    printf("leap seconds - DUT1; ~69200 ms in 2026; 0 costs at most\n");
    printf("~0.05' for the Sun but ~0.6' for the Moon): dynamics run on\n");
    printf("TT, earth rotation on UT1. The Moon's horizontal parallax\n");
    printf("is 54-61' -- the largest correction in celestial navigation;\n");
    printf("an uncorrected Moon sight is wrong by up to a degree, so\n");
    printf("feed --reduce-moon and the fixes an HO that went through\n");
    printf("--correct-moon (whose limb step also augments SD by up to\n");
    printf("~0.3': the observer stands closer to the Moon than the\n");
    printf("geocenter the tabulated SD assumes). --fix-sun is the\n");
    printf("two-Sun-crossing fix: the same body at two instants.\n");
    printf("--fix-n is the overdetermined fix: 2..%d timed sights from\n",
           FIX_N_MAX_SIGHTS);
    printf("ONE position, least squares; BODY is a star index,\n");
    printf("sun:TT_MINUS_UT1_MS, or moon:TT_MINUS_UT1_MS, so stars, the\n");
    printf("Sun, and the Moon mix freely.\n");
    printf("--running-fix is --fix-n underway: COURSE (centidegrees\n");
    printf("true) and SPEED (tenths of a knot) advance every sight to\n");
    printf("the last one's instant; DRLAT/DRLON is the DR at that\n");
    printf("instant, and so is the fix.\n");
    printf("--predict is the sight run backward from a KNOWN position\n");
    printf("(GPS): where the body stands (Zn) and the Hs modeled at IE=0\n");
    printf("for an unobstructed natural sea horizon. BODY uses --fix-n\n");
    printf("syntax; stars require LIMB 0. With an actual reading HS,\n");
    printf("predicted minus observed is the aggregate correction in\n");
    printf("--correct's IE convention. It also contains observation,\n");
    printf("horizon, atmosphere, and model errors. Treat only a stable\n");
    printf("component across controlled repeated shots as estimated IE;\n");
    printf("artificial and land horizons are outside this model.\n");
    printf("LAT/DEC: north positive; LON: east positive; GHA: [0, 36000).\n");
    printf("Example: %s --reduce 4000 -7400 6000 2000 4012800\n", program);
}

static int cli_range_error(const char *name, int32_t value,
                           int32_t minimum, int32_t maximum)
{
    if (value >= minimum && value <= maximum) return 0;
    fprintf(stderr, "error: %s=%ld outside [%ld, %ld]\n",
            name, (long)value, (long)minimum, (long)maximum);
    return 1;
}

static int run_reduction_cli(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "error: --reduce expects LAT LON GHA DEC and optional HO\n");
        print_usage(argv[0]);
        return 2;
    }

    const char *names[] = { "LAT", "LON", "GHA", "DEC", "HO" };
    int32_t values[5] = {0, 0, 0, 0, 0};
    int count = argc - 2;
    for (int i = 0; i < count; i++) {
        if (!parse_i32(argv[i + 2], &values[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32 value\n",
                    names[i]);
            return 2;
        }
    }

    int bad = 0;
    bad |= cli_range_error("LAT", values[0], -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("LON", values[1], -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    bad |= cli_range_error("GHA", values[2], 0, CDEG_PER_TURN - 1);
    bad |= cli_range_error("DEC", values[3], -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    if (count == 5)
        bad |= cli_range_error("HO", values[4], -5400000, 5400000);
    if (bad) return 2;

    sight_result_t result;
    sight_reduce_trig(values[0], values[1], values[2], values[3], &result);
    square_result_t square;
    sight_reduce_square(values[0], values[1], values[2], values[3], &square);
    machine_sight_t machine;
    sight_reduce_machine(values[0], values[1], values[2], values[3], &machine);

    printf("Hc(A): "); print_cdeg(result.hc_cdeg); printf(" deg\n");
    printf("Hc(B): "); print_cdeg(square.sight.hc_cdeg); printf(" deg\n");
    printf("Hc(C): ");
    print_cdeg(astro_nav_hc_cdeg_from_sin_q30(machine.sin_hc_q30));
    printf(" deg   (machine sin_hc=%ld/2^30)\n", (long)machine.sin_hc_q30);
    printf("Zn(A): ");
    if (result.zn_valid) {
        print_cdeg(result.zn_cdeg);
        printf(" deg true\n");
    } else {
        printf("undefined (body at zenith/nadir)\n");
    }
    printf("Zn(B): ");
    if (square.sight.zn_valid) {
        print_cdeg(square.sight.zn_cdeg);
        printf(" deg true   square-key=%u/65536\n", (unsigned)square.square_key);
    } else {
        printf("undefined (body at zenith/nadir)\n");
    }
    printf("Zn(C): ");
    if (machine.zn_valid) {
        print_cdeg(astro_nav_zn_cdeg_from_square_key(machine.square_key));
        printf(" deg true   square-key=%u/65536\n",
               (unsigned)machine.square_key);
    } else {
        printf("undefined (body at zenith/nadir, or observer at a pole)\n");
    }

    int32_t path_diff = result.hc_cdeg - square.sight.hc_cdeg;
    if (path_diff < 0) path_diff = -path_diff;
    printf("A/B altitude difference: ");
    print_marcmin(astro_nav_cdeg_to_marcmin(path_diff));
    printf("\n");

    if (count == 5) {
        /* HO is in milli-arcminutes (the unit --correct emits), and one
         * milli-arcminute of altitude is one milli-nautical-mile of
         * intercept, so Ho - Hc is exact in milli-nm; only the display
         * rounds, to the tenth of a mile a plot can use. */
        int64_t milli_nm = (int64_t)values[4]
                           - astro_nav_cdeg_to_marcmin(result.hc_cdeg);
        int toward = milli_nm >= 0;
        int64_t magnitude = toward ? milli_nm : -milli_nm;
        int64_t tenths_nm = (magnitude + 50) / 100;
        printf("Intercept: %lld.%lld nm %s\n",
               (long long)(tenths_nm / 10), (long long)(tenths_nm % 10),
               toward ? "TOWARD" : "AWAY");
    }

    return 0;
}

/* Two corrected sights straight to a position: no assumed position per
 * sight, no intercepts, no plotting. The DR argument only breaks the
 * two-point ambiguity. HO is taken in milli-arcminutes -- the same
 * unit --correct emits -- so the corrected sextant altitude feeds the
 * fix at full sextant resolution, with no centidegree rounding in
 * between. */
static int run_fix_cli(int argc, char **argv)
{
    if (argc != 10) {
        fprintf(stderr, "error: --fix expects DRLAT DRLON GHA1 DEC1 HO1"
                        " GHA2 DEC2 HO2\n");
        print_usage(argv[0]);
        return 2;
    }

    const char *names[] = { "DRLAT", "DRLON", "GHA1", "DEC1", "HO1",
                            "GHA2", "DEC2", "HO2" };
    int32_t v[8];
    for (int i = 0; i < 8; i++) {
        if (!parse_i32(argv[i + 2], &v[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32 value\n",
                    names[i]);
            return 2;
        }
    }

    int bad = 0;
    bad |= cli_range_error("DRLAT", v[0], -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("DRLON", v[1], -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    for (int body = 0; body < 2; body++) {
        bad |= cli_range_error(names[2 + 3 * body], v[2 + 3 * body],
                               0, CDEG_PER_TURN - 1);
        bad |= cli_range_error(names[3 + 3 * body], v[3 + 3 * body],
                               -CDEG_PER_QUARTER, CDEG_PER_QUARTER);
        bad |= cli_range_error(names[4 + 3 * body], v[4 + 3 * body],
                               -5400000, 5400000);
    }
    if (bad) return 2;

    astro_nav_unitvec_t b1, b2, hint;
    astro_nav_unitvec_from_cdeg(v[3], -v[2], &b1);
    astro_nav_unitvec_from_cdeg(v[6], -v[5], &b2);
    astro_nav_unitvec_from_cdeg(v[0], v[1], &hint);

    astro_nav_fix_result_t fix;
    astro_nav_fix_two_body(&b1, astro_nav_sin_q30_from_marcmin(v[4]),
                           &b2, astro_nav_sin_q30_from_marcmin(v[7]),
                           &hint, &fix);
    if (!fix.valid) {
        printf("no fix: the two circles of equal altitude are degenerate"
               " (bodies aligned) or do not intersect\n");
        return 1;
    }

    int32_t plat, plon, alat, alon;
    astro_nav_latlon_cdeg_from_unitvec(&fix.position, &plat, &plon);
    astro_nav_latlon_cdeg_from_unitvec(&fix.alternate, &alat, &alon);
    printf("fix:       lat "); print_cdeg(plat);
    printf(" deg   lon "); print_cdeg(plon); printf(" deg\n");
    printf("alternate: lat "); print_cdeg(alat);
    printf(" deg   lon "); print_cdeg(alon);
    printf(" deg   (other circle intersection)\n");
    return 0;
}

/* Timed two-star fix: the full machine chain from timestamps. Each
 * sight is a catalog star INDEX plus the UT1 instant, so the body
 * vector comes from astro_nav_celestial_to_earthfixed() at full Q2.30
 * precision -- no centidegree almanac quantization anywhere, unlike
 * --fix fed from --star's printed GHA/dec. This is also the
 * Bris-sextant shape of a fix (docs/BRIS.md): with a fixed-angle
 * instrument both HO arguments are the same calibrated constant and
 * the only measured inputs are the two times. */
static int run_fix_stars_cli(int argc, char **argv)
{
    if (argc != 10) {
        fprintf(stderr, "error: --fix-stars expects DRLAT DRLON STAR1"
                        " UT1_1 HO1 STAR2 UT1_2 HO2\n");
        print_usage(argv[0]);
        return 2;
    }

    int32_t drlat, drlon;
    if (!parse_i32(argv[2], &drlat) || !parse_i32(argv[3], &drlon)) {
        fprintf(stderr, "error: DRLAT/DRLON must be base-10 int32"
                        " values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("DRLAT", drlat, -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("DRLON", drlon, -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    if (bad) return 2;

    astro_nav_unitvec_t body[2];
    int32_t sin_ho[2];
    for (int i = 0; i < 2; i++) {
        const char *sname = i ? "STAR2" : "STAR1";
        const char *tname = i ? "UT1_2" : "UT1_1";
        const char *hname = i ? "HO2" : "HO1";
        int32_t index;
        int64_t ms, ho;
        if (!parse_i32(argv[4 + 3 * i], &index) || index < 0
            || index >= ASTRO_NAV_STAR_COUNT) {
            fprintf(stderr, "error: %s must be 0..%d\n", sname,
                    ASTRO_NAV_STAR_COUNT - 1);
            return 2;
        }
        if (!parse_i64(argv[5 + 3 * i], &ms)
            || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
            || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
            fprintf(stderr, "error: %s must be within +- 3155760000000"
                            " (100 years of J2000)\n", tname);
            return 2;
        }
        if (!parse_i64(argv[6 + 3 * i], &ho)
            || ho < -5400000 || ho > 5400000) {
            fprintf(stderr, "error: %s must be milli-arcminutes in"
                            " [-5400000, 5400000]\n", hname);
            return 2;
        }
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[index].j2000,
                                          ms, &body[i]);
        sin_ho[i] = astro_nav_sin_q30_from_marcmin(ho);
        printf("%-10s at UT1 J2000 %+lld ms   Ho = %lld milli-arcmin\n",
               astro_nav_stars[index].name, (long long)ms,
               (long long)ho);
    }

    astro_nav_unitvec_t hint;
    astro_nav_unitvec_from_cdeg(drlat, drlon, &hint);

    astro_nav_fix_result_t fix;
    astro_nav_fix_two_body(&body[0], sin_ho[0], &body[1], sin_ho[1],
                           &hint, &fix);
    if (!fix.valid) {
        printf("no fix: the two circles of equal altitude are degenerate"
               " (bodies aligned) or do not intersect\n");
        return 1;
    }

    int32_t plat, plon, alat, alon;
    astro_nav_latlon_cdeg_from_unitvec(&fix.position, &plat, &plon);
    astro_nav_latlon_cdeg_from_unitvec(&fix.alternate, &alat, &alon);
    printf("fix:       lat "); print_cdeg(plat);
    printf(" deg   lon "); print_cdeg(plon); printf(" deg\n");
    printf("alternate: lat "); print_cdeg(alat);
    printf(" deg   lon "); print_cdeg(alon);
    printf(" deg   (other circle intersection)\n");
    return 0;
}

/* One timed sight, machine-native end to end: catalog STAR index plus
 * UT1 instant -> full-precision body vector -> Method C. The
 * single-sight companion of --fix-stars, and the probe that makes
 * Bris crossing times reproducible without going through --star's
 * centidegree printout (docs/BRIS.md): with HO given, the printed
 * sine residual is the raw Q2.30 distance from the crossing. */
static int run_reduce_star_cli(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "error: --reduce-star expects LAT LON STAR"
                        " UT1_MS and optional HO\n");
        print_usage(argv[0]);
        return 2;
    }

    int32_t lat, lon, index;
    int64_t ms, ho = 0;
    if (!parse_i32(argv[2], &lat) || !parse_i32(argv[3], &lon)) {
        fprintf(stderr, "error: LAT/LON must be base-10 int32 values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("LAT", lat, -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("LON", lon, -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    if (bad) return 2;
    if (!parse_i32(argv[4], &index) || index < 0
        || index >= ASTRO_NAV_STAR_COUNT) {
        fprintf(stderr, "error: STAR must be 0..%d\n",
                ASTRO_NAV_STAR_COUNT - 1);
        return 2;
    }
    if (!parse_i64(argv[5], &ms)
        || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
        || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: UT1_MS must be within +- 3155760000000"
                        " (100 years of J2000)\n");
        return 2;
    }
    if (argc == 7) {
        if (!parse_i64(argv[6], &ho) || ho < -5400000 || ho > 5400000) {
            fprintf(stderr, "error: HO must be milli-arcminutes in"
                            " [-5400000, 5400000]\n");
            return 2;
        }
    }

    astro_nav_unitvec_t body, observer;
    astro_nav_celestial_to_earthfixed(&astro_nav_stars[index].j2000, ms,
                                      &body);
    astro_nav_unitvec_from_cdeg(lat, lon, &observer);

    astro_nav_machine_sight_t sight;
    astro_nav_reduce_method_c(&observer, &body, &sight);

    printf("%-10s at UT1 J2000 %+lld ms\n", astro_nav_stars[index].name,
           (long long)ms);
    int32_t hc_cdeg = astro_nav_hc_cdeg_from_sin_q30(sight.sin_hc_q30);
    printf("Hc(C): "); print_cdeg(hc_cdeg);
    printf(" deg   (machine sin_hc=%ld/2^30)\n", (long)sight.sin_hc_q30);
    printf("Zn(C): ");
    if (sight.zn_valid) {
        print_cdeg(astro_nav_zn_cdeg_from_square_key(sight.square_key));
        printf(" deg true   square-key=%u/65536\n",
               (unsigned)sight.square_key);
    } else {
        printf("undefined (body at zenith/nadir, or observer at a"
               " pole)\n");
    }

    if (argc == 7) {
        /* The raw machine comparison, before any display rounding:
         * one Q2.30 unit near Hc = 30 deg is ~0.0002 arcsec of
         * altitude. */
        printf("sine residual: %+lld/2^30 (machine sin_hc - sin Ho)\n",
               (long long)sight.sin_hc_q30
               - astro_nav_sin_q30_from_marcmin(ho));
        int64_t milli_nm = ho - astro_nav_cdeg_to_marcmin(hc_cdeg);
        int toward = milli_nm >= 0;
        int64_t magnitude = toward ? milli_nm : -milli_nm;
        int64_t tenths_nm = (magnitude + 50) / 100;
        printf("Intercept: %lld.%lld nm %s\n",
               (long long)(tenths_nm / 10), (long long)(tenths_nm % 10),
               toward ? "TOWARD" : "AWAY");
    }
    return 0;
}

/* Shared UT1 / TT - UT1 parsing for the Sun and Moon surfaces.
 * TT - UT1 is bounded to +- 10 minutes: about -3 s in 1900, ~69 s in
 * the mid-2020s, forecast toward roughly +200 s by 2100, so +-600 s
 * covers the +-100 year domain with room to spare. The cost of
 * passing 0 for TT - UT1 differs ~13x between the two bodies, so
 * each caller states its own in the refusal text. */
static int parse_body_times(const char *ut1_text, const char *ttmut1_text,
                            const char *zero_cost_note,
                            int64_t *ut1_ms, int64_t *ttmut1_ms)
{
    if (!parse_i64(ut1_text, ut1_ms)
        || *ut1_ms < -ASTRO_NAV_TIME_ABS_MAX_MS
        || *ut1_ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: UT1_MS must be within +- 3155760000000"
                        " (100 years of J2000)\n");
        return 0;
    }
    if (!parse_i64(ttmut1_text, ttmut1_ms)
        || *ttmut1_ms < -ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS
        || *ttmut1_ms > ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS) {
        fprintf(stderr, "error: TT_MINUS_UT1_MS must be milliseconds in"
                        " [-600000, 600000] (~69200 in 2026; %s)\n",
                zero_cost_note);
        return 0;
    }
    return 1;
}

static int parse_sun_times(const char *ut1_text, const char *ttmut1_text,
                           int64_t *ut1_ms, int64_t *ttmut1_ms)
{
    return parse_body_times(ut1_text, ttmut1_text,
                            "0 costs at most ~0.05'", ut1_ms, ttmut1_ms);
}

static int parse_moon_times(const char *ut1_text, const char *ttmut1_text,
                            int64_t *ut1_ms, int64_t *ttmut1_ms)
{
    return parse_body_times(ut1_text, ttmut1_text,
                            "0 costs up to ~0.6'", ut1_ms, ttmut1_ms);
}

/* One Sun almanac entry: UT1 instant + caller-supplied TT - UT1 ->
 * earth-fixed Q2.30 vector (and the inertial J2000 vector the
 * dynamics produced), classical GHA/dec at the human boundary. */
static int run_sun_cli(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "error: --sun expects UT1_MS TT_MINUS_UT1_MS\n");
        print_usage(argv[0]);
        return 2;
    }

    int64_t ms, ttmut1;
    if (!parse_sun_times(argv[2], argv[3], &ms, &ttmut1)) return 2;

    astro_nav_unitvec_t inertial, ef;
    astro_nav_sun_inertial(ms + ttmut1, &inertial);
    astro_nav_celestial_to_earthfixed(&inertial, ms, &ef);

    int32_t dec, lon, aries = astro_nav_gha_aries_cdeg(ms);
    astro_nav_latlon_cdeg_from_unitvec(&ef, &dec, &lon);
    int32_t gha = lon <= 0 ? -lon : CDEG_PER_TURN - lon;

    printf("Sun at UT1 J2000 %+lld ms (TT - UT1 = %lld ms):\n",
           (long long)ms, (long long)ttmut1);
    printf("GHA Aries: "); print_cdeg(aries); printf(" deg\n");
    printf("GHA:       "); print_cdeg(gha);   printf(" deg\n");
    printf("dec:       "); print_cdeg(dec);   printf(" deg\n");
    printf("earth-fixed vector: (%ld, %ld, %ld)/2^30\n",
           (long)ef.x, (long)ef.y, (long)ef.z);
    printf("inertial J2000 vector: (%ld, %ld, %ld)/2^30\n",
           (long)inertial.x, (long)inertial.y, (long)inertial.z);

    int32_t r_uau, sd, hp;
    astro_nav_sun_distance(ms, ttmut1, &r_uau, &sd, &hp);
    printf("distance:  %ld micro-AU\n", (long)r_uau);
    printf("SD:        "); print_marcmin(sd);
    printf(" (%ld milli-arcmin)   HP: ", (long)sd);
    print_marcmin(hp);
    printf(" (%ld milli-arcmin)\n", (long)hp);
    return 0;
}

/* One Moon almanac entry: --sun's Moon sibling, distance in km
 * instead of micro-AU. The HP this prints is 54-61 arcmin -- the
 * largest correction in celestial navigation. */
static int run_moon_cli(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "error: --moon expects UT1_MS TT_MINUS_UT1_MS\n");
        print_usage(argv[0]);
        return 2;
    }

    int64_t ms, ttmut1;
    if (!parse_moon_times(argv[2], argv[3], &ms, &ttmut1)) return 2;

    astro_nav_unitvec_t inertial, ef;
    astro_nav_moon_inertial(ms + ttmut1, &inertial);
    astro_nav_celestial_to_earthfixed(&inertial, ms, &ef);

    int32_t dec, lon, aries = astro_nav_gha_aries_cdeg(ms);
    astro_nav_latlon_cdeg_from_unitvec(&ef, &dec, &lon);
    int32_t gha = lon <= 0 ? -lon : CDEG_PER_TURN - lon;

    printf("Moon at UT1 J2000 %+lld ms (TT - UT1 = %lld ms):\n",
           (long long)ms, (long long)ttmut1);
    printf("GHA Aries: "); print_cdeg(aries); printf(" deg\n");
    printf("GHA:       "); print_cdeg(gha);   printf(" deg\n");
    printf("dec:       "); print_cdeg(dec);   printf(" deg\n");
    printf("earth-fixed vector: (%ld, %ld, %ld)/2^30\n",
           (long)ef.x, (long)ef.y, (long)ef.z);
    printf("inertial J2000 vector: (%ld, %ld, %ld)/2^30\n",
           (long)inertial.x, (long)inertial.y, (long)inertial.z);

    int32_t km, sd, hp;
    astro_nav_moon_distance(ms, ttmut1, &km, &sd, &hp);
    printf("distance:  %ld km\n", (long)km);
    printf("SD:        "); print_marcmin(sd);
    printf(" (%ld milli-arcmin)   HP: ", (long)sd);
    print_marcmin(hp);
    printf(" (%ld milli-arcmin)\n", (long)hp);
    return 0;
}

/* One timed Sun sight, Method C -- --reduce-star's Sun sibling, and
 * the probe for Sun crossing times (docs/BRIS.md): with HO given, the
 * printed sine residual is the raw Q2.30 distance from the crossing. */
static int run_reduce_sun_cli(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "error: --reduce-sun expects LAT LON UT1_MS"
                        " TT_MINUS_UT1_MS and optional HO\n");
        print_usage(argv[0]);
        return 2;
    }

    int32_t lat, lon;
    int64_t ms, ttmut1, ho = 0;
    if (!parse_i32(argv[2], &lat) || !parse_i32(argv[3], &lon)) {
        fprintf(stderr, "error: LAT/LON must be base-10 int32 values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("LAT", lat, -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("LON", lon, -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    if (bad) return 2;
    if (!parse_sun_times(argv[4], argv[5], &ms, &ttmut1)) return 2;
    if (argc == 7) {
        if (!parse_i64(argv[6], &ho) || ho < -5400000 || ho > 5400000) {
            fprintf(stderr, "error: HO must be milli-arcminutes in"
                            " [-5400000, 5400000]\n");
            return 2;
        }
    }

    astro_nav_unitvec_t body, observer;
    astro_nav_sun_earthfixed(ms, ttmut1, &body);
    astro_nav_unitvec_from_cdeg(lat, lon, &observer);

    astro_nav_machine_sight_t sight;
    astro_nav_reduce_method_c(&observer, &body, &sight);

    printf("Sun at UT1 J2000 %+lld ms (TT - UT1 = %lld ms)\n",
           (long long)ms, (long long)ttmut1);
    int32_t hc_cdeg = astro_nav_hc_cdeg_from_sin_q30(sight.sin_hc_q30);
    printf("Hc(C): "); print_cdeg(hc_cdeg);
    printf(" deg   (machine sin_hc=%ld/2^30)\n", (long)sight.sin_hc_q30);
    printf("Zn(C): ");
    if (sight.zn_valid) {
        print_cdeg(astro_nav_zn_cdeg_from_square_key(sight.square_key));
        printf(" deg true   square-key=%u/65536\n",
               (unsigned)sight.square_key);
    } else {
        printf("undefined (body at zenith/nadir, or observer at a"
               " pole)\n");
    }

    if (argc == 7) {
        printf("sine residual: %+lld/2^30 (machine sin_hc - sin Ho)\n",
               (long long)sight.sin_hc_q30
               - astro_nav_sin_q30_from_marcmin(ho));
        int64_t milli_nm = ho - astro_nav_cdeg_to_marcmin(hc_cdeg);
        int toward = milli_nm >= 0;
        int64_t magnitude = toward ? milli_nm : -milli_nm;
        int64_t tenths_nm = (magnitude + 50) / 100;
        printf("Intercept: %lld.%lld nm %s\n",
               (long long)(tenths_nm / 10), (long long)(tenths_nm % 10),
               toward ? "TOWARD" : "AWAY");
    }
    return 0;
}

/* One timed Moon sight, Method C -- --reduce-sun's Moon sibling. The
 * HO fed here must already be parallax-corrected (--correct-moon):
 * the Moon's 54-61' of parallax is the largest correction in
 * celestial navigation, and skipping it is up to a degree of error. */
static int run_reduce_moon_cli(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "error: --reduce-moon expects LAT LON UT1_MS"
                        " TT_MINUS_UT1_MS and optional HO\n");
        print_usage(argv[0]);
        return 2;
    }

    int32_t lat, lon;
    int64_t ms, ttmut1, ho = 0;
    if (!parse_i32(argv[2], &lat) || !parse_i32(argv[3], &lon)) {
        fprintf(stderr, "error: LAT/LON must be base-10 int32 values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("LAT", lat, -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("LON", lon, -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    if (bad) return 2;
    if (!parse_moon_times(argv[4], argv[5], &ms, &ttmut1)) return 2;
    if (argc == 7) {
        if (!parse_i64(argv[6], &ho) || ho < -5400000 || ho > 5400000) {
            fprintf(stderr, "error: HO must be milli-arcminutes in"
                            " [-5400000, 5400000]\n");
            return 2;
        }
    }

    astro_nav_unitvec_t body, observer;
    astro_nav_moon_earthfixed(ms, ttmut1, &body);
    astro_nav_unitvec_from_cdeg(lat, lon, &observer);

    astro_nav_machine_sight_t sight;
    astro_nav_reduce_method_c(&observer, &body, &sight);

    printf("Moon at UT1 J2000 %+lld ms (TT - UT1 = %lld ms)\n",
           (long long)ms, (long long)ttmut1);
    int32_t hc_cdeg = astro_nav_hc_cdeg_from_sin_q30(sight.sin_hc_q30);
    printf("Hc(C): "); print_cdeg(hc_cdeg);
    printf(" deg   (machine sin_hc=%ld/2^30)\n", (long)sight.sin_hc_q30);
    printf("Zn(C): ");
    if (sight.zn_valid) {
        print_cdeg(astro_nav_zn_cdeg_from_square_key(sight.square_key));
        printf(" deg true   square-key=%u/65536\n",
               (unsigned)sight.square_key);
    } else {
        printf("undefined (body at zenith/nadir, or observer at a"
               " pole)\n");
    }

    if (argc == 7) {
        printf("sine residual: %+lld/2^30 (machine sin_hc - sin Ho)\n",
               (long long)sight.sin_hc_q30
               - astro_nav_sin_q30_from_marcmin(ho));
        int64_t milli_nm = ho - astro_nav_cdeg_to_marcmin(hc_cdeg);
        int toward = milli_nm >= 0;
        int64_t magnitude = toward ? milli_nm : -milli_nm;
        int64_t tenths_nm = (magnitude + 50) / 100;
        printf("Intercept: %lld.%lld nm %s\n",
               (long long)(tenths_nm / 10), (long long)(tenths_nm % 10),
               toward ? "TOWARD" : "AWAY");
    }
    return 0;
}

/* Timed two-Sun fix: the same body at two instants -- the sun-only,
 * single-day Bris fix for a stationary observer. Conditioning depends
 * on the resulting azimuth cut; docs/BRIS.md shows both a useful
 * two-angle pair and the nearly parallel equal-altitude trap. One
 * TT - UT1 serves both sights because millisecond-scale change during
 * one day is far below this Sun model's resolution. */
static int run_fix_sun_cli(int argc, char **argv)
{
    if (argc != 9) {
        fprintf(stderr, "error: --fix-sun expects DRLAT DRLON"
                        " TT_MINUS_UT1_MS UT1_1 HO1 UT1_2 HO2\n");
        print_usage(argv[0]);
        return 2;
    }

    int32_t drlat, drlon;
    if (!parse_i32(argv[2], &drlat) || !parse_i32(argv[3], &drlon)) {
        fprintf(stderr, "error: DRLAT/DRLON must be base-10 int32"
                        " values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("DRLAT", drlat, -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("DRLON", drlon, -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    if (bad) return 2;

    int64_t ttmut1;
    if (!parse_i64(argv[4], &ttmut1)
        || ttmut1 < -ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS
        || ttmut1 > ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS) {
        fprintf(stderr, "error: TT_MINUS_UT1_MS must be milliseconds in"
                        " [-600000, 600000] (~69200 in 2026; 0 costs"
                        " at most ~0.05')\n");
        return 2;
    }

    astro_nav_unitvec_t body[2];
    int32_t sin_ho[2];
    for (int i = 0; i < 2; i++) {
        const char *tname = i ? "UT1_2" : "UT1_1";
        const char *hname = i ? "HO2" : "HO1";
        int64_t ms, ho;
        if (!parse_i64(argv[5 + 2 * i], &ms)
            || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
            || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
            fprintf(stderr, "error: %s must be within +- 3155760000000"
                            " (100 years of J2000)\n", tname);
            return 2;
        }
        if (!parse_i64(argv[6 + 2 * i], &ho)
            || ho < -5400000 || ho > 5400000) {
            fprintf(stderr, "error: %s must be milli-arcminutes in"
                            " [-5400000, 5400000]\n", hname);
            return 2;
        }
        astro_nav_sun_earthfixed(ms, ttmut1, &body[i]);
        sin_ho[i] = astro_nav_sin_q30_from_marcmin(ho);
        printf("Sun        at UT1 J2000 %+lld ms   Ho = %lld"
               " milli-arcmin\n", (long long)ms, (long long)ho);
    }
    printf("TT - UT1 = %lld ms for both sights\n", (long long)ttmut1);

    astro_nav_unitvec_t hint;
    astro_nav_unitvec_from_cdeg(drlat, drlon, &hint);

    astro_nav_fix_result_t fix;
    astro_nav_fix_two_body(&body[0], sin_ho[0], &body[1], sin_ho[1],
                           &hint, &fix);
    if (!fix.valid) {
        printf("no fix: the two circles of equal altitude are degenerate"
               " (sights too close in time) or do not intersect\n");
        return 1;
    }

    int32_t plat, plon, alat, alon;
    astro_nav_latlon_cdeg_from_unitvec(&fix.position, &plat, &plon);
    astro_nav_latlon_cdeg_from_unitvec(&fix.alternate, &alat, &alon);
    printf("fix:       lat "); print_cdeg(plat);
    printf(" deg   lon "); print_cdeg(plon); printf(" deg\n");
    printf("alternate: lat "); print_cdeg(alat);
    printf(" deg   lon "); print_cdeg(alon);
    printf(" deg   (other circle intersection)\n");
    return 0;
}

/* One timed sight for the n-sight modes: BODY UT1_MS HO, where BODY
 * is a star catalog index, sun:TT_MINUS_UT1_MS, or
 * moon:TT_MINUS_UT1_MS -- the moving bodies carry their time-contract
 * offset inline, so star, Sun, and Moon sights mix freely in one
 * fix. */
typedef struct {
    astro_nav_unitvec_t body;   /* earth-fixed at the sight instant */
    int32_t sin_ho_q30;
    int64_t ut1_ms;
    int64_t ho_marcmin;
    const char *name;
} timed_sight_t;

static int parse_timed_sight(char *const *args, int sight_no,
                             timed_sight_t *out)
{
    int64_t ms, ho;
    if (!parse_i64(args[1], &ms)
        || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
        || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: sight %d UT1_MS must be within"
                        " +- 3155760000000 (100 years of J2000)\n",
                sight_no);
        return 0;
    }
    if (!parse_i64(args[2], &ho) || ho < -5400000 || ho > 5400000) {
        fprintf(stderr, "error: sight %d HO must be milli-arcminutes"
                        " in [-5400000, 5400000]\n", sight_no);
        return 0;
    }

    if (strncmp(args[0], "sun:", 4) == 0) {
        int64_t ttmut1;
        if (!parse_i64(args[0] + 4, &ttmut1)
            || ttmut1 < -ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS
            || ttmut1 > ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS) {
            fprintf(stderr, "error: sight %d sun:TT_MINUS_UT1_MS must"
                            " be milliseconds in [-600000, 600000]"
                            " (~69200 in 2026)\n", sight_no);
            return 0;
        }
        astro_nav_sun_earthfixed(ms, ttmut1, &out->body);
        out->name = "Sun";
    } else if (strncmp(args[0], "moon:", 5) == 0) {
        int64_t ttmut1;
        if (!parse_i64(args[0] + 5, &ttmut1)
            || ttmut1 < -ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS
            || ttmut1 > ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS) {
            fprintf(stderr, "error: sight %d moon:TT_MINUS_UT1_MS must"
                            " be milliseconds in [-600000, 600000]"
                            " (~69200 in 2026)\n", sight_no);
            return 0;
        }
        astro_nav_moon_earthfixed(ms, ttmut1, &out->body);
        out->name = "Moon";
    } else {
        int32_t star;
        if (!parse_i32(args[0], &star) || star < 0
            || star >= ASTRO_NAV_STAR_COUNT) {
            fprintf(stderr, "error: sight %d BODY must be a star index"
                            " 0..%d, sun:TT_MINUS_UT1_MS, or"
                            " moon:TT_MINUS_UT1_MS\n",
                    sight_no, ASTRO_NAV_STAR_COUNT - 1);
            return 0;
        }
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[star].j2000,
                                          ms, &out->body);
        out->name = astro_nav_stars[star].name;
    }
    out->ut1_ms = ms;
    out->ho_marcmin = ho;
    out->sin_ho_q30 = astro_nav_sin_q30_from_marcmin(ho);
    return 1;
}

/* Shared DR parsing for the n-sight modes. */
static int parse_dr(char *const *args, int32_t *drlat, int32_t *drlon)
{
    if (!parse_i32(args[0], drlat) || !parse_i32(args[1], drlon)) {
        fprintf(stderr, "error: DRLAT/DRLON must be base-10 int32"
                        " values\n");
        return 0;
    }
    if (cli_range_error("DRLAT", *drlat, -CDEG_PER_QUARTER,
                        CDEG_PER_QUARTER)
        | cli_range_error("DRLON", *drlon, -CDEG_PER_HALFTURN,
                          CDEG_PER_HALFTURN))
        return 0;
    return 1;
}

/* The n-body solve and result printing shared by --fix-n and
 * --running-fix, after any advancement has been applied. */
static int solve_and_print_fix_n(const timed_sight_t *sights, int n,
                                 int32_t drlat, int32_t drlon)
{
    astro_nav_unitvec_t bodies[FIX_N_MAX_SIGHTS];
    int32_t sin_ho[FIX_N_MAX_SIGHTS];
    for (int i = 0; i < n; i++) {
        bodies[i] = sights[i].body;
        sin_ho[i] = sights[i].sin_ho_q30;
    }

    astro_nav_unitvec_t seed;
    astro_nav_unitvec_from_cdeg(drlat, drlon, &seed);

    astro_nav_fixn_result_t fix;
    astro_nav_fix_n_body(bodies, sin_ho, n, &seed, &fix);
    if (!fix.valid) {
        printf("no fix: degenerate geometry (circles near-parallel at"
               " the solution) or no convergence\n");
        return 1;
    }

    int32_t plat, plon;
    astro_nav_latlon_cdeg_from_unitvec(&fix.position, &plat, &plon);
    printf("fix:       lat "); print_cdeg(plat);
    printf(" deg   lon "); print_cdeg(plon); printf(" deg\n");
    printf("sights: %d   iterations: %d   worst residual: %lld"
           " milli-arcmin\n", n, fix.iterations,
           (long long)fix.max_residual_marcmin);
    return 0;
}

/* --fix-n: the overdetermined fix. 2..32 timed sights -- stars and
 * the Sun freely mixed -- from ONE observer position, solved by
 * integer Gauss-Newton least squares (astro_nav_fix_n_body). Unlike
 * the closed-form two-body fix there is no alternate point: the DR
 * seed picks the solution basin, and the worst residual is the
 * navigator's scatter number (1000 milli-arcmin = 1 nm). Sights from
 * a vessel underway violate the one-position premise: use
 * --running-fix, which advances them to a common instant first. */
static int run_fix_n_cli(int argc, char **argv)
{
    if (argc < 4 + 3 * 2 || (argc - 4) % 3 != 0
        || (argc - 4) / 3 > FIX_N_MAX_SIGHTS) {
        fprintf(stderr, "error: --fix-n expects DRLAT DRLON then 2..%d"
                        " sights, each BODY UT1_MS HO (BODY = star"
                        " index 0..%d, sun:TT_MINUS_UT1_MS, or"
                        " moon:TT_MINUS_UT1_MS)\n",
                FIX_N_MAX_SIGHTS, ASTRO_NAV_STAR_COUNT - 1);
        print_usage(argv[0]);
        return 2;
    }
    int n = (argc - 4) / 3;

    int32_t drlat, drlon;
    if (!parse_dr(&argv[2], &drlat, &drlon)) return 2;

    timed_sight_t sights[FIX_N_MAX_SIGHTS];
    for (int i = 0; i < n; i++)
        if (!parse_timed_sight(&argv[4 + 3 * i], i + 1, &sights[i]))
            return 2;

    for (int i = 0; i < n; i++)
        printf("%-10s at UT1 J2000 %+lld ms   Ho = %lld"
               " milli-arcmin\n", sights[i].name,
               (long long)sights[i].ut1_ms,
               (long long)sights[i].ho_marcmin);

    return solve_and_print_fix_n(sights, n, drlat, drlon);
}

/* --running-fix: the classic underway fix, machine-native. The track
 * is the great circle whose bearing at the DR is COURSE
 * (centidegrees true), run at SPEED (tenths of a knot); every earlier
 * sight is advanced to the LAST LISTED sight's instant by rotating
 * its body vector along that track
 * (astro_nav_advance_body_for_run) -- exact on the track, and as good
 * as the DR that orients it -- then the n-body solver runs as if all
 * sights were simultaneous. DRLAT/DRLON is the dead reckoning AT the
 * last sight, which is also where the fix comes out. A sight
 * timestamped after the last listed one gets a negative run (it is
 * retarded, not advanced), which the rotation handles exactly. */
static int run_running_fix_cli(int argc, char **argv)
{
    if (argc < 6 + 3 * 2 || (argc - 6) % 3 != 0
        || (argc - 6) / 3 > FIX_N_MAX_SIGHTS) {
        fprintf(stderr, "error: --running-fix expects DRLAT DRLON"
                        " COURSE SPEED then 2..%d sights, each BODY"
                        " UT1_MS HO (BODY = star index 0..%d,"
                        " sun:TT_MINUS_UT1_MS, or"
                        " moon:TT_MINUS_UT1_MS)\n",
                FIX_N_MAX_SIGHTS, ASTRO_NAV_STAR_COUNT - 1);
        print_usage(argv[0]);
        return 2;
    }
    int n = (argc - 6) / 3;

    int32_t drlat, drlon;
    if (!parse_dr(&argv[2], &drlat, &drlon)) return 2;

    int32_t course, speed;
    if (!parse_i32(argv[4], &course) || !parse_i32(argv[5], &speed)) {
        fprintf(stderr, "error: COURSE/SPEED must be base-10 int32"
                        " values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("COURSE", course, 0, CDEG_PER_TURN - 1);
    bad |= cli_range_error("SPEED", speed, 0, 1000); /* <= 100.0 kn */
    if (bad) return 2;

    /* astro_nav_advance_body_for_run() has no defined course
     * direction within ~0.9 arcmin of a pole and returns the body
     * unrotated there -- a silent no-advance. Refuse the polar cap
     * outright rather than print runs that were never applied. */
    if (drlat > 8998 || drlat < -8998) {
        fprintf(stderr, "error: DRLAT %ld is inside the polar cap"
                        " (|DRLAT| > 8998): a course has no direction"
                        " at the pole, so a running fix is undefined"
                        " there -- use --fix-n with simultaneous"
                        " sights instead\n", (long)drlat);
        return 2;
    }

    timed_sight_t sights[FIX_N_MAX_SIGHTS];
    for (int i = 0; i < n; i++)
        if (!parse_timed_sight(&argv[6 + 3 * i], i + 1, &sights[i]))
            return 2;

    astro_nav_unitvec_t dr;
    astro_nav_unitvec_from_cdeg(drlat, drlon, &dr);

    /* run_i = SPEED x (t_last - t_i), tenths of a nautical mile,
     * rounded half away from zero. |run| <= 1000/10 kn x 200 years
     * ~ 1.75e9 tenths-nm: fits int32, and the rotation reduces it
     * mod one earth circumference exactly. */
    const int64_t t_last = sights[n - 1].ut1_ms;
    printf("course "); print_cdeg(course);
    printf(" deg true   speed %ld.%ld kn   fix at the last sight's"
           " instant\n", (long)(speed / 10), (long)(speed % 10));
    for (int i = 0; i < n; i++) {
        int64_t num = (int64_t)speed * (t_last - sights[i].ut1_ms);
        int64_t run = (num >= 0 ? num + 1800000 : num - 1800000)
                      / 3600000;
        astro_nav_advance_body_for_run(&sights[i].body, &dr, course,
                                       (int32_t)run, &sights[i].body);
        int64_t mag = run < 0 ? -run : run;
        printf("%-10s at UT1 J2000 %+lld ms   Ho = %lld milli-arcmin"
               "   run %s%lld.%lld nm\n", sights[i].name,
               (long long)sights[i].ut1_ms,
               (long long)sights[i].ho_marcmin,
               run < 0 ? "-" : "+",
               (long long)(mag / 10), (long long)(mag % 10));
    }

    return solve_and_print_fix_n(sights, n, drlat, drlon);
}

/* --ephemeris-check: the almanac against an INDEPENDENT authority.
 * --native-reference validates the integer arithmetic against a
 * double-precision implementation of the SAME model, so it can never
 * see what the model omits. The committed rows in
 * ephemeris_reference.h come from Skyfield/DE421 apparent places
 * (Hipparcos proper motion, annual aberration, light deflection,
 * IAU 2000A/2006 precession-nutation; regenerate offline with
 * tools/make_ephemeris_reference.py -- no Python at build or test
 * time). The separations measured here therefore ARE the almanac's
 * real-sky error, the "~1-2' total" bound HOWTO.md section 7 claims,
 * and this gate turns that claim into an assertion. */
static uint32_t isqrt_u64(uint64_t n)
{
    uint64_t root = 0, bit = (uint64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

#define EPHEM_MS_2030   946728000000LL /* 30 Julian years */
#define EPHEM_GATE_2030 2000           /* milli-arcmin: the 2' claim */
#define EPHEM_GATE_FULL 2600           /* milli-arcmin, through 2036 */
#define EPHEM_GATE_SUN   600           /* milli-arcmin (measured 0.39') */
#define EPHEM_GATE_SUN_DIST 320        /* micro-AU (measured 78; USNO
                                          states 0.0003 AU) */
#define EPHEM_GATE_SUN_SD 6            /* milli-arcmin (measured 1) */
#define EPHEM_GATE_SUN_HP 1            /* milli-arcmin (measured 1) */
/* Moon gates, sized from the measured basis -- a 37,048-instant DE421
 * sweep over 1900-2053 (tools/sweep_moon_vs_de421.py; DE421 coverage
 * ends 2053-10-09): direction worst 274.8 milli-arcmin earth-fixed,
 * distance 13.0 km, HP 2.42, SD 1.02 milli-arcmin -- with 1.8-4x
 * margin, so they hold over the full validated range, not just the
 * committed rows (which measure 0.16' / 7 km / 1 / 1: the era sampled
 * below is gentler than the 1900s worst). This is the abridged
 * ch. 47 model's own error; the integer transcription contributes
 * under 0.01' / 1 km (double-oracle sweep). */
#define EPHEM_GATE_MOON      500       /* milli-arcmin */
#define EPHEM_GATE_MOON_DIST  40       /* km */
#define EPHEM_GATE_MOON_SD     6       /* milli-arcmin */
#define EPHEM_GATE_MOON_HP    10       /* milli-arcmin */

static void print_arcmin(int64_t marcmin)
{
    printf("%lld.%02lld'", (long long)(marcmin / 1000),
           (long long)((marcmin % 1000) / 10));
}

static int run_ephemeris_check(void)
{
    int64_t worst_star[ASTRO_NAV_STAR_COUNT] = {0};
    int64_t worst = 0, worst_ms = 0, worst_2030 = 0, sum = 0;
    int worst_index = 0;

    for (size_t i = 0; i < EPHEM_REF_ROW_COUNT; i++) {
        const ephem_ref_row_t *r = &ephem_ref_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[r->star].j2000,
                                          r->ut1_ms, &v);
        int64_t dx = (int64_t)v.x - r->x;
        int64_t dy = (int64_t)v.y - r->y;
        int64_t dz = (int64_t)v.z - r->z;
        /* chord between unit vectors, Q2.30; at arcminute scales the
         * chord IS the separation angle in radians to ~1e-8, so
         * milli-arcmin = chord * 3437746.77 / 2^30. */
        int64_t chord = (int64_t)isqrt_u64((uint64_t)(dx * dx + dy * dy
                                                      + dz * dz));
        int64_t marcmin = (chord * 3437747 + ((int64_t)1 << 29)) >> 30;

        sum += marcmin;
        if (marcmin > worst_star[r->star]) worst_star[r->star] = marcmin;
        if (r->ut1_ms <= EPHEM_MS_2030 && marcmin > worst_2030)
            worst_2030 = marcmin;
        if (marcmin > worst) {
            worst = marcmin;
            worst_index = r->star;
            worst_ms = r->ut1_ms;
        }
    }

    printf("ephemeris check: %d rows, %d stars, 2000-2036\n",
           (int)EPHEM_REF_ROW_COUNT, ASTRO_NAV_STAR_COUNT);
    printf("independent reference: Skyfield/DE421 apparent places in"
           " ITRS\n(proper motion, aberration, nutation --"
           " tools/make_ephemeris_reference.py)\n\n");
    printf("worst separation per star across the sampled epochs:\n");
    for (int s = 0; s < ASTRO_NAV_STAR_COUNT; s++) {
        printf("  %-10s ", astro_nav_stars[s].name);
        print_arcmin(worst_star[s]);
        printf("%s", (s % 3 == 2) ? "\n" : "   ");
    }
    printf("\nmean ");
    print_arcmin(sum / (int64_t)EPHEM_REF_ROW_COUNT);
    printf("   worst ");
    print_arcmin(worst);
    printf(" (%s, year %lld.%lld)\n", astro_nav_stars[worst_index].name,
           (long long)(2000 + worst_ms / 31557600000LL),
           (long long)(worst_ms % 31557600000LL / 3155760000LL));

    int fail = 0;
    printf("through 2030: worst ");
    print_arcmin(worst_2030);
    printf("  claim <= ");
    print_arcmin(EPHEM_GATE_2030);
    if (worst_2030 <= EPHEM_GATE_2030) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }
    printf("all epochs:   worst ");
    print_arcmin(worst);
    printf("  gate  <= ");
    print_arcmin(EPHEM_GATE_FULL);
    if (worst <= EPHEM_GATE_FULL) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }

    /* Sun rows: each carries the Skyfield TT - UT1 for its instant,
     * so this also exercises the caller-supplied time contract of
     * astro_nav_sun_earthfixed(). The Sun's error is periodic in the
     * year, not secular like proper motion, so one full-span gate. */
    int64_t sun_worst = 0, sun_worst_ms = 0, sun_sum = 0;
    for (size_t i = 0; i < EPHEM_SUN_ROW_COUNT; i++) {
        const ephem_sun_row_t *r = &ephem_sun_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_sun_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
        int64_t dx = (int64_t)v.x - r->x;
        int64_t dy = (int64_t)v.y - r->y;
        int64_t dz = (int64_t)v.z - r->z;
        int64_t chord = (int64_t)isqrt_u64((uint64_t)(dx * dx + dy * dy
                                                      + dz * dz));
        int64_t marcmin = (chord * 3437747 + ((int64_t)1 << 29)) >> 30;
        sun_sum += marcmin;
        if (marcmin > sun_worst) {
            sun_worst = marcmin;
            sun_worst_ms = r->ut1_ms;
        }
    }
    printf("\nsun: %d rows, 2000-2035, dynamics on TT (TT - UT1 from"
           " the rows)\n", (int)EPHEM_SUN_ROW_COUNT);
    printf("mean ");
    print_arcmin(sun_sum / (int64_t)EPHEM_SUN_ROW_COUNT);
    printf("   worst ");
    print_arcmin(sun_worst);
    printf(" (year %lld.%lld)   gate <= ",
           (long long)(2000 + sun_worst_ms / 31557600000LL),
           (long long)(sun_worst_ms % 31557600000LL / 3155760000LL));
    print_arcmin(EPHEM_GATE_SUN);
    if (sun_worst <= EPHEM_GATE_SUN) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }

    /* Distance rows gate the R model behind SD and HP. Truth SD/HP
     * apply the SAME angular constants to the DE421 distance, so the
     * comparison isolates the distance error (USNO states the source
     * formula's R is good to 0.0003 AU; the rows include explicit
     * perihelion/aphelion samples, the distance extremes). */
    int64_t d_worst = 0, sd_worst = 0, hp_worst = 0;
    for (size_t i = 0; i < EPHEM_SUN_ROW_COUNT; i++) {
        const ephem_sun_row_t *r = &ephem_sun_rows[i];
        int32_t d_uau, sd, hp;
        astro_nav_sun_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                               &d_uau, &sd, &hp);
        int64_t td = r->dist_uau;
        int64_t dd = (int64_t)d_uau - td;
        int64_t dsd = sd - (15996000000LL + td / 2) / td;
        int64_t dhp = hp - (146566667LL + td / 2) / td;
        if (dd < 0) dd = -dd;
        if (dsd < 0) dsd = -dsd;
        if (dhp < 0) dhp = -dhp;
        if (dd > d_worst) d_worst = dd;
        if (dsd > sd_worst) sd_worst = dsd;
        if (dhp > hp_worst) hp_worst = dhp;
    }
    printf("distance: worst %lld micro-AU   gate <= %d",
           (long long)d_worst, EPHEM_GATE_SUN_DIST);
    if (d_worst <= EPHEM_GATE_SUN_DIST) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }
    printf("SD:       worst %lld milli-arcmin   gate <= %d   "
           "HP: worst %lld milli-arcmin   gate <= %d",
           (long long)sd_worst, EPHEM_GATE_SUN_SD,
           (long long)hp_worst, EPHEM_GATE_SUN_HP);
    if (sd_worst <= EPHEM_GATE_SUN_SD && hp_worst <= EPHEM_GATE_SUN_HP)
        printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }

    /* Moon rows: a cadence that walks the anomalistic/synodic phase
     * (the Moon's error periods) across 2000-2035, plus the 2026
     * extreme perigee/apogee pair, the 2025 major-standstill
     * declination extremes, and both nodes. Each row carries its
     * TT - UT1, so the two-timescale contract is exercised where it
     * costs the most (~0.6' if dropped, 13x the Sun). SD/HP truth
     * comes from the rows as full arcsines of the DE421 geometric
     * distance -- at 54-61' the small-angle division the Sun block
     * uses would itself be a few milli-arcmin wrong. */
    int64_t moon_worst = 0, moon_worst_ms = 0, moon_sum = 0;
    for (size_t i = 0; i < EPHEM_MOON_ROW_COUNT; i++) {
        const ephem_moon_row_t *r = &ephem_moon_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_moon_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
        int64_t dx = (int64_t)v.x - r->x;
        int64_t dy = (int64_t)v.y - r->y;
        int64_t dz = (int64_t)v.z - r->z;
        int64_t chord = (int64_t)isqrt_u64((uint64_t)(dx * dx + dy * dy
                                                      + dz * dz));
        int64_t marcmin = (chord * 3437747 + ((int64_t)1 << 29)) >> 30;
        moon_sum += marcmin;
        if (marcmin > moon_worst) {
            moon_worst = marcmin;
            moon_worst_ms = r->ut1_ms;
        }
    }
    printf("\nmoon: %d rows, 2000-2035, dynamics on TT (TT - UT1 from"
           " the rows)\n", (int)EPHEM_MOON_ROW_COUNT);
    printf("mean ");
    print_arcmin(moon_sum / (int64_t)EPHEM_MOON_ROW_COUNT);
    printf("   worst ");
    print_arcmin(moon_worst);
    printf(" (year %lld.%lld)   gate <= ",
           (long long)(2000 + moon_worst_ms / 31557600000LL),
           (long long)(moon_worst_ms % 31557600000LL / 3155760000LL));
    print_arcmin(EPHEM_GATE_MOON);
    if (moon_worst <= EPHEM_GATE_MOON) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }

    int64_t md_worst = 0, msd_worst = 0, mhp_worst = 0;
    for (size_t i = 0; i < EPHEM_MOON_ROW_COUNT; i++) {
        const ephem_moon_row_t *r = &ephem_moon_rows[i];
        int32_t km, sd, hp;
        astro_nav_moon_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                                &km, &sd, &hp);
        int64_t dd = (int64_t)km - r->dist_km;
        int64_t dsd = (int64_t)sd - r->sd_marcmin;
        int64_t dhp = (int64_t)hp - r->hp_marcmin;
        if (dd < 0) dd = -dd;
        if (dsd < 0) dsd = -dsd;
        if (dhp < 0) dhp = -dhp;
        if (dd > md_worst) md_worst = dd;
        if (dsd > msd_worst) msd_worst = dsd;
        if (dhp > mhp_worst) mhp_worst = dhp;
    }
    printf("distance: worst %lld km   gate <= %d",
           (long long)md_worst, EPHEM_GATE_MOON_DIST);
    if (md_worst <= EPHEM_GATE_MOON_DIST) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }
    printf("SD:       worst %lld milli-arcmin   gate <= %d   "
           "HP: worst %lld milli-arcmin   gate <= %d",
           (long long)msd_worst, EPHEM_GATE_MOON_SD,
           (long long)mhp_worst, EPHEM_GATE_MOON_HP);
    if (msd_worst <= EPHEM_GATE_MOON_SD && mhp_worst <= EPHEM_GATE_MOON_HP)
        printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }
    return fail;
}

/* --external-check: the almanac and the correction chain against
 * PRINTED / PUBLISHED sources -- daily-page Sun and Moon GHA/dec (plus
 * hourly Moon HP) and GHA Aries hours, a broad-era NA Sun/Moon/star
 * table, the NA refraction and dip
 * tables, and a Bowditch worked example (external_reference.h,
 * transcribed offline by tools/make_external_reference.py; numbers
 * only, no vendored code). --ephemeris-check compares against
 * Skyfield/DE421; this compares against numbers a human read off a
 * printed page -- a second, fully independent authority. Every gate
 * below is set just above the worst separation MEASURED on this host,
 * with the measurement recorded inline (the repo's EPHEM_GATE pattern).
 *
 * Direction rows (Sun, star) are compared as the chord between our
 * earth-fixed unit vector and the one baked from the printed GHA/dec,
 * so the comparison is free of centidegree-boundary quantization and
 * isolates model + transcription error. Scalar rows (GHA Aries, SD,
 * refraction, dip, correction) compare the integer outputs directly;
 * GHA Aries via astro_nav_gha_aries_cdeg() therefore carries up to
 * half a centidegree (0.30') of boundary rounding, which dominates its
 * gate. */
#define EXT_GATE_SUN         700 /* milli-arcmin (measured worst 607) */
#define EXT_GATE_ARIES       700 /* milli-arcmin (measured worst 600):
                                  * ~0.26' is the equation of the equinoxes
                                  * (NA prints apparent GAST; the library's
                                  * astro_nav_gha_aries_cdeg returns mean
                                  * GMST), plus up to 0.30' cdeg boundary
                                  * rounding. A documented model gap, not a
                                  * transcription error -- see HOWTO. */
#define EXT_GATE_SD          120 /* milli-arcmin (measured worst 76) */
#define EXT_GATE_STAR       1300 /* milli-arcmin (measured worst 1178,
                                  * Arcturus -- proper motion grows with
                                  * distance from the J2000 catalog epoch) */
#define EXT_GATE_MOON        400 /* milli-arcmin (measured worst 330):
                                  * the abridged ch. 47 model's ~0.27'
                                  * plus print rounding and the UT-as-UT1
                                  * policy above. */
#define EXT_GATE_MOON_HP     150 /* milli-arcmin (measured worst 116, a
                                  * ~0.1' systematic in the source's 2021
                                  * block; otherwise print rounding) */
#define EXT_GATE_MOON_SD     200 /* milli-arcmin (measured worst 157: the
                                  * source's SD column is noisier than its
                                  * HP column -- many rows sit 0.10-0.15'
                                  * from their own HP's implied SD, just
                                  * under the 0.15' exclusion line; see
                                  * MOON_NA_EXCLUDE in the tool) */
#define EXT_GATE_MOON_SD_AUG  10 /* milli-arcmin (measured worst 1: the
                                  * augmented SD vs USNO celnav's printed
                                  * value -- ephemeris SD error largely
                                  * cancels in the increment, leaving
                                  * print rounding) */
#define EXT_GATE_REFRACTION  150 /* milli-arcmin (measured worst 103):
                                  * Bennett's formula vs the NA standard
                                  * table (P=1010, T=10 C); largest at the
                                  * horizon, where Bennett is known to
                                  * diverge from the tabulated value. */
#define EXT_GATE_DIP          50 /* milli-arcmin (measured worst 5;
                                  * same 1.76*sqrt(m) formula, rounding only) */
#define EXT_GATE_CORR        100 /* milli-arcmin (measured worst 35) */

static int64_t ext_chord_marcmin(int32_t ax, int32_t ay, int32_t az,
                                 int32_t bx, int32_t by, int32_t bz)
{
    int64_t dx = (int64_t)ax - bx;
    int64_t dy = (int64_t)ay - by;
    int64_t dz = (int64_t)az - bz;
    int64_t chord = (int64_t)isqrt_u64((uint64_t)(dx * dx + dy * dy
                                                  + dz * dz));
    return (chord * 3437747 + ((int64_t)1 << 29)) >> 30;
}

/* Shortest signed distance on a circle of 21,600,000 milli-arcmin
 * (360 deg), returned as a magnitude. */
static int64_t ext_circ_marcmin(int64_t a, int64_t b)
{
    int64_t d = (a - b) % 21600000;
    if (d < 0) d += 21600000;
    if (d > 10800000) d = 21600000 - d;
    return d;
}

static int ext_report(const char *label, int rows,
                      int64_t worst, int64_t sum, int64_t gate)
{
    printf("%-13s %3d rows   mean ", label, rows);
    print_arcmin(rows ? sum / rows : 0);
    printf("   worst ");
    print_arcmin(worst);
    printf("   gate <= ");
    print_arcmin(gate);
    if (worst <= gate) { printf("  PASS\n"); return 0; }
    printf("  FAIL\n");
    return 1;
}

static int run_external_check(void)
{
    int fail = 0;

    printf("external check: almanac + corrections vs PUBLISHED sources\n");
    printf("(printed daily pages, NA tables, Bowditch -- independent of\n"
           "this library's formulas and of Skyfield; see"
           " external_reference.h)\n\n");

    /* Sun direction: our earth-fixed Sun vector vs the printed GHA/dec. */
    int64_t worst = 0, sum = 0;
    for (size_t i = 0; i < EXT_SUN_ROW_COUNT; i++) {
        const ext_sun_row_t *r = &ext_sun_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_sun_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
        int64_t m = ext_chord_marcmin(v.x, v.y, v.z, r->x, r->y, r->z);
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("Sun GHA/dec", (int)EXT_SUN_ROW_COUNT, worst, sum,
                       EXT_GATE_SUN);

    /* GHA Aries (sidereal): scalar, via astro_nav_gha_aries_cdeg(). */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_ARIES_ROW_COUNT; i++) {
        const ext_aries_row_t *r = &ext_aries_rows[i];
        int64_t ours = (int64_t)astro_nav_gha_aries_cdeg(r->ut1_ms) * 600;
        int64_t m = ext_circ_marcmin(ours, r->gha_marcmin);
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("GHA Aries", (int)EXT_ARIES_ROW_COUNT, worst, sum,
                       EXT_GATE_ARIES);

    /* Sun semidiameter: our distance-derived SD vs the NA value. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_SD_ROW_COUNT; i++) {
        const ext_sd_row_t *r = &ext_sd_rows[i];
        int32_t d_uau, sd, hp;
        astro_nav_sun_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                               &d_uau, &sd, &hp);
        int64_t m = sd - r->sd_marcmin;
        if (m < 0) m = -m;
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("Sun SD", (int)EXT_SD_ROW_COUNT, worst, sum,
                       EXT_GATE_SD);

    /* Star directions vs printed GHA/dec, filtered to the catalog. */
    worst = 0; sum = 0;
    int64_t star_worst_star = 0;
    for (size_t i = 0; i < EXT_STAR_ROW_COUNT; i++) {
        const ext_star_row_t *r = &ext_star_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[r->star].j2000,
                                          r->ut1_ms, &v);
        int64_t m = ext_chord_marcmin(v.x, v.y, v.z, r->x, r->y, r->z);
        sum += m;
        if (m > worst) { worst = m; star_worst_star = r->star; }
    }
    fail |= ext_report("star GHA/dec", (int)EXT_STAR_ROW_COUNT, worst, sum,
                       EXT_GATE_STAR);
    printf("              worst star: %s\n",
           astro_nav_stars[star_worst_star].name);

    /* Moon direction vs printed GHA/dec, then HP and SD vs the printed
     * values (astro_nav_moon_distance). HP and SD are printed to 0.1'
     * = 100 milli-arcmin, so +-50 of print quantization dominates the
     * model's own few-milli-arcmin HP/SD error. A scalar of -1 is
     * skipped: the daily pages print no dated SD, and a few OpenCPN
     * rows print HP or SD values that fail the source's own internal
     * HP/SD consistency (or repeat an identical pair across unrelated
     * dates) -- excluded by the transcriber with the evidence recorded
     * in tools/make_external_reference.py (MOON_NA_EXCLUDE). */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_MOON_ROW_COUNT; i++) {
        const ext_moon_row_t *r = &ext_moon_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_moon_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
        int64_t m = ext_chord_marcmin(v.x, v.y, v.z, r->x, r->y, r->z);
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("Moon GHA/dec", (int)EXT_MOON_ROW_COUNT, worst, sum,
                       EXT_GATE_MOON);

    worst = 0; sum = 0;
    int hp_rows = 0, sd_rows = 0;
    int64_t sd_worst = 0, sd_sum = 0;
    for (size_t i = 0; i < EXT_MOON_ROW_COUNT; i++) {
        const ext_moon_row_t *r = &ext_moon_rows[i];
        int32_t km, sd, hp;
        astro_nav_moon_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                                &km, &sd, &hp);
        if (r->hp_marcmin >= 0) {
            int64_t m = (int64_t)hp - r->hp_marcmin;
            if (m < 0) m = -m;
            sum += m;
            hp_rows++;
            if (m > worst) worst = m;
        }
        if (r->sd_marcmin >= 0) {
            int64_t ms = (int64_t)sd - r->sd_marcmin;
            if (ms < 0) ms = -ms;
            sd_sum += ms;
            sd_rows++;
            if (ms > sd_worst) sd_worst = ms;
        }
    }
    fail |= ext_report("Moon HP", hp_rows, worst, sum,
                       EXT_GATE_MOON_HP);
    fail |= ext_report("Moon SD", sd_rows, sd_worst, sd_sum,
                       EXT_GATE_MOON_SD);

    /* The AUGMENTED semidiameter vs the USNO celnav service: our
     * geocentric SD from the instant, plus the augmentation evaluated
     * at USNO's own Hc, against the semidiameter its correction block
     * prints for the observer to apply. Altitude 10..90 deg at both
     * HP extremes -- the altitude-dependent piece no fixed table
     * carries, held against the official service directly. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_USNO_MOON_ROW_COUNT; i++) {
        const ext_usno_moon_row_t *r = &ext_usno_moon_rows[i];
        int32_t km, sd, hp;
        astro_nav_moon_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                                &km, &sd, &hp);
        int64_t ours = sd + astro_nav_moon_augmentation_marcmin(
            r->hc_marcmin, hp, sd);
        int64_t m = ours - r->sd_topo_marcmin;
        if (m < 0) m = -m;
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("Moon SD aug", (int)EXT_USNO_MOON_ROW_COUNT,
                       worst, sum, EXT_GATE_MOON_SD_AUG);

    /* Refraction: Bennett (this library) vs the NA standard table. The
     * gap is expected and by design (a smooth formula vs a printed
     * table, worst at the horizon where Bennett is known to diverge);
     * this measures it rather than hiding it. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_REFRACTION_ROW_COUNT; i++) {
        const ext_refraction_row_t *r = &ext_refraction_rows[i];
        int64_t ours = astro_nav_refraction_marcmin(r->ha_marcmin);
        int64_t m = ours - r->ref_marcmin;
        if (m < 0) m = -m;
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("refraction", (int)EXT_REFRACTION_ROW_COUNT, worst,
                       sum, EXT_GATE_REFRACTION);

    /* Dip: same 1.76'*sqrt(h) formula as the NA table -> near-exact. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_DIP_ROW_COUNT; i++) {
        const ext_dip_row_t *r = &ext_dip_rows[i];
        int64_t ours = astro_nav_dip_marcmin(r->eye_cm);
        int64_t m = ours - r->dip_marcmin;
        if (m < 0) m = -m;
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("dip", (int)EXT_DIP_ROW_COUNT, worst, sum,
                       EXT_GATE_DIP);

    /* Correction chain: our Hs->Ho vs the Bowditch worked example. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < EXT_CORR_ROW_COUNT; i++) {
        const ext_corr_row_t *r = &ext_corr_rows[i];
        int64_t ho = astro_nav_correct_altitude_marcmin(
            r->hs_marcmin, r->ie_marcmin, r->eye_cm,
            r->hp_marcmin, r->sd_marcmin, r->limb);
        int64_t m = ho - r->ho_marcmin;
        if (m < 0) m = -m;
        sum += m;
        if (m > worst) worst = m;
    }
    fail |= ext_report("correction", (int)EXT_CORR_ROW_COUNT, worst, sum,
                       EXT_GATE_CORR);

    return fail;
}

/* --cross-check: the almanac and the fix geometry against independent
 * COMPUTED implementations (cross_reference.h, generated offline by
 * tools/make_cross_reference.py) -- evidence lineages disjoint from
 * both --ephemeris-check (Skyfield + JPL DE421 numerical integration)
 * and --external-check (printed pages). Agreement is reported per
 * independent implementation lineage, not per repository: tools that
 * share an upstream (every Skyfield/JPL-based almanac, for instance)
 * count as ONE family of evidence, however many of them exist.
 *
 *   Lineage 1 -- PyEphem/libastro, the XEphem lineage. Sun/Moon/star
 *   directions, GHA Aries, distances, SD/HP. Independence is scoped
 *   per body: the Sun (Bretagnon's analytic theory) and the Moon
 *   (ELP-derived) are disjoint code AND disjoint underlying data
 *   from JPL integration; the STAR rows are not data-independent --
 *   PyEphem's catalog is the same Hipparcos data, epoch-shifted with
 *   Skyfield (stated in ephem/stars.py) -- so they validate
 *   libastro's own precession/nutation/aberration/sidereal chain,
 *   not independent star positions. Each row carries libastro's own
 *   delta_t, so the library is handed the exact (UT1, TT) pair the
 *   oracle used and no delta-T model difference leaks into the
 *   comparison. The two oracles agree with each other over every
 *   committed row (executable: tools/make_cross_reference.py
 *   --calibrate ephemeris_reference.h; measured Sun 7.33, Moon 2.30,
 *   stars 11.29 milli-arcmin) -- 55x/73x/129x below this library's
 *   measured model error (401, 169, 1452) -- so any separation
 *   measured here is ours, confirmed independently.
 *
 *   Lineage 2 -- an independent spherical-geometry engine (see
 *   cross_reference.h for provenance): Hc/Zn from its own vector
 *   primitives, and two-body fixes by direct circle-of-position
 *   intersection -- a genuinely different algorithm from this
 *   library's iterated least-squares fix. Both engines are fed the
 *   IDENTICAL quantized inputs (centidegree positions, milli-arcmin
 *   altitudes), so scenario quantization cancels and the comparison
 *   isolates engine vs engine, measured at this library's centidegree
 *   readout boundary (up to 0.30' rounding per readout).
 *
 * Every gate is set just above the worst separation MEASURED on this
 * host, with the measurement recorded inline (the repo's EPHEM_GATE
 * pattern). */
#define CROSS_GATE_SUN       500 /* milli-arcmin (measured worst 401 --
                                  * the same 0.4' the Skyfield gate
                                  * measures: the two oracles agree to
                                  * 7.33 milli-arcmin (--calibrate), so
                                  * our Sun error is confirmed to be
                                  * model, not truth) */
#define CROSS_GATE_SUN_DIST  100 /* micro-AU (measured 78 -- identical
                                  * worst as against DE421) */
#define CROSS_GATE_SUN_SD      8 /* milli-arcmin (measured 3) */
#define CROSS_GATE_SUN_HP      5 /* milli-arcmin (measured 0) */
#define CROSS_GATE_MOON      250 /* milli-arcmin (measured 169 --
                                  * TIGHTER than the 0.27'+ measured vs
                                  * DE421, and expected: libastro's Moon
                                  * and this library's abridged ch. 47
                                  * series both descend from ELP-class
                                  * analytic theory, so they are kin on
                                  * the Moon; the fully independent Moon
                                  * verdict stays with
                                  * --ephemeris-check) */
#define CROSS_GATE_MOON_DIST  20 /* km (measured 8) */
#define CROSS_GATE_MOON_SD    35 /* milli-arcmin (measured 25: the two
                                  * implementations adopt slightly
                                  * different lunar-radius/SD constants;
                                  * a constant-convention gap, not a
                                  * distance error -- distance itself
                                  * agrees to 8 km above) */
#define CROSS_GATE_MOON_HP    10 /* milli-arcmin (measured 2) */
#define CROSS_GATE_STAR     1600 /* milli-arcmin (measured 1452,
                                  * Arcturus -- largest proper motion in
                                  * the catalog, same worst star as the
                                  * printed-page gate). Star rows share
                                  * the Hipparcos catalog with the
                                  * Skyfield lineage (see the lineage-1
                                  * scope note above): this family
                                  * checks libastro's independent
                                  * transformation chain, not
                                  * independent star data. */
#define CROSS_GATE_ARIES     500 /* milli-arcmin (measured 400: ~0.26'
                                  * equation of the equinoxes -- libastro
                                  * computes apparent GAST, the library
                                  * mean GMST -- plus up to 0.30' of
                                  * centidegree readout rounding; same
                                  * documented model gap as the
                                  * --external-check Aries gate) */
#define CROSS_GATE_HC        300 /* milli-arcmin (measured 298: the
                                  * 0.30' centidegree readout bound
                                  * itself -- the engines agree below
                                  * the readout step) */
#define CROSS_GATE_ZN        600 /* milli-arcmin (measured 529: the
                                  * square-key resolution, 1/65536 turn
                                  * ~ 0.33', plus centidegree readout
                                  * rounding) */
#define CROSS_GATE_FIX_LAT   300 /* milli-arcmin (measured 299: the
                                  * centidegree readout bound; the two
                                  * fix algorithms agree beneath it) */
#define CROSS_GATE_FIX_LON   200 /* milli-arcmin of longitude
                                  * (measured 131) */

static int run_cross_check(void)
{
    int astro_fail = 0, geom_fail = 0;

    printf("cross check: almanac + geometry vs independent COMPUTED"
           " implementations\n(libastro's analytic theories; a"
           " circle-intersection fix engine --\nSun/Moon code+data"
           " disjoint from Skyfield/DE421 and printed pages;\nstar"
           " rows share the Hipparcos catalog, independent"
           " transformation only;\nsee cross_reference.h)\n\n");
    printf("lineage 1: PyEphem/libastro (XEphem analytic theories)\n");

    /* Sun direction: our earth-fixed Sun vector vs libastro's. */
    int64_t worst = 0, sum = 0;
    for (size_t i = 0; i < CROSS_SUN_ROW_COUNT; i++) {
        const cross_sun_row_t *r = &cross_sun_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_sun_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
        int64_t m = ext_chord_marcmin(v.x, v.y, v.z, r->x, r->y, r->z);
        sum += m;
        if (m > worst) worst = m;
    }
    astro_fail |= ext_report("Sun GHA/dec", (int)CROSS_SUN_ROW_COUNT,
                             worst, sum, CROSS_GATE_SUN);

    /* Sun distance, SD, HP vs libastro's own values (its apparent
     * diameter and its distance; HP as the arcsine of the same
     * equatorial radius over its distance). */
    int64_t d_worst = 0, sd_worst = 0, hp_worst = 0;
    for (size_t i = 0; i < CROSS_SUN_ROW_COUNT; i++) {
        const cross_sun_row_t *r = &cross_sun_rows[i];
        int32_t d_uau, sd, hp;
        astro_nav_sun_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                               &d_uau, &sd, &hp);
        int64_t dd = (int64_t)d_uau - r->dist_uau;
        int64_t dsd = (int64_t)sd - r->sd_marcmin;
        int64_t dhp = (int64_t)hp - r->hp_marcmin;
        if (dd < 0) dd = -dd;
        if (dsd < 0) dsd = -dsd;
        if (dhp < 0) dhp = -dhp;
        if (dd > d_worst) d_worst = dd;
        if (dsd > sd_worst) sd_worst = dsd;
        if (dhp > hp_worst) hp_worst = dhp;
    }
    printf("distance: worst %lld micro-AU   gate <= %d",
           (long long)d_worst, CROSS_GATE_SUN_DIST);
    if (d_worst <= CROSS_GATE_SUN_DIST) printf("  PASS\n");
    else { printf("  FAIL\n"); astro_fail = 1; }
    printf("SD:       worst %lld milli-arcmin   gate <= %d   "
           "HP: worst %lld milli-arcmin   gate <= %d",
           (long long)sd_worst, CROSS_GATE_SUN_SD,
           (long long)hp_worst, CROSS_GATE_SUN_HP);
    if (sd_worst <= CROSS_GATE_SUN_SD && hp_worst <= CROSS_GATE_SUN_HP)
        printf("  PASS\n");
    else { printf("  FAIL\n"); astro_fail = 1; }

    /* Moon direction, then distance/SD/HP, same construction. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < CROSS_MOON_ROW_COUNT; i++) {
        const cross_moon_row_t *r = &cross_moon_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_moon_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
        int64_t m = ext_chord_marcmin(v.x, v.y, v.z, r->x, r->y, r->z);
        sum += m;
        if (m > worst) worst = m;
    }
    astro_fail |= ext_report("Moon GHA/dec", (int)CROSS_MOON_ROW_COUNT,
                             worst, sum, CROSS_GATE_MOON);

    int64_t md_worst = 0, msd_worst = 0, mhp_worst = 0;
    for (size_t i = 0; i < CROSS_MOON_ROW_COUNT; i++) {
        const cross_moon_row_t *r = &cross_moon_rows[i];
        int32_t km, sd, hp;
        astro_nav_moon_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                                &km, &sd, &hp);
        int64_t dd = (int64_t)km - r->dist_km;
        int64_t dsd = (int64_t)sd - r->sd_marcmin;
        int64_t dhp = (int64_t)hp - r->hp_marcmin;
        if (dd < 0) dd = -dd;
        if (dsd < 0) dsd = -dsd;
        if (dhp < 0) dhp = -dhp;
        if (dd > md_worst) md_worst = dd;
        if (dsd > msd_worst) msd_worst = dsd;
        if (dhp > mhp_worst) mhp_worst = dhp;
    }
    printf("distance: worst %lld km   gate <= %d",
           (long long)md_worst, CROSS_GATE_MOON_DIST);
    if (md_worst <= CROSS_GATE_MOON_DIST) printf("  PASS\n");
    else { printf("  FAIL\n"); astro_fail = 1; }
    printf("SD:       worst %lld milli-arcmin   gate <= %d   "
           "HP: worst %lld milli-arcmin   gate <= %d",
           (long long)msd_worst, CROSS_GATE_MOON_SD,
           (long long)mhp_worst, CROSS_GATE_MOON_HP);
    if (msd_worst <= CROSS_GATE_MOON_SD && mhp_worst <= CROSS_GATE_MOON_HP)
        printf("  PASS\n");
    else { printf("  FAIL\n"); astro_fail = 1; }

    /* Star directions across the whole catalog. */
    worst = 0; sum = 0;
    int64_t cross_worst_star = 0;
    for (size_t i = 0; i < CROSS_STAR_ROW_COUNT; i++) {
        const cross_star_row_t *r = &cross_star_rows[i];
        astro_nav_unitvec_t v;
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[r->star].j2000,
                                          r->ut1_ms, &v);
        int64_t m = ext_chord_marcmin(v.x, v.y, v.z, r->x, r->y, r->z);
        sum += m;
        if (m > worst) { worst = m; cross_worst_star = r->star; }
    }
    astro_fail |= ext_report("star GHA/dec", (int)CROSS_STAR_ROW_COUNT,
                             worst, sum, CROSS_GATE_STAR);
    printf("              worst star: %s   (shared Hipparcos catalog:"
           " checks libastro's\n              independent"
           " transformation chain, not independent star data)\n",
           astro_nav_stars[cross_worst_star].name);

    /* GHA Aries: libastro computes apparent GAST; this library's
     * astro_nav_gha_aries_cdeg() returns mean GMST at the centidegree
     * boundary. The ~0.26' equation of the equinoxes plus up to 0.30'
     * readout rounding is the same documented model gap
     * --external-check measures against the printed pages. */
    worst = 0; sum = 0;
    for (size_t i = 0; i < CROSS_ARIES_ROW_COUNT; i++) {
        const cross_aries_row_t *r = &cross_aries_rows[i];
        int64_t ours = (int64_t)astro_nav_gha_aries_cdeg(r->ut1_ms) * 600;
        int64_t m = ext_circ_marcmin(ours, r->gha_marcmin);
        sum += m;
        if (m > worst) worst = m;
    }
    astro_fail |= ext_report("GHA Aries", (int)CROSS_ARIES_ROW_COUNT,
                             worst, sum, CROSS_GATE_ARIES);

    printf("\nlineage 2: independent spherical geometry"
           " (circle intersection)\n");

    /* Hc/Zn: both engines get the identical centidegree observer and
     * GP; ours reduces on Q2.30 unit vectors and reads out at the
     * centidegree boundary (x600 -> milli-arcmin), the oracle in
     * double precision. One row aims 0.74 deg from the zenith and one
     * puts the body below the horizon (negative Hc) on purpose. */
    int64_t hc_worst = 0, hc_sum = 0, zn_worst = 0, zn_sum = 0;
    int zn_invalid = 0;
    for (size_t i = 0; i < CROSS_REDUCE_ROW_COUNT; i++) {
        const cross_reduce_row_t *r = &cross_reduce_rows[i];
        astro_nav_unitvec_t obs, body;
        astro_nav_machine_sight_t m;
        astro_nav_unitvec_from_cdeg(r->lat_cdeg, r->lon_east_cdeg, &obs);
        astro_nav_unitvec_from_cdeg(r->dec_cdeg, -r->gha_cdeg, &body);
        astro_nav_reduce_method_c(&obs, &body, &m);
        int64_t hc = (int64_t)astro_nav_hc_cdeg_from_sin_q30(m.sin_hc_q30)
                     * 600;
        int64_t dh = hc - r->hc_marcmin;
        if (dh < 0) dh = -dh;
        hc_sum += dh;
        if (dh > hc_worst) hc_worst = dh;
        if (!m.zn_valid) { zn_invalid = 1; continue; }
        int64_t zn = (int64_t)astro_nav_zn_cdeg_from_square_key(
                         m.square_key) * 600;
        int64_t dz = ext_circ_marcmin(zn, r->zn_marcmin);
        zn_sum += dz;
        if (dz > zn_worst) zn_worst = dz;
    }
    geom_fail |= ext_report("reduce Hc", (int)CROSS_REDUCE_ROW_COUNT,
                            hc_worst, hc_sum, CROSS_GATE_HC);
    if (zn_invalid) { printf("reduce Zn: zn_valid == 0  FAIL\n");
                      geom_fail = 1; }
    else geom_fail |= ext_report("reduce Zn", (int)CROSS_REDUCE_ROW_COUNT,
                                 zn_worst, zn_sum, CROSS_GATE_ZN);

    /* Two-body fixes: identical GPs, identical milli-arcmin observed
     * altitudes, identical DR to break the two-intersection ambiguity.
     * The oracle intersects the two circles of position directly; this
     * library iterates a least-squares St-Hilaire fix from the DR.
     * Compared at the centidegree lat/lon readout (x600); longitude is
     * compared circularly (a dateline scenario is in the rows). */
    int64_t lat_worst = 0, lat_sum = 0, lon_worst = 0, lon_sum = 0;
    int fix_invalid = 0;
    for (size_t i = 0; i < CROSS_FIX_ROW_COUNT; i++) {
        const cross_fix_row_t *r = &cross_fix_rows[i];
        astro_nav_unitvec_t b1, b2, hint;
        astro_nav_fix_result_t fx;
        astro_nav_unitvec_from_cdeg(r->dec1_cdeg, -r->gha1_cdeg, &b1);
        astro_nav_unitvec_from_cdeg(r->dec2_cdeg, -r->gha2_cdeg, &b2);
        astro_nav_unitvec_from_cdeg(r->dr_lat_cdeg, r->dr_lon_cdeg,
                                    &hint);
        astro_nav_fix_two_body(
            &b1, astro_nav_sin_q30_from_marcmin(r->ho1_marcmin),
            &b2, astro_nav_sin_q30_from_marcmin(r->ho2_marcmin),
            &hint, &fx);
        if (!fx.valid) { fix_invalid = 1; continue; }
        int32_t lat_c, lon_c;
        astro_nav_latlon_cdeg_from_unitvec(&fx.position, &lat_c, &lon_c);
        int64_t dlat = (int64_t)lat_c * 600 - r->lat_marcmin;
        if (dlat < 0) dlat = -dlat;
        int64_t dlon = ext_circ_marcmin((int64_t)lon_c * 600,
                                        r->lon_marcmin);
        lat_sum += dlat;
        lon_sum += dlon;
        if (dlat > lat_worst) lat_worst = dlat;
        if (dlon > lon_worst) lon_worst = dlon;
    }
    if (fix_invalid) { printf("fix: a two-body fix did not converge"
                              "  FAIL\n"); geom_fail = 1; }
    geom_fail |= ext_report("fix lat", (int)CROSS_FIX_ROW_COUNT,
                            lat_worst, lat_sum, CROSS_GATE_FIX_LAT);
    geom_fail |= ext_report("fix lon", (int)CROSS_FIX_ROW_COUNT,
                            lon_worst, lon_sum, CROSS_GATE_FIX_LON);

    printf("\nagreement by independent lineage (not repository"
           " count):\n");
    printf("  libastro / XEphem analytic theories: %s\n",
           astro_fail ? "FAIL" : "PASS");
    printf("  circle-intersection fix geometry:    %s\n",
           geom_fail ? "FAIL" : "PASS");
    return astro_fail | geom_fail;
}

/* --scenario-check: the COMPOSITION -- sky -> corrected altitude ->
 * multi-body fix -> recovered position -- against external answers,
 * the one thing --ephemeris-check and --external-check never exercise
 * (they gate each stage in isolation; nothing checks that feeding the
 * stored altitude inputs through the whole pipeline lands on the right
 * spot).
 * sight_scenarios.h (transcribed/generated offline by
 * tools/make_sight_scenarios.py; numbers only) carries three families:
 *
 *   1. Recovery scenarios. A known navigation-sphere lat/lon, an
 *      instant, and 2-5 bodies (stars, the Sun, the Moon -- including
 *      the classic daytime Sun-Moon cut) whose truth Ho is asin(obs_unit
 *      . body_unit) in the library's model (body_unit the Skyfield/DE421
 *      geocentric apparent place in ITRS, obs_unit the local zenith).
 *      Surface-observer topocentric altaz is deliberately not used: it
 *      still contains parallax already removed from the Ho consumed by
 *      the fix, up to about a degree for the Moon. We build
 *      the bodies from OUR ephemeris at the instant, feed the stored
 *      Ho, run the fix, and measure recovered-vs-truth position: the
 *      almanac error propagated through the fix geometry.
 *   2. The alinnman published fixes: Chicago (two Sun + Vega, 2024-05,
 *      answer 41 deg 51'N 87 deg 39'W) and off Tunis (Capella + Moon +
 *      Vega, 2024-09, answer 36 deg 45'11"N 10 deg 13'8"E) -- the
 *      latter a published RAW MOON SIGHT whose ~40' of lunar parallax
 *      our Moon chain removes using our own ephemeris HP. Raw sextant
 *      altitudes through OUR correction chain (exactly the corrections
 *      their Sight() applied), then our n-body fix, measured against
 *      their published positions.
 *   3. rgleason INTERCEPT_SIGHTS Sun rows: our Sun direction vs their
 *      published GP (chord), our Hs->Ho vs their Ho, our Hc/Zn/intercept
 *      vs theirs (reducing at their DR against their published GP, so
 *      this isolates the reduction from the ephemeris).
 *
 * Every gate is set just above the worst MEASURED on this host, with the
 * measurement inline (the repo's EPHEM_GATE pattern). */
#define SCEN_GATE_RECOVERY  1000 /* milli-arcmin = nm x 1000 (measured
                                  * worst 847): star/Sun/Moon almanac
                                  * error (up to ~1.2' per body) propagated
                                  * through the fix geometry -- averaged
                                  * down to under a nautical mile. The
                                  * worst row is a Moon+star round whose
                                  * greedy pick pairs Arcturus (the
                                  * catalog's largest proper motion, ~1.2'
                                  * of it accumulated by 2028) with
                                  * Polaris' along-parallel LOP; the Moon
                                  * itself is ~0.02' off there. */
#define SCEN_GATE_ALINNMAN   600 /* milli-arcmin (measured worst 308):
                                  * our fix vs their published position.
                                  * 0.3 nm -- their sextant rounding, and
                                  * NOT a geocentric-latitude offset: the
                                  * fit of geodetic-horizon altitudes
                                  * recovers the local-zenith direction,
                                  * whose polar angle is geodetic latitude
                                  * (empirically confirmed; see the tool +
                                  * HOWTO 7). */
#define SCEN_GATE_ALINNMAN2  300 /* milli-arcmin (measured worst 31):
                                  * the raw-Moon-sight fix lands 0.03 nm
                                  * from the published place -- the Moon
                                  * row rides on our ephemeris HP (~61'
                                  * near perigee), so this gates lunar
                                  * parallax through a real published
                                  * sight. Headroom: the cdeg truth
                                  * macros alone quantize the published
                                  * place by up to ~0.2'. */
#define SCEN_GATE_SUN        700 /* milli-arcmin (measured worst 320):
                                  * our Sun direction vs rgleason's GP.
                                  * Gated at the Sun model's own 0.6' band
                                  * (EXT_GATE_SUN, worst 607 over its 257
                                  * rows) -- the same ephemeris. */
#define SCEN_GATE_HO         300 /* milli-arcmin (measured worst 100):
                                  * our Hs->Ho vs rgleason's. The gap is
                                  * our Bennett refraction + 1.76*sqrt dip
                                  * + USNO SD/HP vs their Saemundsson
                                  * refraction + 1.758 dip + almanac SD/HP
                                  * -- and it is only 0.10', i.e. the two
                                  * correction chains agree to a tenth of
                                  * a mile across these rows (a finding:
                                  * Bennett and Saemundsson barely differ
                                  * above ~12 deg). */
#define SCEN_GATE_HC         500 /* milli-arcmin (measured worst 400):
                                  * our Hc vs theirs. Our Hc crosses the
                                  * human boundary as centidegrees, so up
                                  * to 0.3' (300 milli-arcmin) is pure
                                  * cdeg quantization of a full-precision
                                  * sin(Hc); the rest is their gha/dec ->
                                  * Q2.30 baking. The reduction math
                                  * itself agrees (Zn worst 0.01 cdeg). */
#define SCEN_GATE_ZN           6 /* centidegrees (measured worst 1):
                                  * our Zn vs theirs, at Path C's ~0.6
                                  * cdeg square-key azimuth resolution. */
#define SCEN_GATE_INTERCEPT    5 /* tenths of a nm (measured worst 4):
                                  * our (their Ho - our Hc) intercept vs
                                  * theirs; the 0.3' Hc quantization above
                                  * is 3 tenths of a mile, which bounds
                                  * this. */

/* Build a rough DR seed a fixed ~0.4 deg off a truth position, so the
 * fix converges from realistic dead reckoning (and the two-body hint
 * still picks the intersection nearest truth). */
static void scen_seed(int32_t lat_cdeg, int32_t lon_cdeg,
                      astro_nav_unitvec_t *seed)
{
    int32_t slat = lat_cdeg + 30;
    int32_t slon = lon_cdeg + 40;
    if (slon > CDEG_PER_HALFTURN) slon -= CDEG_PER_TURN;
    if (slon < -CDEG_PER_HALFTURN) slon += CDEG_PER_TURN;
    astro_nav_unitvec_from_cdeg(slat, slon, seed);
}

static void scen_build_body(int32_t body, int64_t ut1_ms,
                            int32_t tt_minus_ut1_ms,
                            astro_nav_unitvec_t *out)
{
    if (body == -2)
        astro_nav_moon_earthfixed(ut1_ms, tt_minus_ut1_ms, out);
    else if (body < 0)
        astro_nav_sun_earthfixed(ut1_ms, tt_minus_ut1_ms, out);
    else
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[body].j2000,
                                          ut1_ms, out);
}

static int run_scenario_check(void)
{
    int fail = 0;

    printf("scenario check: end-to-end position recovery vs external"
           " answers\n(sky -> corrected altitude -> multi-body fix ->"
           " position; the\ncomposition the stage gates never see; see"
           " sight_scenarios.h)\n\n");

    /* Family 1: recovery scenarios. */
    int64_t worst = 0, sum = 0;
    size_t worst_i = 0;
    int any_invalid = 0;
    for (size_t i = 0; i < SCENARIO_ROW_COUNT; i++) {
        const scenario_row_t *r = &scenario_rows[i];
        astro_nav_unitvec_t bodies[5];
        int32_t sin_ho[5];
        for (int b = 0; b < r->n; b++) {
            scen_build_body(r->body[b], r->ut1_ms, r->tt_minus_ut1_ms,
                            &bodies[b]);
            sin_ho[b] = astro_nav_sin_q30_from_marcmin(r->ho_marcmin[b]);
        }
        astro_nav_unitvec_t seed;
        scen_seed(r->lat_cdeg, r->lon_cdeg, &seed);

        astro_nav_unitvec_t pos;
        int valid;
        if (r->two_body) {
            astro_nav_fix_result_t f2;
            astro_nav_fix_two_body(&bodies[0], sin_ho[0],
                                   &bodies[1], sin_ho[1], &seed, &f2);
            pos = f2.position;
            valid = f2.valid;
        } else {
            astro_nav_fixn_result_t fn;
            astro_nav_fix_n_body(bodies, sin_ho, r->n, &seed, &fn);
            pos = fn.position;
            valid = fn.valid;
        }

        astro_nav_unitvec_t truth;
        astro_nav_unitvec_from_cdeg(r->lat_cdeg, r->lon_cdeg, &truth);
        int64_t m = ext_chord_marcmin(pos.x, pos.y, pos.z,
                                      truth.x, truth.y, truth.z);
        if (!valid) { any_invalid = 1; fail = 1; }
        sum += m;
        if (m > worst) { worst = m; worst_i = i; }
    }
    printf("recovery      %2d scenarios (2-5 bodies, both hemispheres,"
           " 2000-2035)\n", (int)SCENARIO_ROW_COUNT);
    printf("              mean ");
    print_arcmin(sum / (int64_t)SCENARIO_ROW_COUNT);
    printf("   worst ");
    print_arcmin(worst);
    {
        int32_t wlat, wlon;
        wlat = scenario_rows[worst_i].lat_cdeg;
        wlon = scenario_rows[worst_i].lon_cdeg;
        printf(" (lat "); print_cdeg(wlat);
        printf(" lon "); print_cdeg(wlon); printf(")\n");
    }
    printf("              gate <= ");
    print_arcmin(SCEN_GATE_RECOVERY);
    if (any_invalid) printf("  FAIL (a fix did not converge)\n");
    else if (worst <= SCEN_GATE_RECOVERY) printf("  PASS\n");
    else { printf("  FAIL\n"); fail = 1; }

    /* Family 2: the alinnman published fixes. Each raw sextant
     * altitude gets exactly the corrections their Sight() applied
     * (no ie/dip/SD, standard 10 C / 1010 mb refraction; the Moon
     * row additionally sheds its ~40' of lunar parallax -- through
     * OUR Moon chain with OUR ephemeris HP at the instant, where
     * theirs looked HP up from its almanac), then our n-body fix. */
    {
        static const struct {
            const alinnman_sight_t *sights;
            int n;
            int32_t seed_lat, seed_lon, truth_lat, truth_lon;
            int64_t gate;
            const char *name, *title, *published;
        } fixes[2] = {
            { alinnman_sights, (int)ALINNMAN_SIGHT_COUNT,
              ALINNMAN_SEED_LAT_CDEG, ALINNMAN_SEED_LON_CDEG,
              ALINNMAN_TRUTH_LAT_CDEG, ALINNMAN_TRUTH_LON_CDEG,
              SCEN_GATE_ALINNMAN, "alinnman",
              "Chicago two-Sun + Vega, 2024-05 (github.com/alinnman)",
              "41.85 -87.65" },
            { alinnman2_sights, (int)ALINNMAN2_SIGHT_COUNT,
              ALINNMAN2_SEED_LAT_CDEG, ALINNMAN2_SEED_LON_CDEG,
              ALINNMAN2_TRUTH_LAT_CDEG, ALINNMAN2_TRUTH_LON_CDEG,
              SCEN_GATE_ALINNMAN2, "alinnman2",
              "Capella + raw Moon + Vega off Tunis, 2024-09",
              "36.75 10.22" },
        };
        for (size_t f = 0; f < 2; f++) {
            astro_nav_unitvec_t bodies[5];
            int32_t sin_ho[5];
            for (int i = 0; i < fixes[f].n; i++) {
                const alinnman_sight_t *s = &fixes[f].sights[i];
                int64_t ho;
                if (s->body == -2) {
                    int32_t km, sd, hp;
                    astro_nav_moon_distance(s->ut1_ms,
                                            s->tt_minus_ut1_ms,
                                            &km, &sd, &hp);
                    ho = astro_nav_correct_altitude_moon_tp_marcmin(
                        s->hs_marcmin, 0, 0, hp, sd, 0, 10, 1010);
                } else {
                    ho = astro_nav_correct_altitude_tp_marcmin(
                        s->hs_marcmin, 0, 0, 0, 0, 0, 10, 1010);
                }
                sin_ho[i] = astro_nav_sin_q30_from_marcmin(ho);
                scen_build_body(s->body, s->ut1_ms, s->tt_minus_ut1_ms,
                                &bodies[i]);
            }
            astro_nav_unitvec_t seed;
            astro_nav_unitvec_from_cdeg(fixes[f].seed_lat,
                                        fixes[f].seed_lon, &seed);
            astro_nav_fixn_result_t fn;
            astro_nav_fix_n_body(bodies, sin_ho, fixes[f].n, &seed, &fn);
            astro_nav_unitvec_t truth;
            astro_nav_unitvec_from_cdeg(fixes[f].truth_lat,
                                        fixes[f].truth_lon, &truth);
            int32_t flat, flon;
            astro_nav_latlon_cdeg_from_unitvec(&fn.position,
                                               &flat, &flon);
            int64_t m = ext_chord_marcmin(fn.position.x, fn.position.y,
                                          fn.position.z,
                                          truth.x, truth.y, truth.z);
            printf("\n%-13s %s\n", fixes[f].name, fixes[f].title);
            printf("              recovered lat "); print_cdeg(flat);
            printf(" lon "); print_cdeg(flon);
            printf("   published %s\n", fixes[f].published);
            printf("              miss ");
            print_arcmin(m);
            printf("   gate <= ");
            print_arcmin(fixes[f].gate);
            if (fn.valid && m <= fixes[f].gate) printf("  PASS\n");
            else { printf("  FAIL\n"); fail = 1; }
        }
    }

    /* Family 3: rgleason INTERCEPT_SIGHTS Sun rows. */
    {
        int64_t sun_worst = 0, sun_sum = 0;
        int64_t ho_worst = 0, ho_sum = 0;
        int64_t hc_worst = 0, hc_sum = 0;
        int64_t zn_worst = 0, zn_sum = 0;
        int64_t ic_worst = 0, ic_sum = 0;
        for (size_t i = 0; i < INTERCEPT_ROW_COUNT; i++) {
            const intercept_row_t *r = &intercept_rows[i];

            /* (a) our Sun direction vs their published GP. */
            astro_nav_unitvec_t v;
            astro_nav_sun_earthfixed(r->ut1_ms, r->tt_minus_ut1_ms, &v);
            int64_t ms = ext_chord_marcmin(v.x, v.y, v.z,
                                           r->bx, r->by, r->bz);
            sun_sum += ms;
            if (ms > sun_worst) sun_worst = ms;

            /* (b) our Hs -> Ho vs theirs. */
            int32_t d_uau, sd, hp;
            astro_nav_sun_distance(r->ut1_ms, r->tt_minus_ut1_ms,
                                   &d_uau, &sd, &hp);
            int64_t ho = astro_nav_correct_altitude_tp_marcmin(
                r->hs_marcmin, r->ie_marcmin, r->eye_cm, hp, sd,
                r->limb, r->temp_c, r->pressure_mb);
            int64_t mho = ho - r->ho_marcmin;
            if (mho < 0) mho = -mho;
            ho_sum += mho;
            if (mho > ho_worst) ho_worst = mho;

            /* (c) reduce at their DR against their published GP:
             * isolates the reduction from our ephemeris. */
            astro_nav_unitvec_t obs, body;
            astro_nav_unitvec_from_cdeg(r->dr_lat_cdeg, r->dr_lon_cdeg,
                                        &obs);
            body.x = r->bx; body.y = r->by; body.z = r->bz;
            astro_nav_machine_sight_t sight;
            astro_nav_reduce_method_c(&obs, &body, &sight);
            int64_t hc = astro_nav_cdeg_to_marcmin(
                astro_nav_hc_cdeg_from_sin_q30(sight.sin_hc_q30));
            int64_t mhc = hc - r->hc_marcmin;
            if (mhc < 0) mhc = -mhc;
            hc_sum += mhc;
            if (mhc > hc_worst) hc_worst = mhc;

            int32_t zn = astro_nav_zn_cdeg_from_square_key(
                sight.square_key);
            /* An undefined azimuth must FAIL the gate, not sail
             * through it as a zero-error row. */
            int64_t mzn = sight.zn_valid
                ? circular_diff_cdeg(zn, r->zn_cdeg)
                : SCEN_GATE_ZN + 1;
            zn_sum += mzn;
            if (mzn > zn_worst) zn_worst = mzn;

            /* our intercept = (their Ho) - (our Hc), tenths of a nm,
             * + toward; compared to their published intercept. */
            int64_t ic_ours = (r->ho_marcmin - hc + 50) / 100;
            if (r->ho_marcmin - hc < 0)
                ic_ours = (r->ho_marcmin - hc - 50) / 100;
            int64_t mic = ic_ours - r->intercept_tenths_nm;
            if (mic < 0) mic = -mic;
            ic_sum += mic;
            if (mic > ic_worst) ic_worst = mic;
        }
        printf("\nrgleason      %2d INTERCEPT_SIGHTS Sun rows"
               " (github.com/rgleason)\n", (int)INTERCEPT_ROW_COUNT);
        fail |= ext_report("  Sun GP", (int)INTERCEPT_ROW_COUNT,
                           sun_worst, sun_sum, SCEN_GATE_SUN);
        fail |= ext_report("  Hs->Ho", (int)INTERCEPT_ROW_COUNT,
                           ho_worst, ho_sum, SCEN_GATE_HO);
        fail |= ext_report("  Hc", (int)INTERCEPT_ROW_COUNT,
                           hc_worst, hc_sum, SCEN_GATE_HC);
        printf("  Zn          %2d rows   mean %lld.%02lld deg"
               "   worst %lld.%02lld deg   gate <= 0.%02d deg",
               (int)INTERCEPT_ROW_COUNT,
               (long long)(zn_sum / (int64_t)INTERCEPT_ROW_COUNT / 100),
               (long long)(zn_sum / (int64_t)INTERCEPT_ROW_COUNT % 100),
               (long long)(zn_worst / 100), (long long)(zn_worst % 100),
               SCEN_GATE_ZN);
        if (zn_worst <= SCEN_GATE_ZN) printf("  PASS\n");
        else { printf("  FAIL\n"); fail = 1; }
        printf("  intercept   %2d rows   mean %lld.%lld nm"
               "   worst %lld.%lld nm   gate <= 0.%d nm",
               (int)INTERCEPT_ROW_COUNT,
               (long long)(ic_sum / (int64_t)INTERCEPT_ROW_COUNT / 10),
               (long long)(ic_sum / (int64_t)INTERCEPT_ROW_COUNT % 10),
               (long long)(ic_worst / 10), (long long)(ic_worst % 10),
               SCEN_GATE_INTERCEPT);
        if (ic_worst <= SCEN_GATE_INTERCEPT) printf("  PASS\n");
        else { printf("  FAIL\n"); fail = 1; }
    }

    return fail;
}

/* Sextant altitude corrections, Hs -> Ho, in milli-arcminutes. A
 * typical 0.1' sextant reading is 100 milli-arcmin, finer than a
 * centidegree. */
static void print_marcmin_angle(int64_t marcmin)
{
    const char *sign = marcmin < 0 ? "-" : "";
    if (marcmin < 0) marcmin = -marcmin;
    int64_t deg = marcmin / 60000;
    int64_t tenths = (marcmin % 60000 + 50) / 100;
    if (tenths >= 600) { deg++; tenths -= 600; }
    printf("%s%lld deg %lld.%lld'", sign, (long long)deg,
           (long long)(tenths / 10), (long long)(tenths % 10));
}

/* The chain computation and printout shared by --correct (manual HP
 * and SD) and --correct-sun (HP and SD from the ephemeris). The
 * atmosphere line prints only when the caller departed from the
 * standard 10 C / 1010 mb, at which the tp chain is bit-identical to
 * the standard one -- so standard-atmosphere output is untouched. */
static int print_correction_chain(int32_t hs, int32_t ie,
                                  int32_t eye_cm, int32_t hp,
                                  int32_t sd, int32_t limb,
                                  int32_t temp_c, int32_t pressure_mb,
                                  int moon)
{
    int64_t dip  = astro_nav_dip_marcmin(eye_cm);
    int64_t ha   = (int64_t)hs + ie - dip;
    int64_t refr = astro_nav_refraction_tp_marcmin(ha, temp_c,
                                                   pressure_mb);
    int64_t par  = astro_nav_parallax_marcmin(ha, hp);
    int64_t ho   = moon
        ? astro_nav_correct_altitude_moon_tp_marcmin(hs, ie, eye_cm,
                                                     hp, sd, limb,
                                                     temp_c,
                                                     pressure_mb)
        : astro_nav_correct_altitude_tp_marcmin(hs, ie, eye_cm,
                                                hp, sd, limb,
                                                temp_c, pressure_mb);

    if (temp_c != 10 || pressure_mb != 1010)
        printf("atmosphere: %ld C, %ld mb   (refraction scaled from"
               " the 10 C / 1010 mb standard)\n",
               (long)temp_c, (long)pressure_mb);
    printf("Hs:  "); print_marcmin_angle(hs);
    printf("   (index %+ld, dip -%lld, refraction -%lld,"
           " parallax %+lld, SD %s",
           (long)ie, (long long)dip, (long long)refr, (long long)par,
           limb > 0 ? "+lower" : (limb < 0 ? "-upper" : "none"));
    if (moon && limb != 0)
        printf(" augmented %+lld",
               (long long)astro_nav_moon_augmentation_marcmin(
                   ha - refr + par, hp, sd));
    printf(" milli-arcmin)\n");
    printf("Ho:  "); print_marcmin_angle(ho);
    printf("   = %lld milli-arcmin   sin(Ho) = %ld/2^30\n",
           (long long)ho, (long)astro_nav_sin_q30_from_marcmin(ho));
    return 0;
}

/* Shared by --correct and --correct-sun: the two optional trailing
 * atmosphere arguments. Both present or both absent; defaults are the
 * standard atmosphere, at which the output is bit-identical to the
 * pre-atmosphere command. */
static int parse_atmosphere(char **argv, int index, int have,
                            int32_t *temp_c, int32_t *pressure_mb)
{
    *temp_c = 10;
    *pressure_mb = 1010;
    if (!have) return 1;
    if (!parse_i32(argv[index], temp_c)) {
        fprintf(stderr, "error: TEMP_C must be a base-10 int32"
                        " value\n");
        return 0;
    }
    if (!parse_i32(argv[index + 1], pressure_mb)) {
        fprintf(stderr, "error: PRESSURE_MB must be a base-10 int32"
                        " value\n");
        return 0;
    }
    int bad = 0;
    bad |= cli_range_error("TEMP_C", *temp_c, -60, 60);
    bad |= cli_range_error("PRESSURE_MB", *pressure_mb, 800, 1100);
    return !bad;
}

static int run_correct_cli(int argc, char **argv)
{
    if (argc != 8 && argc != 10) {
        fprintf(stderr, "error: --correct expects HS IE EYE_CM HP SD"
                        " LIMB [TEMP_C PRESSURE_MB]\n");
        print_usage(argv[0]);
        return 2;
    }

    const char *names[] = { "HS", "IE", "EYE_CM", "HP", "SD", "LIMB" };
    int32_t v[6];
    for (int i = 0; i < 6; i++) {
        if (!parse_i32(argv[i + 2], &v[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32 value\n",
                    names[i]);
            return 2;
        }
    }
    int32_t temp_c, pressure_mb;
    if (!parse_atmosphere(argv, 8, argc == 10, &temp_c, &pressure_mb))
        return 2;
    int bad = 0;
    bad |= cli_range_error("HS", v[0], -5400000, 5400000);
    bad |= cli_range_error("IE", v[1], -600000, 600000);
    bad |= cli_range_error("EYE_CM", v[2], 0, 10000);
    bad |= cli_range_error("HP", v[3], 0, 70000);
    bad |= cli_range_error("SD", v[4], 0, 20000);
    bad |= cli_range_error("LIMB", v[5], -1, 1);
    if (bad) return 2;

    return print_correction_chain(v[0], v[1], v[2], v[3], v[4], v[5],
                                  temp_c, pressure_mb, 0);
}

/* --correct with the Sun's SD and HP computed from the sight instant
 * (astro_nav_sun_distance) instead of copied from the daily pages. */
static int run_correct_sun_cli(int argc, char **argv)
{
    if (argc != 8 && argc != 10) {
        fprintf(stderr, "error: --correct-sun expects HS IE EYE_CM"
                        " UT1_MS TT_MINUS_UT1_MS LIMB"
                        " [TEMP_C PRESSURE_MB]\n");
        print_usage(argv[0]);
        return 2;
    }

    const char *names[] = { "HS", "IE", "EYE_CM" };
    int32_t v[3];
    for (int i = 0; i < 3; i++) {
        if (!parse_i32(argv[i + 2], &v[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32"
                            " value\n", names[i]);
            return 2;
        }
    }
    int64_t ms, ttmut1;
    if (!parse_sun_times(argv[5], argv[6], &ms, &ttmut1)) return 2;
    int32_t limb;
    if (!parse_i32(argv[7], &limb)) {
        fprintf(stderr, "error: LIMB must be a base-10 int32 value\n");
        return 2;
    }
    int32_t temp_c, pressure_mb;
    if (!parse_atmosphere(argv, 8, argc == 10, &temp_c, &pressure_mb))
        return 2;
    int bad = 0;
    bad |= cli_range_error("HS", v[0], -5400000, 5400000);
    bad |= cli_range_error("IE", v[1], -600000, 600000);
    bad |= cli_range_error("EYE_CM", v[2], 0, 10000);
    bad |= cli_range_error("LIMB", limb, -1, 1);
    if (bad) return 2;

    int32_t r_uau, sd, hp;
    astro_nav_sun_distance(ms, ttmut1, &r_uau, &sd, &hp);
    printf("Sun at UT1 J2000 %+lld ms:  distance %ld micro-AU"
           "   SD ", (long long)ms, (long)r_uau);
    print_marcmin(sd);
    printf("   HP ");
    print_marcmin(hp);
    printf("\n");
    return print_correction_chain(v[0], v[1], v[2], hp, sd, limb,
                                  temp_c, pressure_mb, 0);
}

/* --correct with the Moon's SD and HP computed from the sight instant
 * (astro_nav_moon_distance), through the Moon chain, which also
 * augments the semidiameter. For the Moon the corrections are the
 * whole game: HP is 54-61' -- the largest correction in celestial
 * navigation, some 400x the Sun's ~0.15' -- and an uncorrected Moon
 * sight is wrong by up to a degree. */
static int run_correct_moon_cli(int argc, char **argv)
{
    if (argc != 8 && argc != 10) {
        fprintf(stderr, "error: --correct-moon expects HS IE EYE_CM"
                        " UT1_MS TT_MINUS_UT1_MS LIMB"
                        " [TEMP_C PRESSURE_MB]\n");
        print_usage(argv[0]);
        return 2;
    }

    const char *names[] = { "HS", "IE", "EYE_CM" };
    int32_t v[3];
    for (int i = 0; i < 3; i++) {
        if (!parse_i32(argv[i + 2], &v[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32"
                            " value\n", names[i]);
            return 2;
        }
    }
    int64_t ms, ttmut1;
    if (!parse_moon_times(argv[5], argv[6], &ms, &ttmut1)) return 2;
    int32_t limb;
    if (!parse_i32(argv[7], &limb)) {
        fprintf(stderr, "error: LIMB must be a base-10 int32 value\n");
        return 2;
    }
    int32_t temp_c, pressure_mb;
    if (!parse_atmosphere(argv, 8, argc == 10, &temp_c, &pressure_mb))
        return 2;
    int bad = 0;
    bad |= cli_range_error("HS", v[0], -5400000, 5400000);
    bad |= cli_range_error("IE", v[1], -600000, 600000);
    bad |= cli_range_error("EYE_CM", v[2], 0, 10000);
    bad |= cli_range_error("LIMB", limb, -1, 1);
    if (bad) return 2;

    int32_t km, sd, hp;
    astro_nav_moon_distance(ms, ttmut1, &km, &sd, &hp);
    printf("Moon at UT1 J2000 %+lld ms:  distance %ld km"
           "   SD ", (long long)ms, (long)km);
    print_marcmin(sd);
    printf("   HP ");
    print_marcmin(hp);
    printf("\n");
    return print_correction_chain(v[0], v[1], v[2], hp, sd, limb,
                                  temp_c, pressure_mb, 1);
}

/* --predict: the sight run backward -- from a KNOWN position (GPS)
 * and instant to what the correction model says the sextant should
 * read against an unobstructed natural sea horizon. Prints Zn (where
 * to face) and Hs at IE = 0; given the
 * actual reading, predicted-minus-observed IS the implied aggregate
 * correction, already in the sign convention the --correct family
 * expects (chain(hs, ie) == chain(hs + ie, 0), pinned in self-test
 * [11]) -- "aggregate" because one shot folds personal, timing,
 * position-grid, and almanac-model error in with the index error;
 * only the repeatable part is IE. The milli-arcminute digits are
 * computational resolution, not physical accuracy. Three refusals,
 * all exit 1, in order: Hc below -2 deg (refraction model
 * meaningless), no reading within +-90 deg produces the altitude
 * (fail closed near the zenith), predicted reading negative (limb
 * below the visible horizon). This is a known-position check against
 * a celestial body AND the horizon, and doubles as a live plausibility
 * gate for a single sight. Artificial and land horizons require
 * different boundary corrections and are outside this mode. */
static int run_predict_cli(int argc, char **argv)
{
    if (argc < 8 || argc > 11) {
        fprintf(stderr, "error: --predict expects LAT LON BODY UT1_MS"
                        " EYE_CM LIMB [HS] [TEMP_C PRESSURE_MB]\n");
        print_usage(argv[0]);
        return 2;
    }
    int have_hs = (argc == 9 || argc == 11);

    int32_t lat, lon;
    if (!parse_i32(argv[2], &lat) || !parse_i32(argv[3], &lon)) {
        fprintf(stderr, "error: LAT/LON must be base-10 int32"
                        " values\n");
        return 2;
    }
    int64_t ms;
    if (!parse_i64(argv[5], &ms)
        || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
        || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: UT1_MS must be within +- 3155760000000"
                        " (100 years of J2000)\n");
        return 2;
    }
    int32_t eye_cm, limb;
    if (!parse_i32(argv[6], &eye_cm) || !parse_i32(argv[7], &limb)) {
        fprintf(stderr, "error: EYE_CM/LIMB must be base-10 int32"
                        " values\n");
        return 2;
    }
    int bad = 0;
    bad |= cli_range_error("LAT", lat, -CDEG_PER_QUARTER,
                           CDEG_PER_QUARTER);
    bad |= cli_range_error("LON", lon, -CDEG_PER_HALFTURN,
                           CDEG_PER_HALFTURN);
    bad |= cli_range_error("EYE_CM", eye_cm, 0, 10000);
    bad |= cli_range_error("LIMB", limb, -1, 1);
    if (bad) return 2;
    int64_t hs_obs = 0;
    if (have_hs
        && (!parse_i64(argv[8], &hs_obs)
            || hs_obs < -5400000 || hs_obs > 5400000)) {
        fprintf(stderr, "error: HS must be milli-arcminutes in"
                        " [-5400000, 5400000]\n");
        return 2;
    }
    int32_t temp_c, pressure_mb;
    if (!parse_atmosphere(argv, have_hs ? 9 : 8, argc >= 10,
                          &temp_c, &pressure_mb))
        return 2;

    /* BODY, in the n-sight modes' grammar: star index,
     * sun:TT_MINUS_UT1_MS, or moon:TT_MINUS_UT1_MS. */
    astro_nav_unitvec_t body;
    int64_t hp = 0, sd = 0;
    int moon = 0;
    if (strncmp(argv[4], "sun:", 4) == 0
        || strncmp(argv[4], "moon:", 5) == 0) {
        moon = argv[4][0] == 'm';
        int64_t ttmut1;
        if (!parse_i64(argv[4] + (moon ? 5 : 4), &ttmut1)
            || ttmut1 < -ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS
            || ttmut1 > ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS) {
            fprintf(stderr, "error: %s TT_MINUS_UT1_MS must be"
                            " milliseconds in [-600000, 600000]"
                            " (~69200 in 2026)\n",
                    moon ? "moon:" : "sun:");
            return 2;
        }
        int32_t dist, sd32, hp32;
        if (moon) {
            astro_nav_moon_earthfixed(ms, ttmut1, &body);
            astro_nav_moon_distance(ms, ttmut1, &dist, &sd32, &hp32);
            printf("Moon at UT1 J2000 %+lld ms (TT - UT1 = %lld ms):"
                   "  distance %ld km   SD ",
                   (long long)ms, (long long)ttmut1, (long)dist);
        } else {
            astro_nav_sun_earthfixed(ms, ttmut1, &body);
            astro_nav_sun_distance(ms, ttmut1, &dist, &sd32, &hp32);
            printf("Sun at UT1 J2000 %+lld ms (TT - UT1 = %lld ms):"
                   "  distance %ld micro-AU   SD ",
                   (long long)ms, (long long)ttmut1, (long)dist);
        }
        print_marcmin(sd32);
        printf("   HP ");
        print_marcmin(hp32);
        printf("\n");
        hp = hp32;
        sd = sd32;
    } else {
        int32_t star;
        if (!parse_i32(argv[4], &star) || star < 0
            || star >= ASTRO_NAV_STAR_COUNT) {
            fprintf(stderr, "error: BODY must be a star index 0..%d,"
                            " sun:TT_MINUS_UT1_MS, or"
                            " moon:TT_MINUS_UT1_MS\n",
                    ASTRO_NAV_STAR_COUNT - 1);
            return 2;
        }
        if (limb != 0) {
            fprintf(stderr, "error: LIMB must be 0 for a star (no"
                            " visible disc)\n");
            return 2;
        }
        astro_nav_celestial_to_earthfixed(&astro_nav_stars[star].j2000,
                                          ms, &body);
        printf("%s (star %ld) at UT1 J2000 %+lld ms\n",
               astro_nav_stars[star].name, (long)star, (long long)ms);
    }

    astro_nav_unitvec_t observer;
    astro_nav_unitvec_from_cdeg(lat, lon, &observer);
    astro_nav_machine_sight_t sight;
    astro_nav_reduce_method_c(&observer, &body, &sight);

    int64_t hc = predict_asin_marcmin(sight.sin_hc_q30);
    printf("observer: lat "); print_cdeg(lat);
    printf(" deg   lon "); print_cdeg(lon);
    printf(" deg   eye %ld cm\n", (long)eye_cm);
    printf("Hc:  "); print_marcmin_angle(hc);
    printf("   = %lld milli-arcmin   (machine sin_hc=%ld/2^30)\n",
           (long long)hc, (long)sight.sin_hc_q30);
    printf("Zn:  ");
    if (sight.zn_valid) {
        print_cdeg(astro_nav_zn_cdeg_from_square_key(sight.square_key));
        printf(" deg true   (face here)\n");
    } else {
        printf("undefined (body at zenith/nadir, or observer at a"
               " pole)\n");
    }

    /* Refusal 1: too far below the geometric horizon for the chain's
     * refraction model to mean anything. NOT hc < 0: refraction plus
     * an upper limb can hold a positive sextant reading on a center
     * that is geometrically just below the horizon, so near zero the
     * decision belongs to the predicted READING, not to Hc. */
    if (hc < -120000) {
        printf("body below the horizon at this position and instant --"
               " no sextant prediction\n");
        return 1;
    }

    int64_t chain_err;
    int64_t hs_pred = predict_hs_marcmin(hc, eye_cm, hp, sd, limb,
                                         temp_c, pressure_mb, moon,
                                         &chain_err);
    /* Refusal 2, fail closed: no reading within the +-90 deg domain
     * produces this altitude (dip can push the required reading past
     * 90 deg near the zenith). A nearest fit is not a calibration
     * answer. */
    if (chain_err > 1) {
        printf("no prediction: no sextant reading within +-90 deg"
               " produces this altitude through the correction chain"
               " (closest misses by %lld milli-arcmin)\n",
               (long long)chain_err);
        return 1;
    }
    /* Refusal 3: the reading exists but is negative -- the limb is
     * still below the VISIBLE horizon, and a sextant brings a body
     * down TO the horizon. */
    if (hs_pred < 0) {
        printf("%s below the visible horizon at this position and"
               " instant (the idealized model requires %lld"
               " milli-arcmin) -- no sextant prediction\n",
               limb > 0 ? "lower limb"
                        : (limb < 0 ? "upper limb" : "body"),
               (long long)hs_pred);
        return 1;
    }
    printf("predicted Hs (IE = 0%s):  ",
           limb > 0 ? ", lower limb"
                    : (limb < 0 ? ", upper limb" : ""));
    print_marcmin_angle(hs_pred);
    printf("   = %lld milli-arcmin\n", (long long)hs_pred);
    if (hc < 300000)
        printf("note: altitude under 5 deg -- refraction there is"
               " weather, not the standard model; calibrate higher if"
               " you can\n");

    if (have_hs) {
        int64_t ic = hs_pred - hs_obs;
        printf("observed Hs:  "); print_marcmin_angle(hs_obs);
        printf("   = %lld milli-arcmin\n", (long long)hs_obs);
        printf("implied aggregate correction: %+lld milli-arcmin (",
               (long long)ic);
        print_marcmin(ic);
        printf(")\n");
        if (ic < 0)
            printf("  aggregate has the sign of an ON-the-arc index"
                   " correction\n"
                   "  use IE %lld with --correct only if controlled repeated\n"
                   "  shots establish the stable component as instrument error\n",
                   (long long)ic);
        else if (ic > 0)
            printf("  aggregate has the sign of an OFF-the-arc index"
                   " correction\n"
                   "  use IE +%lld with --correct only if controlled repeated\n"
                   "  shots establish the stable component as instrument error\n",
                   (long long)ic);
        else
            printf("  observation agrees with the modeled reading exactly\n");
        if (ic < -600000 || ic > 600000)
            printf("  warning: far beyond any real index error --"
                   " wrong body, limb, time, or position?\n");
        printf("  (one shot also folds personal, timing, position, horizon,\n"
               "   atmosphere, and model error into this number; average\n"
               "   several shots and prefer altitudes above ~15 deg)\n");
    }
    return 0;
}

/* --time: the civil-time boundary adapter as a command. A calendar
 * UTC timestamp plus the caller's two policy numbers (DUT1 from IERS
 * Bulletin A, the TAI - UTC leap-second count) become exactly the two
 * arguments every other mode takes. The calendar fields are validated
 * here (real Gregorian date, second 60 allowed as a leap second); the
 * two outputs are validated against the same bounds the other modes
 * enforce, so anything this mode prints is accepted everywhere. */
static int run_time_cli(int argc, char **argv)
{
    if (argc != 11) {
        fprintf(stderr, "error: --time expects YEAR MONTH DAY HOUR MIN"
                        " SEC MS DUT1_MS TAI_MINUS_UTC_S\n");
        print_usage(argv[0]);
        return 2;
    }

    static const char *names[] = { "YEAR", "MONTH", "DAY", "HOUR",
                                   "MIN", "SEC", "MS", "DUT1_MS",
                                   "TAI_MINUS_UTC_S" };
    int32_t v[9];
    for (int i = 0; i < 9; i++) {
        if (!parse_i32(argv[i + 2], &v[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32"
                            " value\n", names[i]);
            return 2;
        }
    }

    int bad = 0;
    int32_t dmax = 0;
    bad |= cli_range_error("YEAR", v[0], 1900, 2100);
    bad |= cli_range_error("MONTH", v[1], 1, 12);
    if (v[1] >= 1 && v[1] <= 12) {
        static const int32_t mlen[12] = { 31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31 };
        dmax = mlen[v[1] - 1];
        if (v[1] == 2 && (v[0] % 4 == 0
                          && (v[0] % 100 != 0 || v[0] % 400 == 0)))
            dmax = 29;
        bad |= cli_range_error("DAY", v[2], 1, dmax);
    }
    bad |= cli_range_error("HOUR", v[3], 0, 23);
    bad |= cli_range_error("MIN", v[4], 0, 59);
    bad |= cli_range_error("SEC", v[5], 0, 60);   /* 60: leap second */
    bad |= cli_range_error("MS", v[6], 0, 999);
    bad |= cli_range_error("DUT1_MS", v[7], -900, 900);
    bad |= cli_range_error("TAI_MINUS_UTC_S", v[8], -600, 600);
    if (bad) return 2;

    /* A leap second is only ever inserted as 23:59:60 UTC on the
     * last day of a month (ITU-R TF.460), so any other SEC=60 label
     * is not a UTC timestamp -- refuse it rather than quietly
     * treating 12:34:60 as valid. */
    if (v[5] == 60 && !(v[3] == 23 && v[4] == 59 && v[2] == dmax)) {
        fprintf(stderr, "error: SEC 60 is a leap second, which only"
                        " occurs at 23:59:60 UTC on the last day of"
                        " a month\n");
        return 2;
    }

    int64_t ut1, ttmut1;
    astro_nav_civil_to_times(v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                             v[7], v[8], &ut1, &ttmut1);
    if (ut1 < -ASTRO_NAV_TIME_ABS_MAX_MS
        || ut1 > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: that instant is outside +- 100 years"
                        " of J2000 (UT1_MS %+lld)\n", (long long)ut1);
        return 2;
    }
    if (ttmut1 < -ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS
        || ttmut1 > ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS) {
        fprintf(stderr, "error: TT - UT1 comes out at %+lld ms,"
                        " outside the +- 600000 contract bound\n",
                (long long)ttmut1);
        return 2;
    }

    printf("%04ld-%02ld-%02ld %02ld:%02ld:%02ld.%03ld UTC"
           "   (DUT1 %+ld ms, TAI - UTC %+ld s)\n",
           (long)v[0], (long)v[1], (long)v[2], (long)v[3],
           (long)v[4], (long)v[5], (long)v[6], (long)v[7], (long)v[8]);
    printf("UT1_MS:          %lld\n", (long long)ut1);
    printf("TT_MINUS_UT1_MS: %lld\n", (long long)ttmut1);
    return 0;
}

/* One machine-native almanac entry: catalog star + UT1 instant ->
 * earth-fixed Q2.30 vector, with the classical GHA/dec shown at the
 * human boundary for cross-checking against a printed almanac. */
static int run_star_cli(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "error: --star expects INDEX UT1_MS\n");
        print_usage(argv[0]);
        return 2;
    }

    int32_t index;
    int64_t ms;
    if (!parse_i32(argv[2], &index) || index < 0
        || index >= ASTRO_NAV_STAR_COUNT) {
        fprintf(stderr, "error: INDEX must be 0..%d\n",
                ASTRO_NAV_STAR_COUNT - 1);
        return 2;
    }
    if (!parse_i64(argv[3], &ms)
        || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
        || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: UT1_MS must be within +- 3155760000000"
                        " (100 years of J2000)\n");
        return 2;
    }

    const astro_nav_star_t *star = &astro_nav_stars[index];
    astro_nav_unitvec_t ef;
    astro_nav_celestial_to_earthfixed(&star->j2000, ms, &ef);

    int32_t dec, lon, aries = astro_nav_gha_aries_cdeg(ms);
    astro_nav_latlon_cdeg_from_unitvec(&ef, &dec, &lon);
    int32_t gha = lon <= 0 ? -lon : CDEG_PER_TURN - lon;

    printf("%s at UT1 J2000 %+lld ms:\n", star->name, (long long)ms);
    printf("GHA Aries: "); print_cdeg(aries); printf(" deg\n");
    printf("GHA:       "); print_cdeg(gha);   printf(" deg\n");
    printf("dec:       "); print_cdeg(dec);   printf(" deg\n");
    printf("earth-fixed vector: (%ld, %ld, %ld)/2^30\n",
           (long)ef.x, (long)ef.y, (long)ef.z);
    return 0;
}

/* A run of shots of one body into one Ho: --average TREF_MS REJECT
 * then 2..32 HO T pairs, HO in milli-arcminutes (as --correct emits),
 * T in milliseconds of any epoch. REJECT is the outlier threshold in
 * milli-arcminutes, 0 to disable. */
static int run_average_cli(int argc, char **argv)
{
    if (argc < 8 || argc % 2 != 0 || (argc - 4) / 2 > 32) {
        fprintf(stderr, "error: --average expects TREF_MS REJECT and"
                        " 2..32 HO T pairs\n");
        print_usage(argv[0]);
        return 2;
    }

    int n = (argc - 4) / 2;
    int64_t t_ref, reject, ho[32], t[32];
    if (!parse_i64(argv[2], &t_ref)) {
        fprintf(stderr, "error: TREF_MS must be a base-10 int64 value\n");
        return 2;
    }
    if (!parse_i64(argv[3], &reject)) {
        fprintf(stderr, "error: REJECT must be a base-10 int64 value\n");
        return 2;
    }
    for (int i = 0; i < n; i++) {
        if (!parse_i64(argv[4 + 2 * i], &ho[i])
            || ho[i] < -5400000 || ho[i] > 5400000) {
            fprintf(stderr, "error: HO%d must be milli-arcminutes in"
                            " [-5400000, 5400000]\n", i + 1);
            return 2;
        }
        if (!parse_i64(argv[5 + 2 * i], &t[i])) {
            fprintf(stderr, "error: T%d must be a base-10 int64 value\n",
                    i + 1);
            return 2;
        }
    }

    astro_nav_avg_result_t r;
    astro_nav_average_sights(ho, t, n, t_ref, reject, &r);
    if (!r.valid) {
        fprintf(stderr, "error: no valid fit (times outside the run's"
                        " 2^40 ms span, a one-instant run evaluated"
                        " at another instant, or the fitted line at"
                        " TREF_MS outside +-90 deg)\n");
        return 1;
    }

    printf("shots: %d taken, %d kept", n, r.used);
    if (n != r.used)
        printf(" (%d rejected beyond %lld milli-arcmin)",
               n - r.used, (long long)reject);
    printf("\n");
    printf("Ho at TREF:     ");
    print_marcmin_angle(r.ho_marcmin);
    printf("   = %lld milli-arcmin   sin(Ho) = %ld/2^30\n",
           (long long)r.ho_marcmin,
           (long)astro_nav_sin_q30_from_marcmin(r.ho_marcmin));
    printf("altitude rate:  ");
    print_marcmin(r.rate_marcmin_per_min);
    printf("/min\n");
    printf("worst residual: ");
    print_marcmin(r.max_residual_marcmin);
    printf("   (scatter of the kept shots)\n");
    return 0;
}

/* Camera fix: --zenith X Y Z UT1_MS, the observer's zenith direction
 * in the J2000 celestial frame as a Q2.30 unit vector (from a
 * plate-solved star photo plus a gravity reference) and the UT1
 * instant. One rotation later that direction, earth-fixed, is the
 * position. */
static int run_zenith_cli(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "error: --zenith expects X Y Z UT1_MS\n");
        print_usage(argv[0]);
        return 2;
    }

    const char *names[] = { "X", "Y", "Z" };
    int32_t v[3];
    int64_t ms;
    for (int i = 0; i < 3; i++) {
        if (!parse_i32(argv[i + 2], &v[i])) {
            fprintf(stderr, "error: %s must be a base-10 int32 Q2.30"
                            " value\n", names[i]);
            return 2;
        }
    }
    if (!parse_i64(argv[5], &ms)
        || ms < -ASTRO_NAV_TIME_ABS_MAX_MS
        || ms > ASTRO_NAV_TIME_ABS_MAX_MS) {
        fprintf(stderr, "error: UT1_MS must be within +- 3155760000000"
                        " (100 years of J2000)\n");
        return 2;
    }

    /* Sanity: a zenith is a direction; require roughly unit norm
     * (within ~0.1%) so a scaled or garbled vector is refused rather
     * than silently accepted (rotation would hide the error). A unit
     * vector's components are within +-2^30, checked first so the
     * norm sum below (at most 3 * 2^60) cannot overflow int64. */
    for (int i = 0; i < 3; i++) {
        if (v[i] < -(1 << 30) || v[i] > (1 << 30)) {
            fprintf(stderr, "error: %s exceeds +-2^30 -- not a Q2.30"
                            " unit-vector component\n", names[i]);
            return 2;
        }
    }
    int64_t n2 = (int64_t)v[0] * v[0] + (int64_t)v[1] * v[1]
               + (int64_t)v[2] * v[2];
    int64_t err = n2 - ((int64_t)1 << 60);
    if (err < -((int64_t)1 << 52) || err > ((int64_t)1 << 52)) {
        fprintf(stderr, "error: (X, Y, Z) is not a unit vector in Q2.30"
                        " (|v|^2 = %lld, want ~2^60)\n", (long long)n2);
        return 2;
    }

    astro_nav_unitvec_t zenith = { v[0], v[1], v[2] }, pos;
    astro_nav_position_from_celestial_zenith(&zenith, ms, &pos);
    int32_t lat, lon;
    astro_nav_latlon_cdeg_from_unitvec(&pos, &lat, &lon);

    printf("zenith fix at UT1 J2000 %+lld ms:\n", (long long)ms);
    printf("lat: "); print_cdeg(lat); printf(" deg\n");
    printf("lon: "); print_cdeg(lon); printf(" deg\n");
    printf("position vector: (%ld, %ld, %ld)/2^30\n",
           (long)pos.x, (long)pos.y, (long)pos.z);
    return 0;
}

#ifdef ASTRO_NAV_NATIVE_REFERENCE
/* ================================================================== */
/*  Native-double oracle and benchmark (separate -lm test binary only) */
/* ================================================================== */

#define REF_PI 3.14159265358979323846264338327950288

typedef struct {
    double hc_deg;
    double zn_deg;
    int zn_valid;
} native_result_t;

/* Direct local-horizontal formulation used by celestial-navigator and
 * equivalent to OpenCPN Sight::AltitudeAzimuth, with atan2 replacing the
 * latter's acos + quadrant reconstruction at the singular seams. */
static native_result_t native_reduce_direct(int32_t phi_cdeg,
                                             int32_t lon_east_cdeg,
                                             int32_t gha_cdeg,
                                             int32_t dec_cdeg)
{
    double phi = (double)phi_cdeg * REF_PI / 18000.0;
    double dec = (double)dec_cdeg * REF_PI / 18000.0;
    double lha = (double)lha_cdeg_from(gha_cdeg, lon_east_cdeg)
               * REF_PI / 18000.0;
    double sphi = sin(phi), cphi = cos(phi);
    double sdec = sin(dec), cdec = cos(dec);
    double slha = sin(lha), clha = cos(lha);
    double sin_hc = sphi * sdec + cphi * cdec * clha;
    if (sin_hc > 1.0) sin_hc = 1.0;
    if (sin_hc < -1.0) sin_hc = -1.0;

    native_result_t r;
    r.hc_deg = asin(sin_hc) * 180.0 / REF_PI;
    double north = cphi * sdec - sphi * cdec * clha;
    double east = -cdec * slha;
    r.zn_valid = north * north + east * east > 1e-24;
    r.zn_deg = r.zn_valid ? atan2(east, north) * 180.0 / REF_PI : 0.0;
    if (r.zn_deg < 0.0) r.zn_deg += 360.0;
    return r;
}

/* Independent Hc identity from sunwheel's haversine implementation:
 * hav(zd) = hav(LHA) cos(phi) cos(dec) + hav(phi-dec), Hc = 90-zd. */
static double native_haversine_hc(int32_t phi_cdeg, int32_t lon_east_cdeg,
                                  int32_t gha_cdeg, int32_t dec_cdeg)
{
    double phi = (double)phi_cdeg * REF_PI / 18000.0;
    double dec = (double)dec_cdeg * REF_PI / 18000.0;
    double lha = (double)lha_cdeg_from(gha_cdeg, lon_east_cdeg)
               * REF_PI / 18000.0;
    double sh = sin(lha * 0.5);
    double sd = sin((phi - dec) * 0.5);
    double hav_zd = sh * sh * cos(phi) * cos(dec) + sd * sd;
    if (hav_zd < 0.0) hav_zd = 0.0;
    if (hav_zd > 1.0) hav_zd = 1.0;
    double zd = 2.0 * asin(sqrt(hav_zd));
    return 90.0 - zd * 180.0 / REF_PI;
}

static double native_zn_error_arcmin(int32_t cdeg, double reference_deg)
{
    double d = fabs((double)cdeg / 100.0 - reference_deg);
    if (d > 180.0) d = 360.0 - d;
    return d * 60.0;
}

static int run_native_reference(void)
{
    const int target = 10000;
    int accepted = 0;
    double sum_ah = 0.0, max_ah = 0.0;
    double sum_bh = 0.0, max_bh = 0.0;
    double sum_az = 0.0, max_az = 0.0;
    double sum_bz = 0.0, max_bz = 0.0;
    double max_oracle_delta = 0.0;

    lcg_state = 0x243F6A8885A308D3ULL;
    for (int tries = 0; tries < 200000 && accepted < target; tries++) {
        int32_t phi = lcg_range(-9000, 9001);
        int32_t dec = lcg_range(-9000, 9001);
        int32_t lha = lcg_range(0, CDEG_PER_TURN);
        native_result_t ref = native_reduce_direct(phi, 0, lha, dec);
        if (ref.hc_deg < 5.0 || ref.hc_deg > 85.0 || !ref.zn_valid) continue;

        double hav_hc = native_haversine_hc(phi, 0, lha, dec);
        double oracle_delta = fabs(hav_hc - ref.hc_deg) * 60.0;
        if (oracle_delta > max_oracle_delta) max_oracle_delta = oracle_delta;

        sight_result_t a;
        square_result_t b;
        sight_reduce_trig(phi, 0, lha, dec, &a);
        sight_reduce_square(phi, 0, lha, dec, &b);

        double ah = fabs((double)a.hc_cdeg / 100.0 - ref.hc_deg) * 60.0;
        double bh = fabs((double)b.sight.hc_cdeg / 100.0 - ref.hc_deg) * 60.0;
        double az = native_zn_error_arcmin(a.zn_cdeg, ref.zn_deg);
        double bz = native_zn_error_arcmin(b.sight.zn_cdeg, ref.zn_deg);
        sum_ah += ah; if (ah > max_ah) max_ah = ah;
        sum_bh += bh; if (bh > max_bh) max_bh = bh;
        sum_az += az; if (az > max_az) max_az = az;
        sum_bz += bz; if (bz > max_bz) max_bz = bz;
        accepted++;
    }

    printf("Native-double validation (%d operational sights, Hc 5..85 deg)\n",
           accepted);
    printf("  independent direct-vs-haversine oracle max delta: %.9f'\n",
           max_oracle_delta);
    printf("  Path A: Hc mean %.3f' max %.3f'; Zn mean %.3f' max %.3f'\n",
           sum_ah / accepted, max_ah, sum_az / accepted, max_az);
    printf("  Path B: Hc mean %.3f' max %.3f'; Zn mean %.3f' max %.3f'\n",
           sum_bh / accepted, max_bh, sum_bz / accepted, max_bz);

    int ok = accepted == target
          && max_oracle_delta < 0.000001
          && max_ah <= 0.31 && max_bh <= 0.31
          && max_az <= 0.31 && max_bz <= 0.91;
    printf("  acceptance gates: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

typedef struct {
    int32_t phi, lon, gha, dec;
} bench_case_t;

static double elapsed_seconds(clock_t start)
{
    return (double)(clock() - start) / (double)CLOCKS_PER_SEC;
}

static int run_native_benchmark(void)
{
    enum { CASES = 256, INTEGER_SIGHTS = 65536, FLOAT_SIGHTS = 4194304 };
    bench_case_t cases[CASES];
    lcg_state = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < CASES; i++) {
        cases[i].phi = lcg_range(-8000, 8001);
        cases[i].lon = lcg_range(-18000, 18001);
        cases[i].gha = lcg_range(0, CDEG_PER_TURN);
        cases[i].dec = lcg_range(-8000, 8001);
    }

    /* Path C's premise is that the almanac already stores unit vectors,
     * so vector construction is prebuilt here, not timed. */
    astro_nav_unitvec_t obs_vec[CASES], body_vec[CASES];
    for (int i = 0; i < CASES; i++) {
        astro_nav_unitvec_from_cdeg(cases[i].phi, cases[i].lon, &obs_vec[i]);
        astro_nav_unitvec_from_cdeg(cases[i].dec, -cases[i].gha, &body_vec[i]);
    }

    volatile int64_t checksum = 0;
    sight_result_t a;
    square_result_t b;
    machine_sight_t c;
    clock_t start = clock();
    for (int i = 0; i < INTEGER_SIGHTS; i++) {
        bench_case_t *x = &cases[i & (CASES - 1)];
        sight_reduce_trig(x->phi, x->lon, x->gha, x->dec, &a);
        checksum += a.hc_cdeg + a.zn_cdeg;
    }
    double sec_a = elapsed_seconds(start);

    start = clock();
    for (int i = 0; i < INTEGER_SIGHTS; i++) {
        bench_case_t *x = &cases[i & (CASES - 1)];
        sight_reduce_square(x->phi, x->lon, x->gha, x->dec, &b);
        checksum += b.sight.hc_cdeg + b.sight.zn_cdeg + b.square_key;
    }
    double sec_b = elapsed_seconds(start);

    start = clock();
    for (int i = 0; i < FLOAT_SIGHTS; i++) {
        int j = i & (CASES - 1);
        astro_nav_reduce_method_c(&obs_vec[j], &body_vec[j], &c);
        checksum += c.sin_hc_q30 + c.square_key;
    }
    double sec_c = elapsed_seconds(start);

    /* Two-body fix: pair each body with its neighbor, "observed" sines
     * generated once from the observer (prebuilt, not timed). */
    int32_t sin1[CASES], sin2[CASES];
    for (int i = 0; i < CASES; i++) {
        machine_sight_t m;
        int j = (i + 1) & (CASES - 1);
        astro_nav_reduce_method_c(&obs_vec[i], &body_vec[i], &m);
        sin1[i] = m.sin_hc_q30;
        astro_nav_reduce_method_c(&obs_vec[i], &body_vec[j], &m);
        sin2[i] = m.sin_hc_q30;
    }
    astro_nav_fix_result_t fx;
    start = clock();
    for (int i = 0; i < FLOAT_SIGHTS; i++) {
        int j = i & (CASES - 1);
        astro_nav_fix_two_body(&body_vec[j], sin1[j],
                               &body_vec[(j + 1) & (CASES - 1)], sin2[j],
                               &obs_vec[j], &fx);
        checksum += fx.position.x + fx.alternate.z + fx.valid;
    }
    double sec_fix = elapsed_seconds(start);

    /* n-body fix: each case fixes from four neighboring bodies, sines
     * prebuilt from the observer, seed a DR ~1.5 deg off truth. */
    static astro_nav_unitvec_t nb_seed[CASES];
    static int32_t nb_sines[CASES][4];
    for (int i = 0; i < CASES; i++) {
        machine_sight_t m;
        for (int k = 0; k < 4; k++) {
            astro_nav_reduce_method_c(&obs_vec[i],
                                      &body_vec[(i + k) & (CASES - 1)], &m);
            nb_sines[i][k] = m.sin_hc_q30;
        }
        astro_nav_unitvec_from_cdeg(cases[i].phi + 100,
                                    cases[i].lon - 150, &nb_seed[i]);
    }
    astro_nav_fixn_result_t fn;
    start = clock();
    for (int i = 0; i < INTEGER_SIGHTS; i++) {
        int j = i & (CASES - 1);
        astro_nav_unitvec_t four[4];
        for (int k = 0; k < 4; k++)
            four[k] = body_vec[(j + k) & (CASES - 1)];
        astro_nav_fix_n_body(four, nb_sines[j], 4, &nb_seed[j], &fn);
        checksum += fn.position.x + fn.valid + fn.iterations;
    }
    double sec_fixn = elapsed_seconds(start);

    /* Almanac generation: catalog star + time -> earth-fixed vector
     * (three CORDIC rotations). This is the per-entry cost Method C's
     * "prebuilt vectors" amortize away. */
    static int64_t alm_ms[CASES];
    for (int i = 0; i < CASES; i++)
        alm_ms[i] = ((int64_t)cases[i].gha * 2718281829LL
                     + (int64_t)cases[i].dec * 314159265358LL)
                    % 3155760000000LL;
    astro_nav_unitvec_t alm_ef;
    start = clock();
    for (int i = 0; i < INTEGER_SIGHTS; i++) {
        int j = i & (CASES - 1);
        astro_nav_celestial_to_earthfixed(
            &astro_nav_stars[j % ASTRO_NAV_STAR_COUNT].j2000,
            alm_ms[j], &alm_ef);
        checksum += alm_ef.x + alm_ef.z;
    }
    double sec_alm = elapsed_seconds(start);

    start = clock();
    for (int i = 0; i < FLOAT_SIGHTS; i++) {
        bench_case_t *x = &cases[i & (CASES - 1)];
        native_result_t r = native_reduce_direct(x->phi, x->lon, x->gha, x->dec);
        checksum += (int64_t)(r.hc_deg * 100.0) + (int64_t)(r.zn_deg * 100.0);
    }
    double sec_f = elapsed_seconds(start);

    printf("Native benchmark (CPU time; full Hc+Zn sight; checksum=%lld)\n",
           (long long)checksum);
    printf("  Path A fixed/CORDIC:       %9.1f ns/sight  (%d sights)\n",
           sec_a * 1e9 / INTEGER_SIGHTS, INTEGER_SIGHTS);
    printf("  Path B square-ray:         %9.1f ns/sight  (%d sights)\n",
           sec_b * 1e9 / INTEGER_SIGHTS, INTEGER_SIGHTS);
    printf("  Path C machine-native:     %9.1f ns/sight  (%d sights, prebuilt vectors)\n",
           sec_c * 1e9 / FLOAT_SIGHTS, FLOAT_SIGHTS);
    printf("  Two-body fix (complete):   %9.1f ns/fix    (%d fixes, prebuilt vectors)\n",
           sec_fix * 1e9 / FLOAT_SIGHTS, FLOAT_SIGHTS);
    printf("  Four-body fix (complete):  %9.1f ns/fix    (%d fixes, prebuilt vectors)\n",
           sec_fixn * 1e9 / INTEGER_SIGHTS, INTEGER_SIGHTS);
    printf("  Almanac entry (star+time): %9.1f ns/entry  (%d entries, 3 CORDIC rotations)\n",
           sec_alm * 1e9 / INTEGER_SIGHTS, INTEGER_SIGHTS);
    printf("  Native double/libm:        %9.1f ns/sight  (%d sights)\n",
           sec_f * 1e9 / FLOAT_SIGHTS, FLOAT_SIGHTS);
    printf("\nAlgorithmic work after one-time CORDIC-table initialization:\n");
    printf("  A: 240 CORDIC micro-iterations (3 sincos + 2 vectoring passes) + 1 sqrt\n");
    printf("  B: 192 CORDIC micro-iterations + 1 sqrt + 1 ratio divide + LUT\n");
    printf("  C: 0 CORDIC micro-iterations -- 9 int64 multiplies + 1 ratio divide\n");
    printf("  fix: 0 CORDIC micro-iterations -- ~30 multiplies + 3 divides + 1 sqrt\n");
    printf("  fix-n: 0 CORDIC micro-iterations -- Gauss-Newton, ~2-4 solves of a 2x2 system\n");
    printf("  float: 6 sin/cos + asin + atan2 libm calls (hardware/platform dependent)\n");
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--self-test") == 0))
        return run_self_tests();
    if (argc >= 2 && strcmp(argv[1], "--reduce-star") == 0)
        return run_reduce_star_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--reduce-sun") == 0)
        return run_reduce_sun_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--reduce-moon") == 0)
        return run_reduce_moon_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--fix-sun") == 0)
        return run_fix_sun_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--sun") == 0)
        return run_sun_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--moon") == 0)
        return run_moon_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--reduce") == 0)
        return run_reduction_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--fix") == 0)
        return run_fix_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--fix-n") == 0)
        return run_fix_n_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--running-fix") == 0)
        return run_running_fix_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--fix-stars") == 0)
        return run_fix_stars_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--correct") == 0)
        return run_correct_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--correct-sun") == 0)
        return run_correct_sun_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--correct-moon") == 0)
        return run_correct_moon_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--predict") == 0)
        return run_predict_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--time") == 0)
        return run_time_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--average") == 0)
        return run_average_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--zenith") == 0)
        return run_zenith_cli(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--star") == 0)
        return run_star_cli(argc, argv);
    if (argc == 2 && strcmp(argv[1], "--ephemeris-check") == 0)
        return run_ephemeris_check();
    if (argc == 2 && strcmp(argv[1], "--cross-check") == 0)
        return run_cross_check();
    if (argc == 2 && strcmp(argv[1], "--external-check") == 0)
        return run_external_check();
    if (argc == 2 && strcmp(argv[1], "--scenario-check") == 0)
        return run_scenario_check();
    if (argc == 2 && strcmp(argv[1], "--golden") == 0)
        return run_golden();
    if ((argc == 2 || argc == 3)
        && strcmp(argv[1], "--fuzz-w128") == 0) {
        int32_t iters = 200000;
        if (argc == 3 && (!parse_i32(argv[2], &iters) || iters <= 0)) {
            fprintf(stderr, "error: bad iteration count\n");
            return 2;
        }
        return run_fuzz_w128(iters);
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0
                   || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
#ifdef ASTRO_NAV_NATIVE_REFERENCE
    if (argc == 2 && strcmp(argv[1], "--native-reference") == 0)
        return run_native_reference();
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0)
        return run_native_benchmark();
#endif

    fprintf(stderr, "error: unknown command\n");
    print_usage(argv[0]);
    return 2;
}
