# Square-Ray Alternative For Sight Reduction

## Method Lineage

The alternative path in this repo uses the square-perimeter idea from
[`p2-odd-rays-visual-math-toy`](https://github.com/nmicic/p2-odd-rays-visual-math-toy):
project a 2D direction onto the boundary of an L-infinity square, keep the
perimeter position as an integer key, and optionally convert it back to a
standard angle.

For this navigation library, the useful part is not prime-ray structure. It is
the integer direction parameter:

```text
local horizontal vector (north, east)
        -> L-infinity square perimeter
        -> 16-bit square_key
        -> small integer atan correction table
        -> conventional Zn in centidegrees
```

## Representation Lineage

`square_key` is a dyadic fraction of an L-infinity square perimeter. The
top 2 bits select the quadrant
(N/E/S/W edges), and every further bit halves the remaining compass arc -- a
binary tree of directions. Two properties fall out of that structure:

- **Exact prefix nesting.** Truncating `square_key` to its top `b` bits gives
  the enclosing direction cell at every resolution (4 bits = cells
  averaging 22.5 degrees, 8 bits = ~1.4 degrees; widths are uniform in
  perimeter, not in angle -- a 4-bit cell spans 26.6 degrees at an edge
  center and 18.4 degrees at a corner), with circular order preserved.
  Truncated decimal degrees do not nest this way. This is what makes the raw
  key useful for indexing, bucketing, and compact storage, independent of its
  size.
- **Direction without trigonometry.** A direction is a ray from the center to
  a dot on a reference shape. On the circle, the natural parameter (arc
  length) is transcendental in the coordinates -- that choice of reference
  shape is where trigonometry comes from. On the square, the natural
  parameter is a ratio of coordinates (`minor/major`) -- rational, and exactly
  representable in integers. In a computing tradition that had standardized
  the square instead of the circle, Method B is what direction-finding looks
  like natively, and the 33-entry table is a translation into circle
  convention for humans, not a correction of an error.

The idea has recognizable cousins. Computational geometry has long used
cheap non-circular direction codes for trig-free angle comparison and
sorting (the *pseudoangle*, the Fowler angle), and 16-bit integer angle
units are classic BAMS (binary angular measurement) from embedded and
avionics systems. The square key sits in that family -- perimeter-linear
where BAMS is arc-linear, and carried through as a first-class output
rather than an internal comparison trick.

The honest boundary, stated plainly: the square key is a complete direction
representation, but it is not an *angle* in any unit -- the perimeter
parameter is uniform in perimeter, not in arc. Converting to angle (degrees,
radians, or any circular measure) is irreducible and lives at the human edge
of the API; the machine never needs it. And the sky itself is a sphere -- altitude
and the navigational triangle remain circle/sphere-native, so the square
representation covers the direction output (Zn), not the whole reduction.
The forward `sin`/`cos` calls exist only because almanac inputs (GHA,
declination) are themselves circle-convention angles; Method C below is
what happens when the almanac publishes fixed-point unit vectors instead
and forward trig leaves the hot path too.

## Paths And Checks

| Path | Altitude Hc | Azimuth Zn | Primary approximation |
|---|---|---|---|
| A | Q16.48 spherical trig + CORDIC vectoring asin | Q16.48 local north/east + CORDIC vectoring atan2 | Centidegree output |
| B | Same altitude as A | L-infinity square key + 33-entry integer atan correction | Corrected square azimuth and raw 16-bit key |
| C | `sin Hc` in Q2.30 from a dot product (no angle output) | Raw 16-bit square key from cross products (no table) | Q2.30 unit-vector inputs; angle conversion only at the human boundary |

The raw square perimeter parameter is monotone in bearing, but it is not linear
in physical angle. Multiplying it directly by 360 degrees gives a maximum error
of about 4.075 degrees. Method B therefore keeps the raw 16-bit key for compact
machine use and uses a 66-byte correction table to return conventional azimuth.

## Method C: The Machine-Native Almanac

Methods A and B still accept angles (GHA, declination) because that is what
a paper almanac publishes. Method C removes that last human convention from
the input side: observer and body arrive as Q2.30 earth-fixed unit vectors,
and the reduction collapses to

```text
sin Hc = O . B                      (dot product)
e = Ox By - Oy Bx                   (= cos phi * E_classical)
n = Bz - Oz (O . B)                 (= cos phi * N_classical)
square_key from (n, e)              (same convention as Method B)
```

Both horizontal components carry the same `cos phi >= 0` factor, so in real
arithmetic the direction -- and therefore the key -- equals the classical
azimuth identically; with rounded Q2.30 inputs the agreement is within
quantization (measured: <= 1 cdeg versus Path A over the 2,000-case
test sweep). The hot path is nine `int64` multiplies and one ratio
divide: zero CORDIC
iterations, zero table lookups, and no angle in any unit. `sin Hc` itself is
the machine-native altitude, with one unit caveat: because sine is monotone
on [-90, 90] degrees, comparing against `sin Ho` in the sine domain gives
the correct intercept *sign* (toward/away) and ordering, but the intercept
*distance* is angular -- 1 arcminute of `Ho - Hc` is 1 nautical mile, and
`sin Ho - sin Hc` compresses by `cos Hc`. A distance in miles therefore
still needs the `asin` boundary call (or an explicit local `cos Hc`
rescaling); `asin` and the atan correction table survive only there, in
the converters a human calls to read the result.

Measured on the Apple M3 host, this is the representation argument made
concrete:
~6.4 ns/sight versus ~32.6 ns for the native double/libm path -- the
integer-only sight beats hardware floating point by ~5x once the almanac
stores what the machine actually needs. The A/B paths' CORDIC cost was
never "integer math being slow"; it was angle-to-vector translation sitting
in the hot loop.

Two boundary notes, stated plainly. First, the comparison is fair on inputs
but asymmetric on outputs: C returns `sin Hc` + key, and converting those to
centidegrees costs a boundary call the other paths have already paid.
Second, at an exact pole a unit vector carries no meridian, so C reports
`Zn` undefined there; A and B, fed angles, still resolve it. That is not a
bug in either convention -- the pole observer's "north" genuinely is the
selected meridian, information the vector never contained.

For star sights the machine-native almanac is no longer hypothetical:
`astro_nav_stars[]` ships 18 navigational stars as Q2.30 J2000 unit
vectors, and `astro_nav_celestial_to_earthfixed()` rotates one to any
UT1 instant with three integer CORDIC rotations (IAU 1976 precession
folded with the ERA/GMST earth-rotation angle) -- the almanac reduces
to a catalog plus a clock. The honest accuracy boundary (proper motion,
aberration, and nutation omitted; ~1-2 arcminutes through ~2030) is
stated at the API.

`make reference` validates operational-domain sights against a separate native
double/libm oracle. That binary is not part of the no-FPU claim.

## The Two-Body Fix Without Lines Of Position

The line of position is worth looking at through the same
representation lens. What a sight actually measures is membership in a
circle of equal altitude -- in vector form, exactly the constraint
`O . B = sin Ho`, a plane cutting the unit sphere. The Marcq St. Hilaire
method (assumed position, intercept, plotted LOP) is not the underlying
geometry; it is a linearization of that circle into a straight line,
invented so a navigator could intersect two of them with a pencil on a
flat chart. On paper, the approximation is the only tractable form. In
vectors, the exact form is simpler than the approximation:

```text
g   = B1 . B2
a   = (s1 - g s2) / (1 - g^2)
b   = (s2 - g s1) / (1 - g^2)
c^2 = (1 - a s1 - b s2) / (1 - g^2)
O   = a B1 + b B2 +- c (B1 x B2)
```

Two dot products, a 2x2 solve, one square root: the intersection of the
two full circles, with no assumed position per sight, no intercept, no
small-angle assumption, and no chart. The +- ambiguity is the honest
remnant of "two circles cross twice"; a rough dead-reckoning direction
picks the branch, and the API returns the other point too rather than
hiding it. In this repo the whole thing runs in Q16.48 through
`fp_mul`/`fp_div`/`fp_sqrt` -- zero CORDIC iterations, no angle in any
unit -- and the 500-fix round-trip sweep recovers the generating
position to 0 measured centidegrees (the test gate allows 2 for
cross-platform headroom).

Three boundaries, stated plainly. First, conditioning is geometric, not
arithmetic: position error scales as observation error divided by
sin(cut angle), the same rule the plotting method obeys. The code
rejects only hopeless geometry (bodies within ~1 degree of aligned or
antipodal, circles that do not intersect); judging the quality of a
20-degree cut remains the navigator's problem, exactly as on paper.
Second, both circles must belong to the same observer point: sights
taken minutes apart on a moving vessel must be advanced to a common
time before intersecting -- the running-fix correction. In vectors that
correction is itself exact: the run along a great-circle track is a
rotation `R` of the sphere, and `(R O) . (R B) = O . B` means advancing
a sight is just rotating its body vector by the run
(`astro_nav_advance_body_for_run`); the chart's "transfer the LOP" is
the flat-paper approximation of this rotation. What survives is the
honest part of the caveat -- the axis of `R` comes from the DR, so the
advancement is as good as the DR that oriented it.
Third, this replaces the *plotting*, not the *judgment*: with three or
more sights the circles no longer meet in a point and a least-squares
step is the right tool, which is also true of paper LOPs.
`astro_nav_fix_n_body` provides it -- Gauss-Newton on the sphere's
tangent plane over the same `O . B_i = sin Ho_i` constraints, residuals
weighted by `1/cos Hc` so each sight counts in angular miss, worst miss
reported in milli-arcminutes. Unlike Method C's azimuth, the fix has no
pole degeneracy -- a position vector at the pole is a perfectly good
answer; only its longitude label is undefined at the human boundary.

## Error In Nautical Miles

Altitude drives the fix: 1 arcminute of Hc error = 1 nautical mile of
intercept error. Methods A and B share the same altitude path, so both are
within 0.30 nm worst case (0.15 nm mean) of the double-precision result --
which is exactly the centidegree quantization floor (1 cdeg = 0.6', max
rounding 0.3'), not accumulated fixed-point error.

Azimuth error only rotates the line of position about the intercept point,
displacing it by about `intercept x sin(Zn error)`. Method B's larger worst
case (0.577' vs 0.300' for A, i.e. 0.0096 deg vs 0.005 deg) therefore costs
roughly 6 m vs 3 m of LOP displacement on a typical 20 nm intercept, and
under 0.02 nm even at 60 nm. At navigation accuracy the square-key detour is
free: the practical error budget is set by the sextant observation
(0.2'-2'), an order of magnitude above either method.

## Sextant Boundary

The main use case is a human sextant sight: corrected observed altitude `Ho`,
almanac `GHA`/declination, assumed position, computed altitude `Hc`, true
azimuth `Zn`, and a Marcq St. Hilaire intercept.

Method B is useful here as an alternative integer direction representation for
`Zn`. It is not trying to replace the sextant, infer the body from a phone
camera, or solve attitude with gyros. Those sensor paths may be interesting
experiments elsewhere, but they are a different project with different
calibration risks. (The library does expose the geometry-side endpoint
for one of them: `astro_nav_position_from_celestial_zenith()` turns an
externally plate-solved zenith direction plus time into a position --
but the detection, solving, and calibration stay outside.)

The next squarely relevant expansion would be integer helpers around ordinary
sextant work:

- altitude corrections -- implemented: dip, Bennett refraction,
  parallax, and semidiameter as deterministic integer evaluations in
  milli-arcminutes (no tables needed; see `docs/WORKFLOW.md`)
- sight averaging -- implemented: a run of shots of one body fitted by
  exact integer least squares with outlier rejection
  (`astro_nav_average_sights`)
- angle and time table interpolation
- local hour angle and assumed-position utilities
- fix arithmetic -- implemented: the exact two-body intersection,
  running-fix advancement as a rotation, and an n-body least-squares
  fix (see the two-body fix section above)
- compact storage formats for almanac/table data
- a machine-native almanac format (fixed-point unit vectors instead of
  GHA/declination angles) -- implemented as Method C, and for star
  sights the ephemeris itself is now generated in-library: an 18-star
  Q2.30 catalog plus integer precession/earth-rotation
  (`astro_nav_celestial_to_earthfixed`), time in, body vector out, with
  the accuracy budget stated in the header -- and integer Sun and Moon
  ephemerides now ride the same rotation on a two-timescale TT/UT1
  contract. Planet ephemerides remain outside

## Performance Interpretation

Method B removes the CORDIC vectoring atan2 pass used by Method A and replaces
it with one ratio division plus a small lookup/interpolation table. Since
Method A's inverse trig moved from a 50-step binary search to single-pass
CORDIC vectoring, the gap between the two has narrowed (roughly 295 vs 256
ns/sight on an Apple M3 host); Method B's remaining edge is one 48-iteration
vectoring pass traded for a divide plus a 66-byte table, which still matters
on FPU-less microcontrollers and for deterministic embedded builds. Method C
(~6.4 ns/sight from prebuilt vectors) removes the remaining CORDIC entirely
and is faster than the native double/libm path; its cost moved into the
almanac format and the optional human-boundary converters.

The broader frame is counterfactual: if general-purpose computing had stayed
integer-first and FP-free for much longer, this is the kind of representation
work a tiny navigation computer would need. Within that frame, square keys are
interesting because they keep direction ordering and indexing cheap.

On modern phones and laptops, hardware floating point is expected to be much
faster than this CORDIC-heavy implementation. For any real target, benchmark on
the target device and treat the integer path as a deterministic alternative,
not a general speed claim.
