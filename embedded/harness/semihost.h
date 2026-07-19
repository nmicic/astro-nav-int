/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * semihost.h -- minimal semihosting console for the embedded harness.
 *
 * Both QEMU target families implement the same ARM-defined semihosting
 * operations (SYS_WRITE0 = 0x04, SYS_EXIT = 0x18); only the trap
 * instruction differs. RISC-V uses the RISC-V semihosting spec's
 * three-instruction sequence, which must not be split across a page --
 * hence the alignment -- and must use uncompressed encodings.
 */

#ifndef EMB_SEMIHOST_H
#define EMB_SEMIHOST_H

#if defined(__arm__)

static inline long sh_call(long op, void *arg)
{
    register long  r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

#elif defined(__riscv)

static inline long sh_call(long op, void *arg)
{
    register long  a0 __asm__("a0") = op;
    register void *a1 __asm__("a1") = arg;
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        ".balign 16\n"
        "slli x0, x0, 0x1f\n"
        "ebreak\n"
        "srai x0, x0, 7\n"
        ".option pop\n"
        : "+r"(a0) : "r"(a1) : "memory");
    return a0;
}

#else
#error "semihost.h supports ARM and RISC-V targets only"
#endif

static inline void sh_write0(const char *s)
{
    sh_call(0x04, (void *)s);
}

/* ADP_Stopped_ApplicationExit: QEMU turns this into host exit code 0. */
__attribute__((noreturn))
static inline void sh_exit(void)
{
    sh_call(0x18, (void *)0x20026);
    for (;;) { }
}

#endif /* EMB_SEMIHOST_H */
