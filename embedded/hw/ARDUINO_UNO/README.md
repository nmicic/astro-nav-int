# HW test: Arduino Uno (ATmega328P, 8-bit AVR) — core + sun profile slices

Status: **PASS × 2** (2026-07-27, tree `055eb70ac8a6`, clean)

First 8-bit target. The whole library does NOT fit the ATmega328P
(measured 2026-07-27, `-O2`, atmelavr@5.3.0 / avr-gcc 7.3: `all`
130,350 B, `fix` 65,070 B, `moon` 49,290 B vs 32,256 B usable), but the
per-feature slices `core` and `sun` DO — the per-feature profile split is
what makes the library hardware-testable on classic AVR at all. This
folder runs the CORE slice; `../ARDUINO_UNO_SUN/` the SUN slice.

| slice | hash (== host reference) | flash | static RAM | runtime |
|---|---|---:|---:|---:|
| core (`profile_core.c`, this folder) | `c06268fe1c98abe4` | 23,132 B (71.7%) | 852 B | ~2.6 s |
| sun (`../ARDUINO_UNO_SUN/`) | `f6d799f2b7e424a7` | 24,342 B (75.5%) | 784 B | ~9.1 s |

Raw captures: `results/2026-07-27-uno-core.txt`,
`../ARDUINO_UNO_SUN/results/2026-07-27-uno-sun.txt`.

Why this PASS is meaningful beyond "it fits": an 8-bit AVR has no native
32-bit registers at all — every 32/64-bit operation in `fp_math.h`'s
portable backend is synthesized by avr-gcc and avr-libgcc. Bit-equality
with the host's `__int128` backend therefore crosses 8-bit vs 64-bit
register width, both compilers' 64-bit division/multiply helper chains,
and a third endianness/ABI regime. The stack question (QEMU Armv6-M
high-water for core is 1,664 B vs ~1.2 KB free on the Uno) resolved
empirically: the hash cannot survive a stack smash, and it reproduced
repeatedly on both slices.

## Hardware

- Board: Arduino Uno **R1/R2-era** (`VID:PID 2341:0001`, ATmega8U2 USB
  bridge), ATmega328P @ 16 MHz, 32 KB flash (31.5 KB usable under
  optiboot), 2 KB SRAM
- USB serial `6493234363835121B0B0`, one CDC port; opening the port
  DTR-resets the board (normal Uno behavior — capture just waits through
  the bootloader)

## Config

- Platform: `atmelavr@5.3.0` (registry, version-pinned), `board = uno`,
  `-O2`, avr-gcc 7.3.0, avrdude 6.3
- Upload: avrdude/optiboot over the CDC port, no button presses needed
- The harness `main.cpp` is byte-identical to the 32-bit targets' — it
  already derives `arch avr8` from `__AVR__`; only `prepare.sh` differs
  (copies `profile_core.c` instead of `profile_all.c` and pins against the
  host's `PROFILE core` line)
