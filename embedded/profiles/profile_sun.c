/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * profile_sun.c -- the Sun almanac feature slice: apparent direction
 * (inertial and earth-fixed, i.e. including the precession + earth
 * rotation machinery), distance with its derived SD/HP, and the GHA
 * Aries human boundary. 32 epochs across the library's full +-100 year
 * time domain, each with its own TT - UT1 across the full +-10 minute
 * contract bound.
 */

#include "astro_nav.h"
#include "harness.h"

const char profile_name[] = "sun";

uint64_t profile_run(void)
{
    uint64_t h = fnv1a_init();
    uint64_t s = 0x13198a2e03707344ULL;

    for (int k = 0; k < 32; k++) {
        int64_t ut1 = emb_rng_ms(&s, ASTRO_NAV_TIME_ABS_MAX_MS);
        int64_t dtt = emb_rng_range(&s, -600000, 600000);

        astro_nav_unitvec_t vi, vef;
        astro_nav_sun_inertial(ut1 + dtt, &vi);
        astro_nav_sun_earthfixed(ut1, dtt, &vef);

        int32_t dist_uau, sd, hp;
        astro_nav_sun_distance(ut1, dtt, &dist_uau, &sd, &hp);

        h = fnv1a_i64(h, ut1);
        h = fnv1a_i32(h, vi.x);
        h = fnv1a_i32(h, vi.y);
        h = fnv1a_i32(h, vi.z);
        h = fnv1a_i32(h, vef.x);
        h = fnv1a_i32(h, vef.y);
        h = fnv1a_i32(h, vef.z);
        h = fnv1a_i32(h, dist_uau);
        h = fnv1a_i32(h, sd);
        h = fnv1a_i32(h, hp);
        h = fnv1a_i32(h, astro_nav_gha_aries_cdeg(ut1));
    }
    return h;
}
