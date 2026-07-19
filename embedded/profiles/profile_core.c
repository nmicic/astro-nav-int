/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * profile_core.c -- the Method C hot path: 16 unit vectors built from
 * angles (the human boundary, inbound), 64 reductions (8 observers x 8
 * bodies), and the outbound boundary readouts. This is the minimal
 * "one sight to one machine sight" feature slice.
 */

#include "astro_nav.h"
#include "harness.h"

const char profile_name[] = "core";

#define N_OBS  8
#define N_BODY 8

uint64_t profile_run(void)
{
    uint64_t h = fnv1a_init();
    uint64_t s = 0x243f6a8885a308d3ULL;

    astro_nav_unitvec_t obs[N_OBS], body[N_BODY];
    for (int i = 0; i < N_OBS; i++) {
        astro_nav_unitvec_from_cdeg(emb_rng_range(&s, -8900, 8900),
                                    emb_rng_range(&s, -17999, 18000),
                                    &obs[i]);
        h = fnv1a_i32(h, obs[i].x);
        h = fnv1a_i32(h, obs[i].y);
        h = fnv1a_i32(h, obs[i].z);
    }
    for (int i = 0; i < N_BODY; i++) {
        /* A body enters as declination and MINUS its GHA. */
        astro_nav_unitvec_from_cdeg(emb_rng_range(&s, -8900, 8900),
                                    -emb_rng_range(&s, 0, 35999),
                                    &body[i]);
        h = fnv1a_i32(h, body[i].x);
        h = fnv1a_i32(h, body[i].y);
        h = fnv1a_i32(h, body[i].z);
    }

    for (int i = 0; i < N_OBS; i++) {
        for (int j = 0; j < N_BODY; j++) {
            astro_nav_machine_sight_t ms;
            astro_nav_reduce_method_c(&obs[i], &body[j], &ms);
            h = fnv1a_i32(h, ms.sin_hc_q30);
            h = fnv1a_u64(h, ms.square_key);
            h = fnv1a_i32(h, ms.zn_valid);
            h = fnv1a_i32(h, astro_nav_hc_cdeg_from_sin_q30(ms.sin_hc_q30));
            h = fnv1a_i32(h, astro_nav_zn_cdeg_from_square_key(ms.square_key));
        }
    }
    return h;
}
