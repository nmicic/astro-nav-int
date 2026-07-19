# The Bris Workflow: A Fixed Angle and a Clock

A [Bris sextant](https://en.wikipedia.org/wiki/Bris_sextant) is a few
small glass plates fused together. The stack's reflections show a
celestial body doubled at a handful of **fixed angles** -- and that is
the entire instrument: no arc, no vernier, no moving parts. You cannot
*read* an angle from it. Instead you wait until the body's reflected
image touches the horizon -- the moment its altitude passes one of the
instrument's fixed angles -- and you write down the **time**. Sven
Yrvind navigated ocean crossings in micro-yachts this way.

That inversion is worth staring at:

> A conventional sextant measures an **angle** at a chosen time.
> A Bris sextant measures a **time** at a fixed angle.

It is a paradigm this library is unusually well suited to, because it
moves the entire measurement burden onto the one quantity machines
handle natively. The instrument reduces to a **calibrated integer
constant**: a fixed angle with no moving parts has no *reading* error,
and its own bias is constant, so one calibration against a known
position removes that one term -- the observation itself still owes
the correction chain and the crossing-detection uncertainty discussed
below. The observation reduces to **timestamping**. The almanac
here is already a pure function of time, and everything in between is
already deterministic integer computation behind a golden-hash gate.
Nothing in the chain below is new mathematics -- a Bris fix is an ordinary
two-body fix whose observed altitudes happen to both be the same
constant, and `--fix-stars` spells exactly that: a star index, a
timestamp and the constant, twice.

This page runs the paradigm through twice with the existing CLI:
first a star night -- two bodies through one fixed angle,
`--fix-stars` -- and then [the Sun day](#the-sun-day) the real
instrument lives on: one body through two fixed angles, `--fix-sun`,
on the built-in integer Sun ephemeris.

## The demo

- Instrument: one fixed angle, calibrated to **30 deg 00.0'** =
  `1800000` milli-arcminutes after all altitude corrections. (Chosen
  for the demo because sin 30 deg is exactly 1/2 -- the machine-side
  value is exactly `536870912/2^30`.)
- True position (which the navigator does not know): **45 N 0 E**.
- Night of 2026-06-30 (UT1). The navigator's log contains exactly two
  measurements, and both are *times*. (The instants were chosen for
  geometry, not observing realism -- the first falls in bright
  twilight at this latitude in June. A real Bris navigator shoots the
  Sun; that run is [below](#the-sun-day).)

| Event | UT1 clock | UT1 ms since J2000 |
| --- | --- | --- |
| Deneb rising through the fixed angle | 2026-06-30 20:05:13.051 | `836121913051` |
| Altair rising through the fixed angle | 2026-06-30 21:32:01.859 | `836127121859` |

(Millisecond timestamps are not observing realism -- no eye at a
horizon resolves that -- they are deliberate: the demo pins the
*arithmetic* at its own resolution, and the timing-error budget below
prices what real clocks and real eyes cost.)

One idealization runs through the whole page: Ho is taken to be
*exactly* the calibrated constant. A real crossing still owes the full
correction chain (refraction above all, plus dip and -- for the Sun --
semidiameter and parallax), and deciding the instant the reflected
image actually touches the horizon is an observation with its own
uncertainty. What the paradigm removes is the *arc-reading* error, not
observing itself.

### Step 1 -- almanac at each logged instant

```
$ ./sight_reduction --star 15 836121913051
Deneb ...
GHA:       269.62 deg
dec:       45.38 deg

$ ./sight_reduction --star 9 836127121859
Altair ...
GHA:       303.95 deg
dec:       8.94 deg
```

### Step 2 -- the fix

Two circles of equal altitude, both with the *same* observed altitude:
the instrument constant. `--fix-stars` takes the log directly -- star
index, timestamp, constant, twice -- and a dead reckoning of 44 N 1 W
(a degree off in both axes) picks the intersection:

```
$ ./sight_reduction --fix-stars 4400 -100 15 836121913051 1800000 9 836127121859 1800000
Deneb      at UT1 J2000 +836121913051 ms   Ho = 1800000 milli-arcmin
Altair     at UT1 J2000 +836127121859 ms   Ho = 1800000 milli-arcmin
fix:       lat 45.00 deg   lon 0.00 deg
alternate: lat -11.07 deg   lon 112.92 deg   (other circle intersection)
```

The true position, recovered to the centidegree display -- and how
close underneath the display can be checked, because `--reduce-star`
(the one-sight version of the same timestamp chain) prints the raw
machine altitude at each logged instant:

```
$ ./sight_reduction --reduce-star 4500 0 15 836121913051 1800000
Deneb      at UT1 J2000 +836121913051 ms
Hc(C): 30.00 deg   (machine sin_hc=536870930/2^30)
Zn(C): 54.21 deg true   square-key=10477/65536
sine residual: +18/2^30 (machine sin_hc - sin Ho)
Intercept: 0.0 nm TOWARD

$ ./sight_reduction --reduce-star 4500 0 9 836127121859 1800000
Altair     at UT1 J2000 +836127121859 ms
Hc(C): 30.00 deg   (machine sin_hc=536870899/2^30)
Zn(C): 108.88 deg true   square-key=19186/65536
sine residual: -13/2^30 (machine sin_hc - sin Ho)
Intercept: 0.0 nm TOWARD
```

Sine residuals of +18 and -13 Q2.30 units are about **0.00007'** of
altitude -- some 13 centimetres of line of position, five orders of
magnitude below anything physical in the problem. (The fix and both
residuals are value-pinned in `make test`.)

The same fix works through the paper chain: read the GHA/dec off the
Step 1 printouts and hand them to `--fix` --

```
$ ./sight_reduction --fix 4400 -100 26962 4538 1800000 30395 894 1800000
fix:       lat 45.00 deg   lon 0.00 deg
```

-- but this chain is coarser than it looks: `--star` prints GHA
rounded to 0.01 deg = 0.6', and here the rounding merely happens to
land on the same displayed centidegree. Log Altair's crossing 141 ms
later (`836127122000`) and the printed GHA ticks one step to 303.96;
the hand-worked fix moves to `lon -0.01` while `--fix-stars` still
prints `45.00  0.00`. That one-step wobble *is* the quantization made
visible -- `--fix-stars`/`--reduce-star` never leave the machine side,
so the body vectors keep full Q2.30 precision from timestamp to fix.
(Both hand-worked variants are pinned in `make test` too.)

The two azimuths (54 deg and 109 deg) are ~55 deg apart -- a healthy
cut. With a single fixed angle the azimuths on offer are whatever the
sky provides that night; a real Bris stack softens this by providing
several fixed angles from one piece of glass, and the classic Sun
routine combines a morning crossing with the noon transit.

## The timing-error budget

Since time is now the measurement, the error budget is the altitude
*rate* at each crossing. Five minutes after each event the computed
altitudes have risen to 30.72 deg (Deneb) and 30.83 deg (Altair):
rates of about **8.6'/min and 10.0'/min** -- roughly 0.15-0.17 arcmin
per second, and 1' of altitude is 1 nm of line of position.

Measured directly: re-run the fix with Altair's crossing logged a full
minute late (`836127121859` + 60000 ms):

```
$ ./sight_reduction --fix-stars 4400 -100 15 836121913051 1800000 9 836127181859 1800000
fix:       lat 45.17 deg   lon -0.17 deg
```

Sixty seconds of clock error moved the fix about 12 nm -- which is the
theory confirming itself: 60 s at 10'/min shifts Altair's line of
position by 10 nm, and the 55 deg cut amplifies the displacement by
1/sin 55 deg ~ 1.2. Scaling down:

| Timing error | Fix error (this geometry) |
| --- | --- |
| 60 s | ~12 nm |
| 1 s (wristwatch) | ~0.2 nm |
| 0.1 s (stopwatch reflex) | ~0.02 nm |
| 1 ms (MCU timestamp) | negligible |

A wristwatch already beats the +-0.5' a hand-held sextant is typically
read to. But note what electronic timestamping removes: the
*clock-reading* term only. Detecting the crossing -- the instant the
reflected image actually touches the horizon -- remains an observation
with its own uncertainty, and the fixed angle still owes the model
corrections (refraction above all; see the
[limitations](HOWTO.md#7-model-assumptions-and-current-limitations)).
What is left after a good clock is the glass's calibration constant,
the corrections, and the observer's eye.
The flip side: the clock's *epoch* now matters as much as its
resolution. The almanac wants UT1; carrying UTC uncorrected costs up
to 0.9 s = up to 0.23'.

## Reproducing the crossing times

The demo's timestamps were generated with the same binary, by scanning
time until the machine-side altitude equals the machine-side
instrument constant (sin 30 deg = `536870912/2^30`). `--reduce-star`
keeps the whole scan on the full-precision side -- no printed angles
re-enter the computation:

```sh
# closest approach of star IDX to 30 deg altitude, seen from 45N 0E
for (( t = T0; t <= T1; t += 2000 )); do
    s=$(./sight_reduction --reduce-star 4500 0 $IDX $t | \
        sed -n 's/.*sin_hc=\([0-9-]*\).*/\1/p')
    echo $(( s > 536870912 ? s - 536870912 : 536870912 - s )) $t
done | sort -n | head -1
```

A rising star's `sin_hc` is monotonic through the crossing, so the
coarse hit refines to the millisecond by bisection. Exact equality
still almost never lands -- at these rates the sine moves ~40 Q2.30
units per millisecond, so the closest 1 ms step sits within ~+-20
units of the constant. That is the +18 / -13 pinned above: the scan's
granularity, not the arithmetic's.

Pick your own position, angle, stars, and night, and the same scan
produces a fresh demo -- or, pointed at real logged times from a real
piece of glass, a real fix.

## The Sun day

Everything above used stars because two bodies through one fixed
angle is the simplest possible log -- but the instrument's home
ground is the Sun, and the built-in integer Sun ephemeris makes that
the same workflow with one new column. Sun modes take **TT - UT1**
explicitly, because solar-system dynamics run on TT while earth
rotation runs on UT1 (the time contract; see
[HOWTO 4.9](HOWTO.md#49---sun---reduce-sun---fix-sun----the-sun-and-the-time-contract)).
In 2026 it is about `69200` ms, and even passing 0 costs at most
~0.05' of Sun position.

A real Bris stack provides several fixed angles from one piece of
glass, so take two: **30 deg** (`1800000` milli-arcmin) and **60 deg**
(`3600000`), both again idealized as fully corrected values -- for
the Sun the correction chain also includes semidiameter and parallax,
both computable from the instant with `--correct-sun`
(see [HOWTO 4.1](HOWTO.md#41---correct----sextant-altitude-to-observed-altitude)).
Same true position, 45 N 0 E; the day is 2026-07-14 (UT1). The log is
again nothing but times:

| Event | UT1 clock | UT1 ms since J2000 |
| --- | --- | --- |
| Sun rising through 30 deg | 07:31:12.832 | `837286272832` |
| Sun rising through 60 deg | 10:34:08.811 | `837297248811` |

`--fix-sun` is the same two-circle fix with the same body at two
instants:

```
$ ./sight_reduction --fix-sun 4400 -100 69200 837286272832 1800000 837297248811 3600000
Sun        at UT1 J2000 +837286272832 ms   Ho = 1800000 milli-arcmin
Sun        at UT1 J2000 +837297248811 ms   Ho = 3600000 milli-arcmin
TT - UT1 = 69200 ms for both sights
fix:       lat 45.00 deg   lon 0.00 deg
alternate: lat -7.36 deg   lon 15.09 deg   (other circle intersection)
```

The alternate intersection is an ocean away, so an ordinary DR near
the expected operating area selects the intended branch unambiguously.
The two azimuths are 88.6 deg and 133.6 deg -- a
45 deg cut -- and the timing budget behaves like the star night's:
log the 60 deg crossing a full minute late and the fix moves to
`45.18 -0.01`, about 11 nm. (Fix and residuals are value-pinned in
`make test`.)

### The equal-altitude trap

One fixed angle also crosses *twice* in a day -- 30 deg rising at
07:31:12.832 and setting at 16:40:23.434 -- and that symmetric
morning/afternoon pair looks like a free fix from the simplest
possible instrument. It even prints one:

```
$ ./sight_reduction --fix-sun 4400 -100 69200 837286272832 1800000 837319223434 1800000
Sun        at UT1 J2000 +837286272832 ms   Ho = 1800000 milli-arcmin
Sun        at UT1 J2000 +837319223434 ms   Ho = 1800000 milli-arcmin
TT - UT1 = 69200 ms for both sights
fix:       lat 45.00 deg   lon 0.00 deg
alternate: lat 49.86 deg   lon -0.01 deg   (other circle intersection)
```

But look at the geometry the binary reports. The azimuths are
88.6 deg and 271.4 deg: the two lines of position both run nearly
north-south, almost parallel to each other. Longitude is pinned hard
-- it is set by the *midpoint* of the two times, which is apparent
noon measured without ever looking at noon -- while latitude comes
only from the sliver of non-parallelism, and the alternate
intersection sits just 4.9 deg away. Log the afternoon crossing 5 s
late and latitude drifts to 45.32 deg (19 nm); 30 s late and the two
circles no longer intersect at all -- `no fix`, exit status 1. This
is the machine rediscovering the classical **equal-altitude method**:
the symmetric pair is a superb longitude (and chronometer-checking)
observation and a poor fix, which is why the two-angle routine above
-- or the classic morning line + noon transit -- is the day's actual
workflow. And once the vessel moves between crossings, the same log
goes to `--running-fix`, which advances each sight along course and
speed to the last crossing's instant before solving (see
[HOWTO 4.10](HOWTO.md#410---fix-n---running-fix----more-than-two-sights-and-underway):
the underway Sun-day fix there is pinned in `make test` too).

The Sun crossing times were produced exactly like the stars': the
[same bisection scan](#reproducing-the-crossing-times) with
`--reduce-sun 4500 0 $t 69200` in place of `--reduce-star`, and the
pinned sine residuals (+12, -6, -8 Q2.30 units) are again the scan's
1 ms granularity, not the arithmetic's.

## Why this matters beyond the curiosity

Every observation style this library supports reduces to a timed
constraint on the observer: a classic sight (measured angle at a
time), a Bris crossing (fixed angle, measured time), a meridian
transit (fixed azimuth, measured time), a plate-solved zenith photo
(`--zenith`: measured direction at a time). The Bris case is the
purest of them: the instrument contributes one integer, the navigator
contributes timestamps, and the fix is

> **in this idealized workflow: fixed glass + a clock + this binary = a position.**

No moving parts, no arc to read, no floating point, and every step of
the computation bit-reproducible under `make determinism`.

See [`HOWTO.md`](HOWTO.md) for the general operating manual and
[`WORKFLOW.md`](WORKFLOW.md) for the conventional sextant chain this
plugs into.
