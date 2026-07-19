/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * start_rv32.c -- RV32 startup for QEMU's virt machine with -bios
 * none: set the stack pointer, clear .bss, paint the stack, run the
 * harness. The ELF loader places .data directly in RAM, so unlike the
 * ARM flash targets there is nothing to copy.
 */

#include <stdint.h>

extern uint32_t __bss_start[], __bss_end[], __stack_top[];

void harness_main(void);
void start_c(void);

/* Where the paint window starts; harness_main() scans up from here.
 * Set after .bss is zeroed, so the zeroing cannot clobber it. */
uint32_t *emb_stack_paint_base;

#define STACK_PAINT 0xa5a5a5a5u
#define STACK_PAINT_WORDS 8192 /* 32 KiB window, far above any real depth */

__attribute__((naked, section(".start"), used))
void _start(void)
{
    __asm__ volatile(
        "la sp, __stack_top\n"
        "call start_c\n"
        "1: j 1b\n");
}

__attribute__((used))
void start_c(void)
{
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
}
