# Changelog

Pre-1.0 stability: no API or ABI promise. Any 0.x release may change
the public header, the CLI surface, and internal representations
without a deprecation cycle. Pin an exact revision (or the release
tag) if you build on this. What never changes silently is the
numeric contract: the `--golden` determinism gate pins every output
bit of its 4096-case schedule (`make test` separately pins selected
CLI transcripts), and a change to the golden hash is a documented
event, not drift.

## v0.1.0 — initial public release

First public snapshot of the integer-only celestial sight-reduction
library:

- Method A/B/C sight reduction, two-body and n-body fixes, running
  fix, and the full sextant correction chain — C99 integer
  arithmetic only, no floating point, no libm.
- Built-in Sun, Moon, Aries, and 18-star almanac core with
  caller-supplied time (UT1 + TT−UT1).
- `--predict` known-position verb: predicted Hc/Zn/Hs and implied
  aggregate correction, with a three-refusal fail-closed contract.
- Determinism gate (`--golden` FNV-1a bit hash), UBSan/reference
  builds, and four external validation gates: `--ephemeris-check`,
  `--external-check`, `--cross-check`, `--scenario-check`.
- Freestanding embedded builds (Armv6-M, Cortex-M3/M4, RV32I/RV32IM)
  with QEMU bit-exactness checks against the host build.
- Version identity: `ASTRO_NAV_VERSION_MAJOR/MINOR/PATCH` in
  `astro_nav.h`.
