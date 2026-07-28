# HW test: ESP32-C6 devkit — `all` profile hash on real RISC-V silicon

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

First astro-nav-int run on physical hardware. The target executes
`embedded/profiles/profile_all.c` (every public entry point at least once,
portable two-limb backend of `fp_math.h`) and reproduces the host reference
hash (native `__int128` backend) bit-exactly — a cross-backend AND
cross-ISA check on real silicon, same semantics as the repo's QEMU
`make check` gate.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **ESP32-C6 (RISC-V rv32imac, two-limb backend)** | **`37d6a1208d99182f`** | 274 ms |

Raw capture: `results/2026-07-27-esp32-c6.txt` (append-only, provenance
header + transcript).

## Pin provenance

astro-nav-int's profile TUs carry no committed pin — host equality is the
contract. `prepare.sh` therefore builds and runs the host reference
(`embedded/harness/host_main.c` + the same copied `profile_all.c` +
`astro_nav.c`) at prepare time and generates `include/profile_pin.h` from
its output. The wrapper hardcodes nothing.

## Hardware

- Board: ESP32-C6 devkit (DevKitC-1 form factor), chip revision v0.2, QFN40,
  base MAC `ac:eb:e6:0e:65:e8`
- SoC: ESP32-C6, 1× RV32IMAC @ 160 MHz + LP core, no FPU
- Enumerates as TWO serial ports for one board (same MAC via
  `esptool chip-id` on both):
  - `VID:PID 303A:1001` — native USB-Serial/JTAG (do NOT capture here)
  - `VID:PID 1A86:55D3` — CH343 USB-UART bridge wired to UART0 (capture here)

Run with the CH343 port explicit:

    sh run_test.sh ESP32_C6 /dev/cu.usbmodem<CH343-serial>

## Config

- Platform: pioarduino `platform-espressif32` release **54.03.21-2** (pinned
  release artifact; registry platform's Arduino core 2.x has no C6 support)
- Arduino core 3.2.1 (ESP-IDF 5.4 libs), toolchain-riscv32-esp gcc 14.2.0,
  `board = esp32-c6-devkitc-1`, `-O2`
- `ARDUINO_USB_CDC_ON_BOOT=0` pins `Serial` to UART0 — the native
  USB-Serial/JTAG CDC drops TX without the right DTR state and can be reset
  into download mode just by opening it
- Image size: 272,923 B flash (20.8% of the 1.3 MB app partition; includes
  the whole Arduino/ESP-IDF baseline), 13,244 B static RAM
