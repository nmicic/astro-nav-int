# HW test: Seeed Studio XIAO RP2040 — `all` profile hash on Armv6-M

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

The RP2040's Cortex-M0+ is the narrowest ISA in the matrix — Thumb-1, no
hardware divide (Armv6-M is the worst-case column in
`embedded/README.md`'s tables). Runs `embedded/profiles/profile_all.c`
(portable two-limb backend) and reproduces the host reference bit-exactly.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **XIAO RP2040 Cortex-M0+ (Armv6-M)** | **`37d6a1208d99182f`** | 501 ms |

Raw capture: `results/2026-07-27-xiao-rp2040.txt`.

## Hardware

- Board: Seeed Studio XIAO RP2040, RP2040 (2× Cortex-M0+ @ 133 MHz, no FPU),
  USB serial `4250305539333104`
- Enumerates as `VID:PID 2E8A:000A` (running firmware), one CDC port

## Config

- Platform: maxgerhardt `platform-raspberrypi` (arduino-pico core), pinned
  `#aa70b802be8851668053d4f09734e4089fe41932` (same as the Pico 2 targets),
  `board = seeed_xiao_rp2040`, `-O2`
- Image: 85,252 B flash (4.1% of 2 MB), 9,132 B static RAM
- Upload: arduino-pico 1200-baud touch, capture on the same CDC port
