#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Nenad Micic <nenad@micic.be>
#
# measure.sh -- the whole measurement pass in one run: per-profile
# flash/.data/.bss, runtime stack high-water, dynamic instruction
# counts (if the QEMU plugin is available), map-derived libgcc
# dependencies per target, a soft-float helper-symbol scan plus an
# FP-opcode audit of every image (complementary: the symbol scan sees
# soft-float helpers, which are integer code; the opcode audit sees
# stray FPU instructions), and the largest static stack frames.
# Prints to stdout; the curated numbers live in README.md.
#
# QEMU counts instructions, not cycles: use the counts for magnitude
# and for ISA-vs-ISA ratios, never as cycle estimates.

set -eu
cd "$(dirname "$0")"

TARGETS="${TARGETS:-m0 m3 m4 rv32i rv32im}"
PROFILES="${PROFILES:-baseline core fix sun moon all}"
ARM_PREFIX="${ARM_PREFIX:-arm-none-eabi-}"
RV_PREFIX="${RV_PREFIX:-riscv64-elf-}"
QEMU_ARM="${QEMU_ARM:-qemu-system-arm}"
QEMU_RV="${QEMU_RV:-qemu-system-riscv32}"
QEMU_FLAGS="-semihosting -nographic -monitor none -serial none"
TAB="$(printf '\t')"

make -s all

PLUGIN=""
if make -s plugin 2>/dev/null; then
    PLUGIN="$(ls build/insncount.* 2>/dev/null | head -1)"
fi
[ -n "$PLUGIN" ] || echo "note: QEMU plugin unavailable; skipping instruction counts"

prefix_of() {
    case "$1" in
        m*) echo "$ARM_PREFIX" ;;
        rv*) echo "$RV_PREFIX" ;;
    esac
}

qemu_of() {
    case "$1" in
        m0) echo "$QEMU_ARM -M microbit" ;;
        m3) echo "$QEMU_ARM -M mps2-an385" ;;
        m4) echo "$QEMU_ARM -M mps2-an386" ;;
        rv*) echo "$QEMU_RV -M virt -bios none" ;;
    esac
}

# QEMU's semihosting console writes to stderr; fold it into stdout so
# the callers' line extraction sees it.
run_elf() { # target elf [plugin]
    _cmd="$(qemu_of "$1")"
    if [ "${3:-}" = plugin ] && [ -n "$PLUGIN" ]; then
        perl -e 'alarm 120; exec @ARGV' \
            $_cmd $QEMU_FLAGS -kernel "$2" -plugin "./$PLUGIN" -d plugin 2>&1
    else
        perl -e 'alarm 120; exec @ARGV' $_cmd $QEMU_FLAGS -kernel "$2" 2>&1
    fi
}

echo "== size / stack / instruction counts =="
printf '%-7s %-9s %8s %6s %6s %8s %14s  %s\n' \
    target profile text data bss stack insns hash
for t in $TARGETS; do
    SIZE="$(prefix_of "$t")size"
    for p in $PROFILES; do
        elf="build/$t/profile_$p.elf"
        set -- $("$SIZE" -B "$elf" | tail -1)
        text=$1; data=$2; bss=$3
        out="$(run_elf "$t" "$elf")"
        stack="$(printf '%s\n' "$out" | sed -n 's/^stack-high-water: \([0-9]*\) bytes$/\1/p')"
        hash="$(printf '%s\n' "$out" | sed -n 's/^PROFILE .* hash=\(0x[0-9a-f]*\)$/\1/p')"
        insns="-"
        if [ -n "$PLUGIN" ]; then
            insns="$(run_elf "$t" "$elf" plugin \
                     | sed -n 's/^guest-insns: \([0-9]*\)$/\1/p')"
        fi
        printf '%-7s %-9s %8s %6s %6s %8s %14s  %s\n' \
            "$t" "$p" "$text" "$data" "$bss" "$stack" "$insns" "$hash"
    done
done

echo
echo "== compiler-runtime (libgcc) dependencies per target =="
echo "   (derived from the linker maps: the archive members the linker"
echo "    actually included -- libgcc is the only archive on the link"
echo "    line -- and, per member, whether our objects pulled it in"
echo "    directly or libgcc itself did transitively. Split into what"
echo "    the library object astro_nav.o imports and what the harness"
echo "    + profile drivers import; memcpy is NOT here, it resolves"
echo "    from the harness's libc_min.o.)"
for t in $TARGETS; do
    NM="$(prefix_of "$t")nm"
    # "member kind symbol" triples from every profile's map. GNU ld
    # emits each included member as a path line ending in
    # "libgcc.a(member)" followed by an indented line naming the
    # referencing object and the (first) symbol that pulled it in;
    # with these toolchains' install paths the pair never fits one
    # line, so the two-line form is the only one parsed.
    triples="$(awk '
        /^Archive member included/ { in_s = 1; next }
        in_s && NF > 0 && $0 !~ /^[\/ \t]/ { in_s = 0 }
        !in_s { next }
        /libgcc\.a\(/ {
            member = $0
            sub(/.*libgcc\.a\(/, "", member); sub(/\).*/, "", member)
            if ((getline ref) <= 0) next
            sym = ref
            sub(/.*\(/, "", sym); sub(/\).*/, "", sym)
            kind = (ref ~ /libgcc\.a\(/) ? "transitive" : "direct"
            print member, kind, sym
        }' build/"$t"/*.elf.map | sort -u)"
    # The parse feeds a gate, so it must not fail open: at least one
    # member on every target (all five use 64-bit division helpers),
    # and every triple well-formed.
    if [ -z "$triples" ]; then
        echo "FAIL: no libgcc archive members parsed from build/$t/*.elf.map"
        exit 1
    fi
    bad="$(printf '%s\n' "$triples" | awk \
        'NF != 3 || $1 !~ /\.o$/ || ($2 != "direct" && $2 != "transitive") { print }')"
    if [ -n "$bad" ]; then
        echo "FAIL: malformed linker-map parse for $t:"
        printf '%s\n' "$bad" | sed 's/^/    /'
        exit 1
    fi
    members_line="$(printf '%s\n' "$triples" | awk '{ print $1 }' | sort -u | tr '\n' ' ')"
    # The map names only the FIRST symbol that pulled each member in.
    # The true import list is every symbol an object references that
    # an included libgcc member defines: intersect the two sets (both
    # are unique, so duplicates of the concatenation = intersection).
    libgcc="$(sed -n 's/^\(\/.*libgcc\.a\)(.*/\1/p' build/"$t"/profile_all.elf.map | head -1)"
    if [ -z "$libgcc" ]; then
        echo "FAIL: libgcc path not found in build/$t/profile_all.elf.map"
        exit 1
    fi
    defined="$("$NM" --defined-only "$libgcc" 2>/dev/null | awk -v keep="$members_line" '
        BEGIN { n = split(keep, k); for (i = 1; i <= n; i++) want[k[i]] }
        /:$/ { m = $0; sub(/:$/, "", m); in_m = (m in want); next }
        in_m && NF >= 3 { print $3 }' | sort -u)"
    if [ -z "$defined" ]; then
        echo "FAIL: no symbols read from the included libgcc members for $t"
        exit 1
    fi
    refs_lib="$("$NM" --undefined-only build/"$t"/astro_nav.o 2>/dev/null \
        | awk '$1 == "U" { print $2 }' | sort -u)"
    refs_oth="$(for o in build/"$t"/*.o; do
            [ "$o" = "build/$t/astro_nav.o" ] || "$NM" --undefined-only "$o" 2>/dev/null
        done | awk '$1 == "U" { print $2 }' | sort -u)"
    lib_imports="$(printf '%s\n%s\n' "$refs_lib" "$defined" | sort | uniq -d | tr '\n' ' ')"
    oth_imports="$(printf '%s\n%s\n' "$refs_oth" "$defined" | sort | uniq -d | tr '\n' ' ')"
    if [ -z "$lib_imports" ]; then
        echo "FAIL: empty library import set for $t (parse or nm problem)"
        exit 1
    fi
    direct_m="$(printf '%s\n' "$triples" | awk '$2 == "direct" { print $1 }' | sort -u | tr '\n' ' ')"
    trans_m="$(printf '%s\n' "$triples" | awk '
        $2 == "direct" { d[$1] }
        $2 == "transitive" { t[$1] = $3 }
        END { for (m in t) if (!(m in d)) print m "(" t[m] ")" }' | sort | tr '\n' ' ')"
    printf '%-7s library (astro_nav.o): %s\n' "$t" "$lib_imports"
    printf '        harness+profiles:      %s\n' "${oth_imports:-<none>}"
    printf '        members: %s%s\n' "$direct_m" "${trans_m:++ transitive: $trans_m}"
    leftover="$("$NM" --undefined-only build/"$t"/profile_all.elf | awk 'NF')"
    if [ -n "$leftover" ]; then
        echo "FAIL: unresolved symbols remain in build/$t/profile_all.elf:"
        echo "$leftover"
        exit 1
    fi
done
echo "final images: no unresolved symbols"

echo
echo "== soft-float helper scan (final ELF symbol tables) =="
echo "   (the opcode audit below cannot see software floating point:"
echo "    __adddf3-class helpers are ordinary integer code. Any symbol"
echo "    from the soft-float helper families in any image is a FAIL.)"
SF_RE='^_?(__(add|sub|mul|div)[dsth]f3|__(eq|ne|lt|le|gt|ge|cmp|unord|neg)[dsth]f2|__(mul|div)[sdxth]c3|__float[a-z0-9]*|__fix[a-z0-9]*|__(extend|trunc)[a-z0-9]*|__powi[a-z0-9]*|__gnu_[dfh]2[dfh][a-z0-9_]*|__aeabi_[cdfh][a-z0-9]*|__aeabi_u?[il]2[dfh][a-z0-9]*)$'

# Denylist self-test before trusting it: every canary helper must trip
# the pattern (negation is the f2 family: __negdf2, not __negdf3), and
# none of the integer helpers the images legitimately contain may --
# so a pattern regression can never fail open or drown the report in
# false positives.
sf_canary_bad="__adddf3 __subsf3 __muldf3 __divsf3 __negdf2 __negsf2 \
__eqdf2 __gedf2 __unorddf2 __floatsidf __floatunssidf __fixdfsi \
__fixunssfsi __extendsfdf2 __truncdfsf2 __powidf2 __muldc3 \
__aeabi_dadd __aeabi_fcmplt __aeabi_h2f __aeabi_d2iz __aeabi_i2d \
__aeabi_ul2d __aeabi_cdcmpeq __gnu_h2f_ieee __gnu_f2h_ieee"
sf_canary_ok="__aeabi_uidiv __aeabi_uidivmod __aeabi_idiv __aeabi_idivmod \
__aeabi_lmul __aeabi_ldivmod __aeabi_uldivmod __aeabi_idiv0 __aeabi_ldiv0 \
__clzsi2 __clzdi2 __muldi3 __mulsi3 __divdi3 __divsi3 __moddi3 __modsi3 \
__udivsi3 __umoddi3 __umodsi3 __udivmoddi4 __clz_tab \
__gnu_ldivmod_helper memcpy memset memmove"
for s in $sf_canary_bad; do
    if ! printf '%s\n' "$s" | grep -qE "$SF_RE"; then
        echo "FAIL: soft-float denylist self-test: $s not caught"
        exit 1
    fi
done
for s in $sf_canary_ok; do
    if printf '%s\n' "$s" | grep -qE "$SF_RE"; then
        echo "FAIL: soft-float denylist self-test: $s wrongly flagged"
        exit 1
    fi
done
set -- $sf_canary_bad; sf_nbad=$#
set -- $sf_canary_ok; sf_nok=$#
echo "denylist self-test: $sf_nbad helpers caught, $sf_nok integer symbols passed"

# The filter chain must mask grep's benign no-match exit (the clean
# case), so the producer's status is checked on its own first: a dead
# or missing nm must not read as a clean image. Runs at top level, not
# in a command substitution, so the FAIL exits reach the script;
# result comes back in $hits (empty if clean).
sf_nm_tmp="build/.sf_nm.tmp"
sf_hits() { # elf -> $hits
    "$NM" "$1" > "$sf_nm_tmp" || { echo "FAIL: $NM failed on $1"; exit 1; }
    sf_rc=0
    hits="$(awk 'NF { print $NF }' "$sf_nm_tmp" | sort -u \
            | grep -E "$SF_RE")" || sf_rc=$?
    rm -f "$sf_nm_tmp"
    if [ "$sf_rc" -gt 1 ]; then
        echo "FAIL: symbol filter failed on $1 (grep exit $sf_rc)"
        exit 1
    fi
}

# End-to-end canary: inject two soft-float symbols into a scratch copy
# of a real image and require the exact scan pipeline used below to
# flag it -- proves the audit as a whole (nm | extract | match), not
# just the pattern, cannot fail open.
t0="${TARGETS%% *}"
NM="$(prefix_of "$t0")nm"
canary_elf="build/$t0/.sf_canary.elf"
"$(prefix_of "$t0")objcopy" --add-symbol __adddf3=0 --add-symbol __negdf2=0 \
    "build/$t0/profile_baseline.elf" "$canary_elf"
sf_hits "$canary_elf"
rm -f "$canary_elf"
if [ -z "$hits" ]; then
    echo "FAIL: end-to-end canary: injected __adddf3/__negdf2 not detected"
    exit 1
fi
echo "end-to-end canary: injected __adddf3/__negdf2 detected"

sf_fail=0
for t in $TARGETS; do
    NM="$(prefix_of "$t")nm"
    for p in $PROFILES; do
        sf_hits "build/$t/profile_$p.elf"
        if [ -n "$hits" ]; then
            echo "FAIL: soft-float helper symbols in build/$t/profile_$p.elf:"
            printf '%s\n' "$hits" | sed 's/^/    /'
            sf_fail=1
        fi
    done
done
[ "$sf_fail" = 0 ] && echo "no soft-float helper symbols in any image"

echo
echo "== FP-opcode audit (must be zero everywhere) =="
fail=0
for t in $TARGETS; do
    OBJDUMP="$(prefix_of "$t")objdump"
    case "$t" in
        # M-profile soft-float: any v* mnemonic would be a VFP/NEON leak.
        m*)  pattern='^v' ;;
        # RV32I/RV32IM have no F/D extension; any f* mnemonic except
        # fence would be an F-extension leak.
        rv*) pattern='^f' ; exclude='^fence' ;;
    esac
    for p in $PROFILES; do
        elf="build/$t/profile_$p.elf"
        # Disassemble first and check objdump's own status: piping it
        # straight into the counter would turn an objdump failure into
        # an empty stream and a zero count. (The string test below
        # also fails closed if the count comes back non-numeric.)
        od_tmp="build/$t/.fp_audit.tmp"
        "$OBJDUMP" -d "$elf" > "$od_tmp" \
            || { echo "FAIL: $OBJDUMP failed on $elf"; exit 1; }
        n="$(awk -F"$TAB" -v pat="$pattern" -v exc="${exclude:-^\$}" \
                 'NF >= 3 { m = $3; sub(/[ \t].*/, "", m);
                            if (m ~ pat && m !~ exc) n++ }
                  END { print n + 0 }' "$od_tmp")"
        rm -f "$od_tmp"
        if [ "$n" != 0 ]; then
            echo "FAIL: $n FP opcodes in build/$t/profile_$p.elf"
            fail=1
        fi
    done
done
[ "$fail" = 0 ] && echo "no FP opcodes in any image"

echo
echo "== largest static stack frames (bytes, -fstack-usage) =="
for t in $TARGETS; do
    echo "-- $t"
    cat build/"$t"/*.su | sort -t"$TAB" -k2,2 -rn | head -8 \
        | sed 's/^/   /'
done

# Both audits must be green (checking either variable alone would let
# the other fail open).
[ "$fail" = 0 ] && [ "$sf_fail" = 0 ]
