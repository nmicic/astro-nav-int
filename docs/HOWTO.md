# HOWTO: Operating Manual

This is the practical manual for astro-nav-int: how to build it, how to
drive every CLI mode, and what the numbers mean -- with worked examples
taken verbatim from the test suite, so every command here is one the CI
gate already runs (or a close variant you can check against `--star`
output yourself).

For the *conceptual* walk from a raw sextant reading to a position --
why each step exists and which function performs it -- read
[`WORKFLOW.md`](WORKFLOW.md) first. This document is the reference you
come back to with a terminal open.

Contents:

1. [What this program does](#1-what-this-program-does)
2. [Build and verify](#2-build-and-verify)
3. [Units cheat sheet](#3-units-cheat-sheet)
4. [CLI reference, mode by mode](#4-cli-reference-mode-by-mode)
5. [End-to-end examples](#5-end-to-end-examples)
6. [Using the library directly](#6-using-the-library-directly)
7. [Model assumptions and current limitations](#7-model-assumptions-and-current-limitations)
8. [Scope](#8-scope)

## 1. What this program does

Celestial navigation answers one question: *where am I on Earth, given
measured angles to celestial bodies at known times?* Each measured
altitude of a star places you on a circle on the Earth's surface (the
circle of equal altitude); two circles intersect at your position.

Classically this is worked with sight-reduction tables (Pub. 229) or a
scientific calculator. This program does the whole chain -- almanac,
altitude corrections, sight averaging, reduction, intercept, and direct
two-body / n-body fixes -- in **pure integer arithmetic**: no `double`,
no libm, no FPU instructions. Supported host and embedded targets
produce the same output bytes for the same tested inputs. A committed
golden hash (`make determinism`), the CI compiler/architecture matrix,
and the embedded cross-ISA profiles enforce that claim; they are not a
universal statement about untested C implementations.

What you can do with it today:

- **Correct a sextant reading** (index error, dip, refraction,
  parallax, semidiameter) from milli-arcminute inputs.
- **Average a run of shots** of one body with least squares, outlier
  rejection, and the altitude rate.
- **Look up any of 18 navigational stars** in a built-in vector
  almanac: GHA Aries, GHA, dec, and the earth-fixed unit vector, for
  any UT1 instant within +-100 years of J2000.
- **Reduce a sight** by three paths: spherical trig in Q16.48; the same
  altitude with a square-key azimuth; or a pure vector path in Q2.30.
  Methods A and B deliberately share their altitude calculation, while
  Method C is a separate vector formulation.
- **Fix a position** from two sights by exact circle intersection (no
  plotting), from three or more by least squares (library), or from a
  single plate-solved zenith photograph (`--zenith`).
- **Run the sight backward** from a known (GPS) position: the reading
  modeled for a natural sea horizon and where to face (`--predict`) --
  a known-position check whose repeated component can validate index
  error.

What it deliberately refuses to do is guess. Malformed or out-of-domain
CLI input -- an altitude past +-90 deg, a timestamp that would overflow,
or a vector that is not a unit vector -- exits `2` with a message.
Well-formed input for which no answer exists, such as unusable fix
geometry, exits `1`. Several examples below exercise both classes on
purpose.

## 2. Build and verify

Requirements: a C99 compiler and make. No libraries beyond libc.

```sh
make            # builds ./sight_reduction
make check      # the full gate -- run this after any change
```

`make check` aggregates every gate; each also works standalone:

| Target | What it proves |
| --- | --- |
| `make test` | self-tests + every CLI mode, including the must-fail cases, + `--ephemeris-check` (almanac vs Skyfield/DE421) + `--external-check` (almanac vs printed sources) + `--cross-check` (almanac + geometry vs independent computed implementations) + `--scenario-check` (the whole pipeline vs external position answers) + `check-libm` |
| `make determinism` | `--golden`: FNV-1a hash of every output bit of a 4096-case schedule matches the committed constant; CI and `embedded/` exercise it across the supported compiler/target matrix |
| `make reference` | rebuilds with `-DASTRO_NAV_NATIVE_REFERENCE -lm` and compares against native `double` math |
| `make ubsan` | the same runs (including `--golden`) under `-fsanitize=undefined -fno-sanitize-recover`; also checks that rejection paths exit 2 *cleanly* rather than trapping |
| `make check-libm` | `nm` audit: the linked binary imports no libm or soft-float symbol |
| `make check-symbols` | compiles with `-mgeneral-regs-only` where supported: any FPU register use is a compile error |
| `make check-lib` | builds `libastro_nav.a` and a standalone consumer (`examples/consumer.c`) whose only project inputs are `astro_nav.h` and the archive; the consumer must reproduce the pinned star-pair fix exactly, and the same `nm` denylist then audits the archive and the consumer binary |
| `make public-audit` | standalone pre-publication hygiene scan (deliberately not part of `check`): committed HEAD content and file names, commit/tag metadata, and tracked-but-ignored files, against generic patterns for AI/session markers, absolute home paths, and key/token shapes; requires a clean worktree so the scanned tree is the publishable tree, self-tests every pattern branch with run-time canary probes, and a git failure is a FAIL, never a silent pass |

Running `./sight_reduction` with no arguments (or `--self-test`) prints
the built-in validation suite: reference cases against Pub. 229 and
double-precision values, random-geometry sweeps, two-body fix
round-trips, domain-guard checks. `--help` (or any unknown flag) prints
usage.

## 3. Units cheat sheet

Everything is an integer. Two angular scales dominate the CLI; the
remaining units are listed alongside them:

| Quantity | Unit | Meaning |
| --- | --- | --- |
| latitude, longitude, GHA, dec, Zn, course | **centidegrees** (cdeg) | 100 = 1 deg. Lat/dec north positive; **lon east positive** (1 deg W = `-100`); GHA in [0, 36000) |
| sextant altitudes (Hs, Ho), index error, dip, refraction, parallax, SD, residuals | **milli-arcminutes** (marcmin) | 1000 = 1.0' = 1 nm of intercept. 1 deg = 60000 |
| height of eye | centimetres | `200` = 2 m |
| time | UT1 milliseconds since J2000.0 (2000-01-01 12:00:00 UT1) | `836136000000` = 2026-07-01 00:00:00 UT1 |
| distance run, intercept | tenths of a nautical mile | `120` = 12.0 nm |
| unit vectors | **Q2.30** | component / 2^30; `1073741824` = 1.0 |

The two angle scales exist for a reason: a sextant reads to 0.1' and
0.1' = 0.1 nm of position, so altitudes carry milli-arcminutes end to
end (`--correct` -> `--average` -> `--reduce`/`--fix`) with no
centidegree rounding. Almanac angles at 0.01 deg (= 0.6') are the
coarse layer; the library's vector path (Method C, Q2.30) bypasses even
that.

## 4. CLI reference, mode by mode

```
./sight_reduction --reduce  LAT LON GHA DEC [HO]
./sight_reduction --reduce-star LAT LON STAR UT1_MS [HO]
./sight_reduction --fix     DRLAT DRLON GHA1 DEC1 HO1 GHA2 DEC2 HO2
./sight_reduction --fix-stars DRLAT DRLON STAR1 UT1_1 HO1 STAR2 UT1_2 HO2
./sight_reduction --correct HS IE EYE_CM HP SD LIMB [TEMP_C PRESSURE_MB]
./sight_reduction --correct-sun HS IE EYE_CM UT1_MS TT_MINUS_UT1_MS LIMB [TEMP_C PRESSURE_MB]
./sight_reduction --correct-moon HS IE EYE_CM UT1_MS TT_MINUS_UT1_MS LIMB [TEMP_C PRESSURE_MB]
./sight_reduction --predict LAT LON BODY UT1_MS EYE_CM LIMB [HS] [TEMP_C PRESSURE_MB]
./sight_reduction --average TREF_MS REJECT HO T [HO T ...]
./sight_reduction --time    YEAR MONTH DAY HOUR MIN SEC MS DUT1_MS TAI_MINUS_UTC_S
./sight_reduction --star    INDEX UT1_MS
./sight_reduction --sun     UT1_MS TT_MINUS_UT1_MS
./sight_reduction --moon    UT1_MS TT_MINUS_UT1_MS
./sight_reduction --reduce-sun LAT LON UT1_MS TT_MINUS_UT1_MS [HO]
./sight_reduction --reduce-moon LAT LON UT1_MS TT_MINUS_UT1_MS [HO]
./sight_reduction --fix-sun DRLAT DRLON TT_MINUS_UT1_MS UT1_1 HO1 UT1_2 HO2
./sight_reduction --fix-n   DRLAT DRLON BODY UT1_MS HO [BODY UT1_MS HO ...]
./sight_reduction --running-fix DRLAT DRLON COURSE SPEED BODY UT1_MS HO [...]
./sight_reduction --zenith  X Y Z UT1_MS
./sight_reduction --golden
./sight_reduction --fuzz-w128 [N]
./sight_reduction --ephemeris-check
./sight_reduction --external-check
./sight_reduction --cross-check
./sight_reduction --scenario-check
```

Exit status is uniform across modes: `0` = a result was printed;
`1` = the arguments were well-formed but there is no answer
(degenerate fix geometry, no valid averaging fit); `2` = malformed or
out-of-domain input. `make test` asserts the exact codes.

### 4.1 `--correct` -- sextant altitude to observed altitude

`HS IE EYE_CM HP SD LIMB [TEMP_C PRESSURE_MB]`: sextant altitude Hs
and index error in marcmin, height of eye in cm, horizontal parallax
and semidiameter in marcmin (0 if not applicable), limb 1 = lower,
-1 = upper, 0 = center. The optional trailing pair is the air
temperature in whole degrees C (-60..60) and the sea-level pressure in
whole millibars (800..1100); both or neither.

A star sight, Hs = 31 deg 04.0', index error +0.1', eye 2 m:

```
$ ./sight_reduction --correct 1864038 100 200 0 0 0
Hs:  31 deg 4.0'   (index +100, dip -2489, refraction -1649, parallax +0, SD none milli-arcmin)
Ho:  31 deg 0.0'   = 1860000 milli-arcmin   sin(Ho) = 553017922/2^30
```

Each correction is itemized so you can check it against the Nautical
Almanac's altitude-correction tables. The `sin(Ho)` Q2.30 value is what
the vector fix path actually consumes.

A lower-limb Sun-type sight with parallax and semidiameter (this is the
`make test` case; HP = 0.15', SD = 16.1'):

```
$ ./sight_reduction --correct 1514300 100 200 150 16100 1
Hs:  25 deg 14.3'   (index +100, dip -2489, refraction -2102, parallax +136, SD +lower milli-arcmin)
Ho:  25 deg 26.0'   = 1526045 milli-arcmin   sin(Ho) = 461142501/2^30
```

For Sun sights the HP/SD lookup can be skipped entirely:
[`--correct-sun`](#49---sun---reduce-sun---fix-sun----the-sun-and-the-time-contract)
computes both from the sight instant.

Refraction assumes the standard atmosphere (10 C, 1010 mb) unless the
trailing pair says otherwise. Non-standard air rescales it by the
standard density factor `(P/1010) x (283/(273+T))` -- exactly the
Nautical Almanac's additional-correction table -- and prints an
`atmosphere:` line so the output shows which chain ran. The same sight
on a cold high-pressure winter morning (factor ~1.24):

```
$ ./sight_reduction --correct 1514300 100 200 150 16100 1 -40 1030
atmosphere: -40 C, 1030 mb   (refraction scaled from the 10 C / 1010 mb standard)
Hs:  25 deg 14.3'   (index +100, dip -2489, refraction -2604, parallax +136, SD +lower milli-arcmin)
Ho:  25 deg 25.5'   = 1525543 milli-arcmin   sin(Ho) = 461000898/2^30
```

Spelling out `10 1010` is bit-identical to omitting the pair (the
scale factor's numerator equals its denominator exactly, so the
standard chain IS the scaled chain at standard conditions; `make test`
asserts the byte equality).

Rejection example (Hs beyond +90 deg -- `make test` requires this to
fail):

```
$ ./sight_reduction --correct 5400001 0 200 150 16100 1   # exit status 2
```

### 4.2 `--star` -- the built-in vector almanac

`INDEX UT1_MS`. Index 0..17:

| | | | |
| --- | --- | --- | --- |
| 0 Sirius | 5 Rigel | 10 Aldebaran | 15 Deneb |
| 1 Canopus | 6 Procyon | 11 Spica | 16 Regulus |
| 2 Arcturus | 7 Betelgeuse | 12 Antares | 17 Polaris |
| 3 Vega | 8 Achernar | 13 Pollux | |
| 4 Capella | 9 Altair | 14 Fomalhaut | |

```
$ ./sight_reduction --star 2 836136000000
Arcturus at UT1 J2000 +836136000000 ms:
GHA Aries: 279.06 deg
GHA:       64.84 deg
dec:       19.06 deg
earth-fixed vector: (431521977, -918563902, 350643188)/2^30
```

The catalog stores each star as a Q2.30 J2000 unit vector; the program
rotates it to the earth-fixed frame at your instant (IAU 1976
precession + Earth rotation angle -> GMST). GHA/dec are printed in
centidegrees precision for use with `--reduce`/`--fix`; the vector is
the full-precision form the library's Method C uses.

Out-of-range index or timestamp is rejected:

```
$ ./sight_reduction --star 18 0   # exit status 2
```

### 4.3 `--reduce` -- one sight: Hc, Zn, and the intercept

`LAT LON GHA DEC [HO]`: assumed position, body coordinates, and
optionally your observed altitude. All three reduction methods run and
are printed side by side -- agreement between them is itself a check:

```
$ ./sight_reduction --reduce 4000 -7400 6000 2000 4012800
Hc(A): 66.68 deg
Hc(B): 66.68 deg
Hc(C): 66.68 deg   (machine sin_hc=986027972/2^30)
Zn(A): 144.95 deg true
Zn(B): 144.96 deg true   square-key=27022/65536
Zn(C): 144.96 deg true   square-key=27022/65536
A/B altitude difference: 0.000'
Intercept: 12.0 nm TOWARD
```

That is the textbook Marcq St. Hilaire case: assumed position
40 deg N 74 deg W, body at GHA 60 deg / dec 20 deg N, Ho = 66 deg 52.8'
(4012800 marcmin). Hc = 66.68 deg, Zn = 145 deg, and since Ho > Hc the
position line lies 12.0 nm from the assumed position **toward** the
body's azimuth.

- Method A is classical spherical trigonometry in Q16.48 fixed point.
- Method B derives azimuth via the *square key*, a 16-bit integer that
  parameterizes direction by perimeter position on a square instead of
  arc length on a circle (`27022/65536` above). See
  [`ALTERNATIVE_METHODS.md`](../ALTERNATIVE_METHODS.md).
- Method C never leaves unit vectors (Q2.30): `sin Hc` is a dot
  product; `986027972/2^30` is the machine-native answer before any
  angle is reconstructed.

A body at the zenith has no azimuth, and the program says so instead
of inventing one:

```
$ ./sight_reduction --reduce 4500 0 0 4500
Hc(A): 90.00 deg
...
Zn(A): undefined (body at zenith/nadir)
```

Latitude outside +-90 deg is rejected (`--reduce 9001 0 0 0` -> exit
status 2).

### 4.4 `--fix` -- two sights: position without plotting

`DRLAT DRLON GHA1 DEC1 HO1 GHA2 DEC2 HO2`. Each (GHA, dec, Ho) defines
a circle of equal altitude; the program intersects the two circles
*exactly* (vector algebra, no iteration, no plotting sheet) and uses
your dead-reckoning position only to pick which of the two
intersection points is yours -- the other is printed as the alternate.

See the [two-star fix example](#52-a-two-star-fix-arcturus--altair) for
the full workflow. Degenerate geometry (same body twice, antipodal
bodies, circles that don't intersect) is refused rather than answered
-- exit status `1`, the well-formed-input-but-no-answer code:

```
$ ./sight_reduction --fix 4400 100 0 0 2700000 0 0 2700000   # exit status 1
```

The `--fix-stars` variant takes each body as a catalog star INDEX plus
a UT1 instant instead of GHA/dec: the body vectors come straight from
the almanac at full Q2.30 precision, with no centidegree rounding in
between. Feed it timestamps and it is the Bris timed-crossing fix (see
[5.4](#54-the-bris-variant-a-fixed-angle-and-a-clock)); feed it
ordinary sight times and it simply skips the hand-copying of GHA/dec.
`--reduce-star LAT LON STAR UT1_MS [HO]` is the one-sight version of
the same chain (Method C only); with HO it also prints the raw Q2.30
sine residual `sin_hc - sin(Ho)` -- the machine-side distance from the
circle of equal altitude, before any display rounding.

### 4.5 `--average` -- several shots of one body

`TREF_MS REJECT HO T [HO T ...]`: reference instant, outlier-rejection
threshold in marcmin (0 = off), then (Ho, t) pairs. Fits a straight
line Ho(t) by least squares, iteratively drops the worst shot while it
exceeds the threshold, and evaluates the line at TREF_MS:

```
$ ./sight_reduction --average 120000 1000 1500000 0 1503000 60000 1511000 120000 1509000 180000 1512000 240000
shots: 5 taken, 4 kept (1 rejected beyond 1000 milli-arcmin)
Ho at TREF:     25 deg 6.0'   = 1506000 milli-arcmin   sin(Ho) = 455480662/2^30
altitude rate:  3.000'/min
worst residual: 0.000'   (scatter of the kept shots)
```

Five shots a minute apart; four sit exactly on a 3.0'/min rising line,
the third (1511000 at t = 120000, where the line predicts 1506000) is
5.0' off and gets rejected. The fitted Ho at the reference instant --
not any single shot -- is what you feed to `--reduce` or `--fix`.

The altitude rate doubles as a sanity check: a star near the prime
vertical changes altitude fast; near meridian transit the rate should
be near zero. If the printed rate contradicts where you know the body
was, a clock or reading error is likely.

Refusals: fewer than 2 shots; spans or values that would leave the
arithmetic's proven-safe domain, including a
fitted line that leaves +-90 deg at TREF_MS (extrapolating a steep run
to a distant instant is not an altitude -- refused, not clamped):

```
$ ./sight_reduction --average 1099511627776 0 -5400000 0 5400000 1   # exit status 2
```

### 4.6 `--zenith` -- camera fix from a plate-solved photo

`X Y Z UT1_MS`: a J2000 celestial-frame zenith direction as a Q2.30
unit vector, plus the instant. Point a camera straight up (gravity
defines up), plate-solve the star field to get the celestial
coordinates of the zenith, and the fix is direct -- your position *is*
the sub-point of your zenith:

```
$ ./sight_reduction --zenith 134321514 -826150956 672572549 836136000000
zenith fix at UT1 J2000 +836136000000 ms:
lat: 38.81 deg
lon: 0.39 deg
position vector: (836698133, 5757400, 672922426)/2^30
```

The input here is literally Vega's J2000 catalog vector: "if Vega is
exactly overhead at 2026-07-01 00:00 UT1, you are at 38.81 N 0.39 E"
(Vega's sub-stellar point at that instant -- compare `--star 3
836136000000`).

Vectors that aren't unit length (checked to Q2.30 tolerance), or with
components outside Q2.30 range, are rejected with exit status 2:

```
$ ./sight_reduction --zenith 1 2 3 0                                   # not unit length
$ ./sight_reduction --zenith 2000000000 2000000000 2000000000 0        # not a Q2.30 component
```

### 4.7 `--golden` -- the determinism gate

No arguments. Hashes every output bit of a fixed 4096-case
pseudo-random schedule plus degenerate edge cases with FNV-1a 64 and
compares against the constant committed in `sight_reduction.c`. Any
difference -- one bit, one case -- is a hard failure. This is what
makes "integer-only" a checkable claim rather than a slogan: two
builds that both pass `--golden` agree on every value in that committed
schedule. The gate detects drift; it does not prove untested inputs or
exclude a theoretical hash collision.

If you *intentionally* change numerical behavior, `--golden` prints
the new hash; update `GOLDEN_HASH` in the same commit and say why.

### 4.8 `--ephemeris-check` -- the almanac against an independent authority

No arguments. `--golden` and `--native-reference` can only prove the
arithmetic is faithful to the *model*; this mode measures the model
against the sky. It compares `astro_nav_celestial_to_earthfixed()` for
every catalog star at 15 epochs spanning 2000-2036 (270 rows) against
truth vectors committed in `ephemeris_reference.h`, generated offline
from Skyfield/DE421 apparent places -- Hipparcos astrometry with
proper motion, annual aberration, light deflection, and IAU 2000A/2006
precession-nutation, i.e. everything this almanac deliberately omits.
It prints per-star worst separations and asserts the documented bound
(see [section 7](#7-model-assumptions-and-current-limitations)). No
Python is needed at build or test time; regenerating the truth table
is a one-time offline step (`tools/make_ephemeris_reference.py`).

The same gate covers the Sun: 45 truth rows -- 41 sampled ~315.6 days
apart (so the annual error cycle is walked through several times over
2000-2035) plus the 2026 and 2035 perihelion/aphelion epochs, where
the distance term peaks -- each carrying Skyfield's TT - UT1 for its
instant so the caller-supplied time contract below is exercised end to
end. Measured across those 45 sampled epochs: mean 0.17', worst 0.39',
gated at 0.60'.

The Moon completes the set: 47 truth rows -- 41 sampled ~322.6 days
apart over 2000-2035 plus the 2026 apogee/perigee pair, both 2025
major-standstill declination extremes (dec +-28.7 deg), and one
crossing of each node -- each carrying its TT - UT1 and truth
distance/SD/HP next to the direction (truth distance is geometric,
not the light-time-retarded range; the tool explains the up-to-38 km
difference). Measured on the committed rows: direction mean 0.10',
worst 0.16', gate 0.50' (the gate covers the abridged series'
full-sweep worst of ~0.27' -- section 4.10 -- with margin); distance
worst 7 km, gate 40 km; SD worst 1 milli-arcmin, gate 6; HP worst 1
milli-arcmin, gate 10.

#### `--external-check` -- the almanac against a *printed* authority

No arguments. `--ephemeris-check` measures the model against Skyfield/
DE421; this mode measures it against numbers a human read off a printed
page -- a second authority independent of both this library's formulas
and of Skyfield. Two independent authorities disagreeing with us the
same way is stronger evidence than either alone. Truth rows are
committed in `external_reference.h`, transcribed offline (numeric facts
only, no vendored code) by `tools/make_external_reference.py`; that
tool carries full per-source provenance. The published sources:

- **Sun GHA/dec and GHA Aries** from printed daily pages -- Paracay
  Nautical Almanac 2021, NA 2002, USNO Air Almanac 2023, EZ Celestial
  Almanac 2023 (collected via the `quantenschaum/nautical_almanac`
  repo) -- plus a broad-era 2001-2025 Sun table and NA GHA-Aries wrap
  values.
- **Sun semidiameter** and **star GHA/dec** rows from the Nautical
  Almanac (2001-2025), via the OpenCPN `celestial_navigation_pi` test
  suite.
- **Moon GHA/dec (0.1') and hourly HP** from the same daily pages (the
  Air Almanac tabulates the Moon to whole arcminutes only and is
  skipped as pure quantization; the pages' one Moon SD per 3-day page
  is not attributable to an instant), plus **Moon GHA/dec/HP/SD** rows
  from the Nautical Almanac (2001-2025) via the same OpenCPN suite.
- The **NA standard refraction and dip tables**.
- One **Bowditch (2019)** Hs->Ho worked example, pinning the whole
  correction chain against a printed answer.
- Nine **USNO celnav service** rows (aa.usno.navy.mil, transcribed
  2026-07): the *augmented* Moon semidiameter its correction block
  prints for the observer, across altitudes 10-90 deg at both HP
  extremes -- the altitude-dependent piece no fixed table carries,
  held against the official service directly (section 4.10).

Direction rows (Sun, star) are compared as the chord between our
earth-fixed unit vector and the one baked from the printed GHA/dec, so
the comparison is free of centidegree-boundary quantization. Each gate
is set just above the worst separation measured on this host. Measured
worst / gate per family: Sun 0.60' / 0.70'; GHA Aries 0.60' / 0.70';
Sun SD 0.07' / 0.12'; stars 1.17' / 1.30' (Arcturus, proper motion);
Moon GHA/dec 0.33' / 0.40'; Moon HP 0.11' / 0.15'; Moon SD 0.15' /
0.20'; Moon SD augmented 0.001' / 0.01' (USNO); refraction 0.10' /
0.15'; dip 0.00' / 0.05'; correction 0.03' / 0.10'.

Two families carry a documented model gap rather than transcription
error. **GHA Aries**: the NA prints apparent sidereal time (GAST, which
includes the equation of the equinoxes, ~0.26'), while
`astro_nav_gha_aries_cdeg()` returns mean sidereal time (GMST); that
offset plus up to 0.30' of centidegree boundary rounding accounts for
the 0.60' worst. **Refraction**: see [section 7](#7-model-assumptions-and-current-limitations).

Nine of the 146 OpenCPN Moon rows carry upstream misprints, excluded
at transcription with the evidence recorded rather than absorbed by
wider gates: one prints HP 16' (impossible -- lunar HP spans 54-61';
likely a mangled 60.1', and only that scalar is excluded: the row's
direction, 0.02-0.03' from DE421, and its printed SD 16.4' are
ordinary and stay in), three unrelated dates print the identical
HP/SD pair (a fill artifact, 0.65-2.7' from DE421 in a column
otherwise never worse than 0.08'), and five print an SD contradicting
the row's *own* printed HP by 0.18-0.44' (both columns print from one
distance and agree to under 0.1' everywhere else) -- with DE421
siding against every excluded value. See `MOON_NA_EXCLUDE` in the
tool for the row-by-row evidence; the exclusions strike only the
misprinted scalars, and every transcribed row's direction stays in
the comparison.

Almanac daily pages tabulate against UT and this library's rotation
runs on UT1, so each tabulated page hour is treated as UT1 directly; if
a publisher meant UTC the error is at most |DUT1| < 0.9 s of rotation =
0.23' of GHA, inside every gate above.

#### `--cross-check` -- the almanac and geometry against independent *computed* implementations

No arguments. The first two gates compare against a numerical
integration (Skyfield/DE421) and against printed pages. This gate adds
computed cross-implementation evidence -- other people's *code*,
solving the same problems by different methods -- and reports
agreement **per independent implementation lineage, not per
repository**: ten tools that all call Skyfield inherit JPL's answers
and count as one family of evidence, however many of them exist.
Truth rows are committed in `cross_reference.h`, generated offline by
`tools/make_cross_reference.py` (numeric rows only, no vendored code;
the tool pins the exact upstream revisions and *enforces* them --
regeneration refuses on any ephem-version or geometry-revision
mismatch, and the explicit `--allow-unpinned` override stamps the
emitted header UNPINNED so drifted provenance can never look pinned).
Two lineages:

- **PyEphem/libastro** (the XEphem lineage): 45 Sun, 47 Moon, and 270
  star direction rows on the same epoch schedules as
  `--ephemeris-check` (so the lineages sample identical instants),
  plus GHA Aries and distance/SD/HP. Independence is scoped per body:
  the Sun (Bretagnon's analytic planetary theory) and the Moon
  (ELP-derived) are code *and* underlying data disjoint from JPL
  numerical integration and from printed pages; the star rows are
  **not** data-independent -- PyEphem's bundled catalog is the same
  Hipparcos data, and its epoch shift to J2000 was itself computed
  with Skyfield (stated in `ephem/stars.py`) -- so they validate
  libastro's own precession/nutation/aberration/sidereal-time chain,
  not independent star positions. Each row carries libastro's own
  delta_t, so the library receives the exact (UT1, TT) pair the
  oracle used and no delta-T model difference leaks in. The two
  oracles are also compared with *each other*, and the comparison is
  executable, not prose:

  ```sh
  venv/bin/python tools/make_cross_reference.py \
      --calibrate ephemeris_reference.h
  ```

  replays every committed Skyfield row through libastro (Sun and Moon
  TT-aligned per row with sidereal time at true UT1; stars at the
  row's UT1 with libastro's own delta_t, a negligible difference at
  star scale) and reports the worst oracle-vs-oracle separation per
  family. The ephem pin is enforced here exactly as for generation
  (`--allow-unpinned` overrides), and the version always prints.
  Recorded output:

  ```
  ephem 4.2.1 (pinned)
  sun     45 rows  worst  7.33 milli-arcmin
  moon    47 rows  worst  2.30 milli-arcmin
  stars  270 rows  worst 11.29 milli-arcmin
  ```

  55x (Sun), 73x (Moon), 129x (stars) below this library's measured
  model error (0.40', 0.16', 1.45'), so whatever separation this gate
  measures is ours, confirmed by two authorities (fully independent
  ones for the Sun and Moon).
- **An independent spherical-geometry engine** (the alinnman
  celestial-navigation project; revision pinned in the tool): 16 Hc/Zn
  reductions from its own vector primitives and 14 two-body fixes
  solved by direct circle-of-position intersection -- a genuinely
  different algorithm from this library's iterated least-squares.
  Both engines receive bit-identical quantized inputs (centidegree
  positions and GPs, milli-arcmin altitudes derived from an off-grid
  true position), so scenario quantization cancels and the comparison
  isolates engine against engine. Scenarios cover all azimuth
  quadrants, the equator, 89 deg latitude, the dateline, GHA 0/359.99
  wraps, a sight 0.74 deg from the zenith, a body below the horizon,
  narrow (~30 deg) and wide (~150 deg) cut angles, and a 60 nm DR
  error.

Measured worst / gate per family: Sun 0.40' / 0.50' (the same 0.39-0.40'
the DE421 gate measures -- the model error, confirmed twice); Sun
distance 78 / 100 micro-AU (identical worst as against DE421); Sun
SD 0.003' / 0.008'; Moon 0.16' / 0.25'; Moon distance 8 / 20 km; Moon
SD 0.025' / 0.035' (a lunar-radius constant convention gap, not a
distance error); stars 1.45' / 1.60' (Arcturus -- the same worst star
every lineage finds); GHA Aries 0.40' / 0.50' (the documented
GAST-vs-GMST equation-of-the-equinoxes gap, as in `--external-check`);
reduce Hc 0.298' / 0.30' and fix lat 0.299' / 0.30' (the centidegree
readout step itself -- the engines agree beneath the resolution the
readout can express); reduce Zn 0.52' / 0.60' (square-key resolution,
1/65536 turn, plus readout rounding); fix lon 0.13' / 0.20'.

Two caveats are stated rather than hidden. First: libastro's Moon and
this library's abridged Meeus ch. 47 series both descend from
ELP-class analytic theory, so their 0.16' Moon agreement is partly
kinship; the fully independent Moon verdict remains with
`--ephemeris-check` (numerical integration) and `--external-check`
(printed pages). Second: the star rows share the Hipparcos catalog
with the Skyfield lineage (see the scope note in the bullet above),
so the star family confirms libastro's independent *transformation*
of shared positions. Every star table in these lineages descends
from Hipparcos (Gaia exists, but none of these implementations
ingests it), so what the three gates diversify is the *reduction
chain*: Skyfield, libastro, and the almanac office each turn the
catalog into apparent places with their own code.

#### `--scenario-check` -- the whole pipeline against external answers

No arguments. `--ephemeris-check` and `--external-check` gate each stage
in isolation (a body's position, one correction, one angle). Neither
checks the *composition* -- that feeding the stored altitude inputs
through the whole pipeline (sky -> corrected altitude -> multi-body fix ->
position) actually lands on the right spot. `--scenario-check` does,
against three external families committed in `sight_scenarios.h`
(generated/transcribed offline by `tools/make_sight_scenarios.py`;
numbers only, no vendored code):

- **Recovery scenarios** (29, from Skyfield/DE421): a known observer
  position and instant with 2-5 bodies, both hemispheres, high and low
  latitudes, epochs 2000-2035 -- star rounds, Sun+star and Moon+star
  mixes, a Sun+Moon+star triple, and the classic daytime Sun-Moon cut
  taken both ways (closed-form `astro_nav_fix_two_body`, and the
  n-body solver at n=2). Each
  body's truth Ho is `asin(obs_unit . body_unit)` in this library's
  navigation-sphere model: `body_unit` is Skyfield's geocentric
  apparent direction and `obs_unit` is the local zenith. A
  surface-observer topocentric altitude is deliberately not used
  because it still contains observer-displacement parallax, already
  removed from the corrected Ho the fix consumes (up to about a degree
  for the Moon). We build the bodies
  from our own ephemeris, feed the stored Ho, run the fix, and measure
  recovered-vs-truth position: the almanac error propagated through the
  fix geometry. Measured mean 0.26', worst 0.85' / gate 1.00' --
  averaging across bodies pulls the per-body almanac error (up to
  ~1.2') under a nautical mile. The worst row is a Moon+star round
  whose greedy body pick pairs Arcturus (the catalog's largest proper
  motion, ~1.2' accumulated by 2028) with Polaris' along-parallel line
  of position; the Moon itself is ~0.02' off there.
- **Two alinnman published fixes** (`github.com/alinnman/
  celestial-navigation`). Chicago: two Sun sights plus Vega,
  2024-05-05/06, published answer 41 deg 51'N 87 deg 39'W; the raw
  sextant altitudes run through our correction chain (refraction only
  -- exactly the corrections their `Sight()` applied), then our
  n-body fix, missing the published answer by 0.30' / gate 0.60'.
  Off Tunis: Capella + **Moon** + Vega, 2024-09-17, published answer
  36 deg 45'11"N 10 deg 13'8"E -- a published *raw Moon sight*: the
  sextant altitude 48 deg 22' carries ~40' of lunar parallax (HP
  ~61', a near-perigee Moon) that our Moon chain removes using our
  own ephemeris HP at the instant, and the fix lands 0.03' / gate
  0.30' from their answer. Both answers are geodetic; ours match them
  directly (see
  [section 7](#7-model-assumptions-and-current-limitations) on why the
  spherical fit recovers the geodetic local-zenith direction rather
  than the ellipsoid's geocentric surface-radius direction).
- **rgleason INTERCEPT_SIGHTS Sun rows** (`github.com/rgleason/
  celestial_navigation_pi`): 10 rows spanning both limbs and a range of
  intercept signs. Our Sun direction vs their published ground position
  (worst 0.32' / gate 0.70', the Sun model's own band), our Hs->Ho vs
  theirs (worst 0.10' / gate 0.30'), and our Hc / Zn / intercept vs
  theirs (reducing at their DR against their published GP, so the
  reduction is isolated from our ephemeris): Hc worst 0.40' / gate
  0.50', Zn worst 1 cdeg (0.01 deg), intercept worst 0.4 / gate 0.5 nm.
  The
  Hs->Ho agreement is the notable one: their chain uses Saemundsson
  refraction and a 1.758 dip constant, ours Bennett and 1.76, yet the
  two agree to 0.10' across these rows -- above ~12 deg the refraction
  models barely differ. The Hc gap is dominated by our Hc crossing the
  human boundary as centidegrees (0.3' quantization of a full-precision
  `sin(Hc)`); the reduction math itself agrees, as the near-zero Zn gap
  confirms.

Every gate is set just above the worst measured on this host, with the
measurement recorded inline in the `SCEN_GATE_*` defines. Wired into
`make test` and `make ubsan` alongside the two stage checks.

### 4.9 `--sun`, `--reduce-sun`, `--fix-sun` -- the Sun and the time contract

The Sun is the first body in this library whose *own position* is a
function of time, and that brings in a second timescale. The contract,
stated once in `astro_nav.h` and everywhere the same:

- Earth **rotation** runs on **UT1** -- every `UT1_MS` argument.
- Solar-system **dynamics** run on **TT** (Terrestrial Time).
- The caller supplies **TT - UT1** as integer milliseconds. The math
  core never decides civil time: UTC, leap seconds, and DUT1 stay
  outside it (the [`--time` boundary adapter](#412---time----calendar-utc-to-the-librarys-timescales)
  does the calendar arithmetic, from caller-supplied policy numbers).
  TT - UT1 = 32.184 s + accumulated leap seconds - DUT1: about
  **69200 ms in 2026**. It changes negligibly during one sight round,
  but the caller still supplies the value for the observation epoch.
  Passing 0 mis-places the Sun by at most ~0.05' this era -- inside
  the model budget, but the honest value costs nothing.

The model is the USNO low-precision solar formula (mean longitude plus
equation of center; annual aberration folded into its constants,
nutation deliberately omitted like everywhere else in this library),
evaluated in Q16.48 with angles accumulated in integer
micro-arcseconds. It produces an inertial J2000-frame Q2.30 vector
that the existing star rotation consumes unchanged -- the Sun enters
the fix machinery exactly like a nineteenth star. Real-sky accuracy is
measured, not estimated: see `--ephemeris-check` above (worst sampled
0.39').

`--sun UT1_MS TT_MINUS_UT1_MS` is the almanac entry, `--star`'s Sun
sibling:

```
$ ./sight_reduction --sun 837259200000 69200      # 2026-07-14 00:00 UT1
Sun at UT1 J2000 +837259200000 ms (TT - UT1 = 69200 ms):
GHA Aries: 291.88 deg
GHA:       178.54 deg
dec:       21.70 deg
earth-fixed vector: (-997304137, -25481309, 397060029)/2^30
inertial J2000 vector: (-388780881, 918319505, 398070620)/2^30
distance:  1016533 micro-AU
SD:        15.736' (15736 milli-arcmin)   HP: 0.144' (144 milli-arcmin)
```

The last two lines are the daily-pages numbers a Sun sight needs,
computed instead of copied: the same USNO formula that gives the
direction gives the distance (here 1.016533 AU, mid-July, near
aphelion), and semidiameter and horizontal parallax follow from it
(`astro_nav_sun_distance`). `--ephemeris-check` gates the distance
against DE421 too -- worst 78 micro-AU across the sampled epochs,
including explicit perihelion/aphelion rows, which keeps computed SD
within 0.001' of the DE421-distance value.

`--reduce-sun LAT LON UT1_MS TT_MINUS_UT1_MS [HO]` is `--reduce-star`
for the Sun (Method C, raw Q2.30 sine residual with `HO`), and
`--fix-sun DRLAT DRLON TT_MINUS_UT1_MS UT1_1 HO1 UT1_2 HO2` is the
**timed two-Sun-crossing fix**: the same body at two instants, the
Sun having moved ~15 deg of GHA per hour in between. One TT - UT1
serves both sights; any millisecond-scale change during one day is far
below this Sun model's resolution.
Section 5.5 and [`BRIS.md`](BRIS.md) work a full Sun day; the
geometry lesson that falls out (symmetric morning/afternoon equal
altitudes pin longitude, not latitude) is described there.

`--correct-sun HS IE EYE_CM UT1_MS TT_MINUS_UT1_MS LIMB
[TEMP_C PRESSURE_MB]` closes the loop on Sun altitude corrections: it
is [`--correct`](#41---correct----sextant-altitude-to-observed-altitude)
with the `HP` and `SD` arguments replaced by the sight instant, the
values coming from `astro_nav_sun_distance` instead of the daily
pages (the optional atmosphere pair works exactly as in 4.1):

```
$ ./sight_reduction --correct-sun 1810000 100 200 837286272832 69200 1
Sun at UT1 J2000 +837286272832 ms:  distance 1016520 micro-AU   SD 15.736'   HP 0.144'
Hs:  30 deg 10.0'   (index +100, dip -2489, refraction -1709, parallax +125, SD +lower milli-arcmin)
Ho:  30 deg 21.8'   = 1821763 milli-arcmin   sin(Ho) = 542746862/2^30
```

A complete Sun sight now needs no almanac at all: `--correct-sun`
for the altitude, `--reduce-sun`/`--fix-sun`/`--fix-n` for the
geometry.

### 4.10 `--moon`, `--reduce-moon`, `--correct-moon` -- the Moon: the same contract, with teeth

The Moon takes the Sun's time contract unchanged -- rotation on UT1,
dynamics on TT, caller-supplied `TT_MINUS_UT1_MS` -- and raises the
stakes: its mean motion is ~0.55' per minute of time, thirteen times
the Sun's, so passing 0 for TT - UT1 mis-places the Moon by ~0.6',
more than the model's own error (the same shortcut costs the Sun
~0.05'). The model is the abridged ELP-2000/82 lunar series from
Meeus ch. 47 -- the 60 + 60 largest periodic terms plus additive
corrections and eccentricity damping -- evaluated in Q16.48 exactly
like the Sun's formula and emitted as an inertial J2000 vector the
same rotation consumes. Real-sky accuracy is measured, not estimated:
a 37,048-instant sweep against Skyfield/DE421 apparent places over
the DE421-covered 1900-2053 part of the library's time domain
(DE421 ends 2053-10-09) --
puts the earth-fixed direction at worst ~0.27', median ~0.10'
(reproducible with `tools/sweep_moon_vs_de421.py` against the shipped
binary); the 47 committed `--ephemeris-check` rows (section 4.8) gate
direction at 0.50' with distance/SD/HP alongside, and 242 printed
almanac rows hold it against paper at 0.40' (`--external-check`).

`--moon UT1_MS TT_MINUS_UT1_MS` is the almanac entry:

```
$ ./sight_reduction --moon 837259200000 69200     # 2026-07-14 00:00 UT1
Moon at UT1 J2000 +837259200000 ms (TT - UT1 = 69200 ms):
GHA Aries: 291.88 deg
GHA:       184.17 deg
dec:       26.09 deg
earth-fixed vector: (-961810325, 70110543, 472151369)/2^30
inertial J2000 vector: (-286640076, 920394258, 472898913)/2^30
distance:  359518 km
SD:        16.613' (16613 milli-arcmin)   HP: 60.992' (60992 milli-arcmin)
```

The last line is where the Moon differs from everything else in the
sky. HP here is 54-61' -- the largest correction in celestial
navigation, some 400x the Sun's, up to a full degree of altitude if
skipped -- and SD swings 14.7'-16.8' with the distance across the
month (the daily pages tabulate both three times a day; this computes
them from the instant). Hence the one rule for Moon sights: **the
`HO` fed to `--reduce-moon`, `--fix-n`, or `--running-fix` must have
gone through `--correct-moon`**, which is `--correct` with `HP` and
`SD` taken from the sight instant -- and with one Moon-only step the
generic chain does not have, the semidiameter **augmentation**:

```
$ ./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1
Moon at UT1 J2000 +837295272832 ms:  distance 360177 km   SD 16.583'   HP 60.880'
Hs:  56 deg 40.0'   (index +100, dip -2489, refraction -656, parallax +33489, SD +lower augmented +250 milli-arcmin)
Ho:  57 deg 27.3'   = 3447277 milli-arcmin   sin(Ho) = 905127413/2^30
```

Parallax +33489 milli-arcmin: at this altitude the Moon correction is
~50x the refraction, and it *raises* the observed altitude -- the
geocentric direction the almanac computes is what the fix machinery
needs, and the sextant stood on the Earth's surface, not its center.
The `augmented +250` on the SD term is the surface-vs-center fact
applied to the Moon's disc: at altitude ~57 deg the observer stands
most of an Earth radius closer to the Moon than the geocenter the
tabulated 16.583' assumes, so the limb sits 0.25' farther from the
center. The chain computes it exactly (law of cosines at the
parallax-corrected altitude, `astro_nav_moon_augmentation_marcmin()`),
and `--external-check` holds the augmented SD against the USNO celnav
service's printed values to 0.01' across altitudes 10-90 deg at both
HP extremes. It peaks at ~0.3' at the zenith and vanishes at the
horizon -- omit it and every high-altitude limb sight is
systematically ~0.2-0.3' short.

`--reduce-moon LAT LON UT1_MS TT_MINUS_UT1_MS [HO]` is
`--reduce-sun`'s Moon sibling (Method C; with `HO` it prints the raw
sine residual and the intercept), and `moon:TT_MINUS_UT1_MS` puts
timed Moon sights straight into `--fix-n` and `--running-fix` logs
next to stars and `sun:` entries (section 4.11). There is no
`--fix-moon`: the two-crossing fix is a Bris-instrument regime, and
the Moon's classical role is the opposite one -- the daytime second
body that turns a Sun line into a fix at one instant.

### 4.11 `--fix-n`, `--running-fix` -- more than two sights, and underway

`--fix-n DRLAT DRLON BODY UT1_MS HO [BODY UT1_MS HO ...]` is the
overdetermined fix: 2..32 timed sights from **one** observer
position, solved by integer Gauss-Newton least squares
(`astro_nav_fix_n_body`). `BODY` is a star catalog index,
`sun:TT_MINUS_UT1_MS`, or `moon:TT_MINUS_UT1_MS` -- the Sun and the
Moon carry their time-contract offset inline, so stars, the Sun, and
the Moon mix freely in one log (Moon `HO` through `--correct-moon`
first: section 4.10). Unlike the
closed-form two-body fix there is no alternate point (the DR seed
picks the solution basin), and the printed **worst residual** is the
navigator's scatter number: the worst single sight's distance from
its circle, in milli-arcminutes (1000 = 1 nm).

```
$ ./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 9 836127121859 1800000 3 836127121859 3701231
Deneb      at UT1 J2000 +836121913051 ms   Ho = 1800000 milli-arcmin
Altair     at UT1 J2000 +836127121859 ms   Ho = 1800000 milli-arcmin
Vega       at UT1 J2000 +836127121859 ms   Ho = 3701231 milli-arcmin
fix:       lat 45.00 deg   lon 0.00 deg
sights: 3   iterations: 4   worst residual: 0 milli-arcmin
```

`--running-fix DRLAT DRLON COURSE SPEED BODY UT1_MS HO [...]` is
`--fix-n` underway -- the classic running fix, machine-native. The
track is modeled as the **great circle whose bearing at the DR is
`COURSE`** (centidegrees true), run at `SPEED` (tenths of a
knot); each sight is advanced to the **last listed** sight's instant
by rotating its body vector along that track
(`astro_nav_advance_body_for_run`: run = speed x elapsed time,
rounded to the nearest tenth of a nautical mile), then the same
least-squares solve runs as if the sights were simultaneous. (A
helmsman actually holding `COURSE` steers a rhumb line, which
diverges from this great circle by about tan(lat) x s^2 / 2, with
the run s and the result both angular distances in radians --
roughly 0.13' over a 30 nm due-east run at 45 N, 0.7' at 80 N --
and the polar cap `|DRLAT| > 8998` is rejected outright, since a
course has no direction at the pole.) `DRLAT`/`DRLON` is the dead reckoning *at the last sight*,
which is where the fix comes out. Three Sun sights from a vessel steaming due
north at 12.0 kn (true track 44.5 N -> 44.8 N -> 45.0 N on 0 E, the
Bris Sun morning):

```
$ ./sight_reduction --running-fix 4490 10 0 120 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969
course 0.00 deg true   speed 12.0 kn   fix at the last sight's instant
Sun        at UT1 J2000 +837286272832 ms   Ho = 1799167 milli-arcmin   run +30.0 nm
Sun        at UT1 J2000 +837291672832 ms   Ho = 2746731 milli-arcmin   run +12.0 nm
Sun        at UT1 J2000 +837295272832 ms   Ho = 3323969 milli-arcmin   run +0.0 nm
fix:       lat 45.00 deg   lon 0.00 deg
sights: 3   iterations: 3   worst residual: 3 milli-arcmin
```

The residual is also the mode's built-in honesty check: feed that
same underway log to `--fix-n` (whose premise is one position) and it
still prints a fix -- 44.97 N 0.01 E -- but the worst residual jumps
to 2414 milli-arcmin (2.4 nm). A tight fix with a large residual
means the model was violated somewhere: the run was ignored, a
timestamp is wrong, or one sight is bad. List the log newest-first
and the runs come out negative (sights are *retarded*): the fix then
lands at the track's first position instead -- both directions are
pinned in `make test`. The usual running-fix caveat carries over
unchanged: the advancement is exactly as good as the course, speed,
and DR that orient it.

### 4.12 `--time` -- calendar UTC to the library's timescales

Every mode above takes `UT1_MS` (and, for the Sun and the Moon,
`TT_MINUS_UT1_MS`).
Your watch shows calendar UTC. `--time` is the one adapter across that
boundary -- pure integer calendar arithmetic (Fliegel-Van Flandern
Julian day number), producing exactly the two arguments everything
else takes:

```
$ ./sight_reduction --time 2026 7 1 0 0 0 0 -16 37
2026-07-01 00:00:00.000 UTC   (DUT1 -16 ms, TAI - UTC +37 s)
UT1_MS:          836135999984
TT_MINUS_UT1_MS: 69200
```

The two trailing arguments are the civil-time **policy** numbers the
math core refuses to know, and they stay the caller's to supply:

- `DUT1_MS` = UT1 - UTC in milliseconds, published in IERS Bulletin A.
  The CLI accepts `-900..900`; if you do not care about the last
  quarter-arcminute of longitude, 0 is fine.
- `TAI_MINUS_UTC_S` = the accumulated leap-second count. It is **37**
  for this 2026 example; callers must verify the value in force at the
  observation rather than treating this document as a live policy feed.

From those, `UT1_MS = UTC + DUT1` and `TT_MINUS_UT1_MS = 32184 +
1000 x leap seconds - DUT1` (TT = TAI + 32.184 s exactly). The
example above is the repo's own worked instant, and its `-16` is an
**illustrative** DUT1 chosen so 2026-07-01 lands on the round
69200 ms used throughout these docs; the actual IERS value for that
date is UT1 - UTC = +0.014514 s (DUT1 `+15`), which gives a true
TT - UT1 of 69169 ms. Look yours up rather than copying the docs'.

`SEC` may be `60` -- a leap second is simply one more elapsed second,
spilling into the next civil day. Note what that does *not* mean:
`23:59:60.5` and the next day's `00:00:00.5` are **consecutive**
instants, one SI second apart, and the +1 s step lives in the policy
numbers (across the 2016-12-31 insertion, IERS UT1 - UTC went
-0.4087 s to +0.5913 s and TAI - UTC went 36 to 37). Fed each instant's
own DUT1 the two land 1000 ms apart in `UT1_MS`, while
`TT_MINUS_UT1_MS` comes out identical -- the two +1 s steps cancel,
so TT - UT1 is continuous across the leap (pinned in `make test`).
Since leap seconds are only ever inserted at 23:59:60 UTC on the last
day of a month, any other `SEC=60` label is refused (exit 2). Dates
are Gregorian and validated (2026-02-29 is refused, exit 2); the
outputs are validated against the same bounds every other mode
enforces, so anything `--time` prints is accepted everywhere.

Library call: `astro_nav_civil_to_times()`.

### 4.13 `--predict` -- the sight run backward: a known-position check

Every mode above starts from a sextant reading and works toward a
position. `--predict` is the same chain inverted: you already **know**
the position (GPS) and the time, and want to know what the sextant
should read under the same correction model -- because you are checking
the sight system, not finding the ship. The model assumes an
unobstructed natural sea horizon: it applies height-of-eye dip, standard
or supplied atmospheric refraction, and the body's parallax and
semidiameter. A raw artificial-horizon or land-horizon reading is not
an input to this mode. Point the instrument at a body and bring it to
that horizon; the difference between the drum and the prediction is an
*aggregate*. It is index error proper only where it repeats across
well-controlled shots (the honesty notes below).

```
./sight_reduction --predict LAT LON BODY UT1_MS EYE_CM LIMB [HS] [TEMP_C PRESSURE_MB]
```

`LAT`/`LON` are your known position (centidegrees, the GPS readout);
`BODY` uses the n-sight grammar (star index `0..17`,
`sun:TT_MINUS_UT1_MS`, or `moon:TT_MINUS_UT1_MS`); stars take
`LIMB 0` (no visible disc). Output: the body's almanac line, **Hc**
at milli-arcminute resolution, **Zn** (where to face), and the
**predicted Hs at IE = 0** -- Hc pushed *backward* through the full
correction chain (dip at your `EYE_CM`, refraction, parallax,
semidiameter with the Moon's topocentric augmentation), i.e. what a
zero-index-error instrument would show in this idealized model:

```
$ ./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1
Sun at UT1 J2000 +836367200000 ms (TT - UT1 = 69200 ms):  distance 1016703 micro-AU   SD 15.733'   HP 0.144'
observer: lat 40.00 deg   lon -74.00 deg   eye 200 cm
Hc:  70 deg 14.2'   = 4214247 milli-arcmin   (machine sin_hc=1010500573/2^30)
Zn:  146.33 deg true   (face here)
predicted Hs (IE = 0, lower limb):  70 deg 1.3'   = 4201316 milli-arcmin
```

The milli-arcminute digits are computational *resolution*, not
physical accuracy. The prediction inherits the almanac model's error
(the Sun a few tenths of an arcminute, stars up to ~1.5' -- measured
numbers in the README validation snapshot, the budget in
`astro_nav.h`) and the centidegree position grid: one `LAT` step
moves this example's Hc by 0.499', so a GPS readout rounded onto the
grid can contribute ~0.3' per axis. What repeats across shots is the
signal; the trailing digits are not.

Add the reading you actually took as `HS` and the difference comes out
as the **implied aggregate correction**, already in the sign
convention the `--correct` family's `IE` argument expects:

```
$ ./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 4203316
...
observed Hs:  70 deg 3.3'   = 4203316 milli-arcmin
implied aggregate correction: -2000 milli-arcmin (-2.000')
  aggregate has the sign of an ON-the-arc index correction
  use IE -2000 with --correct only if controlled repeated
  shots establish the stable component as instrument error
```

The loop closes exactly: feed that reading and that IE back through
`--correct-sun` and Ho reproduces the predicted Hc bit-for-bit
(`make test` asserts it). This works because the correction chain sees
`HS` and `IE` only as their sum, so predicted-minus-observed *is* the
aggregate correction that makes this one sight agree with the modeled
almanac altitude -- an identity pinned in self-test `[11]`. The identity
does not prove which part came from the instrument.

Three honesty notes, all printed by the tool where they apply:

- One shot folds **personal error, timing error, position error, and
  the model's own error** into the aggregate. Take several shots and
  compare, prefer altitudes above ~15 deg, and treat a stable
  repeated offset as IE -- a scattered one is technique.
- Below ~5 deg the standard refraction model is weather, not physics:
  the tool prints a warning and you should calibrate higher.
- An implied correction beyond +-10 deg gets a "wrong body, limb,
  time, or position?" warning -- that is not an index error.

Three refusals, all exit `1` (Hc and Zn still print; the prediction
does not). A body more than 2 deg below the geometric horizon is a
well-formed question with no sextant answer. Nearer the horizon the
tool decides from the predicted *reading*, not from Hc: refraction
plus an upper limb can hold a positive reading on a center that is
geometrically just below the horizon (the prediction prints --
`make test` pins a rising-Sun case at Hc = -9.9'), while a predicted
reading below the visible horizon refuses. And close to the zenith,
height of eye can push the required reading past the sextant's
90 deg: no reading produces that altitude, so the tool refuses
rather than print a nearest fit (self-test `[11]` pins both sides of
that boundary).

There is no new core math here: the mode is the public surface
composed backward (bisection over `astro_nav_sin_q30_from_marcmin()`
for the arcsine; fixed-point inversion of
`astro_nav_correct_altitude[_moon]_tp_marcmin()` for the chain), so
its determinism is inherited from functions every other gate already
pins.

## 5. End-to-end examples

Date for all examples: **2026-07-01 00:00:00 UT1** = J2000
+836136000000 ms. True position (which the observer doesn't know):
**45 N 0 E**. Index error +0.1', height of eye 2 m.

### 5.1 A single line of position (Arcturus)

Shoot Arcturus and read Hs = 31 deg 04.0' (1864038 marcmin -- tenth-of-
arcminute sextant, the trailing digits are the exact test value).

Correct it:

```
$ ./sight_reduction --correct 1864038 100 200 0 0 0
Ho:  31 deg 0.0'   = 1860000 milli-arcmin
```

Almanac at the instant of the shot:

```
$ ./sight_reduction --star 2 836136000000
GHA:       64.84 deg
dec:       19.06 deg
```

Reduce from the dead-reckoning position 45 N 1 W (deliberately ~42 nm
off the truth):

```
$ ./sight_reduction --reduce 4500 -100 6484 1906 1860000
Hc(A): 31.71 deg
Zn(A): 265.70 deg true
Intercept: 42.6 nm AWAY
```

Read: from the assumed position, Arcturus *should* be at 31.71 deg but
you measured 31.00 deg -- you are farther from the body than assumed.
Plot the line of position 42.6 nm from the AP, **away** from azimuth
265.7 deg (i.e., displaced toward 085.7 deg -- eastward, which is
exactly where the true position lies). One sight gives one line; you
are somewhere on it.

### 5.2 A two-star fix (Arcturus + Altair)

Same evening, second star. Altair: Hs = 50 deg 27.2' (3027212 marcmin).

```
$ ./sight_reduction --correct 3027212 100 200 0 0 0
Ho:  50 deg 24.0'   = 3024000 milli-arcmin

$ ./sight_reduction --star 9 836136000000
GHA:       341.05 deg
dec:       8.94 deg
```

The two azimuths (Arcturus ~266 deg, Altair ~150 deg) are ~116 deg
apart -- a strong cut. Now intersect the two circles directly, using a
dead-reckoning position 44 N 1 W (a full degree off in both axes):

```
$ ./sight_reduction --fix 4400 -100 6484 1906 1860000 34105 894 3024000
fix:       lat 45.00 deg   lon 0.00 deg
alternate: lat -12.75 deg   lon -14.40 deg   (other circle intersection)
```

The fix lands on the true position to the centidegree, and the
alternate intersection is 6000 km away in the South Atlantic -- the DR
only had to be closer to the right one, which even a day-old DR is.

Note what did *not* happen: no assumed position rounding, no plotting,
no iteration. The two circles of equal altitude intersect where they
intersect, computed once, in integers.

### 5.3 Averaging a run of shots first

In practice you'd take several shots of each star and average before
reducing -- one bad reading shouldn't move your fix. The
[`--average` example above](#45---average----several-shots-of-one-body)
shows the shape: five shots over four minutes, the outlier rejected,
and the fitted Ho at your chosen reference instant becomes the `HO`
argument to `--reduce` or `--fix`, with the shot *times* collapsed to
TREF_MS -- so use TREF_MS as the almanac instant too.

### 5.4 The Bris variant: a fixed angle and a clock

A [Bris sextant](https://en.wikipedia.org/wiki/Bris_sextant) has no
arc to read -- a stack of fused glass plates shows the body at a few
*fixed* angles, and the observation is the **time** at which the body
crosses one of them. A Bris fix is therefore an ordinary two-body fix
whose Ho arguments are both the same instrument constant; the only
measured quantities are two timestamps -- which is literally the
`--fix-stars` argument list. With a fixed angle of exactly
30 deg 00.0' (1800000 marcmin) and Deneb / Altair crossing it at
20:05:13.051 and 21:32:01.859 UT1 on 2026-06-30 as seen from the same
45 N 0 E truth position:

```
$ ./sight_reduction --fix-stars 4400 -100 15 836121913051 1800000 9 836127121859 1800000
fix:       lat 45.00 deg   lon 0.00 deg
```

Two timestamps and one calibrated constant recover the position to
the centidegree display, with machine-side crossing residuals of a few
Q2.30 units (~0.0001'; the fix and both residuals are value-pinned in
`make test` via `--reduce-star`). The full
walk-through -- how the crossing times were generated with this same
binary, the timing-error budget, and why the paradigm suits an
integer-deterministic library -- is in [`BRIS.md`](BRIS.md).

### 5.5 The Bris Sun day: one wedge, one clock, no stars

The realistic Bris regime is the Sun through shades, and one day at
anchor gives a fix with nothing but crossing times. 2026-07-14, truth
position 45 N 0 E, instrument angles 30 deg and 60 deg, TT - UT1 =
69200 ms. The Sun crosses 30 deg rising at 07:31:12.832 UT1 and 60 deg
before local noon at 10:34:08.811 UT1:

```
$ ./sight_reduction --fix-sun 4400 -100 69200 837286272832 1800000 837297248811 3600000
fix:       lat 45.00 deg   lon 0.00 deg
alternate: lat -7.36 deg   lon 15.09 deg   (other circle intersection)
```

Two crossing times of one body recover the position, and the
ambiguity resolves itself (the alternate is an ocean away). Both this
fix and its crossing residuals are pinned in `make test`; the full
day -- including why the *symmetric* 30 deg morning + 30 deg
afternoon pair pins longitude but barely constrains latitude -- is
worked in [`BRIS.md`](BRIS.md).

### 5.6 The camera alternative

No sextant, no horizon: photograph the zenith at a known UT1 instant,
plate-solve, and hand the resulting J2000 direction to
[`--zenith`](#46---zenith----camera-fix-from-a-plate-solved-photo).
Accuracy is set by how well the camera's vertical is known -- gravity
sensing to 1' gives a ~1 nm fix, which is the same error budget as one
good sextant sight, without needing a visible horizon.

## 6. Using the library directly

The CLI's operational navigation math is composed from the public
`astro_nav.h` + `astro_nav.c` core (plus `fp_math.h` inside that
translation unit). Parsing, reporting, validation gates, and the
CLI-only inverse search used by `--predict` stay in `sight_reduction.c`.
`make lib` packages the public core into `libastro_nav.a`, and a
consumer needs exactly two project artifacts: the archive and
`astro_nav.h` (which includes only
`<stdint.h>`; `fp_math.h` is a build-time detail of the library's own
translation unit, not part of its interface). No `-lm` and no setup
call:

```sh
make lib
cc -std=c99 -O2 -I. -o my_fix my_fix.c libastro_nav.a
```

The one dependency left is the toolchain's normal compiler runtime:
the build uses `__int128`, so `nm -u` on the archive shows the
128-bit division helpers (`__divti3`, `__udivti3`) plus whatever the
host compiler emits for stack protection and `memset`-family fills
(`memset_pattern16` on Mach-O; the exact set varies by platform). A
hosted link resolves all of these implicitly; bare-metal consumers
supply compiler-rt/libgcc (or equivalent) and those few libc-level
routines themselves. None of them is a floating-point routine --
that is the audited guarantee (`make check-lib`).

`examples/consumer.c` is a complete worked consumer -- the test
schedule's star-pair fix, from catalog vectors to a latitude and
longitude through the public entry points alone -- and `make
check-lib` builds it exactly that way, runs it, and then audits both
the archive and the consumer binary for floating-point symbols.

The CLI file `sight_reduction.c` is the reference for calling
conventions; the header documents every contract. The map:

| CLI mode | Library entry points |
| --- | --- |
| `--correct` | `astro_nav_correct_altitude_marcmin` / `_tp_` with the atmosphere pair (itemized: `astro_nav_dip_marcmin`, `astro_nav_refraction_marcmin` / `_tp_`, `astro_nav_parallax_marcmin`) |
| `--correct-sun` | `astro_nav_sun_distance`, then the `--correct` chain |
| `--correct-moon` | `astro_nav_moon_distance`, then `astro_nav_correct_altitude_moon_marcmin` / `_moon_tp_` (the `--correct` chain plus `astro_nav_moon_augmentation_marcmin`) |
| `--predict` | CLI-only inverse composition over `astro_nav_reduce_method_c`, the body/time entry points, `astro_nav_sin_q30_from_marcmin`, and the public correction chain; there is no public inverse API |
| `--average` | `astro_nav_average_sights` |
| `--time` | `astro_nav_civil_to_times` |
| `--star` | `astro_nav_stars[]`, `astro_nav_celestial_to_earthfixed`, `astro_nav_gha_aries_cdeg` |
| `--reduce` | `astro_nav_reduce_method_a` / `_b` / `_c`, `astro_nav_intercept_tenths_nm` |
| `--reduce-star` | `astro_nav_stars[]`, `astro_nav_celestial_to_earthfixed`, `astro_nav_reduce_method_c` |
| `--fix` | `astro_nav_fix_two_body` |
| `--fix-stars` | `astro_nav_stars[]`, `astro_nav_celestial_to_earthfixed`, `astro_nav_fix_two_body` |
| `--sun` | `astro_nav_sun_inertial`, `astro_nav_celestial_to_earthfixed`, `astro_nav_gha_aries_cdeg`, `astro_nav_sun_distance` |
| `--moon` | `astro_nav_moon_inertial`, `astro_nav_celestial_to_earthfixed`, `astro_nav_gha_aries_cdeg`, `astro_nav_moon_distance` |
| `--reduce-sun` | `astro_nav_sun_earthfixed`, `astro_nav_reduce_method_c` |
| `--reduce-moon` | `astro_nav_moon_earthfixed`, `astro_nav_reduce_method_c` |
| `--fix-sun` | `astro_nav_sun_earthfixed`, `astro_nav_fix_two_body` |
| `--fix-n` | `astro_nav_celestial_to_earthfixed` / `astro_nav_sun_earthfixed` / `astro_nav_moon_earthfixed` per sight, `astro_nav_fix_n_body` |
| `--running-fix` | the same, plus `astro_nav_advance_body_for_run` per sight |
| `--zenith` | `astro_nav_position_from_celestial_zenith` |

Conventions that hold everywhere: angles in centidegrees, altitudes in
milli-arcminutes, vectors in Q2.30. Every library function requires
in-domain inputs and does *not* re-validate (the domain contract at
the top of `astro_nav.h` states the ranges); where a result struct
carries a `valid` flag it reports geometric or numerical failure on
in-domain data, not input validation. Complete boundary validation
lives in the CLI. No allocation, no globals except the CORDIC tables filled in by
`fp_math_init()` (idempotent, called internally on first use, but not
itself thread-safe -- threaded code initializes once from a single
thread at startup: source-level consumers call `fp_math_init()`
directly, archive consumers make one angle-consuming call, e.g.
`astro_nav_sin_q30_from_cdeg(0)`, before going parallel; the
trig-free hot paths alone never fill the tables).

## 7. Model assumptions and current limitations

Honest error budget. The arithmetic inside is bit-exact; these are the
*model* simplifications between the arithmetic and the sky. For
context, classical navigation-table practice shares most of them, and
a hand-held sextant sight is often around +-0.5' (0.5 nm), with actual
quality depending on the instrument, observer, horizon, and conditions.

**Star almanac (~1-2' total through ~2030 -- measured, not
estimated).** Star positions use IAU 1976 precession from J2000
catalog vectors and the IERS 2003 Earth rotation angle. Not modeled:
nutation (up to ~0.3'), annual aberration (up to ~0.34'), proper
motion (grows with time from J2000; small for most navigational stars
on this timescale). These are omissions of the almanac model, not of
the arithmetic -- the vectors are exact once chosen. The combined
real-sky error is measured by
[`--ephemeris-check`](#48---ephemeris-check----the-almanac-against-an-independent-authority)
against independent Skyfield/DE421 apparent places and asserted in
`make test`: worst 1.17' through 2030 and 1.45' by 2036 (Arcturus,
the fastest proper motion in the catalog; Sirius and Procyon follow at
~1'), with the rest of the catalog at the ~0.4-0.5'
aberration-plus-nutation floor and a catalog mean of 0.34'.

**Refraction is Bennett's formula**, at the standard atmosphere
(10 C, 1010 mb) by default. The optional `TEMP_C PRESSURE_MB` pair on
`--correct`/`--correct-sun` (library:
`astro_nav_refraction_tp_marcmin`) rescales it by the standard density
factor `(P/1010) x (283/(273+T))` -- the Nautical Almanac's
additional-correction table, worth ~24% at -40 C. That linear factor
is itself good only to a few percent of R, and no formula sees
anomalous refraction: below ~5 deg altitude the error can exceed 1'
and grows rapidly toward the horizon. Standard practice applies:
avoid sights below ~10 deg when you can. Bennett's formula is a smooth
approximation to the printed Nautical Almanac refraction table, not a
copy of it; `--external-check` measures the gap between the two at the
standard atmosphere and finds it at most 0.10', largest at the horizon
(0 deg), where Bennett is known to diverge -- negligible against the
sub-horizon errors above, but now pinned rather than assumed.

**Dip assumes standard terrestrial refraction.** Anomalous dip (warm
sea, cold air and vice versa) can reach several arcminutes and no
formula sees it. This is a limitation of every dip table, not just
this one.

**Sun almanac (measured over 45 sampled epochs: worst 0.39', mean
0.17').** The built-in Sun
is the USNO low-precision formula (mean longitude + equation of
center; aberration in the constants, nutation omitted -- consistently
with the star pipeline), stated good to ~1' within two centuries of
J2000.
`--ephemeris-check` measures it against Skyfield/DE421 apparent
places across 2000-2035 and asserts a 0.60' gate. The same formula's
distance term supplies SD and HP (`--correct-sun`), gated against
DE421 distances at 320 micro-AU (measured worst: 78).

**Rotation time is UT1; dynamics time is TT.** The caller supplies
UT1 milliseconds everywhere, plus TT - UT1 for the Sun and the Moon
(the [time
contract](#49---sun---reduce-sun---fix-sun----the-sun-and-the-time-contract)).
The program never *decides* leap seconds or DUT1 -- [`--time`](#412---time----calendar-utc-to-the-librarys-timescales)
converts a calendar UTC timestamp, but the two policy numbers are
arguments. Using UTC uncorrected as UT1 (DUT1 = 0) costs up to 0.9 s
= up to 0.23' of longitude; the honest DUT1 is in IERS Bulletin A.
Passing TT - UT1 = 0 costs at most ~0.05' this era for the Sun, but
~0.6' for the Moon -- at ~0.55' of motion per minute the Moon is the
body the contract exists for.

**Earth model and latitude.** Intercepts and fixes are computed on a
unit sphere. The observer vector built from latitude/longitude is the
modeled local-zenith direction. On a physical ellipsoid, geodetic
latitude labels that surface-normal direction; the ellipsoid's
geocentric surface-radius direction can differ by about 11.5' near
45 deg latitude. Those are two different vectors, not an automatic
11.5' error in a celestial fix.

A corrected sextant altitude Ho is referenced to the sea horizon,
idealized here as perpendicular to the geodetic local zenith Z. After
parallax correction the body direction B is geocentric, so
`sin(Ho) = Z . B`. The spherical fix solves `O . B = sin(Ho)` and
therefore recovers `O = Z`; the polar angle of Z is geodetic latitude.
This is why the two alinnman cases in `--scenario-check` can take raw
sextant observations and land on their published geodetic coordinates
to 0.30' (Chicago) and 0.03' (off Tunis) without an 11.5' offset.

The recovery scenarios likewise form truth Ho from a geocentric
apparent body direction dotted with Z. They do not use Skyfield's
surface-observer topocentric altitude because that still includes
parallax already removed from the Ho consumed by the fix, especially
for the Moon. What remains unmodeled is the ellipsoid-dependent
observer radius in corrections such as parallax and dip, plus local
deflection of the astronomical vertical from the geodetic normal. The
two published fixes demonstrate consistency at their locations; they
do not establish a global sub-0.1' bound for those omitted terms. Treat
the output as a navigational fix, not a geodetic survey.

**Centidegree almanac quantization (0.6').** The CLI's `--reduce` and
`--fix` take GHA/dec in centidegrees, so the CLI chain carries up to
0.6' of body-position rounding. The library's Method C path takes
Q2.30 vectors directly and does not have this floor. Altitudes never
pass through centidegrees anywhere.

**Stars, the Sun, and the Moon -- no planets.** `--correct` already
handles any body's altitude corrections (HP, SD, limb), so if you
bring GHA/dec from a Nautical Almanac, planet sights work through
`--reduce`/`--fix` today; only the built-in almanac stops at the
Moon. For the Moon itself, remember the order: `--correct-moon`
first, then the geometry -- its 54-61' parallax is the one
correction nothing downstream can absorb.

**Averaging assumes altitude changes linearly** over the run of
shots. Near meridian transit the altitude curve is quadratic and a
line fit will bias the result: keep averaging runs short (a few
minutes) and away from transit, or reduce the shots individually.

**Not certified navigation equipment.** This is a mathematical
instrument with an unusually strong determinism guarantee, not a
type-approved navigation system. Carry the almanac and the tables.

## 8. Scope

The project's aim is stated in the [README](../README.md#scope):
demonstrate a celestial-navigation chain implemented in integer
arithmetic with bit-reproducible behavior across the tested target
matrix, including targets with no FPU. That is an implementation and
representation claim, not a claim that the physical models are exact.
Possible later planet ephemerides would extend coverage without
changing that foundation; the determinism gate makes any numerical
change visible as a deliberately reviewed new golden hash.
