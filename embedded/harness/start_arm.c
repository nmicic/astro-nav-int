/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * start_arm.c -- Cortex-M startup: vector table, .data copy, .bss
 * clear, stack paint, then the harness. Freestanding on purpose: no C
 * runtime, no newlib, so the linked image is the library plus this
 * file and nothing else.
 */

#include <stdint.h>

extern uint32_t __data_start[], __data_end[], __data_lma[];
extern uint32_t __bss_start[], __bss_end[], __stack_top[];

void harness_main(void);

/* Where the paint window starts; harness_main() scans up from here.
 * Set after .bss is zeroed, so the zeroing cannot clobber it. */
uint32_t *emb_stack_paint_base;

#define STACK_PAINT 0xa5a5a5a5u
#define STACK_PAINT_WORDS 8192 /* 32 KiB window, far above any real depth */

void reset_handler(void)
{
    const uint32_t *src = __data_lma;
    for (uint32_t *dst = __data_start; dst < __data_end; dst++)
        *dst = *src++;
    for (uint32_t *p = __bss_start; p < __bss_end; p++)
        *p = 0;

    /* Paint a fixed window below this frame (minus a margin for the
     * words this function itself has live) so harness_main() can
     * report the stack high-water mark. A fixed window, not the whole
     * bss-to-stack gap: painting all of RAM would make the harness's
     * instruction count scale with the machine's memory size. */
    uint32_t *top = (uint32_t *)((uintptr_t)__builtin_frame_address(0) - 64u);
    uint32_t *base = top - STACK_PAINT_WORDS;
    if (base < __bss_end)
        base = __bss_end;
    for (uint32_t *p = base; p < top; p++)
        *p = STACK_PAINT;
    emb_stack_paint_base = base;

    harness_main();
    for (;;) { }
}

/* Initial SP + reset vector is all these QEMU machines need. */
__attribute__((section(".vectors"), used))
static const void *const vectors[] = {
    __stack_top,
    (const void *)reset_handler,
};
