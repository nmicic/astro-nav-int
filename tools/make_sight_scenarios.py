#!/usr/bin/env python3
"""Generate sight_scenarios.h: end-to-end position-recovery truth.

--ephemeris-check and --external-check validate the almanac (sky ->
body direction) against Skyfield and against printed pages. But the
COMPOSITION -- sky -> corrected altitudes -> multi-body fix -> recovered
position -- is never checked against an external answer. This gate
(`sight_reduction --scenario-check`) does that, in three families:

  1. RECOVERY SCENARIOS.  A known latitude/longitude in the library's
     navigation-sphere model, an instant, and 2-5 bodies (catalog stars,
     the Sun, and/or the Moon). Truth altitude is
     Ho = asin(obs_unit . body_unit), where body_unit is
     Skyfield's APPARENT place in the earth-fixed (ITRS) frame -- the
     same quantity our ephemeris approximates and ephemeris_reference.h
     tabulates -- and obs_unit is the local-zenith vector represented by
     that latitude/longitude. This deliberately does NOT use Skyfield's
     surface-observer topocentric altaz: the fix consumes the
     parallax-corrected Ho against a geocentric body direction, while a
     topocentric altitude still contains observer-displacement parallax
     (up to about a degree for the Moon). Folding that in would test a
     different stage of the chain. The C side then rebuilds each
     body from OUR ephemeris at the instant, feeds the stored Ho through
     astro_nav_fix_{two,n}_body, and measures how far the recovered
     position lands from truth. That distance IS the almanac error
     propagated through the fix geometry.

  2. alinnman WORKED FIXES.  Two published fixes with exact answers,
     transcribed from github.com/alinnman/celestial-navigation at
     revision b3ab049a291ac4da0a0431e798a2125f549bcd8d: the two-Sun +
     Vega fix (Chicago, 2024-05-05/06, answer 41 deg 51'N 87 deg
     39'W, starfixdata_stat_1_na.py) and the Capella + MOON + Vega
     fix (off Tunis, 2024-09-17, answer 36 deg 45'11.01"N 10 deg
     13'8.00"E, starfixdata_stat_2_na.py). The second is a published
     RAW Moon sight: its sextant altitude carries ~40' of lunar
     parallax (their own hint records HP 61.2', a near-perigee Moon)
     that the correction chain must remove -- using OUR ephemeris
     HP -- before the fix can land. Raw sextant altitudes through our
     correction chain, then our n-body fix, against the published
     places.

  3. rgleason INTERCEPT ROWS.  ~10 Sun rows from
     github.com/rgleason/celestial_navigation_pi
     (test/altitude_tests.cpp INTERCEPT_SIGHTS, revision
     e442c28ae0aeea98429391a143592c4c96f1d3a5), each a full
     Hs -> Ho -> Hc/Zn/intercept line traceable to the Nautical Almanac.
     Gates our Sun ephemeris, our Hs->Ho chain, and our reduction against
     their published numbers.

This is a one-time OFFLINE generator, same contract as
tools/make_ephemeris_reference.py: it needs Skyfield + de421.bsp (and
the Hipparcos catalog for the stars) only to precompute truth; the repo
commits the integer output (sight_scenarios.h) and never runs Python at
build or test time. Deterministic: re-running reproduces the header
byte-identical.

  python3 -m venv venv
  venv/bin/pip install skyfield==1.54 pandas==3.0.3
  venv/bin/python tools/make_sight_scenarios.py > sight_scenarios.h

(Downloads, then caches in tools/skyfield-cache/: hip_main.dat and
de421.bsp, shared with make_ephemeris_reference.py. Their SHA-256 and
the skyfield version are recorded in the header for traceability.)

TRANSCRIBED numbers only, never vendored code: the alinnman sight log
and the rgleason table rows below are numeric facts read from the cited
public repositories; the surrounding structure is this tool's own.

TIME POLICY (identical to make_external_reference.py): each published /
scenario instant is treated as UT1 directly. Every source here tabulates
against UTC; |DUT1| < 0.9 s of earth rotation = 0.23' of GHA, inside
every tolerance below. Sun/Moon rows carry Skyfield's TT - UT1 at the
instant so their dynamics and the caller-supplied time contract are
exercised (the Moon, moving ~0.55'/min against the stars, genuinely
needs the real TT; the |DUT1| slack costs it under 0.01').

For the Moon, truth Ho = asin(obs_unit . body_unit) with the GEOCENTRIC
apparent place is the parallax-corrected altitude the fix consumes
(what astro_nav_correct_altitude produces from a sextant reading), so
the scenario structure carries over from the Sun unchanged. The up-to-
~11.5' difference between geodetic latitude and the direction of an
ellipsoid surface-radius vector is not inserted here: geodetic latitude
already specifies the local-normal/zenith direction represented by
obs_unit. Ellipsoid radius refinements remain a separate model issue.
"""

import hashlib
import math
import sys
from datetime import datetime, timezone

import skyfield
from skyfield.api import Loader, Star
from skyfield.data import hipparcos
from skyfield.framelib import itrs

# Catalog order MUST match astro_nav_stars[] in astro_nav.c.
STARS = [
    ("Sirius", 32349), ("Canopus", 30438), ("Arcturus", 69673),
    ("Vega", 91262), ("Capella", 24608), ("Rigel", 24436),
    ("Procyon", 37279), ("Betelgeuse", 27989), ("Achernar", 7588),
    ("Altair", 97649), ("Aldebaran", 21421), ("Spica", 65474),
    ("Antares", 80763), ("Pollux", 37826), ("Fomalhaut", 113368),
    ("Deneb", 102098), ("Regulus", 49669), ("Polaris", 11767),
]
STAR_INDEX = {name: i for i, (name, _hip) in enumerate(STARS)}
SUN_BODY = -1   # sentinel in the body column: not a star index, the Sun
MOON_BODY = -2  # sentinel in the body column: the Moon
POLARIS = STAR_INDEX["Polaris"]  # near the pole (dec ~89 deg)

J2000 = datetime(2000, 1, 1, 12, 0, 0)
J2000_JD = 2451545.0
MS_PER_DAY = 86_400_000.0
Q30 = 1 << 30

# ==================================================================
#  Part 1 -- recovery-scenario seeds
# ==================================================================
# (name, lat_deg, lon_deg, "YYYY-MM-DD HH:MM:SS" UT1, n_bodies,
#  use_sun, use_moon, force_two_body). Spread over both hemispheres,
#  high/low latitudes, epochs 2000-2036, star rounds, Sun+star and
#  Moon+star mixes, daytime Sun-Moon cuts, and 2-, 3-, 4-, 5-body
#  fixes. The generator picks the actual bodies from those genuinely
#  above the horizon at the seed (altitude band [ALT_MIN, ALT_MAX]
#  deg), maximising the azimuth spread so the fix geometry is well
#  conditioned -- deterministic given the ephemeris.
ALT_MIN = 15.0   # below this, refraction dominates and the band is noisy
ALT_MAX = 72.0   # above this, azimuth weighting weakens (repo's own bound)

SCENARIO_SEEDS = [
    ("north-mid twilight",    41.9,  -87.6, "2024-05-06 08:20:00", 4, False, False, False),
    ("south-mid twilight",   -33.9,  151.2, "2015-09-16 09:10:00", 4, False, False, False),
    ("equatorial night",       1.3,  103.8, "2008-03-20 15:30:00", 5, False, False, False),
    ("high-north summer",     64.1,  -21.9, "2001-07-04 01:00:00", 3, False, False, False),
    ("high-south winter",    -54.8,  -68.3, "2030-06-21 23:30:00", 4, False, False, False),
    ("mid-north two-body",    37.8, -122.4, "2019-11-05 04:30:00", 2, False, False, True),
    ("tropic sun+stars",      21.3, -157.9, "2011-12-01 22:00:00", 3, True,  False, False),
    ("north-atlantic dawn",   45.5,  -73.6, "2003-10-15 09:30:00", 4, False, False, False),
    ("indian-ocean dusk",    -20.2,   57.5, "2027-04-10 14:45:00", 4, False, False, False),
    ("pacific mid dawn",      21.5, -158.0, "2033-02-18 15:40:00", 3, False, False, False),
    ("cape-town evening",    -33.9,   18.4, "2006-01-20 18:20:00", 5, False, False, False),
    ("reykjavik winter",      64.1,  -21.9, "2024-12-15 20:00:00", 4, False, False, False),
    ("singapore predawn",      1.3,  103.8, "2020-08-15 21:30:00", 4, False, False, False),
    ("hobart night",         -42.9,  147.3, "2013-05-15 11:00:00", 4, False, False, False),
    ("azores midnight",       37.7,  -25.7, "2000-06-15 01:00:00", 3, False, False, False),
    ("panama sun+stars",       8.9,  -79.5, "2017-09-23 14:30:00", 3, True,  False, False),
    ("nairobi evening",       -1.3,   36.8, "2029-11-11 16:30:00", 4, False, False, False),
    ("valdivia dusk",        -39.8,  -73.2, "2022-07-01 23:00:00", 4, False, False, False),
    ("anchorage night",       61.2, -149.9, "2010-10-30 08:00:00", 3, False, False, False),
    ("mumbai predawn",        19.1,   72.9, "2035-03-05 00:00:00", 4, False, False, False),
    ("perth evening",        -31.9,  115.9, "2005-02-28 12:30:00", 5, False, False, False),
    ("halifax dawn",          44.6,  -63.6, "2026-09-01 08:45:00", 4, False, False, False),
    ("suva night",           -18.1,  178.4, "2031-04-22 09:00:00", 4, False, False, False),
    ("stanley sun+stars",    -51.7,  -57.9, "2014-08-10 16:00:00", 3, True,  False, False),
    # Moon rows (Phase C). The two n=2 Sun-Moon seeds are the classic
    # daytime cut -- the one two-body fix where both bodies really are
    # visible at once -- taken once through the closed-form two-body
    # solver and once through the n-body solver at n=2. The rest mix
    # the Moon into star rounds and a Sun+Moon+star triple.
    ("mid-atlantic sun-moon",  36.0,  -40.0, "2026-04-11 11:00:00", 2, True,  True,  True),
    ("coral-sea sun-moon",    -16.0,  152.0, "2032-09-28 22:00:00", 2, True,  True,  False),
    ("biscay moon+stars",      45.5,   -4.0, "2028-01-14 07:00:00", 4, False, True,  False),
    ("tasman moon+stars",     -38.0,  160.0, "2004-11-21 10:00:00", 4, False, True,  False),
    ("gibraltar sun+moon+star", 36.1,  -5.4, "2016-07-10 18:00:00", 3, True,  True,  False),
]

# ==================================================================
#  Part 2 -- alinnman published Chicago fix (transcribed numbers)
# ==================================================================
# github.com/alinnman/celestial-navigation, starfixdata_stat_1_na.py.
# Two Sun sights + Vega, raw sextant altitudes (their Sight() applies
# only Bennett refraction: observer_height 0 => no dip, limb central =>
# no SD, Sun HP defaults 0 => no parallax). We store the raw altitude as
# Hs and reproduce their correction with our standard-atmosphere chain
# (eye=0, ie=0, hp=0, sd=0, limb=0). Published answer: 41 deg 51'00.1"N,
# 87 deg 39'00.2"W (a geodetic position -- see the note in the emitted
# header).
#   (object, "set_time UTC", measured_alt "d:m:s")
ALINNMAN_SIGHTS = [
    ("Sun",  "2024-05-05 15:55:18", (55,  8,  1.1)),
    ("Sun",  "2024-05-05 23:01:19", (19, 28, 19.0)),
    ("Vega", "2024-05-06 04:04:13", (30, 16, 23.7)),
]
ALINNMAN_TRUTH = (41 + 51/60 + 00.1/3600, -(87 + 39/60 + 00.2/3600))
ALINNMAN_SEED = (40.0, -90.0)  # their rough DRP LatLonGeodetic(40,-90)

# Same repository and revision, starfixdata_stat_2_na.py: Capella +
# Moon + Vega off Tunis. Same conventions as above (raw sextant
# altitudes, observer_height 0 => no dip, limb central => no SD) plus
# the Moon: their Sight() looks HP up from its almanac at the sight
# HOUR and applies hp*cos(alt) after refraction; our reproduction
# supplies OUR ephemeris HP at the exact instant instead (C side).
# Published: "The exact position is 36 deg 45' 11.01", 10 deg 13'
# 8.00"" (geodetic), DRP LatLonGeodetic(35, 10).
ALINNMAN2_SIGHTS = [
    ("Capella", "2024-09-17 23:36:13", (33,  9, 34.0)),
    ("Moon",    "2024-09-17 23:41:13", (48, 22,  5.2)),
    ("Vega",    "2024-09-17 23:46:13", (25, 39,  4.0)),
]
ALINNMAN2_TRUTH = (36 + 45/60 + 11.01/3600, 10 + 13/60 + 8.00/3600)
ALINNMAN2_SEED = (35.0, 10.0)  # their rough DRP LatLonGeodetic(35,10)

# ==================================================================
#  Part 3 -- rgleason INTERCEPT_SIGHTS rows (transcribed numbers)
# ==================================================================
# github.com/rgleason/celestial_navigation_pi, test/altitude_tests.cpp,
# INTERCEPT_SIGHTS. ~10 rows selected across limbs (lower/upper),
# altitudes 20-75 deg, both intercept signs, and both source dates. All
# rows in that table are the standard atmosphere (T=10 C, P=1010 mb, index
# error 1.5', eye 3.5 m), so the tp chain is exercised at its standard
# point. Columns transcribed verbatim from the row literals:
#   ("date UTC", limb, hs_deg, hs_min, ho_deg, gha_deg, dec_deg,
#    dr_lat_deg, dr_lon_deg, hc_deg, zn_deg, intercept_nm, towards)
# limb: +1 lower, -1 upper. ie 1.5', eye 3.5 m, T 10 C, P 1010 mb fixed.
RG_TEMP_C, RG_PRESSURE_MB, RG_IE_MIN, RG_EYE_M = 10, 1010, 1.5, 3.5
INTERCEPT_ROWS = [
    ("2025-07-20 12:47:00", +1, 30, 0.0, 30.1571, 10.1467683333333, 20.548655,
     43.2366916666667, -77.533415, 30.1818316468372, 89.39818, 1.483898810232, False),
    ("2025-07-20 17:16:33", +1, 67, 1.0, 67.1934, 77.5317933333333, 20.51284,
     43.2366916666667, -77.533415, 67.2761482927961, 179.99607, 4.964897567766, False),
    ("2025-07-20 18:17:21", +1, 75, 0.0, 75.1789, 92.7312466666667, 20.5047333333333,
     5.70799666666667, -92.728115, 75.2032630210069, 359.98851, 1.46178, False),
    ("2025-07-20 21:18:20", +1, 35, 5.0, 35.2452, 137.975475, 20.4805416666667,
     43.2366916666667, -77.533415, 35.194154591663, 265.67311, 3.06272450022, True),
    ("2025-07-20 15:43:35", +1, 30, 0.0, 30.1571, 54.2909716666667, 20.525215,
     -18.7725, -100.4382, 30.0978570449878, 51.31473, 3.554577300732, True),
    ("2025-03-12 15:23:04", -1, 37, 7.3, 36.7546, 48.365, -3.055,
     43.2366666666667, -77.5333333333333, 36.7793564087733, 142.58122, 1.485384526398, False),
    ("2025-03-12 17:19:43", -1, 44, 5.0, 43.7207, 77.5328583333333, -3.02301333333333,
     43.2366666666667, -77.5333333333333, 43.7403199980173, 179.99934, 1.177199881038, False),
    ("2025-03-12 20:08:18", +1, 30, 7.3, 30.2848, 119.686666666667, -2.97666666666667,
     43.2366666666667, -77.5333333333333, 30.251467971996, 230.88371, 1.99992168024, True),
    ("2025-03-12 23:32:37", +1, 20, 7.5, 20.2725, 170.775, -2.92166666666667,
     -18.8283, -101.17, 20.2345276777631, 273.91606, 2.278339334214, True),
    ("2025-03-13 04:54:45", +1, 20, 30.0, 20.6483, 251.324101666667, -2.832895,
     45.4166666666667, 165.033333333333, 20.6837317607836, 242.72026, 2.125905647016, False),
]


# ==================================================================
#  Conversions and geometry
# ==================================================================

def ut1_ms(dt_str):
    """'YYYY-MM-DD HH:MM:SS' UT1 -> int64 ms since J2000.0."""
    dt = datetime.strptime(dt_str, "%Y-%m-%d %H:%M:%S")
    return round((dt - J2000).total_seconds() * 1000)


def q30(component):
    return int(round(component * Q30))


def obs_unit(lat_deg, lon_deg):
    """Navigation-sphere/local-zenith unit vector.

    This is astro_nav_unitvec_from_cdeg semantics: x at (0,0), y at
    (0,90 E), z at the north pole. For geodetic latitude it represents
    the ellipsoid normal, not the geocentric surface-radius direction.
    """
    phi = math.radians(lat_deg)
    lam = math.radians(lon_deg)
    return (math.cos(phi) * math.cos(lam),
            math.cos(phi) * math.sin(lam),
            math.sin(phi))


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def alt_az(o, b, lat_deg, lon_deg):
    """Altitude and azimuth (deg) of body direction b at observer o."""
    alt = math.degrees(math.asin(max(-1.0, min(1.0, dot(o, b)))))
    phi = math.radians(lat_deg)
    lam = math.radians(lon_deg)
    east = (-math.sin(lam), math.cos(lam), 0.0)
    north = (-math.sin(phi) * math.cos(lam),
             -math.sin(phi) * math.sin(lam), math.cos(phi))
    az = math.degrees(math.atan2(dot(b, east), dot(b, north))) % 360.0
    return alt, az


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cdeg(deg):
    return int(round(deg * 100))


def marcmin(deg):
    return int(round(deg * 60_000))


class Sky:
    """Skyfield apparent-place helper, shared by all three families."""

    def __init__(self):
        self.load = Loader("tools/skyfield-cache")
        self.ts = self.load.timescale()
        self.eph = self.load("de421.bsp")
        self.earth = self.eph["earth"]
        self.sun = self.eph["sun"]
        self.moon = self.eph["moon"]
        with self.load.open(hipparcos.URL) as f:
            self.df = hipparcos.load_dataframe(f)

    def time(self, ms):
        return self.ts.ut1_jd(J2000_JD + ms / MS_PER_DAY)

    def tt_minus_ut1_ms(self, ms):
        t = self.time(ms)
        return int(round((t.tt - t.ut1) * MS_PER_DAY))

    def _unit(self, apparent):
        x, y, z = apparent.frame_xyz(itrs).au
        norm = (x * x + y * y + z * z) ** 0.5
        return (x / norm, y / norm, z / norm)

    def star_unit(self, hip, ms):
        star = Star.from_dataframe(self.df.loc[hip])
        return self._unit(self.earth.at(self.time(ms)).observe(star).apparent())

    def sun_unit(self, ms):
        return self._unit(self.earth.at(self.time(ms)).observe(self.sun).apparent())

    def moon_unit(self, ms):
        """Geocentric apparent place -- same convention as the ephemeris
        reference; parallax belongs to the altitude correction, not the
        body direction."""
        return self._unit(self.earth.at(self.time(ms)).observe(self.moon).apparent())

    def body_unit(self, body, ms):
        if body == SUN_BODY:
            return self.sun_unit(ms)
        if body == MOON_BODY:
            return self.moon_unit(ms)
        return self.star_unit(STARS[body][1], ms)


def select_bodies(sky, lat, lon, ms, n, use_sun, use_moon):
    """Bodies genuinely up (altitude in [ALT_MIN, ALT_MAX]) at the seed,
    greedily chosen for maximum azimuth spread from the one nearest 45 deg
    altitude. Deterministic given the ephemeris."""
    o = obs_unit(lat, lon)
    cands = []
    pool = list(range(len(STARS)))
    if use_moon:
        pool = [MOON_BODY] + pool
    if use_sun:
        pool = [SUN_BODY] + pool
    for body in pool:
        alt, az = alt_az(o, sky.body_unit(body, ms), lat, lon)
        if ALT_MIN <= alt <= ALT_MAX:
            cands.append((body, alt, az))
    if len(cands) < n:
        raise SystemExit("seed lat=%g lon=%g ms=%d: only %d bodies in the "
                         "altitude band, need %d" % (lat, lon, ms,
                                                     len(cands), n))
    # For a Sun/Moon + star mix, force the Sun and/or Moon in so the
    # mixed-body fix path is genuinely exercised; otherwise seed with
    # the candidate nearest 45 deg altitude. (These are geometric
    # configurations -- every body is above the horizon at the instant --
    # not a claim that all are simultaneously eye-visible; the point is
    # the fix math and almanac.)
    def sep(a, b):
        return min((a[2] - b[2]) % 360.0, (b[2] - a[2]) % 360.0)

    if n == 2:
        # A two-body fix has no averaging, so its accuracy is all cut
        # geometry: choose the pair whose azimuth separation is nearest
        # 90 deg (orthogonal LOPs, lowest DOP). Maximising the spread the
        # way the n>=3 case does would land near 180 deg -- near-parallel
        # circles of position, the WORST cut. Polaris is excluded here:
        # sitting ~1 deg from the pole its LOP runs nearly along a
        # parallel, so its (real) almanac error amplifies into miles in a
        # no-averaging fix -- it stays exercised in the n>=3 rows, where
        # the redundancy tames it. This keeps the two-body row a
        # representative GOOD fix, which is what it is meant to validate.
        pool2 = [c for c in cands if c[0] != POLARIS]
        if use_sun and use_moon:
            # A daytime Sun-Moon cut -- the one two-body fix where both
            # bodies really are visible at once: take exactly that pair.
            pool2 = [c for c in pool2 if c[0] < 0]
            if len(pool2) != 2:
                raise SystemExit("sun-moon seed lat=%g lon=%g ms=%d: Sun "
                                 "or Moon outside the altitude band"
                                 % (lat, lon, ms))
        best = None
        for i in range(len(pool2)):
            for j in range(i + 1, len(pool2)):
                s = sep(pool2[i], pool2[j])
                key = (abs(s - 90.0),
                       abs(pool2[i][1] - 45.0) + abs(pool2[j][1] - 45.0))
                if best is None or key < best[0]:
                    best = (key, [pool2[i], pool2[j]])
        chosen = best[1]
    else:
        forced = [c for c in cands
                  if (use_sun and c[0] == SUN_BODY)
                  or (use_moon and c[0] == MOON_BODY)]
        if forced:
            chosen = forced
        else:
            chosen = [min(cands, key=lambda c: abs(c[1] - 45.0))]
        remaining = [c for c in cands if c not in chosen]
        while len(chosen) < n:
            def min_gap(c):
                return min(sep(c, d) for d in chosen)
            best = max(remaining, key=min_gap)
            chosen.append(best)
            remaining.remove(best)
    # Emit ordered by azimuth for a stable, readable row.
    chosen.sort(key=lambda c: c[2])
    return o, chosen


def emit():
    sky = Sky()
    out = []
    w = out.append

    w("/* Generated by tools/make_sight_scenarios.py -- do not edit.")
    w(" *")
    w(" * End-to-end position-recovery truth for"
      " `sight_reduction --scenario-check`:")
    w(" * the composition sky -> corrected altitude -> multi-body fix ->"
      " position,")
    w(" * checked against external answers. Three families:")
    w(" *")
    w(" *   scenario_rows   known navigation-sphere lat/lon + instant +"
      " 2-5 bodies;")
    w(" *                   Ho = asin(obs_unit . body_unit) with body_unit"
      " the")
    w(" *                   Skyfield/DE421 APPARENT place in ITRS (the"
      " quantity our")
    w(" *                   ephemeris approximates), obs_unit the local"
      " zenith.")
    w(" *                   Surface-observer topocentric altaz is not used:"
      " it still")
    w(" *                   contains parallax already removed from the Ho"
      " consumed")
    w(" *                   by the fix (up to about a degree for the Moon).")
    w(" *                   Recovered position vs truth = the almanac error"
      " propagated")
    w(" *                   through the fix geometry.")
    w(" *   alinnman_sights two published fixes from")
    w(" *   alinnman2_sights github.com/alinnman/celestial-navigation:"
      " Chicago")
    w(" *                   two-Sun + Vega (2024-05, answer 41 deg 51'N"
      " 87 deg 39'W)")
    w(" *                   and Capella + Moon + Vega off Tunis (2024-09,"
      " answer")
    w(" *                   36 deg 45'11\"N 10 deg 13'8\"E). The second"
      " is a raw MOON")
    w(" *                   sight: ~40' of lunar parallax removed by our"
      " chain with")
    w(" *                   our own ephemeris HP. Raw Hs through our"
      " correction")
    w(" *                   chain then our n-body fix.")
    w(" *   intercept_rows  ~10 Sun rows (Hs -> Ho -> Hc/Zn/intercept),"
      " NA-traceable,")
    w(" *                   from github.com/rgleason/celestial_navigation_pi")
    w(" *                   (test/altitude_tests.cpp INTERCEPT_SIGHTS).")
    w(" *")
    w(" * Transcribed numbers only (no vendored code). Instants treated as"
      " UT1;")
    w(" * Sun/scenario rows carry Skyfield's TT - UT1. See the tool for"
      " full")
    w(" * provenance and the Earth-model note on the alinnman truth.")
    w(" *")
    w(" * Provenance (see tools/make_sight_scenarios.py):")
    w(" *   skyfield %s" % skyfield.__version__)
    w(" *   de421.bsp sha256")
    w(" *     %s" % sha256_of("tools/skyfield-cache/de421.bsp"))
    w(" *   hip_main.dat sha256")
    w(" *     %s */" % sha256_of("tools/skyfield-cache/hip_main.dat"))
    w("")

    # ---- Family 1: recovery scenarios ----
    w("/* A recovery scenario: truth navigation-sphere lat/lon, an")
    w(" * instant, and up to 5 bodies with their parallax-corrected Ho.")
    w(" * body[i] is a star index 0..%d, -1 for the Sun, -2 for the Moon;"
      % (len(STARS) - 1))
    w(" * ho_marcmin[i] = round(asin(obs_unit . body_unit) in arcminutes")
    w(" * x 1000). n < 5 rows leave the unused body/ho slots 0. two_body")
    w(" * routes the check through astro_nav_fix_two_body instead of the")
    w(" * n-body solver. */")
    w("typedef struct {")
    w("    int32_t lat_cdeg, lon_cdeg;  /* truth navigation-sphere lat/lon */")
    w("    int64_t ut1_ms;")
    w("    int32_t tt_minus_ut1_ms;     /* for any Sun/Moon body */")
    w("    int32_t n;")
    w("    int32_t two_body;            /* 1 -> use the closed-form fix */")
    w("    int32_t body[5];             /* star index 0..%d, -1 Sun,"
      " -2 Moon */" % (len(STARS) - 1))
    w("    int64_t ho_marcmin[5];")
    w("} scenario_row_t;")
    w("")
    w("static const scenario_row_t scenario_rows[] = {")
    for (name, lat, lon, when, n,
         use_sun, use_moon, two_body) in SCENARIO_SEEDS:
        ms = ut1_ms(when)
        o, chosen = select_bodies(sky, lat, lon, ms, n, use_sun, use_moon)
        ttm = sky.tt_minus_ut1_ms(ms) if any(c[0] < 0
                                             for c in chosen) else 0
        bodies = [c[0] for c in chosen] + [0] * (5 - n)
        hos = []
        labels = []
        for (body, _alt, _az) in chosen:
            b = sky.body_unit(body, ms)
            hos.append(marcmin(math.degrees(math.asin(
                max(-1.0, min(1.0, dot(o, b)))))))
            labels.append("Sun" if body == SUN_BODY else
                          "Moon" if body == MOON_BODY else STARS[body][0])
        hos += [0] * (5 - n)
        w("    { %6d, %7d, %14dLL, %6d, %d, %d, { %s }," % (
            cdeg(lat), cdeg(lon), ms, ttm, n, 1 if two_body else 0,
            ", ".join("%3d" % b for b in bodies)))
        w("      { %s } }, /* %s: %s */" % (
            ", ".join("%9dLL" % h for h in hos), name, ", ".join(labels)))
    w("};")
    w("")
    w("#define SCENARIO_ROW_COUNT"
      " (sizeof scenario_rows / sizeof scenario_rows[0])")
    w("")

    # ---- Family 2: alinnman worked fixes ----
    w("/* alinnman published fixes: raw sextant altitude (Hs) per sight;")
    w(" * the check runs our standard-atmosphere correction chain (eye=0,")
    w(" * ie=0, limb=0, T=10 C, P=1010 mb -- exactly the corrections")
    w(" * their Sight() applied: Bennett refraction, no dip, central")
    w(" * limb) then our n-body fix. body: star index, -1 = Sun, -2 =")
    w(" * Moon. For a Moon row the C side supplies OUR ephemeris HP at")
    w(" * the instant (their Sight() looked HP up from its almanac at")
    w(" * the sight hour), so the raw sight exercises refraction + lunar")
    w(" * parallax end to end through the Moon correction chain. */")
    w("typedef struct {")
    w("    int64_t ut1_ms;")
    w("    int32_t tt_minus_ut1_ms;")
    w("    int32_t body;          /* star index, -1 = Sun, -2 = Moon */")
    w("    int64_t hs_marcmin;    /* raw sextant altitude */")
    w("} alinnman_sight_t;")
    w("")
    # Published answers are geodetic; our spherical fit of geodetic-
    # horizon altitudes recovers the geodetic latitude directly (the
    # observed altitude is measured from the horizon perpendicular to the
    # local geodetic zenith Z, so O . B = sin Ho gives O = Z, whose
    # polar-angle latitude is the geodetic latitude by definition).
    for (pfx, sights, truth, seed, pad, latc, lonc) in [
        ("ALINNMAN", ALINNMAN_SIGHTS, ALINNMAN_TRUTH, ALINNMAN_SEED, 4,
         "41 deg 51'00.1\"N (geodetic; see the tool)",
         "87 deg 39'00.2\"W"),
        ("ALINNMAN2", ALINNMAN2_SIGHTS, ALINNMAN2_TRUTH, ALINNMAN2_SEED, 7,
         "36 deg 45'11.01\"N (geodetic; see the tool)",
         "10 deg 13'8.00\"E"),
    ]:
        arr = pfx.lower() + "_sights"
        w("static const alinnman_sight_t %s[] = {" % arr)
        for (obj, when, (d, m, s)) in sights:
            ms = ut1_ms(when)
            body = (SUN_BODY if obj == "Sun" else
                    MOON_BODY if obj == "Moon" else STAR_INDEX[obj])
            ttm = sky.tt_minus_ut1_ms(ms)
            hs = marcmin(d + m / 60.0 + s / 3600.0)
            w("    { %14dLL, %6d, %3d, %9dLL },"
              " /* %-*s %s  %d:%02d:%04.1f */"
              % (ms, ttm, body, hs, pad, obj, when, d, m, s))
        w("};")
        w("")
        w("#define %s_SIGHT_COUNT (sizeof %s / sizeof %s[0])"
          % (pfx, arr, arr))
        tlat, tlon = truth
        slat, slon = seed
        w("#define %s_TRUTH_LAT_CDEG %d /* %s */" % (pfx, cdeg(tlat), latc))
        w("#define %s_TRUTH_LON_CDEG %d /* %s */" % (pfx, cdeg(tlon), lonc))
        w("#define %s_SEED_LAT_CDEG %d /* their rough DRP */"
          % (pfx, cdeg(slat)))
        w("#define %s_SEED_LON_CDEG %d" % (pfx, cdeg(slon)))
        w("")

    # ---- Family 3: rgleason intercept rows ----
    w("/* rgleason INTERCEPT_SIGHTS: a full Sun sight line. hs_marcmin the")
    w(" * raw sextant altitude; ie_marcmin NEGATIVE because their chain")
    w(" * subtracts the index error (Ha = Hs - IE - dip) while ours adds")
    w(" * it. gha/dec baked into their published Sun direction (Q2.30)")
    w(" * for the ephemeris and reduction gates. ho/hc are their published")
    w(" * observed and computed altitudes; zn their azimuth; intercept in")
    w(" * tenths of a nautical mile, POSITIVE toward. All rows: T=10 C,")
    w(" * P=1010 mb, index error 1.5', eye 3.5 m. */")
    w("typedef struct {")
    w("    int64_t ut1_ms;")
    w("    int32_t tt_minus_ut1_ms;")
    w("    int32_t limb;             /* +1 lower, -1 upper */")
    w("    int32_t temp_c, pressure_mb;")
    w("    int64_t ie_marcmin;       /* signed; negative (their convention) */")
    w("    int32_t eye_cm;")
    w("    int64_t hs_marcmin;")
    w("    int64_t ho_marcmin;       /* their published Ho */")
    w("    int32_t bx, by, bz;       /* their Sun direction, Q2.30 */")
    w("    int32_t dr_lat_cdeg, dr_lon_cdeg;")
    w("    int64_t hc_marcmin;       /* their published Hc */")
    w("    int32_t zn_cdeg;          /* their published azimuth */")
    w("    int32_t intercept_tenths_nm; /* signed; + toward */")
    w("} intercept_row_t;")
    w("")
    w("static const intercept_row_t intercept_rows[] = {")
    for (when, limb, hd, hm, ho, gha, dec, drlat, drlon, hc, zn, inter,
         towards) in INTERCEPT_ROWS:
        ms = ut1_ms(when)
        ttm = sky.tt_minus_ut1_ms(ms)
        # Their Sun direction from published GHA/dec: east-lon = -GHA.
        lam = math.radians(-gha)
        dr = math.radians(dec)
        bx = q30(math.cos(dr) * math.cos(lam))
        by = q30(math.cos(dr) * math.sin(lam))
        bz = q30(math.sin(dr))
        inter_signed = inter if towards else -inter
        w("    { %14dLL, %6d, %+d, %2d, %4d, %9dLL, %4d, %10dLL,"
          % (ms, ttm, limb, RG_TEMP_C, RG_PRESSURE_MB,
             -marcmin(RG_IE_MIN / 60.0), round(RG_EYE_M * 100),
             marcmin(hd + hm / 60.0)))
        w("      %9dLL, %11d, %11d, %11d, %6d, %7d, %9dLL, %6d, %+5d },"
          " /* %s %s */" % (
              marcmin(ho), bx, by, bz, cdeg(drlat), cdeg(drlon),
              marcmin(hc), cdeg(zn), round(inter_signed * 10),
              when, "lower" if limb > 0 else "upper"))
    w("};")
    w("")
    w("#define INTERCEPT_ROW_COUNT"
      " (sizeof intercept_rows / sizeof intercept_rows[0])")

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(emit())
