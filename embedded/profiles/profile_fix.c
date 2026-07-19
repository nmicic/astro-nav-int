/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * profile_fix.c -- the position-fixing feature slice: two-body fixes,
 * running-fix advancement, n-body Gauss-Newton fixes, and the two
 * boundary converters a fix consumes and produces. Observed sines come
 * from reducing a known truth position, so the circles are consistent
 * by construction and the fixes exercise their convergent paths;
 * degenerate geometries the schedule happens to produce are hashed
 * through their valid flags like everything else.
 */

#include "astro_nav.h"
#include "harness.h"

const char profile_name[] = "fix";

static void rng_unitvec(uint64_t *s, int32_t lat_lim,
                        astro_nav_unitvec_t *out)
{
    astro_nav_unitvec_from_cdeg(emb_rng_range(s, -lat_lim, lat_lim),
                                emb_rng_range(s, -17999, 18000), out);
}

uint64_t profile_run(void)
{
    uint64_t h = fnv1a_init();
    uint64_t s = 0x452821e638d01377ULL;

    for (int k = 0; k < 16; k++) {
        int32_t lat = emb_rng_range(&s, -8000, 8000);
        int32_t lon = emb_rng_range(&s, -17999, 18000);
        astro_nav_unitvec_t truth, b1, b2, hint;
        astro_nav_unitvec_from_cdeg(lat, lon, &truth);
        rng_unitvec(&s, 8900, &b1);
        rng_unitvec(&s, 8900, &b2);
        astro_nav_unitvec_from_cdeg(emb_clip(lat + emb_rng_range(&s, -300, 300),
                                             -9000, 9000),
                                    emb_clip(lon + emb_rng_range(&s, -300, 300),
                                             -18000, 18000),
                                    &hint);

        astro_nav_machine_sight_t s1, s2;
        astro_nav_reduce_method_c(&truth, &b1, &s1);
        astro_nav_reduce_method_c(&truth, &b2, &s2);

        astro_nav_fix_result_t fr;
        astro_nav_fix_two_body(&b1, s1.sin_hc_q30, &b2, s2.sin_hc_q30,
                               &hint, &fr);
        h = fnv1a_i32(h, fr.valid);
        h = fnv1a_i32(h, fr.position.x);
        h = fnv1a_i32(h, fr.position.y);
        h = fnv1a_i32(h, fr.position.z);
        h = fnv1a_i32(h, fr.alternate.x);
        h = fnv1a_i32(h, fr.alternate.y);
        h = fnv1a_i32(h, fr.alternate.z);

        int32_t flat, flon;
        astro_nav_latlon_cdeg_from_unitvec(&fr.position, &flat, &flon);
        h = fnv1a_i32(h, flat);
        h = fnv1a_i32(h, flon);
    }

    for (int k = 0; k < 8; k++) {
        astro_nav_unitvec_t body, dr, out;
        rng_unitvec(&s, 8900, &body);
        rng_unitvec(&s, 8000, &dr);
        /* Course and run are periodic over any int32; mix plain sailing
         * with values that exercise the exact modular reduction. */
        int32_t course = (k & 1) ? emb_rng_range(&s, -720000, 720000)
                                 : emb_rng_range(&s, 0, 35999);
        int32_t run    = (k & 2) ? emb_rng_range(&s, -2160000, 2160000)
                                 : emb_rng_range(&s, -6000, 6000);
        astro_nav_advance_body_for_run(&body, &dr, course, run, &out);
        h = fnv1a_i32(h, out.x);
        h = fnv1a_i32(h, out.y);
        h = fnv1a_i32(h, out.z);
    }

    for (int k = 0; k < 8; k++) {
        int32_t lat = emb_rng_range(&s, -8000, 8000);
        int32_t lon = emb_rng_range(&s, -17999, 18000);
        astro_nav_unitvec_t truth, seed, bodies[4];
        int32_t sins[4];
        astro_nav_unitvec_from_cdeg(lat, lon, &truth);
        astro_nav_unitvec_from_cdeg(emb_clip(lat + emb_rng_range(&s, -400, 400),
                                             -9000, 9000),
                                    emb_clip(lon + emb_rng_range(&s, -400, 400),
                                             -18000, 18000),
                                    &seed);
        for (int i = 0; i < 4; i++) {
            astro_nav_machine_sight_t ms;
            rng_unitvec(&s, 8900, &bodies[i]);
            astro_nav_reduce_method_c(&truth, &bodies[i], &ms);
            sins[i] = ms.sin_hc_q30;
        }
        astro_nav_fixn_result_t fn;
        astro_nav_fix_n_body(bodies, sins, 4, &seed, &fn);
        h = fnv1a_i32(h, fn.valid);
        h = fnv1a_i32(h, fn.position.x);
        h = fnv1a_i32(h, fn.position.y);
        h = fnv1a_i32(h, fn.position.z);
        h = fnv1a_i32(h, fn.iterations);
        h = fnv1a_i64(h, fn.max_residual_marcmin);
    }

    for (int32_t cdeg = -9000; cdeg <= 9000; cdeg += 1500)
        h = fnv1a_i32(h, astro_nav_sin_q30_from_cdeg(cdeg));
    for (int k = 0; k < 8; k++)
        h = fnv1a_i32(h, astro_nav_sin_q30_from_marcmin(
                              emb_rng_range(&s, -5400000, 5400000)));
    return h;
}
