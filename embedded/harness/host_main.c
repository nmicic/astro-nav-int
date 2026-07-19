/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * host_main.c -- run the same profile translation unit on the host and
 * print the reference PROFILE line the embedded targets must match.
 */

#include <stdio.h>

#include "harness.h"

int main(void)
{
    printf("PROFILE %s hash=0x%016llx\n", profile_name,
           (unsigned long long)profile_run());
    return 0;
}
