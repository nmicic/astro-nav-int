#!/bin/sh
# Copy the library sources from the repo root into this harness project,
# stamp the tree state into include/build_info.h so the firmware itself
# reports which commit it was built from, and pin the expected profile hash
# into include/profile_pin.h.
#
# Pin provenance: the profile TUs carry no
# committed pin — the expected value is the HOST's output (native __int128
# backend of fp_math.h), computed here at prepare time by building and
# running the same profile TU with harness/host_main.c. The on-target
# compare is therefore a genuine cross-backend (host __int128 vs portable
# two-limb) AND cross-ISA check, matching the repo's `make check` semantics.
#
# Run this before `pio run` so the build always uses the current tree.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
mkdir -p "$HERE/src" "$HERE/include"
cp "$REPO/astro_nav.c"                       "$HERE/src/astro_nav.c"
cp "$REPO/embedded/profiles/profile_sun.c"   "$HERE/src/profile_sun.c"
cp "$REPO/astro_nav.h"                       "$HERE/include/astro_nav.h"
cp "$REPO/fp_math.h"                         "$HERE/include/fp_math.h"
cp "$REPO/embedded/harness/harness.h"        "$HERE/include/harness.h"

COMMIT=$(git -C "$REPO" rev-parse --short=12 HEAD)
DIRTY=""
# The stamp attests that the LIBRARY sources built into the image match the
# commit, so dirty is scoped to exactly the files copied above (plus the
# host reference driver) — an edited doc elsewhere must not taint it.
if ! git -C "$REPO" diff --quiet HEAD -- \
        astro_nav.c astro_nav.h fp_math.h \
        embedded/profiles/profile_sun.c \
        embedded/harness/harness.h embedded/harness/host_main.c 2>/dev/null; then
    DIRTY="-dirty"
fi
printf '#define BUILD_COMMIT "%s%s"\n' "$COMMIT" "$DIRTY" > "$HERE/include/build_info.h"

# Host reference build -> run -> extract the pin. Lives outside src/ so
# PlatformIO never sees host_main.c.
HOSTDIR="$HERE/.hostref"
mkdir -p "$HOSTDIR"
cc -std=c99 -O2 -I"$HERE/include" \
    "$REPO/embedded/harness/host_main.c" \
    "$HERE/src/astro_nav.c" \
    "$HERE/src/profile_sun.c" \
    -o "$HOSTDIR/host_sun"
HOST_LINE=$("$HOSTDIR/host_sun")
echo "host reference: $HOST_LINE"
PIN_HASH=$(printf '%s\n' "$HOST_LINE" \
    | sed -n 's/^PROFILE sun hash=0x\([0-9a-f]\{16\}\)$/\1/p')
if [ -z "$PIN_HASH" ]; then
    echo "prepare: FAIL — host reference did not print a parsable PROFILE line" >&2
    exit 1
fi
{
    printf '#define PINNED_HASH  0x%sULL\n' "$PIN_HASH"
    printf '#define PINNED_PROFILE "sun"\n'
} > "$HERE/include/profile_pin.h"

echo "prepared: sources from $REPO (tree $COMMIT$DIRTY, host pin 0x$PIN_HASH)"
