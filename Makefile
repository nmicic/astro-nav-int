# astro-nav-int -- integer-only celestial sight reduction
#
# No -lm anywhere: the whole point is zero libm / zero FPU dependence.

CC       ?= cc
AR       ?= ar
CPPFLAGS ?=
# WARNFLAGS is deliberately separate from CFLAGS: overriding optimization
# (`make CFLAGS=-O0`) must not silently drop the -Werror gate.
WARNFLAGS = -std=c99 -Wall -Wextra -Werror
CFLAGS   ?= -O2
SRCS      = sight_reduction.c astro_nav.c
HDRS      = astro_nav.h fp_math.h ephemeris_reference.h external_reference.h cross_reference.h sight_scenarios.h
NATIVE_REF = sight_reduction_native
LIB       = libastro_nav.a

all: sight_reduction

sight_reduction: $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -o $@ $(SRCS)

# The library artifact: the whole public API behind astro_nav.h in one
# archive. A consumer needs exactly two project artifacts -- the header
# and the archive (plus the toolchain's normal compiler runtime:
# __int128 helpers etc., resolved implicitly by any hosted link);
# fp_math.h is a build-time detail of the library's own translation
# unit, not part of its interface (astro_nav.h includes only
# <stdint.h>).
lib: $(LIB)

$(LIB): astro_nav.c astro_nav.h fp_math.h
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -c astro_nav.c -o astro_nav.o
	$(AR) rcs $@ astro_nav.o

test: sight_reduction
	./sight_reduction --self-test
	./sight_reduction --reduce 4000 -7400 6000 2000 4012800
	./sight_reduction --reduce 4500 0 0 4500
	./sight_reduction --fix 4400 100 0 0 2700000 6000 3000 2476800
	# Bris-style timed-crossing fix (docs/BRIS.md): both Ho arguments
	# are the same fixed instrument angle (30 deg); the only measured
	# inputs are the two crossing times. Pinned at three depths: the
	# fix display, the raw Q2.30 crossing residuals underneath it
	# (+18/-13 = the 1 ms scan granularity, ~0.00007'), and the
	# hand-worked centidegree chain -- which matches at these instants
	# but ticks to lon -0.01 when Altair's log is 141 ms later and the
	# printed GHA rounds one 0.6' step up (the almanac quantization
	# made visible; --fix-stars is unmoved by the same shift).
	./sight_reduction --fix-stars 4400 -100 15 836121913051 1800000 9 836127121859 1800000 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --reduce-star 4500 0 15 836121913051 1800000 | grep -q 'sine residual: +18/2^30'
	./sight_reduction --reduce-star 4500 0 9 836127121859 1800000 | grep -q 'sine residual: -13/2^30'
	./sight_reduction --fix 4400 -100 26962 4538 1800000 30395 894 1800000 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --fix 4400 -100 26962 4538 1800000 30396 894 1800000 | grep -q 'lat 45.00 deg   lon -0.01 deg'
	./sight_reduction --fix-stars 4400 -100 15 836121913051 1800000 9 836127122000 1800000 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --correct 1514300 100 200 150 16100 1
	./sight_reduction --average 120000 1000 1500000 0 1503000 60000 1511000 120000 1509000 180000 1512000 240000
	./sight_reduction --star 3 836136000000
	# Sun almanac entry pinned at the display boundary (2026-07-14
	# 00:00 UT1; agrees with Skyfield/DE421 to 0.14' at this instant,
	# and --ephemeris-check asserts the bound across the sampled epochs).
	./sight_reduction --sun 837259200000 69200 | grep -q 'dec:       21.70 deg'
	# Distance-derived SD/HP at the 2026 apsides (2026-01-03 and
	# 2026-07-06, both 12:00 UT1): the daily pages' 16.3' / 15.7'
	# extremes, computed instead of copied. --ephemeris-check gates
	# the distance model against DE421 including explicit apsis rows.
	./sight_reduction --sun 820713600000 69200 | grep -q "SD:        16.268' (16268 milli-arcmin)   HP: 0.149' (149 milli-arcmin)"
	./sight_reduction --sun 836611200000 69200 | grep -q "SD:        15.733' (15733 milli-arcmin)   HP: 0.144' (144 milli-arcmin)"
	# Moon almanac entries at the same display boundary and at the
	# Bris star-night instant (agrees with Skyfield/DE421 to 0.11'
	# and 0.10' at these instants -- the model's ~0.10' median;
	# --ephemeris-check asserts the bound across the sampled epochs).
	./sight_reduction --moon 837259200000 69200 | grep -q 'dec:       26.09 deg'
	./sight_reduction --moon 836121913051 69200 | grep -q 'GHA:       289.91 deg'
	./sight_reduction --moon 836121913051 69200 | grep -q 'dec:       -25.62 deg'
	# Distance-derived SD/HP at the 2026 extreme apsides (apogee
	# 2026-12-11 06:00 UT1, perigee 2026-12-24 08:00 UT1 -- 13 days
	# apart): HP sweeps its full 54-61' range, and unlike the Sun's
	# 0.15' this HP is the dominant input to the correction chain.
	./sight_reduction --moon 851374800000 69200 | grep -q "SD:        16.747' (16747 milli-arcmin)   HP: 61.482' (61482 milli-arcmin)"
	./sight_reduction --moon 850244400000 69200 | grep -q "SD:        14.696' (14696 milli-arcmin)   HP: 53.953' (53953 milli-arcmin)"
	# --correct-sun: the --correct chain with SD/HP from the instant.
	# Bris morning Sun, lower limb, 2 m eye height, index +0.1'.
	./sight_reduction --correct-sun 1810000 100 200 837286272832 69200 1 | grep -q 'Ho:  30 deg 21.8'
	# Optional atmosphere arguments: spelling out the standard
	# (10 C, 1010 mb) is byte-identical to omitting it -- the scale
	# factor is exactly 1 there, and no atmosphere line prints.
	test "$$(./sight_reduction --correct 1514300 100 200 150 16100 1 10 1010)" = "$$(./sight_reduction --correct 1514300 100 200 150 16100 1)"
	# A cold high-pressure winter morning (-40 C, 1030 mb) raises the
	# same sight's refraction 2102 -> 2604 milli-arcmin (factor ~1.239)
	# and drops Ho by 0.5'.
	./sight_reduction --correct 1514300 100 200 150 16100 1 -40 1030 | grep -q 'refraction -2604'
	./sight_reduction --correct 1514300 100 200 150 16100 1 -40 1030 | grep -q 'Ho:  25 deg 25.5'
	./sight_reduction --correct-sun 1810000 100 200 837286272832 69200 1 -40 1030 | grep -q 'Ho:  30 deg 21.4'
	# --correct-moon: the Moon chain with the Moon's SD/HP from the
	# instant. Evening Moon at 57 deg, lower limb, 2 m eye: parallax
	# +33489 milli-arcmin -- 33.5', some 50x the refraction here and
	# the correction that makes or breaks a Moon sight -- plus the
	# semidiameter augmentation (+250: the observer is nearly an
	# Earth radius closer to the Moon than the geocenter is).
	./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1 | grep -q 'parallax +33489'
	./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1 | grep -q 'SD +lower augmented +250'
	./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1 | grep -q 'Ho:  57 deg 27.3'
	test "$$(./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1 10 1010)" = "$$(./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1)"
	./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 1 -40 1030 | grep -q 'Ho:  57 deg 27.1'
	# --predict: the sight run backward from a known position -- Zn,
	# Hc at milli-arcminute resolution, and the reading modeled for a
	# zero-index-error instrument and natural sea horizon. The Bris fixture inverts
	# exactly: Hc lands on the 30 deg its HO was built from, and the
	# prediction adds Bennett refraction at 30 deg (1.7', eye 0).
	./sight_reduction --predict 4500 0 15 836121913051 0 0 | grep -q 'Hc:  30 deg 0.0'
	./sight_reduction --predict 4500 0 15 836121913051 0 0 | grep -q '= 1801715 milli-arcmin'
	# Sun lower limb from 40N 74W, 2 m eye: predicted Hs = Hc + dip
	# + refraction - parallax - SD. Feeding an observed reading 2.0'
	# above the prediction back through --correct-sun with the implied
	# IE (-2000) reproduces Hc = 4214247 exactly: the loop closes
	# bit-for-bit.
	./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 | grep -q '= 4214247 milli-arcmin'
	./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 | grep -q '= 4201316 milli-arcmin'
	./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 4203316 | grep -q 'implied aggregate correction: -2000 milli-arcmin'
	./sight_reduction --correct-sun 4203316 -2000 200 836367200000 69200 1 | grep -q '= 4214247 milli-arcmin'
	# Spelling out the standard atmosphere is byte-identical, with
	# and without an observed HS.
	test "$$(./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 10 1010)" = "$$(./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1)"
	test "$$(./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 4203316 10 1010)" = "$$(./sight_reduction --predict 4000 -7400 sun:69200 836367200000 200 1 4203316)"
	# Far below the horizon: well-formed input, no prediction, exit 1.
	./sight_reduction --predict 4000 -7400 sun:69200 836324000000 200 1 >/dev/null; test $$? -eq 1
	# Rising Sun, upper limb: the center is 9.9' BELOW the geometric
	# horizon, but refraction (28.1') plus semidiameter holds the limb
	# above the visible one, so the prediction exists (36.3') -- and
	# --correct-sun brings it back down to the negative Hc exactly.
	./sight_reduction --predict 4000 -7400 sun:69200 836343363553 200 -1 | grep -q 'predicted Hs (IE = 0, upper limb):  0 deg 36.3'
	./sight_reduction --predict 4000 -7400 sun:69200 836343363553 200 -1 | grep -q '= 36320 milli-arcmin'
	./sight_reduction --correct-sun 36320 0 200 836343363553 69200 -1 | grep -q '= -9881 milli-arcmin'
	# Ten minutes earlier the predicted reading itself is negative:
	# the limb is below the VISIBLE horizon, no prediction, exit 1.
	./sight_reduction --predict 4000 -7400 sun:69200 836342823553 200 -1 | grep -q 'upper limb below the visible horizon'
	./sight_reduction --predict 4000 -7400 sun:69200 836342823553 200 -1 >/dev/null; test $$? -eq 1
	# Fail closed at the zenith: Vega overhead with 12 m of eye needs
	# a reading past 90 deg -- refuse (exit 1), never a nearest fit.
	./sight_reduction --predict 3881 5925 3 836121913051 1200 0 | grep -q 'no prediction: no sextant reading within +-90 deg'
	./sight_reduction --predict 3881 5925 3 836121913051 1200 0 >/dev/null; test $$? -eq 1
	# --time: calendar UTC + (DUT1, TAI - UTC) to the two arguments
	# every other mode takes. DUT1 = -16 ms here is ILLUSTRATIVE,
	# chosen so 2026-07-01 reproduces the round 69200 ms used
	# throughout this file (the actual IERS UT1 - UTC for that date
	# is +15 ms, i.e. a true TT - UT1 of 69169 ms); the J2000 epoch
	# itself lands on UT1_MS 0.
	./sight_reduction --time 2026 7 1 0 0 0 0 -16 37 | grep -q 'UT1_MS:          836135999984'
	./sight_reduction --time 2026 7 1 0 0 0 0 -16 37 | grep -q 'TT_MINUS_UT1_MS: 69200'
	./sight_reduction --time 2000 1 1 12 0 0 0 0 32 | grep -q 'UT1_MS:          0'
	# A leap second: 23:59:60.500 and the next day's 00:00:00.500 are
	# CONSECUTIVE instants one SI second apart -- the +1 s step lives
	# in the policy numbers (across the 2016-12-31 insertion IERS
	# UT1 - UTC went -0.4087 s -> +0.5913 s, TAI - UTC 36 -> 37). Fed
	# each instant's own DUT1 they land 1000 ms apart in UT1, and
	# TT - UT1 is IDENTICAL across the leap (the two steps cancel):
	# the continuity that keeps the Sun's dynamics seamless.
	./sight_reduction --time 2016 12 31 23 59 60 500 -409 36 | grep -q 'UT1_MS:          536500800091'
	./sight_reduction --time 2017 1 1 0 0 0 500 591 37 | grep -q 'UT1_MS:          536500801091'
	./sight_reduction --time 2016 12 31 23 59 60 500 -409 36 | grep -q 'TT_MINUS_UT1_MS: 68593'
	./sight_reduction --time 2017 1 1 0 0 0 500 591 37 | grep -q 'TT_MINUS_UT1_MS: 68593'
	# SEC=60 anywhere but 23:59:60 on the last day of a month is not
	# a UTC timestamp: refused, exit 2 -- each condition alone and
	# both together.
	./sight_reduction --time 2026 7 14 12 34 60 0 0 37 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --time 2026 7 31 12 34 60 0 0 37 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --time 2026 7 14 23 59 60 0 0 37 >/dev/null 2>&1; test $$? -eq 2
	# The Bris Sun day (docs/BRIS.md): 30 deg morning crossing + 60 deg
	# midday crossing recovers the position; the symmetric 30/30
	# morning-afternoon pair pins longitude but cuts latitude at ~3 deg
	# (the classical equal-altitude longitude method, so the fix line
	# is the check that its latitude came from the DR-side circle cut).
	./sight_reduction --fix-sun 4400 -100 69200 837286272832 1800000 837297248811 3600000 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --fix-sun 4400 -100 69200 837286272832 1800000 837319223434 1800000 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --reduce-sun 4500 0 837286272832 69200 1800000 | grep -q 'sine residual: +12/2^30'
	./sight_reduction --reduce-sun 4500 0 837297248811 69200 3600000 | grep -q 'sine residual: -6/2^30'
	# n-body fix: the Bris star pair plus Vega, stationary at 45N 0E
	# (residual 0: three consistent circles, least squares lands on
	# the closed-form answer).
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 9 836127121859 1800000 3 836127121859 3701231 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	# Mixed bodies in one fix: Deneb and Vega plus the Sun at the
	# same star-night instant (Ho -816151: the Sun is 13.6 deg below
	# the horizon at 45N 0E then -- a geometry pin, not a sextant
	# scenario; the solver has no horizon). Residual 0.
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 sun:69200 836127121859 -816151 3 836127121859 3701231 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 sun:69200 836127121859 -816151 3 836127121859 3701231 | grep -q 'worst residual: 0 milli-arcmin'
	# All three body kinds in one fix: Deneb, the Sun, and the Moon
	# at the star-night instants (Moon Ho 392148 = its computed Hc at
	# 45N 0E, 6.5 deg up; geometry pins again -- the two-timescale
	# moon: plumbing moves this body ~0.6', so a TT/UT1 mix-up here
	# is a failed fix, not a rounding tick). Residual 0.
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 sun:69200 836127121859 -816151 moon:69200 836127121859 392148 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 sun:69200 836127121859 -816151 moon:69200 836127121859 392148 | grep -q 'worst residual: 0 milli-arcmin'
	# The one-sight probe under it: HO is the asin-rounded machine
	# sin_hc, so the pinned residual is the HO's half-milli-arcmin
	# quantization (-92/2^30 ~ 0.3 milli-arcmin), not model error.
	./sight_reduction --reduce-moon 4500 0 836127121859 69200 392148 | grep -q 'sine residual: -92/2^30'
	# Running fix, underway: due north at 12.0 kn, three Sun sights
	# from 44.5N -> 44.8N -> 45.0N (0E), each Ho computed at the
	# vessel's true position at that instant. Advancement recovers
	# the position AT THE LAST SIGHT (worst residual 3 milli-arcmin
	# = 0.003 nm); feeding the same log to --fix-n (premise: one
	# position) leaves a 2414 milli-arcmin scatter -- the residual is
	# the navigator's tell that the run was ignored.
	./sight_reduction --running-fix 4490 10 0 120 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --running-fix 4490 10 0 120 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969 | grep -q 'worst residual: 3 milli-arcmin'
	./sight_reduction --fix-n 4490 10 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969 | grep -q 'worst residual: 2414 milli-arcmin'
	# Same log listed newest-first: earlier "last sight" means the
	# others get negative runs (retarded), and the fix comes out at
	# the FIRST position on the track.
	./sight_reduction --running-fix 4450 10 0 120 sun:69200 837295272832 3323969 sun:69200 837291672832 2746731 sun:69200 837286272832 1799167 | grep -q 'lat 44.50 deg   lon 0.00 deg'
	# Oblique course -- the track is a GREAT CIRCLE whose bearing
	# at the DR is COURSE, not a rhumb line. Due east at 45N
	# the two differ by ~21 milli-arcmin of latitude over a 12 nm
	# run; these Ho values sit exactly on the great-circle track, so
	# residual 0 pins the model (a rhumb reimplementation fails it).
	./sight_reduction --running-fix 4500 0 9000 120 sun:69200 837286272832 1788004 sun:69200 837288072832 2111876 sun:69200 837289872832 2433541 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --running-fix 4500 0 9000 120 sun:69200 837286272832 1788004 sun:69200 837288072832 2111876 sun:69200 837289872832 2433541 | grep -q 'worst residual: 0 milli-arcmin'
	# Running fix on the Moon: the due-north 12 kn track again, three
	# Moon sights over 150 minutes. The Moon itself moves ~33'/hour
	# against the stars while the vessel makes 30 nm; both motions
	# live in the timed almanac, so advancement still recovers the
	# last-sight position.
	./sight_reduction --running-fix 4490 10 0 120 moon:69200 837286272832 1965705 moon:69200 837291672832 2871534 moon:69200 837295272832 3435965 | grep -q 'lat 45.00 deg   lon 0.00 deg'
	./sight_reduction --running-fix 4490 10 0 120 moon:69200 837286272832 1965705 moon:69200 837291672832 2871534 moon:69200 837295272832 3435965 | grep -q 'worst residual: 2 milli-arcmin'
	./sight_reduction --zenith 134321514 -826150956 672572549 836136000000
	# almanac vs INDEPENDENT truth rows (Skyfield/DE421, committed in
	# ephemeris_reference.h): asserts the documented real-sky bound.
	./sight_reduction --ephemeris-check
	# almanac + corrections vs PUBLISHED sources (printed daily pages,
	# NA refraction/dip tables, Bowditch worked example, committed in
	# external_reference.h): a second authority independent of Skyfield.
	./sight_reduction --external-check
	# almanac + geometry vs independent COMPUTED implementations
	# (PyEphem/libastro's analytic theories; a circle-intersection fix
	# engine, committed in cross_reference.h): a third and fourth
	# evidence lineage, reported per lineage rather than per repo.
	./sight_reduction --cross-check
	# the COMPOSITION -- sky -> corrected altitude -> multi-body fix ->
	# position -- vs external answers (Skyfield recovery scenarios, the
	# alinnman published Chicago fix, rgleason intercept rows, committed
	# in sight_scenarios.h): what the per-stage gates above never see.
	./sight_reduction --scenario-check
	# Refusals assert the exact exit code, not just nonzero:
	# 1 = well-formed input, no answer; 2 = malformed/out-of-domain.
	./sight_reduction --reduce 9001 0 0 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix 4400 100 0 0 2700000 0 0 2700000 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction --correct 5400001 0 200 150 16100 1 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct-sun 1810000 100 200 837286272832 600001 1 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct-sun 1810000 100 200 837286272832 69200 2 >/dev/null 2>&1; test $$? -eq 2
	# Atmosphere arguments: out-of-range temperature and pressure, and
	# a temperature with no pressure (both or neither).
	./sight_reduction --correct 1514300 100 200 150 16100 1 -61 1030 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct 1514300 100 200 150 16100 1 10 1101 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct 1514300 100 200 150 16100 1 -40 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct-sun 1810000 100 200 837286272832 69200 1 10 799 >/dev/null 2>&1; test $$? -eq 2
	# --time: impossible dates (2026 and 2100 are not leap years,
	# 2024 is), out-of-range DUT1, an instant past +-100 years.
	./sight_reduction --time 2026 2 29 0 0 0 0 0 37 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --time 2100 2 29 0 0 0 0 0 37 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --time 2024 2 29 0 0 0 0 0 37 >/dev/null
	./sight_reduction --time 2026 7 1 0 0 0 0 901 37 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --time 2100 12 31 0 0 0 0 0 37 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --average 0 0 1500000 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --average 1099511627776 0 -5400000 0 5400000 1 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction --star 18 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-stars 4400 -100 18 0 1800000 9 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-stars 4400 -100 15 3155760000001 1800000 9 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-stars 4400 -100 15 0 1800000 15 0 1800000 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction --reduce-star 4500 0 18 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --reduce-star 9001 0 15 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --sun 3155760000001 69200 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --sun 0 600001 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --reduce-sun 9001 0 0 69200 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --moon 3155760000001 69200 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --moon 0 600001 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --reduce-moon 9001 0 0 69200 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct-moon 3400000 100 200 837295272832 600001 1 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --correct-moon 3400000 100 200 837295272832 69200 2 >/dev/null 2>&1; test $$? -eq 2
	# --predict refusals: a star has no limb, star index bound, body
	# TT - UT1 bound, LAT bound, UT1 bound, HS bound, atmosphere bound.
	./sight_reduction --predict 4500 0 15 836121913051 0 1 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --predict 4500 0 18 836121913051 0 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --predict 4500 0 sun:600001 836121913051 0 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --predict 9001 0 sun:69200 836121913051 0 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --predict 4500 0 sun:69200 3155760000001 0 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --predict 4500 0 sun:69200 836121913051 0 0 5400001 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --predict 4500 0 sun:69200 836121913051 0 0 10 799 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-n 4400 -100 moon:600001 0 1800000 moon:69200 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-n 4400 -100 moon:x 0 1800000 moon:69200 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-sun 4400 -100 -600001 0 1800000 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-sun 4400 -100 69200 837286272832 1800000 837286272832 1800000 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-n 4400 -100 18 0 1800000 9 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-n 4400 -100 sun:600001 0 1800000 sun:69200 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --fix-n 4400 -100 15 836121913051 1800000 15 836121913051 1800000 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction --running-fix 4490 10 36000 120 sun:69200 0 1800000 sun:69200 3600000 1800000 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --running-fix 4490 10 0 1001 sun:69200 0 1800000 sun:69200 3600000 1800000 >/dev/null 2>&1; test $$? -eq 2
	# Polar cap: a course has no direction at the pole, and the
	# advancement rotation degenerates within ~0.9' of it -- the CLI
	# refuses |DRLAT| > 8998 instead of printing runs it never applied.
	./sight_reduction --running-fix 8999 0 0 120 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --running-fix -8999 0 0 120 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --zenith 1 2 3 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction --zenith 2000000000 2000000000 2000000000 0 >/dev/null 2>&1; test $$? -eq 2
	$(MAKE) check-libm

$(NATIVE_REF): $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -DASTRO_NAV_NATIVE_REFERENCE \
	    -o $@ $(SRCS) -lm

reference: $(NATIVE_REF)
	./$(NATIVE_REF) --native-reference

# determinism: bit-exactness gate. Hashes every output bit of a fixed
# input schedule (4096 LCG cases + degenerate edges) and compares against
# the golden constant committed in sight_reduction.c. Any cross-compiler
# or cross-architecture divergence is a hard failure, which is what the
# tolerance-based sweeps above cannot see.
determinism: sight_reduction
	./sight_reduction --golden

# determinism-portable: the same golden gate on fp_math.h's two-limb
# software backend (the one compilers without __int128 get, and the one
# every 32-bit target in embedded/ runs). Native and portable must
# produce the same hash bit for bit; this leg proves it on every host
# and in CI, where the embedded cross toolchains aren't available.
sight_reduction_portable: $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -DFP_MATH_FORCE_PORTABLE=1 \
	    -o $@ $(SRCS)

determinism-portable: sight_reduction_portable
	./sight_reduction_portable --golden

benchmark: $(NATIVE_REF)
	./$(NATIVE_REF) --benchmark

# check-libm: fail if the binary imports ANY floating-point routine --
# full libm surface (with optional f/l suffix: sinf, sqrtl, ...) plus the
# soft-float compiler-rt helpers a stray float/double pulls in on targets
# without hardware FP (__adddf3, __aeabi_dadd, ...). Denylist rather than
# allowlist: a "known-good" allowlist would have to enumerate every libc/
# stack-protector/errno symbol spelling, which drifts across SDK/glibc
# versions and architectures -- brittle in exactly the way that causes a
# clean rebuild to fail for no reason. The denylist only has to name a
# closed, well-known set (libm + compiler-rt float helpers) that never
# legitimately appears in an int-only build, so it doesn't need updating
# when the platform's libc does.
#
# Matches only UNDEFINED (imported, " U " in `nm`) symbols, so it can't
# false-positive on this file's own locals/statics. `_?` absorbs the one
# leading underscore Mach-O prepends to C symbols (a no-op on ELF, which
# doesn't prepend one). The soft-float builtin names already carry their
# own literal `__` from gcc/clang compiler-rt, so on Mach-O `_?` strips
# only the outer, third underscore (e.g. `___divti3` in `nm` output is
# the int128-division helper `__divti3` -- legitimate, and NOT matched
# below since "ti" (128-bit int) is not one of the sf/df/tf/hf float
# codes).
#
# The denylist lives in one variable because three artifacts get the
# same audit: the CLI binary (check-libm), and the archive plus the
# standalone consumer binary (check-lib).
FLOAT_SYM_DENY = \
    -e ' U _?(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh)[fl]?$$' \
    -e ' U _?(exp|exp2|expm1|log|log2|log10|log1p|logb|ilogb|pow|sqrt|cbrt|hypot)[fl]?$$' \
    -e ' U _?(fabs|floor|ceil|round|lround|llround|trunc|rint|lrint|llrint|nearbyint)[fl]?$$' \
    -e ' U _?(fmod|remainder|remquo|drem|ldexp|frexp|scalbn|scalbln|modf)[fl]?$$' \
    -e ' U _?(fma|copysign|nextafter|nexttoward|erf|erfc|tgamma|lgamma|gamma)[fl]?$$' \
    -e ' U _?(fdim|fmax|fmin|significand|j0|j1|jn|y0|y1|yn)[fl]?$$' \
    -e ' U _?(__add|__sub|__mul|__div)(sf|df|tf|hf)3$$' \
    -e ' U _?(__eq|__ne|__lt|__le|__gt|__ge|__unord|__cmp|__neg|__powi)(sf|df|tf|hf)2$$' \
    -e ' U _?__float(un)?(si|di|ti)(sf|df|tf|hf)$$' \
    -e ' U _?__fix(uns)?(sf|df|tf|hf)(si|di|ti)$$' \
    -e ' U _?__(extend|trunc)[a-z][a-z][a-z]*2$$' \
    -e ' U _?__aeabi_[df][A-Za-z0-9]*$$'

check-libm: sight_reduction
	@! nm sight_reduction | grep -E $(FLOAT_SYM_DENY) \
	    || { echo "FAIL: floating-point / soft-float symbol found" >&2; exit 1; }
	@echo "no libm/soft-float symbols: PASS"

# check-lib: the library artifact, consumed as shipped. Builds the
# archive and a standalone smoke test (examples/consumer.c) whose only
# project inputs are astro_nav.h and the archive -- the proof that the
# public API is self-contained. The consumer reruns the
# pinned Bris star-pair fix through the public entry points and exits
# nonzero unless the position comes out exactly 45.00 N 0.00 E; then
# the check-libm denylist runs against both the archive and the
# consumer binary, so the no-float guarantee is audited on the exact
# artifact a consumer links, not just on the CLI. (The
# instruction-level no-FPU audit of the library's translation unit is
# check-symbols', which already compiles astro_nav.c standalone.)
check-lib: $(LIB) examples/consumer.c
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -I. -o consumer_smoke examples/consumer.c $(LIB)
	./consumer_smoke
	@! nm $(LIB) | grep -E $(FLOAT_SYM_DENY) \
	    || { echo "FAIL: floating-point / soft-float symbol in $(LIB)" >&2; exit 1; }
	@! nm consumer_smoke | grep -E $(FLOAT_SYM_DENY) \
	    || { echo "FAIL: floating-point / soft-float symbol in consumer_smoke" >&2; exit 1; }
	@echo "check-lib: PASS ($(LIB) + standalone consumer, float-free)"

# check-symbols: INSTRUCTION-level float audit.  check-libm scans the linked binary's imported
# symbols; but on a hardware-FP host a stray `double` intermediate uses FP
# registers WITHOUT importing any libm symbol, so nm alone can't see it.
# Compiling every TU with -mgeneral-regs-only (where supported) makes ANY
# FPU register use a hard compile error -- the real teeth for the fixed-point
# CORDIC paths.  Then nm-scan the objects as a portable backstop.  -O0 avoids
# integer-NEON auto-vectorization false positives.
check-symbols: $(SRCS) $(HDRS)
	@nofpu=`$(CC) -mgeneral-regs-only -x c -c /dev/null -o /dev/null 2>/dev/null && echo -mgeneral-regs-only`; \
	if [ -n "$$nofpu" ]; then echo "check-symbols: no-FPU enforced at compile time ($$nofpu)"; \
	else echo "check-symbols: -mgeneral-regs-only unsupported here -- nm scan only"; fi; \
	rm -f audit_sr.o; \
	$(CC) $(CPPFLAGS) $(WARNFLAGS) -O0 $$nofpu -c sight_reduction.c -o audit_sr.o \
	    || { echo "FAIL: sight_reduction.c pulls in floating point / FPU registers" >&2; rm -f audit_*.o; exit 1; }; \
	$(CC) $(CPPFLAGS) $(WARNFLAGS) -O0 $$nofpu -c astro_nav.c -o audit_nav.o \
	    || { echo "FAIL: astro_nav.c pulls in floating point / FPU registers" >&2; rm -f audit_*.o; exit 1; }; \
	bad=`nm audit_sr.o audit_nav.o | awk '$$1=="U"{print $$2}' | grep -Ei '^_?((sin|cos|tan|asin|acos|atan|sinh|cosh|tanh|asinh|acosh|atanh)[fl]?|atan2[fl]?|(exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|hypot)[fl]?|(fabs|floor|ceil|round|trunc|fmod|remainder|ldexp|frexp|scalbn|rint|lrint|llrint|nearbyint|fma|copysign|nextafter|fdim|fmax|fmin)[fl]?|(erf|erfc|tgamma|lgamma)[fl]?|__(add|sub|mul|div)[dst]f3|__neg[dst]f2|__float[a-z0-9]*|__fix[a-z0-9]*|__(extend|trunc)[a-z0-9]*|__aeabi_[dfh][a-z0-9]*)$$' || true`; \
	rm -f audit_sr.o audit_nav.o audit_tlx.o; \
	if [ -n "$$bad" ]; then echo "FAIL: libm/soft-float symbol at instruction level:" >&2; echo "$$bad" >&2; exit 1; fi; \
	echo "check-symbols: PASS (float-free at instruction + symbol level)"

# UBSan build (macOS note: do NOT add -fsanitize=address here -- it hangs)
sight_reduction_ubsan: $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -fsanitize=undefined -fno-sanitize-recover=all \
	    -o $@ $(SRCS)

ubsan: sight_reduction_ubsan
	./sight_reduction_ubsan --self-test
	./sight_reduction_ubsan --reduce 4000 -7400 6000 2000 4012800
	./sight_reduction_ubsan --reduce 4500 0 0 4500
	./sight_reduction_ubsan --fix 4400 100 0 0 2700000 6000 3000 2476800
	./sight_reduction_ubsan --fix-stars 4400 -100 15 836121913051 1800000 9 836127121859 1800000
	./sight_reduction_ubsan --reduce-star 4500 0 15 836121913051 1800000
	./sight_reduction_ubsan --correct 1514300 100 200 150 16100 1
	./sight_reduction_ubsan --average 120000 1000 1500000 0 1503000 60000 1511000 120000 1509000 180000 1512000 240000
	./sight_reduction_ubsan --star 3 836136000000
	./sight_reduction_ubsan --star 0 12000000
	./sight_reduction_ubsan --correct 1000 0 300 150 16100 0
	# atmosphere corners: the extreme scale factors at both altitude
	# extremes must compute UB-free.
	./sight_reduction_ubsan --correct 5400000 600000 10000 70000 20000 1 -60 1100 >/dev/null
	./sight_reduction_ubsan --correct -5400000 -600000 0 0 0 -1 60 800 >/dev/null
	./sight_reduction_ubsan --time 1900 1 1 0 0 0 0 -900 -600
	./sight_reduction_ubsan --time 2099 12 31 23 59 60 999 900 500
	./sight_reduction_ubsan --golden
	./sight_reduction_ubsan --ephemeris-check
	./sight_reduction_ubsan --external-check
	./sight_reduction_ubsan --cross-check
	./sight_reduction_ubsan --scenario-check
	./sight_reduction_ubsan --zenith 134321514 -826150956 672572549 836136000000
	# input-validation paths must REJECT cleanly (exit 2), not trap in
	# UBSan (SIGABRT) -- exact exit codes checked, `!` alone can't tell.
	./sight_reduction_ubsan --zenith 2000000000 2000000000 2000000000 0 >/dev/null 2>&1; test $$? -eq 2
	./sight_reduction_ubsan --fix-stars 4400 -100 18 0 1800000 9 0 1800000 >/dev/null 2>&1; test $$? -eq 2
	# domain extremes (100-year timestamps, +-90 deg altitudes) must
	# compute UB-free; the aligned-body geometry yields "no fix" = 1.
	./sight_reduction_ubsan --fix-stars 4400 -100 15 -3155760000000 -5400000 9 3155760000000 5400000 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction_ubsan --sun 837259200000 69200
	./sight_reduction_ubsan --fix-sun 4400 -100 69200 837286272832 1800000 837297248811 3600000
	# Sun domain extremes: 100-year timestamps with +-10 min TT - UT1
	# must compute UB-free end to end.
	./sight_reduction_ubsan --reduce-sun 4500 0 -3155760000000 -600000 >/dev/null
	./sight_reduction_ubsan --reduce-sun 4500 0 3155760000000 600000 >/dev/null
	./sight_reduction_ubsan --correct-sun -5400000 -600000 10000 -3155760000000 -600000 -1 >/dev/null
	./sight_reduction_ubsan --correct-sun 5400000 600000 10000 3155760000000 600000 1 >/dev/null
	# Moon domain extremes: the same corners through the 60+60-term
	# series -- the arguments' ~477,000 deg/century rates hit their
	# widest intermediates exactly here.
	./sight_reduction_ubsan --moon 837259200000 69200
	./sight_reduction_ubsan --reduce-moon 4500 0 -3155760000000 -600000 >/dev/null
	./sight_reduction_ubsan --reduce-moon 4500 0 3155760000000 600000 >/dev/null
	./sight_reduction_ubsan --correct-moon -5400000 -600000 10000 -3155760000000 -600000 -1 >/dev/null
	./sight_reduction_ubsan --correct-moon 5400000 600000 10000 3155760000000 600000 1 >/dev/null
	# --predict domain extremes: polar observers, 100-year Moon
	# corners, +-90 deg observed readings, extreme atmospheres -- the
	# arcsine bisection and the chain inversion must run UB-free.
	./sight_reduction_ubsan --predict 9000 18000 moon:600000 3155760000000 10000 -1 -5400000 -60 800 >/dev/null
	./sight_reduction_ubsan --predict -9000 -18000 moon:-600000 -3155760000000 10000 1 5400000 60 1100 >/dev/null
	./sight_reduction_ubsan --predict 4500 0 15 836121913051 0 0 1801715 >/dev/null
	./sight_reduction_ubsan --fix-n 4400 -100 15 836121913051 1800000 9 836127121859 1800000 3 836127121859 3701231 >/dev/null
	./sight_reduction_ubsan --fix-n 4400 -100 15 836121913051 1800000 sun:69200 836127121859 -816151 moon:69200 836127121859 392148 >/dev/null
	./sight_reduction_ubsan --running-fix 4490 10 0 120 sun:69200 837286272832 1799167 sun:69200 837291672832 2746731 sun:69200 837295272832 3323969 >/dev/null
	./sight_reduction_ubsan --running-fix 4490 10 0 120 moon:69200 837286272832 1965705 moon:69200 837291672832 2871534 moon:69200 837295272832 3435965 >/dev/null
	# domain-extreme run: 100.0 kn across the full 200-year span is
	# ~1.75e9 tenths-nm -- must round, fit int32, and reduce mod one
	# earth circumference UB-free (aligned geometry: "no fix" = 1).
	./sight_reduction_ubsan --running-fix 4490 10 35999 1000 15 -3155760000000 -5400000 9 3155760000000 5400000 >/dev/null 2>&1; test $$? -eq 1
	./sight_reduction_ubsan --zenith 1 2 3 0 >/dev/null 2>&1; test $$? -eq 2

# check-backend: differential fuzz of fp_math.h's 128-bit op layer.
# The same --fuzz-w128 schedule (a fixed -2^127 boundary block, then
# 200k edge-biased iterations over add/sub/neg/mul/muls, the whole
# [0, 127] shift domain, cmp/bits, and both division routines across
# every sign combination) runs against the native __int128 backend and
# the forced two-limb portable backend, both under UBSan, and the
# printed hash lines must match byte for byte. No committed constant:
# backend agreement IS the contract. The UBSan wrapper additionally
# proves every exercised op is defined on both sides -- this is the
# gate for the modulo-2^128 wrap semantics documented in fp_math.h.
# (On a compiler without __int128 both builds are portable and the
# comparison is vacuous, but the UBSan fuzz run still gates.)
sight_reduction_ubsan_portable: $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -fsanitize=undefined -fno-sanitize-recover=all \
	    -DFP_MATH_FORCE_PORTABLE=1 -o $@ $(SRCS)

check-backend: sight_reduction_ubsan sight_reduction_ubsan_portable
	n=`./sight_reduction_ubsan --fuzz-w128` && \
	    p=`./sight_reduction_ubsan_portable --fuzz-w128` && \
	    echo "native:   $$n" && \
	    echo "portable: $$p" && \
	    test "$$n" = "$$p" \
	    || { echo "FAIL: fp_w128 fuzz gate (backend divergence, UBSan trap, or identity failure above)" >&2; exit 1; }
	echo "check-backend: PASS (native and portable fp_w128 agree)"

clean:
	rm -f sight_reduction sight_reduction_ubsan sight_reduction_ubsan_portable sight_reduction_portable $(NATIVE_REF) audit_sr.o audit_nav.o audit_tlx.o
	rm -f $(LIB) astro_nav.o consumer_smoke
	rm -rf *.dSYM

# Canonical gate: everything `test` covers (self-test, CLI checks,
# binary-level check-libm), the golden determinism hash on both fp_math
# backends, the native-double reference, a UBSan run, the differential
# 128-bit backend fuzz (check-backend), the instruction-level
# check-symbols float audit, and the library artifact + standalone
# consumer (check-lib). Each stays usable standalone; this just
# aggregates them.
check: test determinism determinism-portable reference ubsan check-backend check-symbols check-lib

# Pre-publication hygiene scan, standalone by design (not part of
# `check`): committed HEAD content (file blobs and names), commit and
# tag metadata (messages, author/committer/tagger identities) across
# all refs, and tracked-but-ignored files, matched against generic
# patterns for AI/session markers, absolute home paths, and key/token
# shapes. It audits HEAD, not the working copy -- what would actually
# be published -- and therefore requires a clean worktree/index so
# the scanned tree and the visible tree are the same thing. The
# pattern strings are bracket-split so this recipe never matches
# itself, canary probes assembled at run time cover every pattern
# branch individually (one broken branch cannot hide behind another
# that still matches), and every git command's exit
# status is checked so a git failure can never read as a clean PASS.
# In the private working repository the history leg is expected to
# fail until the fresh public root exists; on the public root every
# leg must pass.
public-audit:
	@git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
	    || { echo "public-audit: FAIL not inside a git worktree"; exit 1; }; \
	fail=0; \
	pat_ai="[C]laude|[C]odex|[C]hatGPT|[O]penAI|[A]nthropic|[C]opilot"; \
	pat_path="/[U]sers/|/[h]ome/[a-z]|/[r]oot/|[C]:.[U]sers"; \
	pat_key="BEGIN[ A-Z]* [P]RIVATE KEY|AKIA[0-9A-Z]{16}|gh[p]_[A-Za-z0-9]{20}|github_[p]at_[A-Za-z0-9_]{20}|xox[bp]-|sk-[a]nt-|sk-[p]roj-"; \
	pat="$$pat_ai|$$pat_path|$$pat_key"; \
	ck() { echo "$$1" | grep -iqE "$$pat" \
	    || { echo "public-audit: FAIL canary probe [$$1] missed (scan regression)"; cfail=1; fail=1; }; }; \
	cfail=0; \
	p=`printf "%slaude" C`; ck "$$p"; \
	p=`printf "%sodex" C`; ck "$$p"; \
	p=`printf "%shatGPT" C`; ck "$$p"; \
	p=`printf "%spenAI" O`; ck "$$p"; \
	p=`printf "%snthropic" A`; ck "$$p"; \
	p=`printf "%sopilot" C`; ck "$$p"; \
	p=`printf "/%ssers/x" U`; ck "$$p"; \
	p=`printf "/%some/x" h`; ck "$$p"; \
	p=`printf "/%soot/" r`; ck "$$p"; \
	p=`printf "%s:/%ssers" C U`; ck "$$p"; \
	p=`printf "BEGIN RSA %sRIVATE KEY" P`; ck "$$p"; \
	p=`printf "%sKIAABCDEFGH12345678" A`; ck "$$p"; \
	p=`printf "gh%s_abcdefghij0123456789" p`; ck "$$p"; \
	p=`printf "github_%sat_abcdefghij0123456789" p`; ck "$$p"; \
	p=`printf "xo%s-" xb`; ck "$$p"; \
	p=`printf "sk-%st-" an`; ck "$$p"; \
	p=`printf "sk-%sroj-" p`; ck "$$p"; \
	if test "$$cfail" = 0; then \
	    echo "public-audit: ok   canary probes match (17 pattern branches)"; \
	fi; \
	dirty=`git status --porcelain`; st=$$?; \
	if test $$st -ne 0; then \
	    echo "public-audit: FAIL git status did not run cleanly"; fail=1; \
	elif test -n "$$dirty"; then \
	    echo "$$dirty" | head -10; \
	    echo "public-audit: FAIL worktree or index not clean; this audit"; \
	    echo "  covers committed HEAD content -- commit or stash first so"; \
	    echo "  the scanned tree is the publishable tree"; fail=1; \
	else \
	    echo "public-audit: ok   worktree clean, auditing HEAD"; \
	fi; \
	hits=`git grep -nIiE "$$pat" HEAD -- .`; st=$$?; \
	if test $$st -ge 2; then \
	    echo "public-audit: FAIL git grep did not run cleanly"; fail=1; \
	elif test -n "$$hits"; then \
	    echo "$$hits" | head -40; \
	    echo "public-audit: FAIL HEAD content matches above"; fail=1; \
	else \
	    echo "public-audit: ok   HEAD content clean"; \
	fi; \
	names=`git ls-tree -r --name-only HEAD`; st=$$?; \
	if test $$st -ne 0; then \
	    echo "public-audit: FAIL git ls-tree did not run cleanly"; fail=1; \
	fi; \
	nhits=`echo "$$names" | grep -iE "$$pat"`; st=$$?; \
	if test $$st -ge 2; then \
	    echo "public-audit: FAIL file-name scan did not run cleanly"; fail=1; \
	elif test -n "$$nhits"; then \
	    echo "$$nhits"; \
	    echo "public-audit: FAIL tracked file names above match"; fail=1; \
	else \
	    echo "public-audit: ok   tracked file names clean"; \
	fi; \
	ign=`git ls-files -ci --exclude-standard`; st=$$?; \
	if test $$st -ne 0; then \
	    echo "public-audit: FAIL git ls-files -ci did not run cleanly"; fail=1; \
	elif test -n "$$ign"; then \
	    echo "$$ign"; \
	    echo "public-audit: FAIL files above are tracked but gitignored"; fail=1; \
	else \
	    echo "public-audit: ok   no tracked-but-ignored files"; \
	fi; \
	hist=`git log --all --format="%an %ae%n%cn %ce%n%B"`; st=$$?; \
	if test $$st -ne 0; then \
	    echo "public-audit: FAIL git log did not run cleanly"; fail=1; \
	fi; \
	tags=`git for-each-ref refs/tags --format="%(taggername) %(taggeremail) %(contents)"`; st=$$?; \
	if test $$st -ne 0; then \
	    echo "public-audit: FAIL git for-each-ref did not run cleanly"; fail=1; \
	fi; \
	msgs=`printf "%s\n%s" "$$hist" "$$tags" | grep -inE "$$pat"`; st=$$?; \
	if test $$st -ge 2; then \
	    echo "public-audit: FAIL history scan did not run cleanly"; fail=1; \
	elif test -n "$$msgs"; then \
	    echo "$$msgs" | head -20; \
	    echo "public-audit: FAIL history metadata matches above (expected"; \
	    echo "  in the private working repo until the fresh public root"; \
	    echo "  exists; must be clean on the public root)"; fail=1; \
	else \
	    echo "public-audit: ok   commit and tag metadata clean"; \
	fi; \
	test "$$fail" = 0 && echo "public-audit: PASS" || { echo "public-audit: FAIL"; exit 1; }

.PHONY: all lib test reference determinism determinism-portable benchmark check-libm check-lib ubsan check-backend check-symbols check public-audit clean
