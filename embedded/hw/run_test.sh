#!/bin/sh
# One-command hardware test run: prepare -> build -> flash -> capture.
#   sh run_test.sh <TARGET_FOLDER> [serial-port]
# e.g.
#   sh run_test.sh PI_PICO_RP2350_RISCV
#   sh run_test.sh PI_PICO_RP2350 /dev/cu.usbmodem101   # explicit port (USB hub)
#
# Writes results/<date>-<target-name>.txt with a provenance header (firmware
# SHA-256, env, platform pin, toolchain) followed by the captured transcript.
# Exits non-zero unless the capture proved a PASS from the expected target —
# the capture script writes the file itself, so there is no tee-in-a-pipeline
# exit-status hole.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TARGET_DIR=${1:?usage: run_test.sh <target-folder> [port]}
PORT=${2:-}

cd "$HERE/$TARGET_DIR/harness"
sh prepare.sh
pio run
pio run -t upload

ENV=$(sed -n 's/^\[env:\(.*\)\]$/\1/p' platformio.ini | head -1)
NAME=$(grep -o 'HW_TEST_TARGET=[^ ]*' platformio.ini | head -1 | sed 's/.*=//; s/[\\"]//g')
# The flashable artifact differs per platform (uf2 for RP2 chips, zip DFU
# package for nRF52, hex/bin elsewhere); hash the first one present.
FW=""
for A in firmware.uf2 firmware.zip firmware.hex firmware.bin; do
    if [ -f ".pio/build/$ENV/$A" ]; then FW=".pio/build/$ENV/$A"; break; fi
done
[ -n "$FW" ] || { echo "run_test: no firmware artifact found" >&2; exit 1; }
SHA=$(shasum -a 256 "$FW" | cut -d' ' -f1)
PLATFORM_PIN=$(sed -n 's/^platform = //p' platformio.ini)

OUT="$HERE/$TARGET_DIR/results/$(date +%F)-$NAME.txt"
{
    echo "# run $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# target-folder $TARGET_DIR env $ENV"
    echo "# firmware sha256 $SHA"
    echo "# platform $PLATFORM_PIN"
    # Keep reproducibility-relevant versions without publishing host install
    # paths or account names from the other `pio system info` fields.
    pio system info 2>/dev/null \
        | grep -E '^(PlatformIO Core|Python)[[:space:]]+[0-9]' \
        | sed 's/^/# /' || true
} >> "$OUT"

# WINDOW env var overrides the capture listen window (seconds) — needed for
# slow 8-bit targets where one profile iteration takes tens of seconds.
python3 "$HERE/capture_serial.py" --expect-target "$NAME" --out "$OUT" \
    ${PORT:+--port "$PORT"} ${WINDOW:+--window "$WINDOW"}
echo "result: $OUT"
