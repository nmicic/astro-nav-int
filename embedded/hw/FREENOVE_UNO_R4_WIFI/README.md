# HW test: Freenove Control Board V5 Rev4 — Renesas RA4M1

Status: **PASS** (2026-07-29, tree `9ce2ac444641`)

The full `all` profile ran on the board's RA4M1 application MCU and
reproduced the host-computed hash:

| | hash | runtime (informational) |
|---|---|---:|
| host pin | `37d6a1208d99182f` | — |
| **RA4M1 Cortex-M4F @ 48 MHz** | **`37d6a1208d99182f`** | **968–970 ms** |

Raw transcript:
[`results/2026-07-29-freenove-ra4m1.txt`](results/2026-07-29-freenove-ra4m1.txt).
The image used 60,056 B flash (22.9% of 256 KB) and 2,888 B static RAM
(8.8% of 32 KB).

## Hardware

- Board: Freenove Control Board V5 Rev4 WiFi (Arduino Uno R4 WiFi
  compatible)
- USB: `VID:PID 2341:1002`, description `UNO WiFi R4 CMSIS-DAP`, serial
  `D885ACA78374`
- Target MCU: Renesas RA4M1, Cortex-M4F @ 48 MHz, 256 KB flash / 32 KB
  SRAM; its FPU is present but unused
- The onboard ESP32-S3 connectivity MCU and board peripherals were not
  exercised
- Flashing: unattended PlatformIO `sam-ba` upload over the board's USB port

## Config

- Platform: registry `renesas-ra @ 1.9.0`
- Framework: `framework-arduinorenesas-uno @ 1.6.0`
- Toolchain: `toolchain-gccarmnoneeabi @ 1.70201.0` (GCC 7.2.1)
- PlatformIO board: `uno_r4_wifi`
- Run:
  `sh run_test.sh FREENOVE_UNO_R4_WIFI /dev/cu.usbmodemD885ACA783742`

Process: [`../HOWTO.md`](../HOWTO.md).
