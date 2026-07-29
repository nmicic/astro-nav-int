# HW test: Arduino MKR Zero — Microchip SAMD21

Status: **PASS** (2026-07-29, tree `9ce2ac444641`)

The full `all` profile ran on the SAMD21 and reproduced the host-computed
hash:

| | hash | runtime (informational) |
|---|---|---:|
| host pin | `37d6a1208d99182f` | — |
| **SAMD21 Cortex-M0+ @ 48 MHz** | **`37d6a1208d99182f`** | **1,576–1,580 ms** |

Raw transcript:
[`results/2026-07-29-mkrzero-samd21.txt`](results/2026-07-29-mkrzero-samd21.txt).
The image used 37,488 B flash (14.3% of 256 KB) and 2,816 B static RAM
(8.6% of 32 KB).

## Hardware

- Board: Arduino MKR Zero
- USB: `VID:PID 2341:804f`, description `Arduino MKRZERO`, serial
  `612FDA905030534D4D2E3120FF122832`
- MCU: Microchip ATSAMD21G18A, Cortex-M0+ @ 48 MHz, 256 KB flash / 32 KB
  SRAM, no FPU
- The SD and audio peripherals were not exercised
- Flashing: PlatformIO's 1200-baud reset and `bossac` upload over native USB

## Config

- Platform: registry `atmelsam @ 8.3.0`
- Framework: `framework-arduino-samd @ 1.8.14`
- Toolchain: `toolchain-gccarmnoneeabi @ 1.70201.0` (GCC 7.2.1)
- PlatformIO board: `mkrzero`
- Run: `sh run_test.sh ARDUINO_MKR_ZERO /dev/cu.usbmodem11301`

Process: [`../HOWTO.md`](../HOWTO.md).
