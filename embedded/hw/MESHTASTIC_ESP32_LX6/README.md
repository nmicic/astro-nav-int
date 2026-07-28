# HW test: LILYGO T-Beam (Meshtastic node) — `all` profile on Xtensa LX6

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

Second Xtensa target, but a different core generation than the Heltec V3:
the original dual-core LX6 (ESP32-D0WDQ6-V3) vs the S3's LX7. The board is
a LILYGO T-Beam running Meshtastic (LoRa radio and GPS unused by this
test); the owner reflashes Meshtastic afterwards, so no flash backup was
kept.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **ESP32-D0WDQ6-V3 Xtensa LX6 @ 240 MHz** | **`37d6a1208d99182f`** | 195 ms |

Raw capture: `results/2026-07-27-esp32-lx6.txt`.

## Serial

Classic ESP32 has no native USB: the board's USB-C is a WCH CH9102
USB-UART bridge (`1A86:55D4`) to UART0, so `Serial` needs no CDC flag —
but the port name is `usbserial-*`, which the capture's auto-detect does
not match, so pass it explicitly:

```sh
sh run_test.sh MESHTASTIC_ESP32_LX6 /dev/cu.usbserial-58971216971
```

Flashing needs no button: esptool auto-resets over the bridge's handshake
lines and works over any running firmware (including Meshtastic).

## Hardware

- Board: LILYGO T-Beam (Meshtastic node), `VID:PID 1A86:55D4`
  (CH9102 bridge),
  USB serial `5897121697`
- SoC: Espressif ESP32-D0WDQ6-V3 (rev v3.1), dual-core Xtensa LX6
  @ 240 MHz, 4 MB flash (Winbond `ef 4016`) / 320 KB SRAM
- MAC: `a0:dd:6c:74:02:84`

## Config

- Platform: pioarduino `platform-espressif32` release `54.03.21-2`
  (same pinned zip as the other Espressif targets), `board = esp32dev`
  (generic devkit definition — the test uses only UART0 and the CPU,
  no board-specific pins), `-O2`
- Image: 329,762 B flash (25.2% of the 1.3 MB app partition),
  21,088 B static RAM (6.4%)
