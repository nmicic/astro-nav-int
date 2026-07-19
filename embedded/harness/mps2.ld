/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * mps2.ld -- QEMU mps2-an385 (Cortex-M3) and mps2-an386 (Cortex-M4):
 * code at 0x00000000, SRAM at 0x20000000. The FLASH region is ZBT
 * SSRAM on the real board, but treating it as flash keeps the
 * flash/RAM split honest for a microcontroller reading.
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 1024K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

SECTIONS
{
  .text : {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  .data : {
    . = ALIGN(4);
    __data_start = .;
    *(.data*)
    . = ALIGN(4);
    __data_end = .;
  } > RAM AT > FLASH
  __data_lma = LOADADDR(.data);

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
