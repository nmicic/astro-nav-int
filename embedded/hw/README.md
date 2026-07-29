# embedded/hw/ — operator hardware-in-the-loop testing

**Record-only, out-of-cycle.** These are operator-run records from
physical boards; they are not part of any automated gate (`make check`
under QEMU remains the gate). Full process, porting notes, and
troubleshooting: `HOWTO.md` in this folder.

What a target runs: `embedded/profiles/profile_all.c` — every public entry
point at least once — with the expected hash computed by a HOST build of the
same TU at prepare time (this repo pins nothing in the profile TUs; host
equality is the contract, exactly like `make check` under QEMU). PASS means
the portable two-limb backend on target silicon reproduces the host's native
`__int128` backend bit-exactly.

## Targets

| target | device | core tested | status |
|---|---|---|---|
| `ESP32_C6` | ESP32-C6 devkit | RV32IMAC (RISC-V, no FPU) | PASS @ `055eb70` — hash `37d6a1208d99182f`, 274 ms |
| `PI_PICO_RP2350` | Raspberry Pi Pico 2 | Cortex-M33 (ARM) | PASS @ `055eb70` — same hash, 188 ms |
| `PI_PICO_RP2350_RISCV` | Raspberry Pi Pico 2 (same board) | Hazard3 (RISC-V rv32) | PASS @ `055eb70` — same hash, 272 ms |
| `XIAO_RP2040` | Seeed Studio XIAO RP2040 | Cortex-M0+ (Armv6-M, no FPU, no HW divide) | PASS @ `055eb70` — same hash, 501 ms |
| `ARDUINO_UNO` | Arduino Uno (R1/R2-era) | ATmega328P (8-bit AVR) — `core` slice | PASS @ `055eb70` — hash `c06268fe1c98abe4`, ~2.6 s |
| `ARDUINO_UNO_SUN` | Arduino Uno (same card) | ATmega328P (8-bit AVR) — `sun` slice | PASS @ `055eb70` — hash `f6d799f2b7e424a7`, ~9.1 s |
| `ARDUINO_MEGA2560` | Arduino Mega 2560 | ATmega2560 (8-bit AVR) — FULL `all` profile | PASS @ `055eb70` — same hash as 32-bit targets, ~26.5 s (run with `WINDOW=150`) |
| `XIAO_NRF52840_SENSE` | Seeed XIAO nRF52840 Sense | Cortex-M4F (Nordic, FPU present, unused) | PASS @ `055eb70` — same hash, 689 ms (DFU entry via double-tap RST) |
| `HELTEC_WIFI_LORA_32_V3` | Heltec WiFi LoRa 32 V3 | ESP32-S3 Xtensa LX7 @ 240 MHz (4th ISA family) | PASS @ `055eb70` — same hash, 186 ms (CP2102 port: pass `/dev/cu.usbserial-*` explicitly) |
| `MESHTASTIC_ESP32_LX6` | LILYGO T-Beam (Meshtastic node) | ESP32-D0WDQ6-V3 Xtensa LX6 @ 240 MHz | PASS @ `055eb70` — same hash, 195 ms (CH9102 port: pass `/dev/cu.usbserial-*` explicitly) |
| `PI_1_MODEL_B_PLUS` | Raspberry Pi 1 B+ V1.2 (2014) | ARM1176JZF-S @ 700 MHz, ARMv6 Linux, NATIVE distro gcc | PASS @ `055eb70` — same hash, 71 ms (own runner: `harness/run_pi.sh`, no PlatformIO) |
| `FREENOVE_UNO_R4_WIFI` | Freenove Control Board V5 Rev4 WiFi | Renesas RA4M1 Cortex-M4F @ 48 MHz (FPU present, unused) | PASS @ `9ce2ac4` — same hash, 968–970 ms |
| `ARDUINO_MKR_ZERO` | Arduino MKR Zero | Microchip SAMD21 Cortex-M0+ @ 48 MHz (no FPU) | PASS @ `9ce2ac4` — same hash, 1,576–1,580 ms |

The `all`-profile hash every 32-bit target reproduces is `37d6a1208d99182f`;
the Uno rows use per-slice hashes (`core`/`sun`) because the full library
does not fit 32 KB — see the flash table below.

## Flash requirements (measured, AVR probe 2026-07-27)

Build-only probe with pio `atmelavr` (`-O2`, Arduino framework, gcc 7.3):

| profile | flash | static RAM | Uno/Nano (32 KB / 2 KB) | Mega2560 (256 KB / 8 KB) |
|---|---:|---:|---|---|
| core | 23,076 B | 856 B | **fits** (71.5%) | fits |
| sun  | 24,154 B | 788 B | **fits** (74.9%) | fits |
| fix  | 65,070 B | 788 B | no (201%) | fits |
| moon | 49,290 B | 1,990 B | no (flash AND RAM) | fits |
| all  | 130,350 B | 2,448 B | no (404%) | **fits** (51.3%, RAM 29.9%) |

- The library is hardware-testable on classic AVR: a Mega2560 runs the
  full `all` profile; an Uno/Nano can run the `core` and `sun` slices.
- The stack concerns both resolved empirically on 2026-07-27: the Uno's
  `core`/`sun` runs (~1.2 KB free for stack) and the Mega's `all` run
  (~5.7 KB free vs 5,912 B Armv6-M QEMU high-water) all reproduced their
  host hashes over repeated iterations — the fail-closed hash compare
  cannot survive a stack smash, so these are empirical fits, and the AVR
  high-water is evidently below the Armv6-M column's.

## Running

    sh run_test.sh <TARGET_FOLDER> [/dev/cu.usbmodemXXX]

Flow: prepare (copy sources + stamp `HEAD` + generate host pin) →
`pio run` → upload → `capture_serial.py` writes
`<TARGET>/results/<date>-<name>.txt` fail-closed (exit 0 only on a
`profile: PASS` from the expected target name).

Record format (no case count in this repo, profile is a name):

    target <name> arch <isa> tree <commit> PROFILE all hash <16 hex> (N ms, informational)
    profile: PASS (target reproduces the host-computed PROFILE hash)
