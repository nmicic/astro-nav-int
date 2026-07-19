# Embedded targets: portability proof and measured cost

The library's premise is a sight-reduction core for machines without
floating point. This directory is that premise made checkable: the
whole library cross-compiled for Armv6-M/M3/M4 and RV32I/RV32IM,
run under QEMU, its outputs hashed and compared bit-for-bit against
the host build, and its flash/RAM/stack/instruction costs measured
per feature.

Two software properties are demonstrated, and one cost is measured:

1. **It compiles and runs on 32-bit targets without floating point.** Every build is
   fully freestanding (`-ffreestanding -nostdlib`, no newlib): the
   image is the library, a ~1 KB harness, and the handful of libgcc
   integer helpers listed below. No libc, no libm, no soft-float:
   a symbol-table scan of every image (which sees `__adddf3`-class
   helpers an opcode audit cannot) and an FP-opcode audit (which sees
   stray FPU instructions) both come up empty.
2. **It is bit-exact across ISAs and backends.** Each profile below
   prints an FNV-1a hash over every output it produces. The host runs
   the native `__int128` backend of `fp_math.h`; the 32-bit targets
   automatically get the portable two-limb backend. `make check`
   asserts all five targets print byte-identical PROFILE lines to the
   host — 30 images, one hash per profile, three ISAs, two backends.
3. **What it costs** — flash, RAM, stack, dynamic instruction counts —
   is measured per feature slice, not for the whole CLI, below.

## Layout

    harness/    semihosting console, startup + stack paint, linker
                scripts, host driver, minimal mem* (freestanding GCC
                may still emit calls to them)
    profiles/   one translation unit per feature slice; each defines
                profile_run() = fixed schedule of library calls, all
                outputs hashed. baseline = empty (harness floor),
                core = Method C reduction, fix = two-body/running/
                n-body fixes, sun / moon = the almanac entries plus
                their correction chains, all = every public entry
                point at least once
    tools/      QEMU TCG plugin counting retired guest instructions
    measure.sh  the whole measurement pass; prints the tables below

## Running it

Requires `arm-none-eabi-gcc`, `riscv64-elf-gcc` (with `rv32i`/`rv32im`
multilibs), `qemu-system-arm`, `qemu-system-riscv32`; instruction
counts additionally need glib and `qemu-plugin.h` (`make plugin`).

    make all        # 5 targets x 6 profiles + 6 host reference builds
    make check      # the cross-ISA bit-exactness gate
    make measure    # sizes, stack, libgcc deps, FP audits, insn counts
    make TARGETS=rv32im check   # narrow the matrix to the tools you have
    make run-m0-moon            # run one image by hand

Targets: `m0` = Armv6-M, compiled with `-mcpu=cortex-m0plus` and run
on QEMU's microbit Cortex-M0/nRF51822 model. Cortex-M0 and M0+ share
the baseline Armv6-M instruction set; the nRF51822's 256 KB flash and
16 KB RAM are the tightest linker bounds here. `m3`/`m4` =
Cortex-M3/M4-softfloat on mps2-an385/386, `rv32i`/`rv32im` on the
`virt` machine with `-bios none`. The M4 build uses
`-mfloat-abi=soft`: the point is an FPU-less build on an FPU-capable
core, and the audits prove neither an FP opcode nor a soft-float
helper slips in.

No physical board has been tested in this phase. QEMU establishes
instruction-set execution and deterministic output; it does not test
startup behavior on silicon, peripheral integration, real cycles, or a
hardware stack watermark.

## Methodology

- **Freestanding on purpose.** No C runtime means the size numbers
  contain the library and the harness floor and nothing else; the
  `baseline` profile measures that floor so it can be subtracted.
- **Per-feature linking.** Everything is compiled with
  `-ffunction-sections -fdata-sections` and linked `--gc-sections`,
  so each profile's image holds exactly the code its feature slice
  reaches. `baseline` proves the drop is real: it links the same
  `astro_nav.o` and keeps none of it.
- **Bit-exactness is the test.** A profile's hash spans every output
  of its schedule (including `valid` flags and degenerate cases), so
  host-equality is a known-answer test of the whole slice, same
  construction as the repo's golden battery.
- **Stack** is measured two ways: runtime high-water (startup paints a
  32 KB window below the initial SP with a pattern; the harness scans
  for the deepest overwrite) and static per-frame maxima
  (`-fstack-usage`).
- **Instruction counts are QEMU TCG retired-instruction counts.** QEMU
  is not cycle-accurate: memory systems, pipelines and flash wait
  states are invisible. The counts are honest for orders of magnitude
  and for ISA-vs-ISA ratios, never for cycles.
- Everything at `-O2` (override with `OPT=`). Numbers below from
  arm-none-eabi-gcc 16.1.0, riscv64-elf-gcc 16.1.0, QEMU 11.0.2;
  regenerate with `make measure`.

## Results

Flash (`text`; `.data` is 0 in every image):

| profile           | Armv6-M |     M3 |     M4 |  RV32I | RV32IM |
|-------------------|-------:|-------:|-------:|-------:|-------:|
| baseline (floor)  |   1032 |    576 |    564 |   1200 |    776 |
| core              |  11392 |   7312 |   7292 |  16872 |  13920 |
| fix               |  32324 |  19668 |  19612 |  42736 |  37000 |
| sun               |  19372 |  12564 |  12468 |  29576 |  24288 |
| moon              |  33924 |  24800 |  24664 |  51344 |  44816 |
| all (whole library)| 80752 |  53012 |  52860 | 107496 |  94968 |

Static RAM is up to 416 bytes of `.bss`, zero `.data`: 404 bytes of
library state (the lazily initialized CORDIC angle/gain tables), 4 of
harness, and alignment padding — 412 in the ARM library profiles, 416
on RV32, 4 in the baseline floor, which never touches the library.
Runtime stack high-water in bytes:

| profile  | Armv6-M |   M3 |   M4 | RV32I | RV32IM |
|----------|-----:|-----:|-----:|------:|-------:|
| core     | 1664 |  760 |  760 |   856 |    792 |
| fix      | 4592 | 2248 | 2256 |  2328 |   2232 |
| sun      | 1624 |  696 |  688 |   808 |    712 |
| moon     | 2208 | 1016 | 1008 |  1160 |   1096 |
| all      | 5912 | 2416 | 2416 |  2472 |   2360 |

The deepest static frame everywhere is `astro_nav_fix_n_body` (1.7 KB
on v7-M/RV32, 3.1 KB on Armv6-M, its 32-sight work arrays). An 8 KB stack
budget leaves 2.3 KB of headroom above the worst measured depth even
on Armv6-M.

Dynamic instruction counts, baseline subtracted (millions):

| profile | Armv6-M |  M3  |  M4  | RV32I | RV32IM | schedule |
|---------|-----:|-----:|-----:|------:|-------:|----------|
| core    |  3.8 |  2.2 |  2.2 |   3.8 |    2.1 | 64 reductions + boundary readouts, 16 vector builds |
| fix     |  7.1 |  3.3 |  3.3 |  12.4 |    3.4 | 16 two-body + 8 advance + 8 four-body fixes |
| sun     | 13.4 |  7.6 |  7.6 |  13.0 |    7.5 | 32 epochs: direction (x2) + distance + GHA Aries |
| moon    | 71.5 | 41.6 | 41.6 |  64.3 |   41.3 | 12 epochs: direction (x2) + distance + 3 corrections |
| all     | 37.9 | 21.5 | 21.5 |  37.7 |   21.3 | every entry point at least once |

Per-bundle magnitudes on the M3/RV32IM class — these are schedule
slices of the profiles above, not minimal single calls: the reduction
bundle includes its share of observer/body vector construction plus
the two inverse-trig boundary readouts, and the almanac bundles
compute the inertial direction standalone and then again inside the
earth-fixed entry point. A Method C reduction bundle is ~34 K
instructions, a complete Sun almanac bundle ~240 K, a complete Moon
bundle (both directions, distance, and its correction chain) ~3.5 M.
At 100 MHz and an unrealistically favorable one instruction per cycle,
those counts correspond to tenths of a millisecond for a reduction and
a few tens of milliseconds for the Moon. That is an order-of-magnitude
floor, not a runtime claim; real cycles remain to be measured on
hardware.

## What the ISA split costs

The library is integer multiply/divide all the way down, so the
matrix isolates exactly that:

- **RV32I vs RV32IM** (software vs hardware multiply/divide): 1.8x
  instructions on the multiply-heavy core path, 3.6x on the
  divide-heavy fix path, 1.6-1.7x on the ephemerides, and 13% more
  flash — the cost of `__muldi3`/`__divdi3` and friends linked into
  every image.
- **Armv6-M vs M4-softfloat** (Thumb-1 vs Thumb-2, no 32-bit divide vs
  hardware divide): 1.7-2.2x instructions and ~1.5x flash. Armv6-M keeps
  a hardware 32x32 multiply, which is why its fix profile (7.1 M)
  beats RV32I's (12.4 M) despite Thumb-1's narrower toolkit.
- **M3 vs M4-softfloat**: identical to within 0.2%. M4's additions
  over M3 are DSP and (optionally) an FPU; this library uses neither,
  by construction.
- **RV32IM vs M3/M4**: within ~4% on every profile — RV32IM is ~4%
  cheaper on core, ~4% dearer on fix, and within 1.5% on the
  ephemeris-dominated profiles. With hardware integer multiply/divide,
  the two ISAs price this workload essentially the same.

Compiler-runtime dependencies per target, as `measure.sh` derives
them from the linker maps: it takes the archive members the linker
actually included (libgcc is the only archive on the link line) and
intersects the objects' undefined symbols with what those members
define, so project, harness, and linker-script symbols cannot appear
here — and `memcpy` does not either, because it resolves from the
harness's `libc_min.o`, not libgcc. Split by who asks: what
`astro_nav.o` — the library itself — imports, and what the harness +
profile drivers add beyond that (the driver sets overlap the
library's; the second column lists only the additions):

| target | library (`astro_nav.o`) | harness + profiles add |
|--------|-------------------------|------------------------|
| Armv6-M | `__aeabi_lmul`, `__aeabi_idiv(mod)`, `__aeabi_uidiv`, `__aeabi_ldivmod`, `__clzdi2` | `__aeabi_uidivmod`, `__aeabi_uldivmod` |
| M3     | `__aeabi_ldivmod` | `__aeabi_uldivmod` |
| M4     | `__aeabi_ldivmod` | `__aeabi_uldivmod` |
| RV32I  | `__muldi3`, `__divsi3`, `__udivsi3`, `__modsi3`, `__divdi3`, `__moddi3`, `__clzdi2` | `__umodsi3`, `__umoddi3` |
| RV32IM | `__divdi3`, `__moddi3`, `__clzdi2` | `__umoddi3` |

Satisfying those, libgcc pulls in a few more members of its own — all
still integer: division guts and CLZ tables (`__udivmoddi4` and the
`__aeabi_*div0` stubs on ARM, plus `__divdi3` and
`__gnu_ldivmod_helper` on M0+; `__clz_tab` on RV32, plus `__mulsi3`
on RV32I). `measure.sh` prints the exact member lists per target.

The M3/M4 row is the punchline in miniature: with `umull` and
hardware `udiv`, the library's entire runtime dependency is signed
64-bit division — one helper, `__aeabi_ldivmod`; even the unsigned
variant is only there for the test drivers. There is no soft-float
code anywhere in the matrix because nothing ever asks for it — and
`measure.sh` gates that from two independent directions: a denylist
scan of every final image's symbol table, which catches soft-float
helpers like `__adddf3`/`__aeabi_dadd` (ordinary integer code,
invisible to any opcode inspection), and an FP-opcode audit of every
image, which catches any stray FPU instruction. The scan is itself
tested on every run before any real image is examined: the denylist
regex must catch 26 known soft-float helpers and clear 26 known
libgcc integer helpers, and a scratch copy of a real image with
`__adddf3`/`__negdf2` injected must come back flagged.

The tightest linked image fits the nRF51822 memory map with room to
spare: `all` uses 79 KB of 256 KB flash and 6.2 KB of 16 KB RAM
including the QEMU-measured stack. Physical-board behavior remains
unverified as stated above.
