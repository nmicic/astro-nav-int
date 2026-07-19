/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * astro_nav.h -- small integer-only celestial-navigation primitives.
 */

#ifndef ASTRO_NAV_H
#define ASTRO_NAV_H

#include <stdint.h>

/* Library version (see CHANGELOG.md). Pre-1.0: no API or ABI
 * stability promise -- any 0.x release may change the header and the
 * type layouts without a deprecation cycle; pin an exact revision.
 * These macros identify a source snapshot; no computed output depends
 * on them. */
#define ASTRO_NAV_VERSION_MAJOR 0
#define ASTRO_NAV_VERSION_MINOR 1
#define ASTRO_NAV_VERSION_PATCH 0

/* Public angles are signed integer centidegrees: 100 = 1 degree.
 * Latitude/declination are north-positive. Longitude is east-positive.
 * Greenwich hour angle is normalized by the caller to [0, 36000).
 *
 * Domain contract: unless a function documents otherwise (e.g.
 * astro_nav_advance_body_for_run() reduces its periodic arguments
 * exactly), callers keep inputs inside the stated ranges -- latitude
 * and declination in [-9000, 9000], longitude in [-18000, 18000], GHA
 * in [0, 36000), altitudes in [-5400000, 5400000] milli-arcminutes,
 * unit vectors actually unit (Q2.30 norm) -- and the low-level helpers
 * do NOT re-validate; feeding arbitrary integers can overflow. All
 * library functions require documented in-domain inputs. Where a
 * result struct carries a `valid` flag, it reports *geometric or
 * numerical* failure on in-domain data (circles that do not
 * intersect, no convergent fit, undefined azimuth) -- it is not input
 * validation. Complete boundary validation is the CLI's job, and
 * sight_reduction.c does it for every mode. */
#define ASTRO_NAV_CDEG_PER_TURN     36000
#define ASTRO_NAV_CDEG_PER_HALFTURN 18000
#define ASTRO_NAV_CDEG_PER_QUARTER   9000
#define ASTRO_NAV_MARCMIN_PER_CDEG    600  /* milli-arcminutes per cdeg */

typedef struct {
    int32_t hc_cdeg;   /* computed altitude, [-9000, 9000] */
    int32_t zn_cdeg;   /* true azimuth, [0, 36000) */
    int zn_valid;      /* false at zenith/nadir, where Zn is undefined */
} astro_nav_sight_result_t;

typedef struct {
    astro_nav_sight_result_t sight;
    uint16_t square_key; /* north=0, east=16384, south=32768, west=49152 */
} astro_nav_square_result_t;

int32_t astro_nav_lha_cdeg(int32_t gha_cdeg, int32_t lon_east_cdeg);
int32_t astro_nav_circular_diff_cdeg(int32_t a_cdeg, int32_t b_cdeg);

/* Returns milli-arcminutes: 1 cdeg = 0.6 arcmin = 600 milli-arcmin. */
int64_t astro_nav_cdeg_to_marcmin(int32_t cdeg);

/* Returns signed tenths of a nautical mile. Positive means Ho is above Hc,
 * conventionally plotted TOWARD the body along Zn. */
int64_t astro_nav_intercept_tenths_nm(int32_t ho_cdeg, int32_t hc_cdeg);

void astro_nav_reduce_method_a(int32_t lat_cdeg, int32_t lon_east_cdeg,
                               int32_t gha_cdeg, int32_t dec_cdeg,
                               astro_nav_sight_result_t *out);

void astro_nav_reduce_method_b(int32_t lat_cdeg, int32_t lon_east_cdeg,
                               int32_t gha_cdeg, int32_t dec_cdeg,
                               astro_nav_square_result_t *out);

/* ------------------------------------------------------------------ */
/*  Method C -- machine-native almanac (unit vectors in, keys out)     */
/* ------------------------------------------------------------------ */

/* Earth-fixed unit vector in Q2.30: 1.0 = 1 << 30, components in
 * [-2^30, 2^30]. x points at (lat 0, lon 0), y at (lat 0, lon 90 E),
 * z at the north pole. This is the machine-native almanac entry: a
 * body's position is published as a vector, not as GHA/dec angles. */
typedef struct {
    int32_t x, y, z;
} astro_nav_unitvec_t;

/* Machine-native sight: altitude as sin(Hc) in Q2.30, azimuth as the
 * 16-bit square key. No angle, in any unit, appears anywhere.
 * zn_valid is false when the observer stands at a pole or at the body's
 * zenith/nadir: the horizontal direction either does not exist or is
 * not recoverable from unit vectors alone (a pole vector carries no
 * meridian). Methods A/B, fed angles, still resolve the pole case;
 * this is the one behavioral difference. */
typedef struct {
    int32_t sin_hc_q30;   /* sin(computed altitude), Q2.30 */
    uint16_t square_key;  /* same convention as Method B */
    int zn_valid;
} astro_nav_machine_sight_t;

/* Human boundary, inbound: build a unit vector from angles. For a body,
 * pass declination as lat and MINUS its GHA as east-longitude. */
void astro_nav_unitvec_from_cdeg(int32_t lat_cdeg, int32_t lon_east_cdeg,
                                 astro_nav_unitvec_t *out);

/* The trig-free hot path: three dot/cross products, one ratio.
 * No CORDIC, no tables, no angles. */
void astro_nav_reduce_method_c(const astro_nav_unitvec_t *observer,
                               const astro_nav_unitvec_t *body,
                               astro_nav_machine_sight_t *out);

/* Human boundary, outbound: convert machine-native outputs to
 * conventional centidegrees when a person needs to read them. */
int32_t astro_nav_hc_cdeg_from_sin_q30(int32_t sin_hc_q30);
int32_t astro_nav_zn_cdeg_from_square_key(uint16_t square_key);

/* ------------------------------------------------------------------ */
/*  Two-body fix -- circle-of-equal-altitude intersection              */
/* ------------------------------------------------------------------ */

/* One sight constrains the observer to O . B = sin Ho: a circle of
 * equal altitude on the unit sphere. Two sights intersect in at most
 * two points, closed-form in vectors -- the machine-native replacement
 * for intercept/line-of-position plotting, which is the hand-chart
 * linearization of the same two circles. dr_hint is a rough earth-fixed
 * position -- dead reckoning or the last fix; only its direction
 * matters, magnitude is irrelevant -- and picks between the two
 * intersections; `alternate` returns the one not picked. Both sines
 * must describe the same observer position: sights taken minutes apart
 * on a moving vessel need the classic running-fix advancement to a
 * common time first (astro_nav_advance_body_for_run() below).
 * valid = 0 when the bodies are near-parallel or near-antipodal (the
 * circles are concentric; no cut) or the circles do not intersect
 * (inconsistent observations). The fix itself has no pole degeneracy.
 * Hot path: integer multiply/divide/sqrt only -- no CORDIC, no tables,
 * no angle in any unit. */
typedef struct {
    astro_nav_unitvec_t position;   /* hint-selected intersection */
    astro_nav_unitvec_t alternate;  /* the other intersection     */
    int valid;
} astro_nav_fix_result_t;

void astro_nav_fix_two_body(const astro_nav_unitvec_t *body1,
                            int32_t sin_ho1_q30,
                            const astro_nav_unitvec_t *body2,
                            int32_t sin_ho2_q30,
                            const astro_nav_unitvec_t *dr_hint,
                            astro_nav_fix_result_t *out);

/* Running fix, machine-native: advancing a sight for vessel run is a
 * rotation. If the vessel's track over the run is the rotation R
 * (the great circle whose bearing at the DR position is the true
 * course -- a helmsman holding that course steers a rhumb line
 * instead, which diverges from this circle by about tan(lat) x s^2/2,
 * s the run as an angular distance in radians, result in radians:
 * roughly 0.13' for a 30 nm due-east run at 45 deg latitude, and
 * steeply more poleward), the old
 * constraint O1 . B = sin Ho becomes O2 . (R B) = sin Ho for the later
 * position O2 = R O1. So the earlier sight is advanced by rotating its
 * BODY vector; sin Ho is untouched. This is exact on the great-circle
 * track -- the chart equivalent ("transfer the LOP by the run") is a
 * flat-paper approximation of the same rotation. The track axis comes
 * from the DR position, so the usual running-fix caveat applies: the
 * advancement is as good as the DR that oriented it. dr must be a unit
 * position vector whose horizontal norm is at least 2^-12 -- within
 * about 0.9 arcmin of either pole a course has no defined direction
 * and the body is returned UNROTATED, so the caller must treat that
 * polar cap as out of domain for a nonzero run (the CLI refuses
 * |DRLAT| > 8998 centidegrees). run is in tenths of a nautical
 * mile (1 nm = 1 arcmin of arc); negative run retards a later sight.
 * Course and run are periodic and reduced exactly (mod 36000 cdeg and
 * mod 216000 tenths-nm = one earth circumference), so any int32 value
 * is accepted. */
void astro_nav_advance_body_for_run(const astro_nav_unitvec_t *body,
                                    const astro_nav_unitvec_t *dr,
                                    int32_t course_cdeg,
                                    int32_t run_tenths_nm,
                                    astro_nav_unitvec_t *out);

/* N-body fix: the same constraints O . B_i = sin Ho_i, now
 * overdetermined, solved by Gauss-Newton least squares on the sphere's
 * tangent plane (angle-domain weighting, so each residual counts in
 * arcminutes = nautical miles). Sights must describe one observer
 * position -- advance them to a common time first. seed is a rough
 * position (DR or a two-body fix); Gauss-Newton is local, so it
 * converges to the solution basin the seed is in -- from a DR-quality
 * seed, n = 2 lands on the closed-form two-body intersection nearest
 * the seed, but a seed nearer the other intersection converges there
 * (the ambiguity astro_nav_fix_two_body() reports explicitly as its
 * alternate point). valid = 0 for n outside
 * [2, 32], a degenerate azimuth spread (all circles near-parallel at
 * the solution), or no convergence. max_residual_marcmin is the worst
 * single-sight distance from its circle in milli-arcminutes (~
 * thousandths of a nautical mile) -- the navigator's scatter number;
 * weights (and thus residual reporting) are capped for altitudes above
 * ~86 degrees. */
typedef struct {
    astro_nav_unitvec_t position;
    int valid;
    int iterations;                 /* Gauss-Newton steps taken        */
    int64_t max_residual_marcmin;   /* worst circle miss, milli-arcmin */
} astro_nav_fixn_result_t;

void astro_nav_fix_n_body(const astro_nav_unitvec_t *bodies,
                          const int32_t *sin_ho_q30,
                          int n,
                          const astro_nav_unitvec_t *seed,
                          astro_nav_fixn_result_t *out);

/* Human boundary, inbound: corrected sextant altitude Ho (centidegrees)
 * to the machine-native sin(Ho) the fix consumes. */
int32_t astro_nav_sin_q30_from_cdeg(int32_t angle_cdeg);

/* Human boundary, outbound: position unit vector to latitude/longitude.
 * Longitude is east-positive in [-18000, 18000]; 0 at the exact poles. */
void astro_nav_latlon_cdeg_from_unitvec(const astro_nav_unitvec_t *v,
                                        int32_t *lat_cdeg,
                                        int32_t *lon_east_cdeg);

/* ------------------------------------------------------------------ */
/*  Altitude corrections -- sextant Hs to observed Ho                  */
/* ------------------------------------------------------------------ */

/* These work in milli-arcminutes (1000 = 1 arcminute = 1 nautical
 * mile). A typical 0.1' drum reading is 100 milli-arcmin; the internal
 * unit is deliberately finer than both that reading and a centidegree
 * (600 milli-arcmin). Same integer-only rule as everything else --
 * fp_math under the hood, no float, no libm.
 *
 * The correction chain, in the classical order:
 *   Ha = Hs + index_error - dip(eye height)      (apparent altitude)
 *   Ho = Ha - refraction(Ha) + HP cos(Ha) +- SD  (observed altitude)
 */

/* Height-of-eye dip of the sea horizon: 1.76' * sqrt(height in m),
 * i.e. exactly 176 * sqrt(height in cm) milli-arcmin. Non-positive
 * heights return 0; heights are capped at 10000 cm (100 m). */
int64_t astro_nav_dip_marcmin(int32_t eye_height_cm);

/* Atmospheric refraction by Bennett's formula at the standard
 * atmosphere (10 C / 1010 mb, the usual almanac-table assumption):
 * R = cot(Ha + 7.31/(Ha + 4.4)) arcmin, Ha in degrees. Input is
 * APPARENT altitude; valid from the horizon up (input clamped to
 * [-1 deg, +90 deg]); never negative. ~34.5' at the horizon, ~1' at
 * 45 deg, 0 at the zenith. */
int64_t astro_nav_refraction_marcmin(int64_t ha_marcmin);

/* The same Bennett refraction adjusted for the actual atmosphere by
 * the standard density factor
 *   f = (P / 1010 mb) * (283 K / (273 + T C)),
 * applied with one exactly-rounded integer scaling, so (10, 1010)
 * reproduces astro_nav_refraction_marcmin() bit-for-bit. Cold and
 * high pressure raise refraction (about +26% at -40 C), warm and low
 * pressure lower it. Domain: temp_c in [-90, +60], pressure_mb in
 * [100, 1100] (the CLI enforces a narrower plausible-weather range);
 * the linear factor is the standard table correction, good to a few
 * percent of R, and does nothing for anomalous refraction -- the
 * avoid-low-sights advice survives unchanged. */
int64_t astro_nav_refraction_tp_marcmin(int64_t ha_marcmin,
                                        int32_t temp_c,
                                        int32_t pressure_mb);

/* Parallax in altitude: HP * cos(Ha). hp_marcmin is the almanac's
 * horizontal parallax (Moon ~54-61', Sun ~0.15', stars 0). */
int64_t astro_nav_parallax_marcmin(int64_t ha_marcmin, int64_t hp_marcmin);

/* The whole chain, Hs -> Ho. index_error_marcmin is the sextant's
 * index correction (signed, added as-is). limb selects semidiameter:
 * positive = lower limb (add SD), negative = upper limb (subtract),
 * zero = center of body / star (SD unused). */
int64_t astro_nav_correct_altitude_marcmin(int64_t hs_marcmin,
                                           int64_t index_error_marcmin,
                                           int32_t eye_height_cm,
                                           int64_t hp_marcmin,
                                           int64_t sd_marcmin,
                                           int limb);

/* The whole chain with the refraction step at the actual atmosphere
 * (astro_nav_refraction_tp_marcmin above; every other step is
 * temperature-independent). (10, 1010) reproduces
 * astro_nav_correct_altitude_marcmin() bit-for-bit -- the standard
 * chain IS this one at the standard atmosphere. */
int64_t astro_nav_correct_altitude_tp_marcmin(int64_t hs_marcmin,
                                              int64_t index_error_marcmin,
                                              int32_t eye_height_cm,
                                              int64_t hp_marcmin,
                                              int64_t sd_marcmin,
                                              int limb,
                                              int32_t temp_c,
                                              int32_t pressure_mb);

/* Augmentation of the Moon's semidiameter. The tabulated SD is
 * geocentric; the observer stands up to an Earth radius closer to the
 * Moon, so its disc looks bigger -- by up to ~0.3' near the zenith, a
 * real term at this library's resolution (the Sun's counterpart is a
 * thousandth of that; nothing else here has a disc). Exact in the
 * spherical model: with the observer one equatorial radius from the
 * geocenter and the Moon's center at geocentric apparent altitude h --
 * the chain after dip, refraction, AND parallax -- the law of cosines
 * gives
 *   Delta_topo / Delta = sqrt(1 - 2 sin(HP) sin(h) + sin^2(HP)),
 * and SD scales as 1/distance (arcsin linearization error < 0.01' at
 * 16.8'). Returns SD_topo - SD in milli-arcminutes: positive above the
 * horizon, slightly negative below it (the observer is then farther
 * from the Moon than the geocenter is). Arguments are clamped to
 * h in [-90, +90] deg and HP in [0, 2 deg] (double any lunar HP). */
int64_t astro_nav_moon_augmentation_marcmin(int64_t h_marcmin,
                                            int64_t hp_marcmin,
                                            int64_t sd_marcmin);

/* The whole chain for the Moon: the generic chain's dip, refraction
 * and parallax land on the center's geocentric apparent altitude, then
 * the limb step applies the AUGMENTED semidiameter (above) instead of
 * the geocentric one the almanac tabulates. At limb 0 this is
 * astro_nav_correct_altitude_marcmin() bit-for-bit; with a limb it
 * differs by exactly the augmentation, up to ~0.3'. Use this chain for
 * Moon sights; the generic chain applies SD exactly as given, which is
 * right for the Sun and for pre-augmented table values. */
int64_t astro_nav_correct_altitude_moon_marcmin(int64_t hs_marcmin,
                                                int64_t index_error_marcmin,
                                                int32_t eye_height_cm,
                                                int64_t hp_marcmin,
                                                int64_t sd_marcmin,
                                                int limb);

/* The Moon chain with the refraction step at the actual atmosphere;
 * (10, 1010) reproduces astro_nav_correct_altitude_moon_marcmin()
 * bit-for-bit, exactly as with the generic pair. */
int64_t astro_nav_correct_altitude_moon_tp_marcmin(int64_t hs_marcmin,
                                                   int64_t index_error_marcmin,
                                                   int32_t eye_height_cm,
                                                   int64_t hp_marcmin,
                                                   int64_t sd_marcmin,
                                                   int limb,
                                                   int32_t temp_c,
                                                   int32_t pressure_mb);

/* Human boundary, inbound without centidegree rounding: Ho in
 * milli-arcminutes to the machine-native sin(Ho) the fix consumes.
 * Exactly consistent with astro_nav_sin_q30_from_cdeg (1 cdeg = 600
 * milli-arcmin). */
int32_t astro_nav_sin_q30_from_marcmin(int64_t angle_marcmin);

/* ------------------------------------------------------------------ */
/*  Sight averaging -- a run of shots of one body into one Ho          */
/* ------------------------------------------------------------------ */

/* The classic practice, automated: take several quick shots of the
 * same body, plot Ho against time, fit a straight line, discard shots
 * that sit off the line, read one Ho from the line at a chosen
 * instant. A short run is approximately linear (the fastest a body
 * moves in altitude is 15'/min); curvature matters most near meridian
 * transit, where runs should be shortened or reduced sight by sight.
 * The fit is exact integer
 * least squares over (t_ms, ho_marcmin) -- 128-bit intermediates,
 * round-to-nearest at the end, deterministic like everything else.
 *
 * ho_marcmin[i] is the i-th corrected altitude (milli-arcminutes,
 * |Ho| <= 90 deg), t_ms[i] its time (any epoch -- only differences
 * matter; the run must span < 2^40 ms). t_ref_ms is the instant the
 * averaged Ho is evaluated at, normally within the run, and is the
 * time the fix should use for the almanac lookup.
 *
 * reject_marcmin <= 0 disables outlier rejection; otherwise shots
 * whose residual from the fit exceeds it are dropped worst-first,
 * refitting after each drop, until every survivor is within the
 * threshold (never dropping below 2 shots). 500 (= 0.5') is a
 * reasonable threshold for hand-held sights.
 *
 * valid = 0 for n outside [2, 32], an altitude or time outside the
 * ranges above, when every kept shot carries one identical
 * timestamp different from t_ref (no rate is observable, so the mean
 * cannot be moved in time), or when the fitted line evaluated at
 * t_ref leaves +-90 deg (a steep run extrapolated to a distant
 * instant is not an altitude -- refused, not clamped).
 * rate_marcmin_per_min is the fitted
 * altitude rate -- a sanity number to compare against the expected
 * motion of the body. max_residual_marcmin is the worst surviving
 * shot's distance from the line: the navigator's scatter number for
 * the run. */
typedef struct {
    int64_t ho_marcmin;             /* fitted Ho at t_ref_ms          */
    int64_t rate_marcmin_per_min;   /* fitted altitude rate           */
    int64_t max_residual_marcmin;   /* worst surviving shot's miss    */
    int used;                       /* shots kept after rejection     */
    int valid;
} astro_nav_avg_result_t;

void astro_nav_average_sights(const int64_t *ho_marcmin,
                              const int64_t *t_ms,
                              int n,
                              int64_t t_ref_ms,
                              int64_t reject_marcmin,
                              astro_nav_avg_result_t *out);

/* ------------------------------------------------------------------ */
/*  Vector ephemeris -- generating the machine-native star almanac     */
/* ------------------------------------------------------------------ */

/* Method C's premise made real for the stars: the almanac IS a table
 * of unit vectors. The catalog below stores each star's J2000
 * equatorial direction as a Q2.30 unit vector; one time-dependent
 * rotation (precession + earth rotation, both integer CORDIC) turns it
 * into the earth-fixed body vector every fix in this library consumes.
 * No GHA, no declination, no angle until a human asks for one.
 *
 * Time input is UT1 as int64 milliseconds since the J2000.0 epoch
 * (2000-01-01 12:00:00 UT1). Negative for earlier times.
 *
 * Time DOMAIN, shared by every *_ms_from_j2000 argument in this
 * header, UT1 or TT alike: |ms| <= ASTRO_NAV_TIME_ABS_MAX_MS, 100
 * Julian years either side of J2000.0. Internally the implementation
 * accepts a further ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS of margin
 * beyond the bound, so the derived instants in this header -- the TT
 * sums the Sun entry points taking (UT1, TT - UT1) form internally --
 * stay in domain whenever each argument obeys its own bound; callers
 * never check a sum. Like every other domain in this
 * header the caller owns the check -- the library does not
 * range-test, and beyond the bound the internal century and
 * micro-arcsecond scalings eventually overflow int64 (undefined
 * behavior, not a graceful wrong answer). The CLI enforces exactly
 * this bound on every time argument it parses.
 *
 * Model and honest accuracy budget: earth rotation is the IERS 2003
 * ERA->GMST relation; orientation is IAU 1976 precession (linear and
 * T^2 terms). Deliberately omitted, in order of size: stellar proper
 * motion (star-dependent; worst in this catalog is Arcturus at
 * ~1.0' by 2026, most stars are under 0.3'), annual aberration
 * (<= 0.35'), nutation (<= 0.3'). Total: positions are good to about
 * 1-2 arcminutes (1-2 nautical miles) through ~2030 -- comparable to,
 * and additive with, the uncertainty of a practical sextant sight.
 * This is a low-precision ephemeris with a measured budget; the primary
 * design claim is the format and rotation machinery. The Sun and the
 * Moon have their own sections below; planet ephemerides are out of
 * scope. */

/* 100 Julian years in milliseconds: the time-domain bound above. */
#define ASTRO_NAV_TIME_ABS_MAX_MS 3155760000000LL

typedef struct {
    const char *name;
    astro_nav_unitvec_t j2000; /* ICRS/J2000 equatorial, Q2.30 */
} astro_nav_star_t;

#define ASTRO_NAV_STAR_COUNT 18
extern const astro_nav_star_t astro_nav_stars[ASTRO_NAV_STAR_COUNT];

/* Rotate a J2000 celestial unit vector into the earth-fixed frame of
 * the given instant: precession (Rz(-zeta) Ry(theta)) then earth
 * rotation folded with the last precession angle (Rz(GMST - z)). The
 * result plugs straight into astro_nav_reduce_method_c() and the
 * two-body / n-body fixes. */
void astro_nav_celestial_to_earthfixed(const astro_nav_unitvec_t *celestial,
                                       int64_t ut1_ms_from_j2000,
                                       astro_nav_unitvec_t *out);

/* Human boundary: Greenwich hour angle of Aries (GMST as an angle) in
 * centidegrees [0, 36000), for cross-checking against a printed
 * almanac's daily pages. */
int32_t astro_nav_gha_aries_cdeg(int64_t ut1_ms_from_j2000);

/* ------------------------------------------------------------------ */
/*  Sun ephemeris -- and the two-timescale time contract               */
/* ------------------------------------------------------------------ */

/* The time contract, stated once for every body that moves:
 *
 *   - Earth ROTATION runs on UT1. Every ut1_ms_from_j2000 argument in
 *     this header means UT1 and nothing else.
 *   - Solar-system DYNAMICS run on TT (Terrestrial Time), the uniform
 *     clock ephemerides are tabulated against. Dynamics arguments are
 *     TT ms since the J2000.0 epoch (JD 2451545.0 TT).
 *   - The caller supplies TT - UT1 as an integer millisecond count,
 *     |tt_minus_ut1_ms| <= ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS (+- 10
 *     minutes: the real value was about -3 s in 1900, is ~69 s in
 *     the mid-2020s, and is forecast to grow toward roughly +200 s
 *     by 2100, so the bound covers the library's whole +-100 year
 *     domain with room to spare). The math core never decides civil
 *     time: UTC, leap seconds, and DUT1 stay outside it. The one
 *     boundary adapter, astro_nav_civil_to_times() below, does the
 *     calendar arithmetic from a UTC timestamp -- but the DUT1 and
 *     leap-second values it consumes are still the caller's to
 *     supply.
 *
 * Domain: each argument obeys its own bound -- ut1_ms_from_j2000 the
 * shared time bound (ASTRO_NAV_TIME_ABS_MAX_MS), tt_minus_ut1_ms the
 * offset bound above -- and that is the whole check: the TT instant
 * astro_nav_sun_earthfixed() forms by adding them lands inside the
 * implementation margin the shared-domain paragraph reserves for it.
 * As everywhere in this header, out-of-domain inputs are the
 * caller's bug, not a checked error.
 *
 * Stars are far enough away that only rotation matters, which is why
 * the star almanac above never mentions TT. The Sun is the first body
 * whose own position is a function of time, so its entry points take
 * both timescales. Magnitudes: TT - UT1 was 63.8 s at J2000, is about
 * 69 s in 2026 (TT - UT1 = 32.184 s + accumulated leap seconds -
 * DUT1). It changes negligibly over one sight round, but callers still
 * supply the value for the observation epoch. The Sun's mean
 * motion is 0.04 arcsec per second of time, so tt_minus_ut1_ms = 0
 * mis-places the Sun by at most ~2.9" (~0.05') this era -- inside the
 * model's budget, but the honest value costs nothing.
 *
 * Model: the USNO low-precision solar formula (mean longitude +
 * equation of center; annual aberration folded into the constants,
 * nutation deliberately omitted like everywhere else in this library),
 * evaluated in Q16.48, ecliptic of date, rotated back to J2000 with
 * the same IAU 1976 angles the forward pipeline applies. Source
 * formula is stated good to ~1' within two centuries of J2000; the
 * committed
 * --ephemeris-check truth rows measure THIS implementation against
 * Skyfield/DE421 apparent places -- see that gate for the asserted
 * bound. The same source formula also gives the distance, so the
 * daily-pages numbers a Sun sight needs -- semidiameter and
 * horizontal parallax -- are computed too (below), not copied. */

/* The tt_minus_ut1_ms bound from the contract above: +- 10 minutes. */
#define ASTRO_NAV_TT_MINUS_UT1_ABS_MAX_MS 600000LL

/* The Sun's apparent geocentric direction in the J2000 equatorial
 * frame -- the star catalog's frame -- as a Q2.30 unit vector. Feed
 * it to astro_nav_celestial_to_earthfixed(), or use the wrapper
 * below. */
void astro_nav_sun_inertial(int64_t tt_ms_from_j2000,
                            astro_nav_unitvec_t *out);

/* The composed almanac entry: dynamics on TT, rotation on UT1 -- the
 * time contract as a signature. Equivalent to
 * astro_nav_sun_inertial(ut1 + tt_minus_ut1) followed by
 * astro_nav_celestial_to_earthfixed(.., ut1, ..). The result plugs
 * into astro_nav_reduce_method_c() and the fixes exactly like a
 * star's vector. */
void astro_nav_sun_earthfixed(int64_t ut1_ms_from_j2000,
                              int64_t tt_minus_ut1_ms,
                              astro_nav_unitvec_t *out);

/* The Sun's distance and the two correction-chain inputs derived from
 * it, from the same USNO source formula as the position:
 *   R  = 1.00014 - 0.01671 cos g - 0.00014 cos 2g   AU
 *   SD = 0.2666 deg / R      (the source page's own semidiameter)
 *   HP = 8.794 arcsec / R    (standard equatorial solar parallax)
 * distance_uau is R in integer micro-AU, [983290, 1016710] over the
 * model's range. sd_marcmin (~15733 aphelion .. ~16268 perihelion,
 * the daily pages' 15.7'-16.3') and hp_marcmin (~144-149, ~0.15')
 * are exactly the SD and HP arguments
 * astro_nav_correct_altitude_marcmin() wants -- angles this small
 * equal their sines at milli-arcmin resolution. Same timescales and
 * domain as astro_nav_sun_earthfixed(); only the TT sum matters here
 * (distance is dynamics, no rotation), the split signature just keeps
 * the one time contract everywhere. */
void astro_nav_sun_distance(int64_t ut1_ms_from_j2000,
                            int64_t tt_minus_ut1_ms,
                            int32_t *distance_uau,
                            int32_t *sd_marcmin,
                            int32_t *hp_marcmin);

/* ------------------------------------------------------------------ */
/*  Moon ephemeris -- the two-timescale contract at its sharpest        */
/* ------------------------------------------------------------------ */

/* The same time contract as the Sun (see above): ut1_ms_from_j2000 is
 * UT1, tt_minus_ut1_ms is TT - UT1 (same +- 10 minute bound), and the
 * TT instant the earth-fixed entry forms by adding them lands inside
 * the shared domain's implementation margin. What changes is how much
 * the split matters. The Moon's mean motion is about 0.55 arcmin per
 * minute of time -- roughly thirteen times the Sun's -- so dropping
 * tt_minus_ut1_ms (about 69 s in the mid-2020s) mis-places the Moon by
 * ~0.6 arcmin, larger than the position error of the model itself.
 * Dynamics on TT and rotation on UT1 is not a nicety here; it is the
 * difference between a good fix and a wrong one. The math core still
 * never touches civil time: UTC, leap seconds and DUT1 stay the
 * caller's to fold into tt_minus_ut1_ms, via astro_nav_civil_to_times()
 * or otherwise.
 *
 * Model: Meeus, Astronomical Algorithms, 2nd ed., ch. 47 -- the
 * abridged ELP-2000/82 lunar series (the 60 + 60 largest periodic
 * terms of longitude, latitude and distance), evaluated in Q16.48,
 * geocentric ecliptic of date, rotated to J2000 with the same IAU
 * 1976 angles and the same mean obliquity the Sun uses. Like every
 * body in this library the direction is geocentric and mean-equinox:
 * parallax is the correction chain's job (through HP below -- and for
 * the Moon it is no footnote: horizontal parallax runs 54-61 arcmin,
 * the largest correction in celestial navigation, so an uncorrected
 * Moon sight can be a full degree wrong), and nutation is omitted like
 * everywhere else. The deliberately omitted effects, earth-fixed and
 * in order of size: nutation, whose right-ascension part cancels
 * against the mean-equinox GMST the same way it does for the stars,
 * leaving <= ~0.15 arcmin; and the net of light-time and geocentric
 * aberration, ~0.01 arcmin (the annual aberration folded into the
 * Sun's constants does not apply to a geocentric Moon). Measured
 * against Skyfield/DE421 apparent places, THIS implementation is good
 * to about 0.3 arcmin earth-fixed across the DE421-covered 1900-2053
 * part of the library's time domain: a 37,048-instant sweep (worst
 * ~0.27, median ~0.10; DE421 ends
 * 2053-10-09), reproducible with tools/sweep_moon_vs_de421.py against
 * the shipped binary. What executes in-repo is narrower: the committed
 * --ephemeris-check rows assert a 0.50 arcmin bound over 47 epochs
 * spanning 2000-2035; the sweep tool is the evidence for the wider
 * window. This is a validated range, not a fit cutoff -- accuracy
 * degrades past it as the series' polynomial arguments run beyond the
 * span they were checked on, so the window is stated, not hidden.
 * This is a low-precision lunar ephemeris claim with a measured budget,
 * alongside the format and rotation machinery. */

/* The Moon's geocentric direction in the J2000 equatorial frame --
 * the star catalog's frame -- as a Q2.30 unit vector (geometric: the
 * ~0.01 arcmin light-time/aberration net above stays omitted). Feed it
 * to astro_nav_celestial_to_earthfixed(), or use the wrapper below. */
void astro_nav_moon_inertial(int64_t tt_ms_from_j2000,
                             astro_nav_unitvec_t *out);

/* The composed almanac entry: dynamics on TT, rotation on UT1.
 * Equivalent to astro_nav_moon_inertial(ut1 + tt_minus_ut1) followed
 * by astro_nav_celestial_to_earthfixed(.., ut1, ..). The result plugs
 * into astro_nav_reduce_method_c() and the fixes exactly like a star's
 * or the Sun's vector. */
void astro_nav_moon_earthfixed(int64_t ut1_ms_from_j2000,
                               int64_t tt_minus_ut1_ms,
                               astro_nav_unitvec_t *out);

/* The Moon's distance and the two correction-chain inputs derived from
 * it, from the same Meeus ch. 47 series as the position:
 *   Delta = 385000.56 km + Sigma_r          (Table 47.A cosine terms)
 *   HP    = arcsin(6378.137 km / Delta)      equatorial horizontal parallax
 *   SD    = arcsin(1737.4 km / Delta)        geocentric semidiameter
 * distance_km is Delta rounded to the nearest km, ~356400-406700 over
 * the model's range. hp_marcmin (~54000-61500, the 54-61 arcmin the
 * daily pages carry) and sd_marcmin (~14700-16800, 14.7-16.8 arcmin)
 * are the HP and SD arguments astro_nav_correct_altitude_moon_marcmin()
 * wants (SD geocentric -- that chain adds the augmentation itself).
 * Unlike the Sun's these angles are large enough that the
 * arcsine is not its argument -- the cubic term is a few milli-arcmin
 * -- so they are computed as arcsines, not small-angle ratios. Same
 * timescales and domain as astro_nav_moon_earthfixed(); only the TT
 * sum matters (distance is dynamics, no rotation), the split signature
 * just keeps the one time contract everywhere. */
void astro_nav_moon_distance(int64_t ut1_ms_from_j2000,
                             int64_t tt_minus_ut1_ms,
                             int32_t *distance_km,
                             int32_t *sd_marcmin,
                             int32_t *hp_marcmin);

/* ------------------------------------------------------------------ */
/*  Civil-time boundary -- calendar UTC to the library's timescales    */
/* ------------------------------------------------------------------ */

/* The one adapter at the civil-time boundary. The core above never
 * decides civil-time POLICY -- what today's DUT1 or leap-second count
 * is -- but the calendar arithmetic from a UTC timestamp to the two
 * numbers every entry point takes is pure integer work, so it lives
 * here rather than being reimplemented (wrongly) by every caller:
 *
 *   ut1_ms_from_j2000 = utc_ms_from_j2000 + dut1_ms
 *   tt_minus_ut1_ms   = 32184 + 1000 * tai_minus_utc_s - dut1_ms
 *
 * The caller supplies the two policy numbers: dut1_ms is DUT1 =
 * UT1 - UTC in milliseconds (IERS Bulletin A; the CLI accepts
 * [-900, 900]), and tai_minus_utc_s is the integer
 * leap-second count TAI - UTC (37 for the 2026 examples in this repo;
 * callers verify the value in force at their instant). TT = TAI +
 * 32.184 s exactly, which is the 32184.
 *
 * The date is proleptic Gregorian (Fliegel-Van Flandern Julian day
 * number, exact in integer division); "ms from J2000" in a timescale
 * means (JD in that timescale - 2451545.0) * 86400000, the same
 * convention as every time argument above. second may be 60: a leap
 * second simply counts one more elapsed second, spilling into the
 * next civil day, which is what the rotating earth does too. Note the
 * dut1_ms that makes this exact is the one IN EFFECT AT that instant
 * (the pre-leap value); the next day's 00:00:00 label carries a DUT1
 * stepped +1 s by the insertion, so the two labels land one SI second
 * apart in UT1 -- consecutive instants, not the same one -- while
 * TT - UT1 is continuous across the step (the +1 s in tai_minus_utc_s
 * and the +1 s in dut1_ms cancel in the formula above). Domain:
 * calendar fields valid (month 1-12, day real for the month/year,
 * hour 0-23, minute 0-59, second 0-60, millisecond 0-999) and the
 * resulting instant inside the shared time bound -- as everywhere,
 * out-of-domain inputs are the caller's bug, not a checked error. */
void astro_nav_civil_to_times(int32_t year, int32_t month, int32_t day,
                              int32_t hour, int32_t minute,
                              int32_t second, int32_t millisecond,
                              int32_t dut1_ms, int32_t tai_minus_utc_s,
                              int64_t *ut1_ms_from_j2000,
                              int64_t *tt_minus_ut1_ms);

/* Camera fix: a star-field photo plus a gravity reference IS a sight.
 * Plate-solving the photo orients the camera in the celestial frame;
 * the gravity vector expressed in that frame is the observer's zenith
 * direction among the stars. And the earth-fixed zenith is, by
 * definition, the observer's position vector -- so one rotation turns
 * a plate-solved zenith plus UT1 into a full fix: no horizon, no
 * circles, no intercepts, both intersections' ambiguity gone. This is
 * the library entry point for the fixed-camera use case; star
 * detection, plate solving, and gravity sensing happen upstream.
 * Accuracy includes the upstream zenith direction (1' of zenith error
 * = 1 nm of position error) and this function's celestial-to-earth-fixed
 * frame model (the mean-equinox precession/rotation approximation
 * documented above). The star-catalog budget is not by itself an error
 * bound for a plate-solving pipeline. Feed the result to
 * astro_nav_latlon_cdeg_from_unitvec() to read it. */
void astro_nav_position_from_celestial_zenith(
        const astro_nav_unitvec_t *zenith_j2000,
        int64_t ut1_ms_from_j2000,
        astro_nav_unitvec_t *out_position);

#endif
