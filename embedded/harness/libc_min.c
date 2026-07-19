/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * libc_min.c -- the four functions a freestanding C implementation is
 * still entitled to call (C99 note: GCC may lower struct copies and
 * aggregate initialization to memcpy/memset even with -ffreestanding).
 * Compiled with -fno-builtin -fno-tree-loop-distribute-patterns so the
 * loops below cannot be recognized and turned back into calls to
 * themselves. Unreferenced ones are dropped by --gc-sections.
 */

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        while (n--)
            d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    for (; n--; pa++, pb++) {
        if (*pa != *pb)
            return *pa < *pb ? -1 : 1;
    }
    return 0;
}
