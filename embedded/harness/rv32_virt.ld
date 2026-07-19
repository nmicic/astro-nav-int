/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * rv32_virt.ld -- QEMU virt machine with -bios none: everything in RAM
 * at 0x80000000. There is no flash region on this machine; the size
 * table reports text+rodata as the flash-equivalent number.
 */

MEMORY
{
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 4M
}

ENTRY(_start)

SECTIONS
{
  .text : {
    KEEP(*(.start))
    *(.text*)
    *(.rodata*)
    *(.srodata*)
    . = ALIGN(4);
  } > RAM

  .data : {
    . = ALIGN(4);
    *(.data*)
    *(.sdata*)
    . = ALIGN(4);
  } > RAM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start = .;
    *(.sbss*)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
