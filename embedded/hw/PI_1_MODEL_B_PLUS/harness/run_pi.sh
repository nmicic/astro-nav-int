#!/bin/sh
# Native-Linux analogue of the MCU harnesses: copy the library sources plus
# the host driver to the Pi, compile them there with the DISTRO's native gcc
# (no cross toolchain, no PlatformIO), run, and capture a provenance-stamped
# transcript.
#
# Pin provenance is the same contract as the MCU targets' prepare.sh: the
# profile TU carries no committed hash, so this script FIRST builds and runs
# the host reference locally (macOS, native __int128 backend of fp_math.h)
# to obtain the expected value, then compares the Pi's output against it.
# On a 32-bit ARM userland gcc has no __int128, so the Pi exercises the
# portable two-limb backend — the compare is cross-backend AND
# cross-compiler (distro gcc vs macOS cc), fail-closed: the script exits
# non-zero unless the hashes match.
#
# Usage: sh run_pi.sh [pi-host]           (default raspberrypi.local)
# Assumes key-based or agent SSH auth, or an interactive password prompt.
set -eu
HOST=${1:-raspberrypi.local}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
RESULTS="$HERE/../results"
STAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
OUT="$RESULTS/$(date -u +%Y-%m-%d)-pi1-bplus.txt"
REMOTE_DIR=/home/pi/astronav-profile

SOURCES="astro_nav.c astro_nav.h fp_math.h \
embedded/profiles/profile_all.c \
embedded/harness/harness.h embedded/harness/host_main.c"

COMMIT=$(git -C "$REPO" rev-parse --short=12 HEAD)
DIRTY=""
if ! git -C "$REPO" diff --quiet HEAD -- $SOURCES 2>/dev/null; then
    DIRTY="-dirty"
fi

# Local host reference -> expected hash (same build prepare.sh does).
HOSTDIR=$(mktemp -d)
trap 'rm -rf "$HOSTDIR"' EXIT
for f in $SOURCES; do cp "$REPO/$f" "$HOSTDIR/"; done
cc -std=c99 -O2 -o "$HOSTDIR/host_all" \
    "$HOSTDIR/host_main.c" "$HOSTDIR/astro_nav.c" "$HOSTDIR/profile_all.c"
HOST_LINE=$("$HOSTDIR/host_all")
PIN_HASH=$(printf '%s\n' "$HOST_LINE" \
    | sed -n 's/^PROFILE all hash=0x\([0-9a-f]\{16\}\)$/\1/p')
[ -n "$PIN_HASH" ] || { echo "run_pi: FAIL — no parsable host PROFILE line" >&2; exit 1; }

ssh "pi@$HOST" "mkdir -p $REMOTE_DIR"
scp "$HOSTDIR/astro_nav.c" "$HOSTDIR/astro_nav.h" "$HOSTDIR/fp_math.h" \
    "$HOSTDIR/profile_all.c" "$HOSTDIR/harness.h" "$HOSTDIR/host_main.c" \
    "pi@$HOST:$REMOTE_DIR/"

TMP=$(mktemp)
{
    echo "# run $STAMP"
    echo "# target-folder PI_1_MODEL_B_PLUS tree $COMMIT$DIRTY"
    echo "# host reference (local): $HOST_LINE"
    echo "# sources sha256 (local tree):"
    (cd "$HOSTDIR" && shasum -a 256 astro_nav.c astro_nav.h fp_math.h \
        profile_all.c harness.h host_main.c | sed 's/^/#   /')
    ssh "pi@$HOST" "
        set -eu
        cd $REMOTE_DIR
        echo \"# remote: \$(uname -a)\"
        echo \"# remote model: \$(tr -d '\\0' < /proc/device-tree/model)\"
        echo \"# remote gcc: \$(gcc --version | head -1)\"
        echo '# remote sha256:'
        sha256sum astro_nav.c astro_nav.h fp_math.h profile_all.c harness.h host_main.c | sed 's/^/#   /'
        # Never run a binary left by an earlier invocation if this compile
        # fails. Remote set -e also propagates the compiler failure to ssh.
        rm -f profile_all.current
        gcc -std=c99 -O2 -Wall -Wextra -Werror -o profile_all.current host_main.c astro_nav.c profile_all.c
        echo 'build: OK (native gcc -O2 -Wall -Wextra -Werror)'
        t0=\$(date +%s%N); LINE=\$(./profile_all.current); t1=\$(date +%s%N)
        echo \"remote: \$LINE\"
        echo \"remote-ms: \$(( (t1 - t0) / 1000000 ))\"
    "
} > "$TMP"

REMOTE_HASH=$(sed -n 's/^remote: PROFILE all hash=0x\([0-9a-f]\{16\}\)$/\1/p' "$TMP")
MS=$(sed -n 's/^remote-ms: //p' "$TMP")
{
    # Synthesize the standard record so all targets read alike; the raw
    # remote lines above are the evidence it is derived from.
    echo "target pi1-bplus arch armv6 tree $COMMIT$DIRTY PROFILE all hash ${REMOTE_HASH:-none} (${MS:-?} ms, informational)"
    if [ -n "$REMOTE_HASH" ] && [ "$REMOTE_HASH" = "$PIN_HASH" ]; then
        echo "profile: PASS (target reproduces the host-computed PROFILE hash)"
    else
        echo "profile: FAIL - expected $PIN_HASH"
    fi
} >> "$TMP"
cat "$TMP"
mv "$TMP" "$OUT"
grep -q "profile: PASS" "$OUT" || { echo "run_pi: FAIL — no PASS in transcript" >&2; exit 1; }
echo "result: $OUT"
