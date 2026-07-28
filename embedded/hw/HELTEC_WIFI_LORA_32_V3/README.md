# HW test: Heltec WiFi LoRa 32 V3 — `all` profile on ESP32-S3 (Xtensa LX7)

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

First Xtensa target — the fourth ISA family in the matrix (ARM, RISC-V,
AVR8, Xtensa) reproducing the same host-computed hash. Also the fastest
MCU result in this matrix: dual-core LX7 @ 240 MHz.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **Heltec V3 ESP32-S3 Xtensa LX7 @ 240 MHz** | **`37d6a1208d99182f`** | 186 ms |

Raw capture: `results/2026-07-27-heltec-esp32-s3.txt`.

## Serial quirk

The board's USB-C goes through a CP2102 USB-UART bridge (`10C4:EA60`,
shows up as `/dev/cu.usbserial-0001`) to UART0 — the S3's native USB is
NOT wired to the connector, so there is only ONE port. Two consequences:

- `-DARDUINO_USB_CDC_ON_BOOT=0` keeps `Serial` on UART0 (same reasoning
  as `ESP32_C6`, which has both connectors).
- `capture_serial.py`'s auto-detect only matches `usbmodem*` names, so
  the port must be passed explicitly:

```sh
sh run_test.sh HELTEC_WIFI_LORA_32_V3 /dev/cu.usbserial-0001
```

Flashing needs no button: esptool auto-resets over the bridge's
handshake lines and works over any running firmware.

## Hardware

- Board: Heltec WiFi LoRa 32 V3 (SX1262 LoRa radio + OLED, both unused
  by this test)
- SoC: Espressif ESP32-S3 (QFN56, rev v0.2), dual-core Xtensa LX7
  @ 240 MHz, 8 MB flash / 320 KB SRAM
- USB: CP2102 UART bridge only, `VID:PID 10C4:EA60`

## Config

- Platform: pioarduino `platform-espressif32` release `54.03.21-2`
  (Arduino core 3.x — same pinned zip as `ESP32_C6`),
  `board = heltec_wifi_lora_32_V3`, `-O2`
- Image: 333,670 B flash (10.0%), 20,336 B static RAM (6.2%)
