# HW test: Raspberry Pi Pico 2 (RP2350, ARM mode) — `all` profile hash

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

Runs `embedded/profiles/profile_all.c` (every public entry point, portable
two-limb backend) on the RP2350's Cortex-M33 cores (ARM mode) and
reproduces the host reference hash bit-exactly.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **Pico 2 Cortex-M33 (ARM)** | **`37d6a1208d99182f`** | 188 ms |

Raw capture: `results/2026-07-27-pico2-arm.txt`.

## Hardware

- Board: Raspberry Pi Pico 2, RP2350, USB serial `902712418212448D`
- Same physical board as `PI_PICO_RP2350_RISCV` (one folder per ISA mode)
- Enumerates as `VID:PID 2E8A:000F`, one CDC port (`/dev/cu.usbmodemXXXX`)

## Config

- Platform: maxgerhardt `platform-raspberrypi` (arduino-pico core), pinned
  `#aa70b802be8851668053d4f09734e4089fe41932`; the registry platform
  cannot target RP2350
- `board = rpipico2`, `-O2`
- Image: 82,680 B flash (2.0% of 4 MB), 9,948 B static RAM
- Upload: arduino-pico 1200-baud touch over the running harness firmware
