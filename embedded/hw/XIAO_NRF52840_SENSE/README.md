# HW test: Seeed XIAO nRF52840 Sense — `all` profile on Cortex-M4F (Nordic)

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

First Nordic silicon and first Cortex-M4F target — an FPU-capable core
running the FPU-less library: bit-equality with the host hash is the
arbiter that no float contaminates the integer results (the same property
`make measure`'s FP audits prove for the QEMU M4-softfloat images).

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **XIAO nRF52840 Sense Cortex-M4F @ 64 MHz** | **`37d6a1208d99182f`** | 689 ms |

Raw capture: `results/2026-07-27-xiao-nrf52840.txt`.

## Flashing (factory-firmware quirk)

Factory firmware ignores the arduino 1200-baud DFU touch, and PlatformIO
banners SUCCESS even when adafruit-nrfutil reports "Target is not in DFU
mode" — never trust the flasher banner. Procedure that worked:

1. Operator double-taps the RST pad to GND (mouse-double-click rhythm) —
   the `XIAO-SENSE` mass-storage drive mounts, board is in DFU mode.
2. `pio run -t upload` NOW works as a real DFU: adafruit-nrfutil prints
   "Device programmed. Activating new firmware."
   (This platform build produced `firmware.zip`/`.hex` only, no `.uf2`,
   so drag-and-drop wasn't available — DFU upload from bootloader mode is
   the reliable path.)
3. From here on, re-flashing needs NO button: the harness firmware's
   TinyUSB CDC honors the 1200-baud touch.

## Hardware

- Board: Seeed Studio XIAO nRF52840 Sense, `VID:PID 2886:8045`,
  USB serial `112C2E2F402F774A`
- SoC: Nordic nRF52840, Cortex-M4F @ 64 MHz, 1 MB flash / 256 KB RAM,
  Adafruit UF2/DFU bootloader

## Config

- Platform: maxgerhardt `platform-nordicnrf52` fork pinned
  `#cac6fcf943a41accd2aeb4f3659ae297a73f422e` (registry platform lacks the
  board), Adafruit nRF52 core, `board = xiaoble_adafruit`, `-O2`
- `-DUSE_TINYUSB` required (`Serial` lives in TinyUSB; link fails without
  it); `main.cpp` guards `#include <Adafruit_TinyUSB.h>` behind it
- Image: 127,708 B flash (15.7%), 7,540 B static RAM
