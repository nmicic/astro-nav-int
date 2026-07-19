/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * profile_baseline.c -- the empty profile: no library call at all, so
 * --gc-sections drops every astro_nav/fp_math section and what remains
 * is the harness floor (startup, semihosting, print helpers). Every
 * other profile's cost is read against this baseline.
 */

#include "harness.h"

const char profile_name[] = "baseline";

uint64_t profile_run(void)
{
    uint64_t h = fnv1a_init();
    for (int i = 0; i < 4; i++)
        h = fnv1a_u64(h, (uint64_t)i);
    return h;
}
