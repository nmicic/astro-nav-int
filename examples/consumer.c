/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * examples/consumer.c -- standalone consumer smoke test for the
 * library artifact.
 *
 * This program includes no project header but astro_nav.h (the
 * standard headers below are this example's own I/O) and links no
 * project code but libastro_nav.a: two project artifacts, plus the
 * toolchain's normal compiler runtime (__int128 division helpers and
 * such -- nothing floating-point).
 * fp_math.h is a build-time detail of the library's own translation
 * unit, never part of its interface, and there is no setup call --
 * the internal fixed-point tables fill themselves on first use.
 * (Threaded consumers: make one angle-consuming call, e.g.
 * astro_nav_sin_q30_from_cdeg(0), from a single thread before going
 * parallel -- docs/HOWTO.md section 6.)
 * Build and run, from the repository root:
 *
 *     make lib
 *     cc -std=c99 -Wall -Wextra -Werror -O2 -I. \
 *        -o consumer_smoke examples/consumer.c libastro_nav.a
 *     ./consumer_smoke
 *
 * No -lm anywhere -- `make check-lib` runs exactly this build and
 * then fails if the archive or this binary imports any
 * floating-point symbol.
 *
 * The scenario is the Bris star pair pinned in the Makefile test
 * schedule: two 30-degree sights, Deneb then Altair, taken from
 * 45 N 0 E on the evening of 2026-06-30 (instants in UT1 ms from
 * J2000), reduced machine-natively -- vectors in, position out, no
 * angle anywhere in between. Exit 0 iff the recovered position is
 * exactly 45.00 N 0.00 E in centidegrees, the same equality
 * `make test` pins through the CLI.
 */

#include "astro_nav.h"

#include <stdio.h>
#include <string.h>

static const astro_nav_unitvec_t *star_named(const char *name)
{
    int i;
    for (i = 0; i < ASTRO_NAV_STAR_COUNT; i++)
        if (strcmp(astro_nav_stars[i].name, name) == 0)
            return &astro_nav_stars[i].j2000;
    return 0;
}

int main(void)
{
    const astro_nav_unitvec_t *deneb_j2000 = star_named("Deneb");
    const astro_nav_unitvec_t *altair_j2000 = star_named("Altair");
    if (!deneb_j2000 || !altair_j2000) {
        fprintf(stderr, "consumer_smoke: star catalog lookup failed\n");
        return 1;
    }

    /* Almanac: rotate each catalog vector into the earth-fixed frame
     * of its own sight instant. */
    astro_nav_unitvec_t deneb, altair;
    astro_nav_celestial_to_earthfixed(deneb_j2000, 836121913051LL, &deneb);
    astro_nav_celestial_to_earthfixed(altair_j2000, 836127121859LL, &altair);

    /* Both corrected altitudes are exactly 30 degrees = 1800000
     * milli-arcminutes; the fix consumes sin(Ho) in Q2.30. */
    int32_t sin_ho = astro_nav_sin_q30_from_marcmin(1800000);

    /* A rough dead-reckoning position (44 N, 1 W) picks between the
     * two intersections of the equal-altitude circles. */
    astro_nav_unitvec_t dr;
    astro_nav_unitvec_from_cdeg(4400, -100, &dr);

    astro_nav_fix_result_t fix;
    astro_nav_fix_two_body(&deneb, sin_ho, &altair, sin_ho, &dr, &fix);
    if (!fix.valid) {
        fprintf(stderr, "consumer_smoke: no fix\n");
        return 1;
    }

    int32_t lat_cdeg, lon_cdeg;
    astro_nav_latlon_cdeg_from_unitvec(&fix.position, &lat_cdeg, &lon_cdeg);
    printf("consumer_smoke: fix lat %ld cdeg, lon %ld cdeg"
           " (expected 4500, 0)\n", (long)lat_cdeg, (long)lon_cdeg);

    return (lat_cdeg == 4500 && lon_cdeg == 0) ? 0 : 1;
}
