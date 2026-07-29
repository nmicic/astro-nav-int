# HOWTO: run the on-hardware determinism test

From "I plugged in a board" to a recorded result. Companion to
`README.md` (conventions and result table); one target folder
(e.g. `ESP32_C6/`) is the working example.

## 0. One-time host setup (macOS)

```sh
brew install platformio picotool
python3 -m pip install pyserial   # capture_serial.py
```

- `pio` — builds firmware and manages toolchains per-project; nothing global.
- `picotool` — talks to RP2040/RP2350 boards over USB (identify, reboot into
  bootloader, flash). Only needed for Pico-family boards.
- `esptool` (via `brew install esptool`) — identifies Espressif chips.

## 1. Identify what you plugged in

```sh
pio device list          # serial ports + USB VID:PID
```

Vendor IDs seen in this folder's targets:

- `2E8A` Raspberry Pi: PID `0003`/`000F` = BOOTSEL bootloader (mass-storage
  drive mounted, ready to flash); a CDC PID (e.g. `000A`) = running
  firmware — `picotool info` names it.
- `303A` Espressif native USB; `esptool --port <port> chip-id` names the
  exact chip. A devkit with both connectors wired shows up as TWO ports for
  ONE board (`1A86` WCH / `10C4` CP210x = the USB-UART bridge side);
  `esptool chip-id` returning the same MAC on both proves it.
- `2341` Arduino: a bridge MCU fronts classic Uno/Mega targets; newer
  boards can expose native or debug USB (MKR Zero and Uno R4).
- `2886` Seeed Studio (XIAO family).

## 2. PlatformIO refresher

All commands run from a `<TARGET>/harness/` folder (where `platformio.ini`
is):

```sh
pio run                  # build (first run downloads platform + toolchain)
pio run -t upload        # build + flash (auto-detects the port)
pio device monitor       # serial terminal (Ctrl-C to exit)
```

`platformio.ini` is the whole config. Every env pins its platform (git
commit, release artifact, or registry version) so a re-run rebuilds with
the same toolchain, and sets a unique `HW_TEST_TARGET` name — that name
appears in the firmware's output record and the capture verifies it.

## 3. Run a test end-to-end

One command from `embedded/hw/`:

```sh
sh run_test.sh <TARGET_FOLDER>                       # e.g. ESP32_C6
sh run_test.sh <TARGET_FOLDER> /dev/cu.usbmodemXXX   # explicit port
WINDOW=150 sh run_test.sh ARDUINO_MEGA2560           # slow 8-bit targets
```

It runs prepare → build → flash → capture, writes
`<TARGET>/results/<date>-<target-name>.txt` with a provenance header
(firmware SHA-256, env, platform pin, host tool versions) followed by the
transcript, and exits non-zero unless the capture proved a PASS **from the
expected target name**. The capture script writes the result file itself;
do NOT hand-roll `capture | tee file; echo $?` — in a plain shell `$?`
after a pipeline is `tee`'s status and the check silently stops being
fail-closed.

`WINDOW=<seconds>` widens the capture listen window; one `all`-profile
iteration takes ~26.5 s on a 16 MHz AVR, so the default 15 s would time
out before the first record.

**TODO — bind capture to a fresh image.** The current capture verifies
the target name and the firmware's on-target PASS, but does not yet
compare the record's tree/profile/hash (or a per-run nonce) with the
image just built. After changing the library or harness, verify those
record fields manually until the runner enforces them. The existing
records were checked against the unchanged source tree; they do not
need to be rerun solely for this deferred automation.

Manual equivalent (what run_test.sh does):

```sh
cd embedded/hw/<TARGET>/harness
sh prepare.sh            # copy library sources + stamp git HEAD + host pin
pio run
pio run -t upload
cd ..
python3 ../capture_serial.py --expect-target <name> --out results/<file>.txt
```

Always re-run `prepare.sh` after pulling — it is what makes the firmware's
`tree <commit>` stamp and its pinned expected value truthful.

## 4. How the pin works

The profile TUs (`embedded/profiles/`) carry no committed expected value —
host equality is the contract, as in the QEMU `make check` gate.
`prepare.sh` builds the host reference (`embedded/harness/host_main.c` +
the same copied profile TU + `astro_nav.c`, native `__int128` backend),
runs it, and generates `include/profile_pin.h` from its output. The
on-target compare is therefore a genuine cross-backend (host `__int128`
vs portable two-limb) and cross-ISA check. The wrapper
(`harness/src/main.cpp`) hardcodes nothing; it is byte-identical across
targets except where a core needs an extra include (nRF52: TinyUSB).

## 5. Flashing: per-family notes

- **RP2040/RP2350 (arduino-pico core)**: `pio run -t upload` reboots the
  running firmware into the bootloader via a 1200-baud serial touch. If
  unknown firmware ignores it: `picotool reboot -f -u`, or hold BOOTSEL
  while plugging in (mask-ROM mode, always works) and
  `picotool load -v -x .pio/build/<env>/firmware.uf2`.
- **Espressif**: esptool does its own auto-reset over the serial handshake
  lines — no button, works over any firmware. Prints `Hash of data
  verified` on success. ESP32-C6 capture quirk: use the UART-bridge port,
  not the native USB CDC (see `ESP32_C6/README.md`).
- **Classic AVR (Uno/Mega)**: avrdude over the board's bridge; opening the
  serial port DTR-resets the board — the capture just waits through the
  bootloader.
- **SAMD21 (MKR Zero)**: the 1200-baud touch selects the SAM-BA bootloader
  and `bossac` flashes it. Native USB briefly re-enumerates after upload;
  the capture retries the explicit port for 20 seconds.
- **Renesas RA (Uno R4)**: the 1200-baud touch selects the uploader and
  PlatformIO flashes through `sam-ba`; no manual button sequence was needed.
- **nRF52840 with Adafruit UF2/DFU bootloader**: factory firmware may
  ignore the 1200-baud touch, and adafruit-nrfutil then prints "Target is
  not in DFU mode" — **while PlatformIO still banners SUCCESS**. Never
  trust the flasher's banner; the capture's PASS/FAIL is the authority.
  Manual bootloader entry = double-tap RST (twice quickly), a
  `XIAO-SENSE`-style drive mounts, and from bootloader mode
  `pio run -t upload` performs a real DFU. Once this harness (TinyUSB) is
  on the board, the 1200-baud touch works and no button is needed again.

## 6. Adding a new device

1. Copy an existing target folder (`harness/` + empty `results/`); one
   folder per board *and* per ISA mode (and per profile slice where the
   full library does not fit, e.g. `ARDUINO_UNO` vs `ARDUINO_UNO_SUN`).
2. Adjust `platformio.ini` (find boards with `pio boards <search>`), give
   it a unique `HW_TEST_TARGET`, pin the platform.
3. If the full library overflows the target's flash, switch `prepare.sh`
   to a smaller slice: replace `profile_all.c` with `profile_core.c` (or
   `_sun`, etc.) and the `PROFILE all` sed pattern with the slice's name —
   `ARDUINO_UNO/harness/prepare.sh` is the example. Measured AVR sizes are
   in `README.md`.
4. `sh run_test.sh <NEW_TARGET>`, then write the target `README.md`
   (hardware IDs, toolchain versions, result table) and add a row to the
   table in `README.md`.
5. Several boards on one USB hub: pass the port explicitly and rely on
   `--expect-target` mismatches to catch flash-one-capture-another
   mistakes.

## 7. Troubleshooting

- **No serial port after flashing**: give it a few seconds to re-enumerate
  (`capture_serial.py` retries an explicit port for 20 s). Still nothing →
  the sketch may have crashed before `Serial.begin`; reflash a known-good
  build via the family's manual bootloader entry.
- **Port busy**: something else (a monitor, another capture) holds it.
- **capture FAILs with a record visible in the transcript**: check the
  `target` name — wrong firmware or wrong port.
- **IDE shows `'Arduino.h' file not found`** in `main.cpp`: harmless — the
  IDE indexer doesn't know PlatformIO's include paths; `pio run` is the
  truth.
- **nRF52840 link error `undefined reference to 'Serial'`**: `-DUSE_TINYUSB`
  missing from `build_flags`.
- **ESP32 native-USB port silent**: expected — capture on the UART-bridge
  port (`ESP32_C6/README.md`).
