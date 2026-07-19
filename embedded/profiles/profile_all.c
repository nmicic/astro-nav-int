/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * profile_all.c -- the whole public surface in one image: every entry
 * point in astro_nav.h is called at least once, so the linked size is
 * the full-library footprint and the hash spans every feature at once.
 * Schedules are smaller than the per-feature profiles'; this profile
 * exists for the total, not for per-call costs.
 */

#include "astro_nav.h"
#include "harness.h"

const char profile_name[] = "all";

static uint64_t hash_vec(uint64_t h, const astro_nav_unitvec_t *v)
{
    h = fnv1a_i32(h, v->x);
    h = fnv1a_i32(h, v->y);
    return fnv1a_i32(h, v->z);
}

uint64_t profile_run(void)
{
    uint64_t h = fnv1a_init();
    uint64_t s = 0x082efa98ec4e6c89ULL;

    /* Civil-time boundary, including a leap second. */
    int64_t ut1_a, dtt_a, ut1_b, dtt_b;
    astro_nav_civil_to_times(2026, 7, 18, 12, 0, 0, 0, -150, 37,
                             &ut1_a, &dtt_a);
    astro_nav_civil_to_times(2016, 12, 31, 23, 59, 60, 500, -407, 36,
                             &ut1_b, &dtt_b);
    h = fnv1a_i64(h, ut1_a);
    h = fnv1a_i64(h, dtt_a);
    h = fnv1a_i64(h, ut1_b);
    h = fnv1a_i64(h, dtt_b);

    /* Star almanac: the catalog itself plus the rotation into the
     * earth-fixed frame at two epochs. */
    for (int i = 0; i < ASTRO_NAV_STAR_COUNT; i++) {
        const astro_nav_unitvec_t *j2000 = &astro_nav_stars[i].j2000;
        h = hash_vec(h, j2000);
        astro_nav_unitvec_t ef;
        astro_nav_celestial_to_earthfixed(j2000, ut1_a, &ef);
        h = hash_vec(h, &ef);
        astro_nav_celestial_to_earthfixed(j2000, -987654321012LL, &ef);
        h = hash_vec(h, &ef);
    }

    /* Camera fix: a plate-solved zenith is a position. */
    for (int i = 0; i < 2; i++) {
        astro_nav_unitvec_t pos;
        int32_t lat, lon;
        astro_nav_position_from_celestial_zenith(&astro_nav_stars[3 * i].j2000,
                                                 ut1_a, &pos);
        astro_nav_latlon_cdeg_from_unitvec(&pos, &lat, &lon);
        h = hash_vec(h, &pos);
        h = fnv1a_i32(h, lat);
        h = fnv1a_i32(h, lon);
    }

    /* Sun and Moon almanac entries. */
    for (int k = 0; k < 6; k++) {
        int64_t ut1 = emb_rng_ms(&s, ASTRO_NAV_TIME_ABS_MAX_MS);
        int64_t dtt = emb_rng_range(&s, -600000, 600000);
        astro_nav_unitvec_t v;
        int32_t d, sd, hp;
        astro_nav_sun_inertial(ut1 + dtt, &v);
        h = hash_vec(h, &v);
        astro_nav_sun_earthfixed(ut1, dtt, &v);
        h = hash_vec(h, &v);
        astro_nav_sun_distance(ut1, dtt, &d, &sd, &hp);
        h = fnv1a_i32(h, d);
        h = fnv1a_i32(h, sd);
        h = fnv1a_i32(h, hp);
        h = fnv1a_i32(h, astro_nav_gha_aries_cdeg(ut1));
    }
    for (int k = 0; k < 4; k++) {
        int64_t ut1 = emb_rng_ms(&s, ASTRO_NAV_TIME_ABS_MAX_MS);
        int64_t dtt = emb_rng_range(&s, -600000, 600000);
        astro_nav_unitvec_t v;
        int32_t d, sd, hp;
        astro_nav_moon_inertial(ut1 + dtt, &v);
        h = hash_vec(h, &v);
        astro_nav_moon_earthfixed(ut1, dtt, &v);
        h = hash_vec(h, &v);
        astro_nav_moon_distance(ut1, dtt, &d, &sd, &hp);
        h = fnv1a_i32(h, d);
        h = fnv1a_i32(h, sd);
        h = fnv1a_i32(h, hp);
    }

    /* Altitude corrections, generic and Moon chains. */
    for (int k = 0; k < 8; k++) {
        int64_t ha = emb_rng_range(&s, -60000, 5400000);
        int64_t hp = emb_rng_range(&s, 0, 61500);
        int64_t sd = emb_rng_range(&s, 14700, 16800);
        int64_t hs = emb_rng_range(&s, 30000, 5340000);
        int64_t ie = emb_rng_range(&s, -3000, 3000);
        int32_t eye = emb_rng_range(&s, 0, 10000);
        int32_t temp = emb_rng_range(&s, -40, 45);
        int32_t press = emb_rng_range(&s, 900, 1050);
        int limb = k % 3 - 1;
        h = fnv1a_i64(h, astro_nav_dip_marcmin(eye));
        h = fnv1a_i64(h, astro_nav_refraction_marcmin(ha));
        h = fnv1a_i64(h, astro_nav_refraction_tp_marcmin(ha, temp, press));
        h = fnv1a_i64(h, astro_nav_parallax_marcmin(ha, hp));
        h = fnv1a_i64(h, astro_nav_correct_altitude_marcmin(
                              hs, ie, eye, hp, sd, limb));
        h = fnv1a_i64(h, astro_nav_correct_altitude_tp_marcmin(
                              hs, ie, eye, hp, sd, limb, temp, press));
        h = fnv1a_i64(h, astro_nav_correct_altitude_moon_marcmin(
                              hs, ie, eye, hp, sd, limb));
        h = fnv1a_i64(h, astro_nav_correct_altitude_moon_tp_marcmin(
                              hs, ie, eye, hp, sd, limb, temp, press));
        h = fnv1a_i64(h, astro_nav_moon_augmentation_marcmin(
                              emb_rng_range(&s, -5400000, 5400000), hp, sd));
    }

    /* All three reduction methods on the same angle sets, plus the
     * angle-domain helpers around them. */
    for (int k = 0; k < 8; k++) {
        int32_t lat = emb_rng_range(&s, -8900, 8900);
        int32_t lon = emb_rng_range(&s, -17999, 18000);
        int32_t gha = emb_rng_range(&s, 0, 35999);
        int32_t dec = emb_rng_range(&s, -8900, 8900);

        astro_nav_sight_result_t ra;
        astro_nav_reduce_method_a(lat, lon, gha, dec, &ra);
        h = fnv1a_i32(h, ra.hc_cdeg);
        h = fnv1a_i32(h, ra.zn_cdeg);
        h = fnv1a_i32(h, ra.zn_valid);

        astro_nav_square_result_t rb;
        astro_nav_reduce_method_b(lat, lon, gha, dec, &rb);
        h = fnv1a_i32(h, rb.sight.hc_cdeg);
        h = fnv1a_i32(h, rb.sight.zn_cdeg);
        h = fnv1a_i32(h, rb.sight.zn_valid);
        h = fnv1a_u64(h, rb.square_key);

        astro_nav_unitvec_t obs, body;
        astro_nav_machine_sight_t ms;
        astro_nav_unitvec_from_cdeg(lat, lon, &obs);
        astro_nav_unitvec_from_cdeg(dec, -gha, &body);
        astro_nav_reduce_method_c(&obs, &body, &ms);
        h = fnv1a_i32(h, ms.sin_hc_q30);
        h = fnv1a_u64(h, ms.square_key);
        h = fnv1a_i32(h, ms.zn_valid);
        h = fnv1a_i32(h, astro_nav_hc_cdeg_from_sin_q30(ms.sin_hc_q30));
        h = fnv1a_i32(h, astro_nav_zn_cdeg_from_square_key(ms.square_key));

        int32_t ho = emb_clip(ra.hc_cdeg + emb_rng_range(&s, -50, 50),
                              -9000, 9000);
        h = fnv1a_i64(h, astro_nav_intercept_tenths_nm(ho, ra.hc_cdeg));
        h = fnv1a_i32(h, astro_nav_lha_cdeg(gha, lon));
        h = fnv1a_i32(h, astro_nav_circular_diff_cdeg(ra.zn_cdeg, gha));
        h = fnv1a_i64(h, astro_nav_cdeg_to_marcmin(ra.hc_cdeg));
        h = fnv1a_i32(h, astro_nav_sin_q30_from_cdeg(ho));
        h = fnv1a_i32(h, astro_nav_sin_q30_from_marcmin(
                              astro_nav_cdeg_to_marcmin(ho)));
    }

    /* Fixes: two-body with advancement, then n-body. */
    for (int k = 0; k < 4; k++) {
        int32_t lat = emb_rng_range(&s, -8000, 8000);
        int32_t lon = emb_rng_range(&s, -17999, 18000);
        astro_nav_unitvec_t truth, hint, bodies[5];
        int32_t sins[5];
        astro_nav_unitvec_from_cdeg(lat, lon, &truth);
        astro_nav_unitvec_from_cdeg(emb_clip(lat + 200, -9000, 9000),
                                    emb_clip(lon - 200, -18000, 18000),
                                    &hint);
        for (int i = 0; i < 5; i++) {
            astro_nav_machine_sight_t ms;
            astro_nav_unitvec_from_cdeg(emb_rng_range(&s, -8900, 8900),
                                        emb_rng_range(&s, -17999, 18000),
                                        &bodies[i]);
            astro_nav_reduce_method_c(&truth, &bodies[i], &ms);
            sins[i] = ms.sin_hc_q30;
        }

        astro_nav_unitvec_t adv;
        astro_nav_advance_body_for_run(&bodies[0], &hint,
                                       emb_rng_range(&s, 0, 35999),
                                       emb_rng_range(&s, -6000, 6000), &adv);
        h = hash_vec(h, &adv);

        astro_nav_fix_result_t fr;
        astro_nav_fix_two_body(&bodies[0], sins[0], &bodies[1], sins[1],
                               &hint, &fr);
        h = fnv1a_i32(h, fr.valid);
        h = hash_vec(h, &fr.position);
        h = hash_vec(h, &fr.alternate);

        astro_nav_fixn_result_t fn;
        astro_nav_fix_n_body(bodies, sins, 5, &hint, &fn);
        h = fnv1a_i32(h, fn.valid);
        h = hash_vec(h, &fn.position);
        h = fnv1a_i32(h, fn.iterations);
        h = fnv1a_i64(h, fn.max_residual_marcmin);
    }

    /* Sight averaging: a linear run with one planted outlier. */
    for (int k = 0; k < 4; k++) {
        int64_t ho[8], t[8];
        int64_t t0 = emb_rng_ms(&s, 1000000000LL);
        int64_t base = emb_rng_range(&s, 600000, 4800000);
        int32_t rate = emb_rng_range(&s, -700, 700);
        for (int i = 0; i < 8; i++) {
            t[i] = t0 + (int64_t)i * 30000;
            ho[i] = base + (int64_t)rate * i / 2
                  + emb_rng_range(&s, -300, 300);
        }
        ho[5] += 5000;
        astro_nav_avg_result_t ar;
        astro_nav_average_sights(ho, t, 8, t0 + 105000, 500, &ar);
        h = fnv1a_i64(h, ar.ho_marcmin);
        h = fnv1a_i64(h, ar.rate_marcmin_per_min);
        h = fnv1a_i64(h, ar.max_residual_marcmin);
        h = fnv1a_i32(h, ar.used);
        h = fnv1a_i32(h, ar.valid);
    }

    return h;
}
