# astro-nav-int - celestial sight reduction in integer math

Celestial navigation answers "where am I?" with a sextant, a clock, and
an almanac: measure how high a star, the Sun, or the Moon stands above
the horizon and record the time. One sight places you on a circle
of possible positions; two or more well-conditioned sights can produce
a fix. `astro-nav-int` is an experimental C99 library
that computes that whole chain -- the almanac, the sextant corrections,
the sight reduction, and the position fix -- in fixed-point integer
arithmetic, as though floating-point hardware were unavailable.

Given an assumed position, Greenwich hour angle, and declination, it computes:

- `Hc`: computed altitude
- `Zn`: true azimuth
- optional Marcq St. Hilaire intercept from an observed altitude `Ho`

Given two corrected sights, it also computes the position directly: an
exact two-body fix as the intersection of the two circles of equal
altitude, with no line-of-position plotting step. For sights taken while
under way it advances the earlier sight by the vessel's run (the classic
running fix, done as a rotation), and for three or more sights it solves
a weighted least-squares fix. Integer altitude corrections (dip, Bennett
refraction -- standard atmosphere by default, with an optional
temperature/pressure rescale -- parallax, semidiameter) turn the raw
sextant reading `Hs` into the observed altitude `Ho` the fix consumes -- see
[`docs/WORKFLOW.md`](docs/WORKFLOW.md) for the whole chain with concrete,
test-pinned numbers and [`docs/HOWTO.md`](docs/HOWTO.md) for the operating manual
(every CLI mode with worked examples, and the model's honest error
budget). For star sights the almanac itself is generated in-library: an
18-star catalog of Q2.30 unit vectors plus integer earth-rotation and
precession turns UT1 time directly into the body vector the fix
consumes.

The shipping library and CLI execute no floating-point operations and
link no `libm`. Runtime math is `int64_t` over Q16.48 fixed point. The vendored
[`fp_math.h`](https://github.com/nmicic/int-llm/blob/8b16a00bb9126d2a99db9ad7683c3c064b9eb7fc/fp_math.h)
is derived from that pinned public revision and extended here with the portable
wide-integer backend and its differential tests. Its 128-bit intermediates
use `__int128` where the compiler has one and a portable two-limb C99
backend where it does not; the two backends are gated bit-identical
(`make determinism-portable` and a differential op-level fuzz under
UBSan, `make check-backend`, on the host; `embedded/` across ISAs), so
the library also builds for strict-C99 32-bit targets.

## At a glance

**What it does.** Star, Sun, and Moon almanac positions from explicit
time inputs; the full sextant correction chain (index error, dip,
refraction, parallax, semidiameter); sight reduction by three
methods; and direct position fixes -- the exact two-body circle
intersection, the three-plus-body least-squares fix, and the running
fix under way -- plus sight averaging and a Bris-sextant
timed-crossing mode. The library is one source file and two headers
(`astro_nav.c`, `astro_nav.h`, vendored `fp_math.h`): C99, no
third-party dependencies, and no floating-point operations in the
shipping library or CLI. Hosted builds use libc and the compiler
runtime; `embedded/` documents the freestanding equivalents.

**Measured accuracy.** Altitude and Method A azimuth outputs land within
0.30' (= 0.30 nautical miles for altitude) of a double-precision
reference; Method B azimuth reaches 0.577'. The 0.30' altitude bound is
the output rounding step itself, not accumulated integer error. The
built-in almanac is deliberately budgeted at ~1-2'
through ~2030 (stellar proper motion, aberration, and nutation are
omitted, and `astro_nav.h` says so); every `make check` gates it
against an independent ephemeris (Skyfield/DE421), printed almanac
pages, USNO data, and independently developed implementations.
Numbers: [Current Validation Snapshot](#current-validation-snapshot).

**What it is not.** Not a navigation instrument (an experimental
research library -- carry the paper tables). No planet ephemerides,
star identification, camera capture, plate solving, sensor fusion, or
formal uncertainty propagation from sight to fix. Validation is
computational, including published observation logs; this release does
not claim an author-run sextant field trial or a physical-board run.

## Framing

The point of this repo is a counterfactual one: what would a small celestial
navigation computer look like in a computing branch where cheap chips never
standardized hardware floating point?

That makes the project useful as a compact FP-free math artifact, an embedded
determinism exercise, and a representation experiment. It is not a claim that
integer CORDIC should beat modern hardware floating point on phones or laptops.

## Methods

**Method A - fixed-point spherical trig**

Method A is the main path:

```text
sin Hc = sin(phi) sin(dec) + cos(phi) cos(dec) cos(LHA)
N      = cos(phi) sin(dec) - sin(phi) cos(dec) cos(LHA)
E      = -cos(dec) sin(LHA)
Zn     = atan2(E, N)
```

Forward trig is CORDIC rotation mode. Inverse trig is CORDIC vectoring mode:
one shift/add pass over the same angle table rotates the target vector onto
the x-axis and accumulates `atan2` directly, the way historical integer-first
navigation hardware did it. `asin(s)` is `atan2(s, sqrt(1 - s^2))`. External
angles are integer centidegrees.

**Method B - square-ray azimuth key**

Method B reuses Method A's altitude and computes Zn via the **square
key** -- a square-perimeter direction code (the L-infinity analogue of
arc length: 65536 units per lap of the unit square's perimeter) serving
as an integer azimuth key. The *square-ray* construction the method is
named for shoots the local `(north, east)` direction as a ray onto the
square's perimeter; where it lands is the key, and the raw 16-bit
`square_key` preserves circular order:

```text
north = 0, east = 16384, south = 32768, west = 49152
```

A 33-entry integer correction table converts the square-perimeter ratio back to
standard true azimuth in centidegrees (see `ALTERNATIVE_METHODS.md` for the
lineage of the square-perimeter idea).

**Method C - machine-native almanac**

Method C asks what the reduction looks like when the almanac itself is
machine-native: bodies published as Q2.30 earth-fixed unit vectors instead of
GHA/declination angles. The whole sight is then three dot/cross products plus
one ratio divide:

```text
sin Hc = O . B
e = Ox By - Oy Bx        (= cos phi * E)
n = Bz - Oz (O . B)      (= cos phi * N)
square_key from (n, e)   -- same key convention as Method B
```

No CORDIC, no correction table, no angle in any unit anywhere in the hot
path. Outputs are `sin Hc` in Q2.30 and the 16-bit square key; converting
either to centidegrees is a separate human-boundary call
(`astro_nav_hc_cdeg_from_sin_q30`, `astro_nav_zn_cdeg_from_square_key`).
One behavioral difference: at an exact pole a unit vector carries no
meridian, so Method C reports `Zn` undefined there while A/B, fed angles,
still resolve it.

**Two-body fix - exact circle intersection**

In the same vector representation, a corrected sight is exactly the
constraint `O . B = sin Ho`: a plane cutting the unit sphere in the
body's circle of equal altitude. Two sights give two planes, and with
`|O| = 1` the observer position is closed-form (`g = B1 . B2`):

```text
O   = a B1 + b B2 +- c (B1 x B2)
a   = (s1 - g s2) / (1 - g^2)
b   = (s2 - g s1) / (1 - g^2)
c^2 = (1 - a s1 - b s2) / (1 - g^2)
```

`astro_nav_fix_two_body()` returns both intersection points, using a
rough dead-reckoning direction only to label which is which. There is no
assumed position per sight, no intercept, and no plotted line: the
classic line of position is the hand-chart linearization of these same
circles, and in vector form the exact answer is simpler than the
approximation. Degenerate geometry (bodies aligned or antipodal,
non-intersecting circles) is reported as invalid rather than answered.

One navigation caveat the math cannot remove: both sights must describe
the **same observer position**. Two sights taken minutes apart on a
moving vessel are circles for two different points; advance the earlier
sight to a common time (the classic running-fix correction) before
intersecting. In this representation the advancement is itself exact:
the vessel's run along a great-circle track is a rotation `R` of the
sphere, and since `(R O) . (R B) = O . B`, advancing a sight just means
rotating its **body vector** by the run -- `sin Ho` is untouched.
`astro_nav_advance_body_for_run()` does exactly that (Rodrigues rotation
about the axis the DR position and true course define); the chart method
of "transferring the LOP by the run" is the flat-paper approximation of
this rotation. The usual running-fix caveat survives, as it must: the
track axis comes from the DR, so the advancement is as good as the DR
that oriented it.

**N-body fix - weighted least squares on the sphere**

With three or more sights the circles no longer meet in one point, and
the honest answer is the position that minimizes the misses.
`astro_nav_fix_n_body()` takes up to 32 sights (advanced to a common
time) and runs Gauss-Newton on the sphere's tangent plane over the same
`O . B_i = sin Ho_i` constraints -- a 2x2 integer solve per iteration.
The tested exact cases usually converge in 3-5 iterations from
DR-quality seeds; failure to converge is reported. Residuals are
weighted by `1/cos Hc` so every sight counts in angular miss
(arcminutes = nautical miles), and the worst single-sight miss is
reported in milli-arcminutes -- the navigator's scatter number.
Degenerate geometry (all azimuths within ~2 degrees of parallel) is
rejected rather than answered. Gauss-Newton is a local solver: from a
DR-quality seed, `n = 2` lands on the same intersection the closed-form
two-body fix would label as nearest -- but a seed closer to the other
intersection converges there instead, the same two-circle ambiguity
`astro_nav_fix_two_body()` exposes explicitly via its alternate point.

**Sight averaging - a run of shots into one Ho**

Navigators rarely trust one shot: the practice is several quick sights
of the same body, `Ho` plotted against time, a straight line fitted by
eye, and off-line shots discarded. `astro_nav_average_sights()` is that
practice as exact integer least squares -- 128-bit intermediates, one
rounding at the end -- with worst-first outlier rejection against a
caller-chosen threshold and the fitted `Ho` evaluated at a chosen
instant. It reports the fitted altitude rate (a sanity check against
the body's expected motion) and the worst surviving residual, the
run's scatter number. A short run is approximately linear (altitude
changes at most 15'/min); curvature matters most near meridian transit,
so runs there should be shortened or reduced sight by sight. Degenerate
runs are refused rather than answered.

**Vector ephemeris - generating the machine-native almanac**

Method C's premise -- an almanac that publishes unit vectors -- is
implemented for the stars, because for stars the almanac is one catalog
plus a clock: `astro_nav_stars[]` holds 18 navigational stars as Q2.30
J2000 equatorial unit vectors, and
`astro_nav_celestial_to_earthfixed()` rotates one into the earth-fixed
frame of any UT1 instant with three integer CORDIC rotations
(IAU 1976 precession folded with the IERS ERA->GMST earth rotation
angle). Time in, body vector out, no GHA or declination anywhere --
`astro_nav_gha_aries_cdeg()` exists only as a human-boundary
cross-check against a printed almanac's daily pages.

The primary design claim is the format and rotation machinery; the
ephemeris is deliberately low precision. Omitted, in order of size:
stellar proper motion (worst in the catalog is Arcturus, ~1.0' by 2026; most stars under
0.3'), annual aberration (<= 0.35'), and nutation (<= 0.3') -- so star
positions are good to roughly 1-2 arcminutes (1-2 nm) through ~2030.
That is comparable to, and must be included alongside, the uncertainty
of a practical sextant sight. The bound is *measured*, not estimated:
`sight_reduction --ephemeris-check` (part of `make test`) compares the
almanac against 270 committed truth vectors generated offline from an
independent authoritative model (Skyfield/DE421 apparent places:
Hipparcos proper motion, aberration, nutation -- see `tools/`), and
asserts the result over its 15 sampled epochs: worst 1.17' through
2030, worst 1.45' by 2036 (Arcturus, as predicted), catalog mean
0.34'. A second gate, `--external-check`, cross-checks the same almanac
against numbers read off *printed* pages -- daily-page Sun GHA/dec and
GHA Aries, the Nautical Almanac refraction and dip tables, and a
Bowditch worked example -- an authority independent of Skyfield as well
(worst 1.17' on stars, all families gated; see `docs/HOWTO.md`). A
third gate, `--cross-check`, holds the almanac *and* the reduction/fix
geometry against independent **computed** implementations, with
agreement reported per implementation lineage rather than per
repository (tools sharing an upstream count as one family of
evidence): PyEphem/libastro's analytic theories -- Sun and Moon code
*and* underlying data disjoint from both DE421 and the printed pages;
the star rows an independent runtime transformation of the shared
Hipparcos catalog (PyEphem's star table was itself prepared with
Skyfield, so star *data* independence is not claimed) -- confirm the
Sun at 0.40', the Moon at 0.16', the stars at 1.45' (Arcturus again,
as the other two lineages found), and an independent
circle-intersection engine, fed bit-identical quantized scenarios,
reproduces our Hc/Zn reductions and two-body fixes beneath the
library's own 0.30' centidegree readout step -- a genuinely different
fix algorithm agreeing with our iterated least-squares at the
resolution the library can express.

The Sun is built in too, and it brings the time contract with it:
earth rotation runs on UT1, solar-system dynamics run on TT, and the
caller supplies TT - UT1 as integer milliseconds. Civil-time POLICY
stays outside the math core, but the calendar arithmetic is provided
at the boundary: `--time` (`astro_nav_civil_to_times()`) turns a
calendar UTC timestamp plus caller-supplied DUT1 and leap-second
count into exactly the two time arguments every mode takes --
integer Fliegel-Van Flandern, verified day-by-day against an
independent calendar algorithm across 1899-2101. The model is the USNO low-precision
solar formula evaluated in Q16.48, emitted as an inertial J2000 vector
the existing rotation consumes like a nineteenth star; the same
`--ephemeris-check` gate measures it against DE421 at mean 0.17',
worst 0.39' across 45 sampled epochs spanning 2000-2035 (gated at
0.60'). The formula's distance term is computed too, so semidiameter
and horizontal parallax come from the ephemeris instead of the daily
pages (`--correct-sun`; distance gated against DE421 at 320 micro-AU,
measured worst 78, with explicit perihelion/aphelion rows).
`--fix-sun` is the timed
two-Sun-crossing fix -- one body, two instants.

The Moon is built in as well, and it is where the time contract stops
being a formality: at ~0.55' of motion per minute of time, dropping
the ~69 s of TT - UT1 mis-places the Moon by ~0.6' -- thirteen times
the Sun's stake, larger than the model's own error. The model is the
abridged ELP-2000/82 series from Meeus ch. 47 (60 + 60 periodic terms
plus additives, eccentricity damping, full fundamental-argument
polynomials) evaluated in Q16.48, emitted as an inertial J2000 vector
like the Sun's; a 37,048-instant sweep against Skyfield/DE421 apparent
places over the DE421-covered 1900-2053 part of the library's time
domain (reproducible with `tools/sweep_moon_vs_de421.py` against the
shipped binary) puts it at worst ~0.27', median ~0.10', earth-fixed.
`--ephemeris-check` gates 47
committed rows (with apogee/perigee, standstill, and node epochs) at
0.50' direction and 40 km distance, and `--external-check` holds it
against 242 printed almanac rows at 0.40'. The same series gives the
distance (worst 13 km against DE421), so `--correct-moon` computes
semidiameter and -- the number that decides whether a Moon sight works
at all -- horizontal parallax from the sight instant: 54-61', the
largest correction in celestial navigation, up to a degree of error
if skipped. The Moon correction chain also augments the semidiameter
(the observer stands up to an Earth radius closer to the Moon than the
geocenter the tables assume; up to ~0.3' at high altitudes), and
`--external-check` holds the augmented SD against the USNO celnav
service to 0.01'. `--moon`, `--reduce-moon`, and `moon:TT_MINUS_UT1_MS`
bodies in `--fix-n`/`--running-fix` complete the surface, so stars,
the Sun, and the Moon mix freely in one fix. The planets still need
an external almanac.

The same rotation, run backwards conceptually, is a whole other
instrument: a plate-solved star photo plus a gravity reference gives
the observer's zenith direction *among the stars*, and the earth-fixed
zenith is by definition the observer's position vector.
`astro_nav_position_from_celestial_zenith()` turns that zenith plus
UT1 into a full fix in one rotation -- no horizon, no circles of equal
altitude, no two-point ambiguity. This is the library-side entry point
for a fixed-camera (smartphone-on-tripod) navigation pipeline; the
imaging steps -- star detection, plate solving, gravity sensing --
happen upstream. The error budget includes the upstream zenith error
(1' = 1 nm) and this library's documented celestial-to-earth-fixed
frame approximation; it is not established by the idealized `--zenith`
self-test alone.

## Build / Run

```sh
make check       # self-tests, CLI checks, determinism, no-libm audit, UBSan
make test        # build, self-tests, CLI checks, no-libm symbol audit
make determinism # bit-exact golden-hash gate over a fixed input schedule
make determinism-portable # same golden hash on the two-limb no-__int128 backend
make check-backend # differential fuzz: native vs portable 128-bit backend, both under UBSan
make reference   # separate native-double/libm oracle; validates A/B
make benchmark   # native timing of A/B versus standard double/libm
make lib         # libastro_nav.a -- the public API as a linkable archive
make ubsan
make clean
```

Run without arguments, or with `--self-test`, for the validation suite.

To consume the library instead of the CLI, a program needs exactly two
project artifacts: `libastro_nav.a` and `astro_nav.h` (`fp_math.h` is
a build-time detail of the library, not part of its interface):

```sh
make lib
cc -std=c99 -O2 -I. -o my_fix my_fix.c libastro_nav.a
```

No `-lm` and no other *library* -- what remains is the toolchain's
normal compiler runtime: the build uses `__int128`, so `nm -u` on the
archive shows the 128-bit division helpers (`__divti3`, `__udivti3`)
plus host-compiler conveniences like stack-protector and
`memset`-family symbols (the exact set varies by platform). A hosted
link resolves all of these implicitly; bare-metal consumers supply
compiler-rt/libgcc (or equivalent) and those few libc-level routines
themselves. None of them is a floating-point routine -- that is the
audited guarantee. Building with `-DFP_MATH_FORCE_PORTABLE=1` (or with
any compiler that has no `__int128`) swaps in the two-limb backend and
the 128-bit helpers disappear entirely; `embedded/README.md` lists the
exact libgcc import set per 32-bit target.

`examples/consumer.c` is a complete worked consumer, and `make
check-lib` builds and runs it against the archive, then audits both
for floating-point symbols -- the no-float guarantee checked on the
artifact a consumer actually links.

For a sight, pass position and almanac angles in integer centidegrees
(`100 = 1 degree`); the optional observed altitude `HO`, like every
sextant altitude in the CLI, is milli-arcminutes (`1000 = 1'`), exactly
as `--correct` emits it:

```sh
./sight_reduction --reduce LAT LON GHA DEC [HO]
./sight_reduction --reduce 4000 -7400 6000 2000 4012800
```

For a two-body fix, pass a rough dead-reckoning position and two sights
(`GHA`, `DEC` in centidegrees; corrected altitude `HO` in
milli-arcminutes, exactly as `--correct` emits it) that describe the
same vessel position -- simultaneous, or already advanced to a common
time:

```sh
./sight_reduction --fix DRLAT DRLON GHA1 DEC1 HO1 GHA2 DEC2 HO2
./sight_reduction --fix 4400 100 0 0 2700000 6000 3000 2476800
```

To correct a raw sextant altitude into `Ho`, work in milli-arcminutes
(`1000 = 1'`; a typical 0.1' drum reading is 100 of these units) --
sextant altitude, index error, eye height in cm, horizontal parallax,
semidiameter, limb
(`1` lower / `-1` upper / `0` center or star):

```sh
./sight_reduction --correct HS IE EYE_CM HP SD LIMB
./sight_reduction --correct 1514300 100 200 150 16100 1
```

To average a run of shots of one body into one `Ho`, pass the instant
to evaluate at, an outlier threshold in milli-arcminutes (`0` = keep
everything), and 2-32 `HO T` pairs (times in ms, any epoch):

```sh
./sight_reduction --average TREF_MS REJECT HO1 T1 [HO2 T2 ...]
./sight_reduction --average 120000 1000 1500000 0 1503000 60000 \
    1511000 120000 1509000 180000 1512000 240000
```

For a star almanac entry, pass a catalog index (`0..17`) and UT1 as
milliseconds since the J2000.0 epoch (2000-01-01 12:00:00 UT1):

```sh
./sight_reduction --star INDEX UT1_MS
./sight_reduction --star 3 836136000000    # Vega, 2026-07-01 00:00 UT1
```

For a camera fix, pass the observer's zenith direction in the J2000
celestial frame (a Q2.30 unit vector, from a plate-solved star photo
plus a gravity reference) and the UT1 instant:

```sh
./sight_reduction --zenith X Y Z UT1_MS
./sight_reduction --zenith 134321514 -826150956 672572549 836136000000
```

(That example feeds Vega's own catalog vector in as the zenith, so the
answer is Vega's substellar point at that instant.)

To run the sight *backward* from a known (GPS) position -- where a
body stands (`Zn`) and what an idealized sextant should read under this
natural-sea-horizon correction model (`Hs` at `IE = 0`) -- give
`--predict` the position, a body in the n-sight
grammar, the instant, eye height, and limb; append your actual reading
and the difference comes back as the implied aggregate correction
(index error plus everything else in the shot), in the sign convention
`--correct` expects. A stable component across controlled repeated shots
can be treated as an estimate of index error. This is a known-position
aid to sextant calibration and index-error validation against the sky
and horizon
([HOWTO section 4.13](docs/HOWTO.md)):

```sh
./sight_reduction --predict LAT LON BODY UT1_MS EYE_CM LIMB [HS]
./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1
```

Latitude and declination are north-positive, longitude is east-positive, and
GHA is in `[0, 36000)`.

## Sextant Workflow

The intended use case is the classic small-boat workflow:

1. Take a sextant altitude `Hs` and UTC time.
2. Use a nautical almanac or precomputed table to obtain body `GHA` and
   `declination` for that time.
3. Correct `Hs` into observed altitude `Ho` using index error, height-of-eye
   dip, refraction, and body-specific corrections such as semidiameter and
   parallax.
4. Pick an assumed position and call `astro_nav_reduce_method_a()` or
   `astro_nav_reduce_method_b()`.
5. Convert `Ho - Hc` with `astro_nav_intercept_tenths_nm()` and plot the line
   of position perpendicular to `Zn`.
6. Combine two or more sights, or one sight plus dead reckoning, for a fix.

This repo implements steps 3, 4, and 5, and step 6 directly:
`astro_nav_correct_altitude_marcmin()` runs the step 3 correction chain
in integer milli-arcminutes (`astro_nav_average_sights()` then merges a
run of repeated shots into one Ho), `astro_nav_fix_two_body()` solves the
two-sight fix exactly as a circle intersection,
`astro_nav_advance_body_for_run()` advances sights taken under way to a
common time (the running fix, as a rotation), and
`astro_nav_fix_n_body()` least-squares fixes three or more sights -- so
no line of position is plotted at all. The CLI drives the whole chain
from timestamps: `--fix-n` is the overdetermined fix (stars, the Sun,
and the Moon mixed freely), and `--running-fix` advances every sight
along course and speed first -- the classic underway fix,
machine-native.
[`docs/WORKFLOW.md`](docs/WORKFLOW.md) walks the whole chain with real
numbers. Step 2 is covered for star sights by the vector ephemeris
(catalog + `astro_nav_celestial_to_earthfixed()`) and for Sun and Moon
sights by `astro_nav_sun_earthfixed()` / `astro_nav_moon_earthfixed()`;
planet almanac data still comes from outside, and a phone sensor
pipeline stays out of scope.

The workflow also runs *inverted*: a [Bris
sextant](https://en.wikipedia.org/wiki/Bris_sextant) has no arc to
read -- it shows the body at a few fixed angles and you record the
**time** of the crossing instead of an angle. That fix is two
timestamps plus one calibrated integer constant, and it works with
this binary today -- `--fix-stars` takes exactly that log (star index,
timestamp, constant, twice), and `--fix-sun` does the same with the
Sun alone, the Bris instrument's realistic daytime regime:
[`docs/BRIS.md`](docs/BRIS.md) runs both end to end (value-pinned in
`make test`).

## Scope

This repo currently implements the geometric sight-reduction kernel, the
two-body and n-body fixes, running-fix advancement, the sextant altitude
correction chain (dip, Bennett refraction with optional
temperature/pressure inputs, parallax, semidiameter),
sight averaging (line fit + outlier rejection over a run of shots),
intercept unit conversion, a star vector ephemeris (18-star Q2.30
catalog + integer precession/earth-rotation), an integer Sun ephemeris
(USNO low-precision formula, TT/UT1 time contract, distance-derived
semidiameter and horizontal parallax), an integer Moon ephemeris
(Meeus ch. 47 abridged ELP-2000/82, same time contract, distance-derived
semidiameter and the 54-61' horizontal parallax), and the
zenith-vector camera fix those ephemerides enable. It does not include
planet ephemerides, star identification, plate solving, camera
calibration, or sensor fusion.

External astro-navigation projects were used only as examples for common
operations and naming. They are not vendored, required, or part of the public
source tree.

## Current Validation Snapshot

`make check` validates 29 built-in truth/edge cases (all three methods), a
2,000-case integer differential sweep, a 500-fix two-body round-trip sweep
plus degenerate-pair rejection, running-fix and n-body cases with a
200-triple three-body sweep, altitude-correction truth rows against a
double oracle, ephemeris truth rows (GHA Aries anchors, all 18 substellar
points, Polaris precession) against a double oracle, 270 star, 45 Sun,
and 47 Moon truth rows generated offline from Skyfield/DE421 apparent
places and gated on real-sky error (`--ephemeris-check`: stars 2.0'
through 2030 / 2.6' across all sampled epochs, Sun 0.6' plus a 320
micro-AU distance gate with explicit perihelion/aphelion rows, Moon
0.5' plus 40 km distance and SD/HP gates with apogee/perigee,
standstill, and node rows), 1,250 published-source
truth rows read off printed almanac pages, NA/Bowditch tables, and the
USNO celnav service
(`--external-check`, an authority independent of Skyfield: Sun and GHA
Aries 0.7', stars 1.3', Moon GHA/dec 0.4' with HP/SD at 0.15'/0.2',
the augmented Moon SD against USNO celnav at 0.01',
Sun SD/refraction/dip/correction all sub-0.15'), 411 truth rows
computed offline from two further independent lineages
(`--cross-check`, agreement reported per implementation lineage, not
per repository: PyEphem/libastro analytic theories -- Sun 0.5', Moon
0.25', stars 1.6' (Sun/Moon data-disjoint; star rows shared-catalog,
transformation-independent), GHA Aries 0.5', with distance/SD/HP
gates -- and an
independent circle-intersection geometry engine whose Hc/Zn and
two-body fixes ours matches beneath the 0.30' centidegree readout
step),
an end-to-end position-recovery gate that checks the *composition* the
stage gates never see -- sky through corrected altitude and multi-body
fix to recovered position -- against external answers
(`--scenario-check`: 29 Skyfield recovery scenarios -- stars, Sun, and
Moon, including the classic daytime Sun-Moon cut both closed-form and
through the n-body solver -- landing within
0.85' of truth, two alinnman published fixes -- Chicago two-Sun + Vega
recovered to 0.30' of its 41 deg 51'N 87 deg 39'W answer, and the
off-Tunis Capella + Moon + Vega fix, a published RAW Moon sight whose
~61' near-perigee parallax our chain removes, to 0.03' of its 36 deg
45'N 10 deg 13'E answer -- and 10 rgleason intercept rows
for the Sun ground position, Hs->Ho, and Hc/Zn/intercept), a
bit-exact golden-hash determinism
gate (4,096 cases + degenerate edges, covering A, B, C, both fixes,
running-fix advancement, the correction chain at standard and
non-standard atmospheres (and the Moon chain with its augmented
semidiameter), sight averaging, the
vector ephemeris, the Sun and Moon ephemerides with distance/SD/HP
(the Moon under a nonzero TT - UT1, so the hash pins the
two-timescale plumbing), the zenith
fix, the civil-time adapter, and
every boundary converter), a 10,000-sight native-double reference run,
UBSan, and binary/instruction-level no-FPU audits. CI runs the same gate on x86-64
gcc/clang and arm64 clang, so the golden hash is checked across
architectures and compilers on every push.

On the current Apple M3 / Apple clang 17 `-O2` host:

| Check | Result |
|---|---:|
| Method A Hc vs native double | mean 0.149', max 0.300' |
| Method A Zn vs native double | mean 0.150', max 0.300' |
| Method B Hc vs native double | same as A |
| Method B Zn vs native double | mean 0.184', max 0.577' |
| Method A/B altitude differential | exactly 0.000' in sweep |
| Method A/B azimuth differential | mean 0.19 cdeg, max 1 cdeg |
| Method C vs A altitude (via boundary converter) | exactly 0 cdeg in sweep |
| Method C vs A azimuth (key -> cdeg round trip) | max 1 cdeg in sweep |
| Two-body fix position round trip (500-fix sweep) | 0 cdeg measured (gate: <= 2) |
| Three-body least-squares round trip (200-triple sweep) | 0 cdeg, 0 marcmin residual measured |
| Running fix (advance 30 nm, intersect with fresh sight) | recovers end position (gate: <= 2 cdeg) |
| Altitude corrections (dip/refraction/parallax/Hs->Ho) vs double oracle | within 0.03' |
| GHA Aries vs J2000 anchor and year-2000 almanac page | within 1 cdeg |
| 18 substellar points at 2026-07-01 vs double oracle of same model | within 2 cdeg |
| Time + catalog + two star sights -> fix, no external almanac | recovers position (gate: <= 2 cdeg) |
| Camera zenith fix (star-as-zenith -> its substellar point, all 18) | within 2 cdeg |
| Sight averaging (exact line recovered; +5' outlier rejected) | exact Ho/rate, 0 residual |

**What this means in nautical miles.** One arcminute of altitude error is
one nautical mile of intercept error, so both methods place the line of
position within **0.30 nm worst case (0.15 nm mean)** of the
double-precision result. That 0.30 nm is the centidegree output
quantization floor itself (1 cdeg = 0.6 arcmin, so rounding alone
contributes up to 0.3'), not accumulated math error. Azimuth error matters
far less: it rotates the LOP about the intercept point, displacing it by
roughly `intercept x sin(Zn error)` -- Method A's worst 0.300' (0.005 deg)
and Method B's worst 0.577' (0.0096 deg) each contribute **about 0.01 nm
or less** even on an unusually long 60 nm intercept. Both are an order of magnitude
below the 0.2'-2' uncertainty of the sextant observation itself.

Representative benchmark on the same host:

| Full Hc+Zn path | CPU time per sight |
|---|---:|
| Method A fixed/CORDIC | ~300 ns |
| Method B square-ray | ~260 ns |
| Method C machine-native (prebuilt vectors) | ~7 ns |
| Two-body fix, complete position (prebuilt vectors) | ~70 ns |
| Four-body least-squares fix, complete position | ~850 ns |
| Almanac entry, star + time -> earth-fixed vector | ~170 ns |
| Native double/libm | ~32 ns |

(Apple Silicon laptop, `-O2`. The A/B numbers wobble +-20% run to run
at this iteration count; the ratios are the signal, not the digits.)

Methods A and B are most defensible for deterministic embedded builds,
FPU-less chips, auditability, and representation experiments; they should not
be presented as faster than hardware floating point on phones or laptops.
Method C is the representation argument made concrete: once the almanac
stores unit vectors instead of angles, the integer-only sight is ~5x faster
than the native double/libm path on the same modern hardware -- the cost was
never "integer math", it was translating between human (angle) and machine
representations in the hot loop. The comparison is fair on inputs (the libm
path starts from angles because angles are what a paper almanac publishes)
but C's output is `sin Hc` + key, with angle conversion deferred to the
human boundary. The two-body fix row is a complete position from two
sights -- dot/cross products, three divides, one integer square root --
in roughly the time of two libm sights. The four-body row is a full
iterative least-squares solve (Gauss-Newton to sub-milli-arcminute
convergence) in under a microsecond. The almanac-entry row is the cost
Method C's "prebuilt vectors" amortize: three CORDIC rotations turn a
catalog star and a UT1 time into the earth-fixed vector, paid once per
body per instant, after which every sight against it is 6.4 ns.

## Embedded targets

The FP-free-chip framing has a reproducible cross-build and QEMU proof:
`embedded/` builds the full library freestanding (no libc, no newlib)
for Armv6-M, Cortex-M3, Cortex-M4-softfloat, RV32I, and RV32IM, runs
every image under QEMU, and asserts all five print output hashes
byte-identical to the host build -- three ISAs and both `fp_math.h`
backends agreeing bit for bit, with a symbol-table scan and a
disassembly audit showing no soft-float helper and zero
floating-point instructions in any image. Measured there (tables and
methodology in [`embedded/README.md`](embedded/README.md)): the whole
library is 53-107 KB of flash depending on ISA, up to 416 bytes of
static RAM, under 6 KB of measured stack. On a hardware-multiply/divide
core (M3, RV32IM) the profiled bundles cost: a Method C reduction with
its boundary readouts ~34 K executed instructions, a Sun almanac entry
computed both inertial and earth-fixed ~240 K, and a Moon entry with
its correction chain ~3.5 M -- and the linked image fits the
micro:bit nRF51822 memory map (256 KB flash, 16 KB RAM) with room to
spare. This is QEMU and toolchain evidence, not a physical-board test.
The RV32I-vs-RV32IM and Armv6-M-vs-M3/M4 columns put numbers on what
software multiply/divide costs this workload: 1.6-3.6x the instructions, depending on how
divide-heavy the feature is.

## Visual guide

The [visualization pages](https://nmicic.github.io/astro-nav-int/viz/)
explain the integer-only claim, compare traditional tables with floating
point and integer CORDIC, and walk through the sextant-to-fix chain.

## Files

- `astro_nav.h` - small public C API for integer-only sight reduction
- `astro_nav.c` - Method A/B/C, fixes, corrections, shared unit helpers
- `sight_reduction.c` - CLI, tests, native oracle, benchmark
- `fp_math.h` - Q16.48 math derived from the pinned `int-llm` revision above, with native `__int128` and portable two-limb backends
- `embedded/` - freestanding Cortex-M/RV32 builds, QEMU bit-exactness gate, and flash/RAM/stack/instruction measurements
- `viz/` - static visual explanations and worked integer-navigation examples; start with [`viz/index.html`](viz/index.html)
- `examples/consumer.c` - standalone consumer of `libastro_nav.a`: the only project inputs are `astro_nav.h` + the archive
- `docs/WORKFLOW.md` - the sextant-to-position chain, step by step
- `docs/HOWTO.md` - operating manual: build, CLI reference, worked examples, limitations
- `docs/BRIS.md` - Bris-sextant workflow: a position from a fixed angle and a clock
- `ALTERNATIVE_METHODS.md` - square-ray notes and scope boundary
- `THIRD_PARTY.md` - provenance and licensing of every external source: what was taken, from where, pinned by revision
- `CHANGELOG.md` - release history and the pre-1.0 stability policy
- `Makefile` - build, checks, determinism gate, native reference, benchmark
- `.github/workflows/ci.yml` - gcc/clang x x86-64/arm64 `make check` matrix

## Development note

AI-assisted tools were used during development. The maintainer remains
responsible for the design, implementation, review, testing, and release
decisions.

## License

Apache 2.0 (SPDX-License-Identifier: Apache-2.0). External data
sources and their terms are inventoried in `THIRD_PARTY.md`.
