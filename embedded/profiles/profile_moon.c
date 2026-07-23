/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * profile_moon.c -- the Moon feature slice, the heaviest in the
 * library: the abridged ELP-2000/82 series (direction and distance
 * with derived HP/SD), plus the Moon-specific correction chain the
 * derived values feed -- augmentation and the augmented-limb
 * Hs -> Ho chains. 12 epochs across the full time domain.
 */

#include "astro_nav.h"
#include "harness.h"

const char profile_name[] = "moon";

uint64_t profile_run(void)
{
    uint64_t h = fnv1a_init();
    uint64_t s = 0xa4093822299f31d0ULL;

    for (int k = 0; k < 12; k++) {
        int64_t ut1 = emb_rng_ms(&s, ASTRO_NAV_TIME_ABS_MAX_MS);
        int64_t dtt = emb_rng_range(&s, -600000, 600000);

        astro_nav_unitvec_t vi, vef;
        astro_nav_moon_inertial(ut1 + dtt, &vi);
        astro_nav_moon_earthfixed(ut1, dtt, &vef);

        int32_t dist_km, sd, hp;
        astro_nav_moon_distance(ut1, dtt, &dist_km, &sd, &hp);

        h = fnv1a_i64(h, ut1);
        h = fnv1a_i32(h, vi.x);
        h = fnv1a_i32(h, vi.y);
        h = fnv1a_i32(h, vi.z);
        h = fnv1a_i32(h, vef.x);
        h = fnv1a_i32(h, vef.y);
        h = fnv1a_i32(h, vef.z);
        h = fnv1a_i32(h, dist_km);
        h = fnv1a_i32(h, sd);
        h = fnv1a_i32(h, hp);

        /* The derived HP/SD feeding the Moon's own correction chain. */
        int64_t hs = emb_rng_range(&s, 60000, 5340000);
        int64_t ie = emb_rng_range(&s, -3000, 3000);
        int32_t eye = emb_rng_range(&s, 0, 3000);
        int limb = k % 3 - 1;
        h = fnv1a_i64(h, astro_nav_correct_altitude_moon_marcmin(
                              hs, ie, eye, hp, sd, limb));
        /* One sequenced statement per draw (unspecified argument
         * evaluation order, C99 6.5.2.2p10). */
        int32_t temp  = emb_rng_range(&s, -40, 45);
        int32_t press = emb_rng_range(&s, 900, 1050);
        h = fnv1a_i64(h, astro_nav_correct_altitude_moon_tp_marcmin(
                              hs, ie, eye, hp, sd, limb, temp, press));
        h = fnv1a_i64(h, astro_nav_moon_augmentation_marcmin(
                              emb_rng_range(&s, -5400000, 5400000), hp, sd));
    }
    return h;
}
