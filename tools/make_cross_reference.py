#!/usr/bin/env python3
"""Generate cross_reference.h: truth rows from independent COMPUTED
implementations -- a third and fourth evidence lineage.

The repo already gates the almanac against two lineages:
  --ephemeris-check  Skyfield + JPL DE421 (the JPL-ephemeris family;
                     SFalmanac/SkyAlmanac-class tools all inherit it)
  --external-check   numbers a human read off printed almanac pages
This third gate, --cross-check, measures the almanac against
PyEphem/libastro (the XEphem lineage), and the reduction/fix geometry
against an independent spherical-geometry engine that solves the fix
by direct circle-of-position intersection instead of this library's
iterated least-squares -- different algorithm, same sphere. Three
computational lineages plus print agreeing is consensus; each alone is
just a correlation.

INDEPENDENCE SCOPE, per body. Sun and Moon: libastro evaluates
Bretagnon's analytic planetary theory and an ELP-derived Moon -- code
AND underlying data disjoint from JPL numerical integration and from
printed pages. Stars: NOT data-independent of the Skyfield lineage --
PyEphem's bundled catalog is the same Hipparcos data, and its 1991.25
-> J2000 epoch shift was itself computed with Skyfield (stated in
ephem/stars.py); the star rows therefore validate libastro's
independent runtime TRANSFORMATION chain (precession, nutation,
aberration, sidereal time -- all libastro's own C), not independent
source positions. The header, the C gate comments, and the docs carry
the same scoping.

The cross-oracle calibration is EXECUTABLE, not prose: run

  venv/bin/python tools/make_cross_reference.py \\
      --calibrate ephemeris_reference.h

to recompute libastro-vs-Skyfield/DE421 agreement over every committed
row (Sun/Moon TT-aligned per row -- dynamics at the row's TT, sidereal
time at the row's UT1; stars at the row's UT1 with libastro's own
delta_t, where the delta-T difference is negligible). Measured with
ephem 4.2.1 over all rows: Sun worst 7.33, Moon worst 2.30, stars
worst 11.29 milli-arcmin -- 55x (Sun), 73x (Moon), 129x (stars) below
this library's measured model error (401, 169, 1452), so any
--cross-check disagreement is ours, confirmed twice for the Sun and
Moon and confirmed-in-code for the stars. The pin is enforced here
too: --calibrate refuses under a non-pinned ephem unless
--allow-unpinned is given, and always prints the version it ran.

This is a one-time OFFLINE generator: the repo commits its integer
output (cross_reference.h) and never needs Python or third-party code
at build or test time. Regenerate with:

  venv/bin/pip install ephem==4.2.1
  venv/bin/python tools/make_cross_reference.py \\
      --geometry-src /path/to/celestial-navigation > cross_reference.h

where --geometry-src is a local read-only clone of
github.com/alinnman/celestial-navigation, pinned at revision
b3ab049a291ac4da0a0431e798a2125f549bcd8d (MIT license; the clone
stays outside this repo -- only these normalized rows and their
provenance are committed). The pins are ENFORCED: generation refuses
unless ephem's version and the clone's revision match the constants
below (an unreadable revision is an error, not "unknown"); pass
--allow-unpinned to generate anyway, which stamps the emitted header
UNPINNED with the actual versions so drifted provenance can never
look pinned.

Almanac families (PyEphem/libastro lineage): geocentric apparent
equinox-of-date RA/dec and apparent GAST -> GHA -> the library's ITRS
frame (x Greenwich, y east, z north), Q2.30; same epoch schedules as
tools/make_ephemeris_reference.py so the two lineages sample identical
instants. Geometry families (independent-geometry lineage): synthetic
observer/GP scenarios quantized to the library's integer boundary
(centidegrees in, milli-arcmin altitudes), oracle outputs from the
clone's own vector primitives and circle intersection.

TIME POLICY: dates are fed to libastro as UT1 (its "UT"); each row
records libastro's OWN delta_t as tt_minus_ut1_ms, so --cross-check
hands the library the exact (UT1, TT) pair the oracle used and the
comparison carries no delta-T model disagreement. The geometry
families are timeless.
"""

import argparse
import math
import os
import re
import subprocess
import sys

import ephem
import ephem.stars

GEOMETRY_URL = "github.com/alinnman/celestial-navigation"
GEOMETRY_PIN = "b3ab049a291ac4da0a0431e798a2125f549bcd8d"
EPHEM_PIN = "4.2.1"  # libastro lineage the committed rows came from

# Catalog order must match astro_nav_stars[] in astro_nav.c (and the
# schedule mirrors tools/make_ephemeris_reference.py so the Skyfield
# and libastro lineages sample the same instants).
STARS = [
    ("Sirius", 32349), ("Canopus", 30438), ("Arcturus", 69673),
    ("Vega", 91262), ("Capella", 24608), ("Rigel", 24436),
    ("Procyon", 37279), ("Betelgeuse", 27989), ("Achernar", 7588),
    ("Altair", 97649), ("Aldebaran", 21421), ("Spica", 65474),
    ("Antares", 80763), ("Pollux", 37826), ("Fomalhaut", 113368),
    ("Deneb", 102098), ("Regulus", 49669), ("Polaris", 11767),
]

EPOCHS_MS = [n * 81_240_000_000 + 12_345_678 for n in range(15)]

SUN_EPOCHS_MS = [n * 27_270_000_000 + 9_876_543 for n in range(41)] + [
    820_731_600_000,     # 2026-01-03 17:00 UT1, perihelion
    836_629_200_000,     # 2026-07-06 17:00 UT1, aphelion
    1_104_670_800_000,   # 2035-01-03 01:00 UT1, perihelion
    1_120_636_800_000,   # 2035-07-06 20:00 UT1, aphelion
]

MOON_EPOCHS_MS = [n * 27_870_000_000 + 7_654_321 for n in range(41)] + [
    850_244_400_000,     # 2026-12-11 06:00 UT1, apogee
    851_374_800_000,     # 2026-12-24 08:00 UT1, perigee
    794_635_200_000,     # 2025-03-07 16:00 UT1, standstill north
    795_898_800_000,     # 2025-03-22 07:00 UT1, standstill south
    836_424_000_000,     # 2026-07-04 08:00 UT1, ascending node
    835_164_000_000,     # 2026-06-19 18:00 UT1, descending node
]

ARIES_EPOCHS_MS = EPOCHS_MS + SUN_EPOCHS_MS[41:]

Q30 = 1 << 30
MARCMIN_PER_RAD = 180.0 * 60.0 * 1000.0 / math.pi
TURN_MARCMIN = 21_600_000
AU_KM = 149_597_870.7
EARTH_R_KM = 6378.137

# Reduction scenarios: (lat, lon_east, gha, dec) centidegrees.
# Coverage: all four azimuth quadrants, equator, both hemispheres,
# 89 deg observer, dateline and GHA 0/35999 wraps, a near-zenith body
# (row 10) and one body below the horizon (row 11, negative Hc).
REDUCE_SCENARIOS = [
    (4500, -3000, 4500, 2000),
    (0, 0, 33000, 0),
    (0, 17950, 21000, -1200),
    (-3356, 15123, 20200, -1650),
    (7800, -500, 12000, 6215),
    (8900, 4500, 20000, 8000),
    (2312, -15801, 16000, -2800),
    (5150, -1, 0, 2326),
    (5150, -1, 35999, 2326),
    (4500, -3000, 3100, 4480),
    (4500, -3000, 15000, -2000),
    (6912, 1033, 27000, 1512),
    (-5601, -6754, 8000, -5000),
    (1500, 12000, 29500, 2100),
    (3333, -17999, 15000, 333),
    (2000, 5999, 26000, -100),
]

# Two-body fix scenarios: true position (deg floats, deliberately off
# the centidegree grid), DR (centidegrees), two GPs (dec/GHA
# centidegrees). Altitudes are derived from the true position with the
# oracle's own vectors, rounded to milli-arcmin, and both engines are
# fed the identical quantized inputs -- so the comparison isolates the
# fix algorithm (their direct circle intersection vs our iterated
# least-squares), not the scenario setup. Coverage: narrow ~30 deg and
# wide ~150 deg cuts, equator, 78 deg north, deep south, dateline
# straddle, a near-zenith sight, a 60 nm DR error.
FIX_SCENARIOS = [
    (45.1234, -30.5678, 4450, -3100, 2000, 4500, 1000, 1000),
    (36.8700, -25.4100, 3700, -2500, 1500, 4000, 500, 3000),
    (10.2000, 140.7000, 1000, 14000, 3000, 22500, -2500, 20500),
    (0.8500, 0.4000, 0, 0, 1200, 33000, -1500, 3000),
    (78.3450, 15.6700, 7800, 1500, 6000, 33000, 7000, 12000),
    (-42.5000, 147.3000, -4200, 14800, -2000, 20000, -6000, 25000),
    (12.3400, 179.8500, 1200, -17950, 500, 17000, 2500, 19500),
    (21.3000, -157.8600, 2100, -15800, 2050, 15850, 1800, 16400),
    (50.9000, -1.4000, 5000, -200, 1500, 6000, -1000, 32000),
    (33.0000, -118.0000, 3400, -11700, 1400, 12500, 2800, 10000),
    (-0.5000, 179.2000, -100, 17900, 1000, 15500, -1200, 18200),
    (58.7100, -152.4400, 5900, -15200, 2300, 17500, 4000, 13000),
    (24.5600, 54.3800, 2400, 5500, 1200, 33500, 2600, 30800),
    (-33.9200, 18.4200, -3400, 1800, -1800, 34500, -800, 35500),
]


def edate(ut1_ms):
    # ephem.Date counts days from 1899-12-31 12:00 (Dublin JD);
    # J2000.0 is JD 2451545.0 = Dublin day 36525.0 exactly.
    return ephem.Date(36525.0 + ut1_ms / 86_400_000.0)


def gast_rad(ut1_ms):
    obs = ephem.Observer()
    obs.lon, obs.lat, obs.elevation = 0.0, 0.0, 0.0
    obs.pressure = 0
    obs.date = edate(ut1_ms)
    return float(obs.sidereal_time())


def itrs_float(gha_rad, dec_rad):
    # Library frame: x toward Greenwich meridian, y east, z north;
    # GHA is westward, so east-longitude = -GHA.
    return (math.cos(dec_rad) * math.cos(gha_rad),
            -math.cos(dec_rad) * math.sin(gha_rad),
            math.sin(dec_rad))


def itrs_q30(gha_rad, dec_rad):
    return tuple(round(c * Q30) for c in itrs_float(gha_rad, dec_rad))


def dtt_ms(ut1_ms):
    return round(ephem.delta_t(edate(ut1_ms)) * 1000.0)


def body_rows(body, epochs):
    rows = []
    for ms in epochs:
        body.compute(edate(ms))
        gha = gast_rad(ms) - float(body.ra)
        x, y, z = itrs_q30(gha, float(body.dec))
        dist_km = body.earth_distance * AU_KM
        # body.size is apparent diameter in arcseconds; SD = half of it.
        sd = round(body.size * 1000.0 / 120.0)
        hp = round(math.asin(EARTH_R_KM / dist_km) * MARCMIN_PER_RAD)
        rows.append((ms, dtt_ms(ms), x, y, z, dist_km, sd, hp,
                     body.earth_distance))
    return rows


def geometry_engine(src):
    sys.path.insert(0, src)
    here = os.getcwd()
    os.chdir(src)  # its almanac config paths are CWD-relative
    try:
        import starfix
    finally:
        os.chdir(here)
    return starfix


def geometry_rev(src):
    try:
        out = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True)
    except Exception as exc:
        sys.exit("error: cannot determine geometry source revision at %s "
                 "(%s); a source whose revision cannot be verified must "
                 "not feed pinned fixtures" % (src, exc))
    return out.stdout.strip()


def zd_deg(sf, a, b):
    return math.degrees(math.acos(
        sf.dot_product(sf.to_rectangular(a), sf.to_rectangular(b))))


def gp_latlon(sf, gha_cdeg, dec_cdeg):
    return sf.LatLonGeocentric(dec_cdeg / 100.0,
                               sf.mod_lon(-gha_cdeg / 100.0))


def tt_aligned_date(ut1_ms, row_dtt_ms):
    """Date whose libastro-internal TT (date + libastro's own delta_t)
    equals a committed Skyfield row's TT (ut1 + the row's
    tt_minus_ut1_ms). Removes the oracles' differing TT-UT1
    extrapolations -- an earth-rotation unknowable, not a theory
    difference -- from the dynamics comparison; sidereal time still
    uses true UT1."""
    d0 = edate(ut1_ms)
    shift_s = row_dtt_ms / 1000.0 - ephem.delta_t(d0)
    return ephem.Date(float(d0) + shift_s / 86400.0)


def header_rows(path, name, nfields):
    text = open(path).read()
    block = re.search(r"%s\[\] = \{(.*?)\n\};" % name, text, re.S)
    if block is None:
        sys.exit("error: no array %s[] in %s" % (name, path))
    rows = []
    for m in re.finditer(r"\{([^}]*)\}", block.group(1)):
        vals = [int(v.strip().rstrip("L")) for v in m.group(1).split(",")]
        if len(vals) != nfields:
            sys.exit("error: %s[] row has %d fields, expected %d"
                     % (name, len(vals), nfields))
        rows.append(vals)
    return rows


def chord_vs_q30_marcmin(v, q30):
    dx = v[0] - q30[0] / Q30
    dy = v[1] - q30[1] / Q30
    dz = v[2] - q30[2] / Q30
    return math.sqrt(dx * dx + dy * dy + dz * dz) * MARCMIN_PER_RAD


def calibrate(path, allow_unpinned):
    """--calibrate: replay EVERY Sun/Moon/star row of the committed
    Skyfield/DE421 header (ephemeris_reference.h) through libastro
    (Sun/Moon TT-aligned per row; stars at the row's UT1) and print
    the worst oracle-vs-oracle disagreement per family. This is the
    executable form of the calibration claim quoted in the module
    docstring and docs/HOWTO.md; needs no geometry source. The ephem
    pin applies here exactly as it does to generation."""
    if ephem.__version__ != EPHEM_PIN:
        if not allow_unpinned:
            sys.exit("error: ephem %s installed, pin is %s; the "
                     "recorded calibration is only comparable under "
                     "the pinned version (--allow-unpinned overrides)"
                     % (ephem.__version__, EPHEM_PIN))
        print("warning: calibrating under UNPINNED ephem %s (pin %s)"
              % (ephem.__version__, EPHEM_PIN), file=sys.stderr)
        print("ephem %s (UNPINNED, pin %s)"
              % (ephem.__version__, EPHEM_PIN))
    else:
        print("ephem %s (pinned)" % ephem.__version__)
    sun = header_rows(path, "ephem_sun_rows", 6)
    moon = header_rows(path, "ephem_moon_rows", 8)
    stars = header_rows(path, "ephem_ref_rows", 5)

    worst = 0.0
    for ut1, dtt, x, y, z, _dist in sun:
        body = ephem.Sun()
        body.compute(tt_aligned_date(ut1, dtt))
        gha = gast_rad(ut1) - float(body.ra)
        worst = max(worst, chord_vs_q30_marcmin(
            itrs_float(gha, float(body.dec)), (x, y, z)))
    print("sun    %3d rows  worst %5.2f milli-arcmin" % (len(sun), worst))

    worst = 0.0
    for ut1, dtt, x, y, z, _dist, _sd, _hp in moon:
        body = ephem.Moon()
        body.compute(tt_aligned_date(ut1, dtt))
        gha = gast_rad(ut1) - float(body.ra)
        worst = max(worst, chord_vs_q30_marcmin(
            itrs_float(gha, float(body.dec)), (x, y, z)))
    print("moon   %3d rows  worst %5.2f milli-arcmin" % (len(moon), worst))

    worst = 0.0
    for si, ut1, x, y, z in stars:
        body = ephem.star(STARS[si][0])
        body.compute(edate(ut1))
        gha = gast_rad(ut1) - float(body.ra)
        worst = max(worst, chord_vs_q30_marcmin(
            itrs_float(gha, float(body.dec)), (x, y, z)))
    print("stars  %3d rows  worst %5.2f milli-arcmin" % (len(stars), worst))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--geometry-src",
                    help="local clone of %s at %s"
                         % (GEOMETRY_URL, GEOMETRY_PIN))
    ap.add_argument("--calibrate", metavar="EPHEMERIS_REFERENCE_H",
                    help="do not generate; replay the committed "
                         "Skyfield/DE421 rows through libastro and "
                         "report the worst oracle-vs-oracle "
                         "disagreement per family")
    ap.add_argument("--allow-unpinned", action="store_true",
                    help="proceed despite a pin mismatch (generation "
                         "checks ephem and the geometry revision, "
                         "--calibrate checks ephem); a header "
                         "generated unpinned is stamped UNPINNED")
    args = ap.parse_args()

    if args.calibrate:
        calibrate(args.calibrate, args.allow_unpinned)
        return
    if not args.geometry_src:
        ap.error("--geometry-src is required (unless --calibrate)")

    src = os.path.abspath(args.geometry_src)
    pin_mismatch = []
    if ephem.__version__ != EPHEM_PIN:
        pin_mismatch.append("ephem %s installed, pin is %s"
                            % (ephem.__version__, EPHEM_PIN))
    rev = geometry_rev(src)
    if rev != GEOMETRY_PIN:
        pin_mismatch.append("geometry revision %s, pin is %s"
                            % (rev, GEOMETRY_PIN))
    for msg in pin_mismatch:
        print("pin mismatch: %s" % msg, file=sys.stderr)
    if pin_mismatch and not args.allow_unpinned:
        sys.exit("error: refusing to generate fixtures from unpinned "
                 "sources (--allow-unpinned overrides and stamps the "
                 "header UNPINNED)")

    sf = geometry_engine(src)

    print("/* Generated by tools/make_cross_reference.py -- do not edit.")
    if pin_mismatch:
        print(" *")
        print(" * *** UNPINNED GENERATION ***")
        for msg in pin_mismatch:
            print(" * *** %s ***" % msg)
    print(" *")
    print(" * Truth rows from independent COMPUTED implementations,")
    print(" * consumed by `sight_reduction --cross-check`. Two lineages:")
    print(" *")
    print(" *   PyEphem/libastro %s (the XEphem lineage):" % ephem.__version__)
    print(" *   cross_sun_rows, cross_moon_rows, cross_star_rows,")
    print(" *   cross_aries_rows. Geocentric apparent equinox-of-date")
    print(" *   RA/dec + apparent GAST -> GHA -> ITRS Q2.30. Each row")
    print(" *   carries libastro's own delta_t as tt_minus_ut1_ms.")
    print(" *   Independence scope: the Sun (Bretagnon) and Moon")
    print(" *   (ELP-derived) rows are code AND data disjoint from both")
    print(" *   Skyfield/DE421 (ephemeris_reference.h) and printed")
    print(" *   pages (external_reference.h). The STAR rows are not")
    print(" *   data-independent: PyEphem's catalog is the same")
    print(" *   Hipparcos data, epoch-shifted with Skyfield (per")
    print(" *   ephem/stars.py), so they validate libastro's own")
    print(" *   precession/nutation/aberration/sidereal chain, not")
    print(" *   independent star positions.")
    print(" *")
    print(" *   %s" % GEOMETRY_URL)
    print(" *   revision %s" % rev)
    print(" *   (independent spherical-geometry engine, MIT):")
    print(" *   cross_reduce_rows (Hc/Zn from its vector primitives),")
    print(" *   cross_fix_rows (two-body fix by direct circle-of-")
    print(" *   position intersection -- a different algorithm from")
    print(" *   this library's iterated least-squares).")
    print(" *")
    print(" * Angles: Q2.30 unit vectors or milli-arcmin; positions in")
    print(" * rows are centidegrees (inputs) and milli-arcmin (oracle")
    print(" * outputs); time is UT1 ms since J2000.0. */")
    print("")

    # ---- libastro: Sun ----
    print("typedef struct {")
    print("    int64_t ut1_ms;          /* UT1 ms since J2000.0 */")
    print("    int32_t tt_minus_ut1_ms; /* libastro's delta_t */")
    print("    int32_t x, y, z;         /* ITRS apparent direction, Q2.30 */")
    print("    int32_t dist_uau;        /* apparent distance, micro-AU */")
    print("    int32_t sd_marcmin;      /* libastro apparent SD */")
    print("    int32_t hp_marcmin;      /* asin(6378.137 km / dist) */")
    print("} cross_sun_row_t;")
    print("")
    print("static const cross_sun_row_t cross_sun_rows[] = {")
    for ms, dtt, x, y, z, dist_km, sd, hp, dist_au in \
            body_rows(ephem.Sun(), SUN_EPOCHS_MS):
        print("    { %14dLL, %6d, %11d, %11d, %11d, %7d, %5d, %3d },"
              % (ms, dtt, x, y, z, round(dist_au * 1e6), sd, hp))
    print("};")
    print("#define CROSS_SUN_ROW_COUNT"
          " (sizeof cross_sun_rows / sizeof cross_sun_rows[0])")
    print("")

    # ---- libastro: Moon ----
    print("typedef struct {")
    print("    int64_t ut1_ms;")
    print("    int32_t tt_minus_ut1_ms;")
    print("    int32_t x, y, z;         /* ITRS apparent direction, Q2.30 */")
    print("    int32_t dist_km;")
    print("    int32_t sd_marcmin;      /* libastro apparent SD */")
    print("    int32_t hp_marcmin;      /* asin(6378.137 km / dist) */")
    print("} cross_moon_row_t;")
    print("")
    print("static const cross_moon_row_t cross_moon_rows[] = {")
    for ms, dtt, x, y, z, dist_km, sd, hp, _ in \
            body_rows(ephem.Moon(), MOON_EPOCHS_MS):
        print("    { %14dLL, %6d, %11d, %11d, %11d, %6d, %5d, %5d },"
              % (ms, dtt, x, y, z, round(dist_km), sd, hp))
    print("};")
    print("#define CROSS_MOON_ROW_COUNT"
          " (sizeof cross_moon_rows / sizeof cross_moon_rows[0])")
    print("")

    # ---- libastro: stars ----
    print("typedef struct {")
    print("    int32_t star;    /* index into astro_nav_stars[] */")
    print("    int64_t ut1_ms;")
    print("    int32_t x, y, z; /* ITRS apparent direction, Q2.30 */")
    print("} cross_star_row_t;")
    print("")
    print("static const cross_star_row_t cross_star_rows[] = {")
    for ms in EPOCHS_MS:
        for index, (name, _hip) in enumerate(STARS):
            st = ephem.star(name)
            st.compute(edate(ms))
            gha = gast_rad(ms) - float(st.ra)
            x, y, z = itrs_q30(gha, float(st.dec))
            print("    { %2d, %14dLL, %11d, %11d, %11d },"
                  % (index, ms, x, y, z))
    print("};")
    print("#define CROSS_STAR_ROW_COUNT"
          " (sizeof cross_star_rows / sizeof cross_star_rows[0])")
    print("")

    # ---- libastro: GHA Aries (apparent GAST; the library returns mean
    # GMST, so the known ~0.26' equation-of-equinoxes model gap is part
    # of the measured separation, as in --external-check) ----
    print("typedef struct {")
    print("    int64_t ut1_ms;")
    print("    int32_t gha_marcmin; /* apparent GAST as GHA Aries */")
    print("} cross_aries_row_t;")
    print("")
    print("static const cross_aries_row_t cross_aries_rows[] = {")
    for ms in ARIES_EPOCHS_MS:
        gha = round(gast_rad(ms) * MARCMIN_PER_RAD) % TURN_MARCMIN
        print("    { %14dLL, %8d }," % (ms, gha))
    print("};")
    print("#define CROSS_ARIES_ROW_COUNT"
          " (sizeof cross_aries_rows / sizeof cross_aries_rows[0])")
    print("")

    # ---- geometry: Hc/Zn ----
    print("typedef struct {")
    print("    int32_t lat_cdeg, lon_east_cdeg; /* observer */")
    print("    int32_t gha_cdeg, dec_cdeg;      /* body GP */")
    print("    int32_t hc_marcmin;              /* oracle Hc */")
    print("    int32_t zn_marcmin;              /* oracle Zn */")
    print("} cross_reduce_row_t;")
    print("")
    print("static const cross_reduce_row_t cross_reduce_rows[] = {")
    for lat, lon, gha, dec in REDUCE_SCENARIOS:
        obs = sf.LatLonGeocentric(lat / 100.0, lon / 100.0)
        gp = gp_latlon(sf, gha, dec)
        hc = 90.0 - zd_deg(sf, obs, gp)
        zn = sf.get_azimuth(gp, obs)
        print("    { %6d, %6d, %5d, %5d, %8d, %8d },"
              % (lat, lon, gha, dec, round(hc * 60000),
                 round(zn * 60000) % TURN_MARCMIN))
    print("};")
    print("#define CROSS_REDUCE_ROW_COUNT"
          " (sizeof cross_reduce_rows / sizeof cross_reduce_rows[0])")
    print("")

    # ---- geometry: two-body fix ----
    print("typedef struct {")
    print("    int32_t dr_lat_cdeg, dr_lon_cdeg;")
    print("    int32_t gha1_cdeg, dec1_cdeg;")
    print("    int32_t ho1_marcmin;")
    print("    int32_t gha2_cdeg, dec2_cdeg;")
    print("    int32_t ho2_marcmin;")
    print("    int32_t lat_marcmin, lon_marcmin; /* oracle fix */")
    print("} cross_fix_row_t;")
    print("")
    print("static const cross_fix_row_t cross_fix_rows[] = {")
    for (plat, plon, drlat, drlon,
         dec1, gha1, dec2, gha2) in FIX_SCENARIOS:
        p = sf.LatLonGeocentric(plat, plon)
        g1 = gp_latlon(sf, gha1, dec1)
        g2 = gp_latlon(sf, gha2, dec2)
        ho1 = round((90.0 - zd_deg(sf, p, g1)) * 60000)
        ho2 = round((90.0 - zd_deg(sf, p, g2)) * 60000)
        assert 0 < ho1 < 5_400_000 and 0 < ho2 < 5_400_000, \
            (plat, plon, ho1, ho2)
        c1 = sf.Circle(g1, 90.0 - ho1 / 60000.0)
        c2 = sf.Circle(g2, 90.0 - ho2 / 60000.0)
        dr = sf.LatLonGeocentric(drlat / 100.0, drlon / 100.0)
        fix, _fitness, _diag = sf.get_intersections(
            c1, c2, estimated_position=dr)
        assert abs(fix.get_lat() - plat) < 0.2 \
            and abs(sf.mod_lon(fix.get_lon() - plon)) < 0.4, \
            (plat, plon, fix.get_lat(), fix.get_lon())
        print("    { %6d, %6d, %5d, %5d, %7d, %5d, %5d, %7d,"
              " %8d, %9d },"
              % (drlat, drlon, gha1, dec1, ho1, gha2, dec2, ho2,
                 round(fix.get_lat() * 60000),
                 round(sf.mod_lon(fix.get_lon()) * 60000)))
    print("};")
    print("#define CROSS_FIX_ROW_COUNT"
          " (sizeof cross_fix_rows / sizeof cross_fix_rows[0])")


if __name__ == "__main__":
    main()
