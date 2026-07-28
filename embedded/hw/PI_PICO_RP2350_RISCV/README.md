# HW test: Raspberry Pi Pico 2 (RP2350, RISC-V mode) — `all` profile hash

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

Same physical board as `PI_PICO_RP2350`, switched to its Hazard3 RISC-V
(rv32) cores via `board_build.mcu = rp2350-riscv`. Together with the
ESP32-C6 PASS this is the second independent RISC-V implementation
(different vendor, different core design) reproducing the same hash.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **Pico 2 Hazard3 (RISC-V rv32)** | **`37d6a1208d99182f`** | 272 ms |

Raw capture: `results/2026-07-27-pico2-riscv.txt`.

## Quirk hit on this run

After flashing the RISC-V image over the running ARM-mode firmware, the CDC
port re-enumerated slowly enough that the capture's first open failed
(`No such file or directory`); the transcript in `results/` is from the
immediate re-capture (same firmware, which loops its record every 3 s).
`capture_serial.py` now retries an explicitly-given port for up to 20 s.

## Config

- Platform: maxgerhardt `platform-raspberrypi` pinned
  `#aa70b802be8851668053d4f09734e4089fe41932`, `board = rpipico2`,
  `board_build.mcu = rp2350-riscv`, `-O2`
- Image: 108,208 B flash (2.6% of 4 MB), 22,172 B static RAM
