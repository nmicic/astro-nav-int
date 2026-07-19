# From Sextant Reading To Position: The Complete Integer Chain

This page walks the classic small-boat workflow end to end through this
library, with concrete test-pinned numbers at every step. (For the
command-by-command operating manual -- build targets, every CLI mode,
units, limitations --
see [`HOWTO.md`](HOWTO.md); this page is the conceptual chain.) Everything below runs in
integer arithmetic -- no float, no libm -- and the units are chosen so
the integers mean something physical:

| Quantity | Unit | Why |
|---|---|---|
| Sextant altitudes, corrections | milli-arcminutes (1000 = 1') | the sextant reads to 0.1' = 100 units |
| Almanac angles, positions | centidegrees (100 = 1 degree) | almanac publication resolution |
| Runs and intercepts | tenths of a nautical mile | 1 nm = 1 arcminute of arc |
| Machine-side altitudes | sin(H) in Q2.30 | what the fix actually consumes |
| Machine-side positions | Q2.30 unit vectors | what the fix actually produces |

## Step 1 -- Take the sight

You measure the Sun's lower limb with a sextant: **Hs = 25 deg 14.3'**
at a known UTC. Your instrument has index error **+0.1'**, and your eye
is **2 m** above the water.

In milli-arcminutes: `Hs = 25*60000 + 14300 = 1514300`, `IE = +100`,
eye height `200` cm. The UTC timestamp itself converts at the
boundary: `--time Y M D h m s ms DUT1 LEAP` turns it into the
`UT1_MS` (and `TT_MINUS_UT1_MS`) every step below takes.

## Step 2 -- Almanac lookup

For the UTC of the sight, the almanac gives the Sun's **GHA** and
**declination**, plus **HP** (horizontal parallax, ~0.15' = 150 for the
Sun) and **SD** (semidiameter, ~16.1' = 16100). For the planets those
numbers come in from outside; for the Sun and the Moon the library
computes all of them itself (`--sun`/`--moon` for GHA/dec,
`--correct-sun`/`--correct-moon` for a correction chain with computed
SD/HP -- the Moon's HP is 54-61', the correction that decides the
sight; see
[HOWTO 4.9](HOWTO.md#49---sun---reduce-sun---fix-sun----the-sun-and-the-time-contract)
and
[HOWTO 4.10](HOWTO.md#410---moon---reduce-moon---correct-moon----the-moon-the-same-contract-with-teeth)),
so the external lookup below is the general, any-body path.

For a **star** sight this library generates the almanac entry itself:
the 18-star catalog stores Q2.30 J2000 unit vectors, and one call
rotates a star to the earth-fixed frame of the sight's UT1 time --
directly producing the body vector steps 5-7 consume:

```sh
./sight_reduction --star 3 836136000000     # Vega, 2026-07-01 00:00 UT1
```

```text
Vega at UT1 J2000 +836136000000 ms:
GHA Aries: 279.06 deg
GHA:       359.61 deg
dec:       38.81 deg
earth-fixed vector: (836698133, 5757400, 672922426)/2^30
```

Library calls: `astro_nav_celestial_to_earthfixed()` on
`astro_nav_stars[i].j2000` (and `astro_nav_gha_aries_cdeg()` to
cross-check against a printed almanac). Accuracy is ~1-2' through
~2030 -- proper motion, aberration, and nutation are deliberately
omitted; see the header for the budget. Stars have HP = SD = 0, so
step 3 needs only dip, refraction, and index error.

## Step 3 -- Correct Hs to Ho

```sh
./sight_reduction --correct 1514300 100 200 150 16100 1
```

```text
Hs:  25 deg 14.3'   (index +100, dip -2489, refraction -2102, parallax +136, SD +lower milli-arcmin)
Ho:  25 deg 26.0'   = 1526045 milli-arcmin   sin(Ho) = 461142501/2^30
```

The chain applied, in the classical order:

```text
Ha = Hs + IE - dip(eye)          dip = 1.76' sqrt(h m) = 176 sqrt(h cm) milli-arcmin
Ho = Ha - refraction(Ha)         Bennett: cot(Ha + 7.31/(Ha + 4.4)), standard atmosphere
        + HP cos(Ha)             parallax in altitude
        +- SD                    lower limb adds, upper subtracts, star/center none
```

Library calls: `astro_nav_correct_altitude_marcmin()` for the whole
chain, or `astro_nav_dip_marcmin()` / `astro_nav_refraction_marcmin()` /
`astro_nav_parallax_marcmin()` individually. The corrections are
deterministic integer evaluations of the stated standard formulas.
Bennett refraction assumes the standard atmosphere (10 C, 1010 mb), which is the usual
almanac-table assumption, and is the dominant approximation in the
chain. On a day that is nothing like 10 C / 1010 mb, append the
temperature and pressure (`--correct ... TEMP_C PRESSURE_MB`, library
`astro_nav_refraction_tp_marcmin()`) to rescale the refraction by the
standard density factor.

## Step 3b -- Several shots of one body: average the run

One shot is one sample of a shaky hand and a moving horizon. The
classic remedy is a quick run of sights of the same body, plotted
against time and fitted by eye; `astro_nav_average_sights()` is that
fit as exact integer least squares. Five shots a minute apart, one of
them misread by 5':

```sh
./sight_reduction --average 120000 1000 1500000 0 1503000 60000 \
    1511000 120000 1509000 180000 1512000 240000
```

```text
shots: 5 taken, 4 kept (1 rejected beyond 1000 milli-arcmin)
Ho at TREF:     25 deg 6.0'   = 1506000 milli-arcmin   sin(Ho) = 455480662/2^30
altitude rate:  3.000'/min
worst residual: 0.000'   (scatter of the kept shots)
```

The fitted Ho is evaluated at `TREF_MS` (here the middle of the run),
which is then the time to use for the almanac lookup in step 2. The
rate is a sanity check -- a body should move at up to 15'/min, and a
wildly different fitted rate means the run is bad, not the math. The
linear model is an approximation over a short run; curvature matters
most near meridian transit, where runs should be shortened or the
shots reduced individually.

## Step 4 -- One sight: reduce and plot (Methods A/B/C)

With one sight you get a line of position, not a fix. Pick an assumed
position, reduce, and plot the intercept from `Ho - Hc` -- the trailing
argument is step 3's `Ho`, in milli-arcminutes:

```sh
./sight_reduction --reduce 4000 -7400 6000 2000 4012800
```

Library calls: `astro_nav_reduce_method_a/b()` from centidegree angles,
or `astro_nav_reduce_method_c()` from unit vectors, then
`astro_nav_intercept_tenths_nm()`. This is the classical Marcq
St. Hilaire step, and it exists here for exactly that use.

## Step 5 -- Two sights: skip the plot, intersect the circles

Each corrected sight constrains you to a circle of equal altitude:
`O . B = sin Ho`. Two sights, two circles, and the intersection is
closed-form -- no assumed position, no intercept, no plotting:

```sh
./sight_reduction --fix 4400 100 0 0 2700000 6000 3000 2476800
```

```text
fix:       lat 45.00 deg   lon 0.00 deg
alternate: lat -15.78 deg   lon -42.71 deg   (other circle intersection)
```

The DR argument (here 44 N, 1 E -- a degree off, deliberately) only
picks which of the two mathematical intersections is your vessel; the
other is reported, not hidden.

Library calls: `astro_nav_sin_q30_from_marcmin(ho)` to convert step 3's
output at full sextant resolution, body vector from the almanac angles
via `astro_nav_unitvec_from_cdeg(dec, -gha)`, then
`astro_nav_fix_two_body()`, and `astro_nav_latlon_cdeg_from_unitvec()`
to read the answer.

## Step 6 -- Sights taken under way: advance by the run

Both circles must describe the same vessel position. If you steamed
between sights, advance the earlier sight to the later time first --
the classic running fix. In vectors the advancement is a rotation, and
it is exact on the great-circle track: rotate the earlier sight's
**body vector** by the run (`sin Ho` is untouched):

```c
astro_nav_advance_body_for_run(&body1, &dr_at_first_sight,
                               course_cdeg, run_tenths_nm, &body1_adv);
```

30 nm is `run_tenths_nm = 300`. A negative run retards a later sight
instead. The honest caveat survives from paper: the rotation axis comes
from your DR position, so the advancement is only as good as the DR
that oriented it -- same as transferring an LOP on a chart, minus the
flat-paper approximation.

## Step 7 -- Three or more sights: least squares

A round of three or more star sights over-determines the position; the
circles no longer meet in one point. `astro_nav_fix_n_body()` takes up
to 32 advanced sights and Gauss-Newton-solves for the position that
minimizes the weighted angular misses:

```c
astro_nav_unitvec_t bodies[3];      /* from almanac, advanced to one time */
int32_t sines[3];                   /* astro_nav_sin_q30_from_marcmin(ho) */
astro_nav_unitvec_t seed;           /* DR, or a two-body fix              */
astro_nav_fixn_result_t r;
astro_nav_fix_n_body(bodies, sines, 3, &seed, &r);
```

`r.max_residual_marcmin` is the worst single-sight miss in
milli-arcminutes -- thousandths of a nautical mile, the navigator's
scatter number. A big value means one observation disagrees with the
others: the same judgment call a spread of paper LOPs ("the cocked
hat") would show you, just quantified.

This whole chain is exercised on a published worked fix with raw sight
inputs: `make test`'s
`--scenario-check` runs the raw sextant altitudes from a worked
two-Sun-plus-Vega example
([`alinnman/celestial-navigation`](https://github.com/alinnman/celestial-navigation),
Chicago, 2024-05) through the correction chain and this n-body solve,
and lands within 0.30' of the published 41 deg 51'N 87 deg 39'W. See
HOWTO section 4.8 for the full scenario gate.

## The same chain, machine-native

Steps 4-7 never needed the angles: if the almanac published Q2.30 unit
vectors directly (Method C's premise), the human-unit conversions above
collapse to the input boundary of step 3 -- one `sin` evaluation of the
corrected altitude -- and everything after is dot products, cross
products, and one square root. For star sights the premise is now
implemented: step 2's catalog + rotation IS that almanac, so a round of
star sights runs time -> vectors -> corrected sines -> position with no
angle anywhere between the sextant drum and the final latitude/longitude
readout. That is the representation experiment this repo exists for;
see `ALTERNATIVE_METHODS.md`.

And the sextant itself is optional. A plate-solved star photo plus a
gravity reference gives the observer's zenith direction in the
celestial frame, and the earth-fixed zenith IS the position vector, so
`astro_nav_position_from_celestial_zenith()` is a complete fix in one
rotation -- no horizon, no circles, no ambiguity:

```sh
./sight_reduction --zenith X Y Z UT1_MS
```

Star detection, plate solving, and gravity sensing happen upstream of
this library. The error budget includes the recovered zenith direction
(1' = 1 nm) and the mean-equinox celestial-to-earth-fixed frame model;
the idealized `--zenith` self-test does not establish end-to-end camera
accuracy.

## What is deliberately missing

Planet ephemerides, star-almanac accuracy beyond ~1-2'
(proper motion, aberration, nutation), star detection / plate solving
/ camera calibration for the zenith fix, unusual
refraction conditions, and any production-navigation safety claim. This
is an experimental research library: the numbers above are validated against
double-precision oracles and locked by a bit-exact determinism gate,
but nobody should navigate a vessel with it.
