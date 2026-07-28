# HW test: Arduino Mega 2560 (ATmega2560, 8-bit AVR) — FULL `all` profile

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

The only classic AVR that fits the whole library — and it reproduces the
SAME `all`-profile hash as the host and every 32-bit target: one number,
`37d6a1208d99182f`, now spans arm64+`__int128`, Cortex-M33, Hazard3 rv32,
RV32IMAC, Cortex-M0+, and an 8-bit ATmega2560.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **Mega 2560 ATmega2560 (8-bit AVR)** | **`37d6a1208d99182f`** | 26.5 s |

Raw capture: `results/2026-07-27-mega2560-all.txt`.

The stack question resolved empirically: 8,192 B SRAM − 2,448 B static
left ~5.7 KB for stack against a 5,912 B QEMU high-water on Armv6-M
(the worst 32-bit column, `fix_n_body`'s work arrays dominating); the
AVR's actual high-water is evidently below that, and the fail-closed hash
reproduced over repeated iterations.

## Hardware

- Board: Arduino Mega 2560 (`VID:PID 2341:0010`, ATmega16U2 bridge),
  ATmega2560 @ 16 MHz, 256 KB flash (253,952 B usable), 8 KB SRAM
- USB serial `64932343638351317162`; DTR auto-reset on port open as usual

## Config

- Platform: `atmelavr@5.3.0`, `board = megaatmega2560`, `-O2`,
  avr-gcc 7.3.0; upload via avrdude (stk500v2/wiring)
- Image: 130,774 B flash (51.5%), 2,448 B static RAM
- `prepare.sh` is byte-identical to the 32-bit targets' (full
  `profile_all.c`, host-pinned at prepare time); only `platformio.ini`
  differs
- Capture run with `WINDOW=150` (`run_test.sh` env override): one profile
  iteration takes ~26.5 s on this core, so the default 15 s listen window
  would time out before the first record
