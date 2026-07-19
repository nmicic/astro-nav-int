/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * harness_main.c -- run the linked profile, print its hash and the
 * stack high-water mark over semihosting, exit. The PROFILE line must
 * be byte-identical to the host build's (host_main.c): the measurement
 * scripts compare the two lines verbatim.
 */

#include <stdint.h>

#include "harness.h"
#include "semihost.h"

extern uint32_t __stack_top[];
extern uint32_t *emb_stack_paint_base;

#define STACK_PAINT 0xa5a5a5a5u

static void put_hex64(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = digits[v & 0xf];
        v >>= 4;
    }
    buf[16] = '\0';
    sh_write0(buf);
}

static void put_u32(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    sh_write0(buf + i);
}

void harness_main(void)
{
    uint64_t h = profile_run();

    sh_write0("PROFILE ");
    sh_write0(profile_name);
    sh_write0(" hash=0x");
    put_hex64(h);
    sh_write0("\n");

    /* The startup code painted a window ending at initial SP minus a
     * small margin; the first word scanning upward that no longer
     * holds the pattern is the deepest the stack ever reached. The
     * margin makes this a lower bound that can read up to 64 bytes
     * short on a profile that never went deeper than the margin --
     * every real profile goes far deeper. A profile deeper than the
     * whole window would saturate at the window size; the window is
     * kept several times larger than the worst depth measured. */
    const uint32_t *p = emb_stack_paint_base;
    while (p < __stack_top && *p == STACK_PAINT)
        p++;
    sh_write0("stack-high-water: ");
    put_u32((uint32_t)((uintptr_t)__stack_top - (uintptr_t)p));
    sh_write0(" bytes\n");

    sh_exit();
}
