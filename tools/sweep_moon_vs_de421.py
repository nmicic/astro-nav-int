#!/usr/bin/env python3
"""Broad Moon sweep: the SHIPPED binary vs Skyfield/DE421, 1900-2053.

This reproduces the accuracy claim astro_nav.h makes for the Moon core
("good to about 0.3 arcmin earth-fixed across the DE421-covered
1900-2053 part of the library's time domain"): a 37,048-instant
sweep of `./sight_reduction --moon`
against Skyfield's DE421 apparent places. It is the OFFLINE, broad
companion to the committed in-repo gate (`--ephemeris-check`, 47 Moon
epochs asserted at 0.50'): same contract as the other tools here --
Python and DE421 are needed only to recompute truth; nothing at build
or test time depends on this file.

  python3 -m venv venv
  venv/bin/pip install skyfield==1.54 numpy==2.5.1
  make
  venv/bin/python tools/sweep_moon_vs_de421.py [./sight_reduction]

(Uses the cached tools/skyfield-cache/de421.bsp, shared with
make_ephemeris_reference.py. No downloads if the cache is present.
Runtime is dominated by ~37k subprocess spawns; expect a few minutes.)

INSTANTS (deterministic, seed 20260718): 25,000 uniform over the
intersection of DE421's span and the library's +-100 yr domain
(JD_TT 2415025..2471180, ~1900.0 .. 2053.77 -- DE421 itself ends
2053-10-09), 10,000 uniform over 1990-2050, 2,000 uniform over
calendar 2026, and up to 48 targeted epochs (8 each: perigee, apogee,
standstill, node crossing, new moon, full moon) found by an hourly
scan of 2025.0-2027.5. Total 37,048 with the pinned kernel.

TRUTH (per instant t = ts.tt_jd(jd)):
  direction  earth.at(t).observe(moon).apparent() in the ITRS frame --
             the apparent earth-fixed place, the quantity
             astro_nav_moon_earthfixed() approximates;
  distance   (moon - earth).at(t) GEOMETRIC center distance in km
             (deliberately not .observe(): light-time displaces the
             apparent direction but the SD/HP an observer measures
             follow the instantaneous distance);
  SD / HP    asin(1737.4 / dist), asin(6378.137 / dist) -- the same
             radii the library documents.

MEASURED against the C side, parsed at full precision from --moon
output (the Q2.30 earth-fixed vector and the exact milli-arcmin SD/HP;
distance is printed in whole km, so up to 0.5 km of the distance
column is print quantization).

TIME: each instant's UT1 comes from Skyfield's timescale at that TT
(its bundled Delta-T tables, extrapolated beyond them), and the binary
receives (ut1_ms, tt_minus_ut1_ms) exactly per the library's
caller-supplied-time contract; both sides use the same t.

EXPECTED (the committed claim, measured with skyfield 1.54,
numpy 2.5.1, and the pinned de421.bsp): earth-fixed worst ~275
marcmin (~0.27'), median ~100 (~0.10'); distance worst ~13 km; HP
worst ~2.4 marcmin; SD worst ~1.0 marcmin. The script GATES these
(direction worst 300 / median 120 marcmin, distance 15 km, HP 3.0,
SD 1.5 marcmin) and exits nonzero if any is exceeded. Numbers drift
only if the ephemeris code, the kernel, or skyfield's Delta-T model
change.
"""

import math
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from skyfield.api import Loader
from skyfield.framelib import itrs

J2000 = 2451545.0
MS_PER_DAY = 86_400_000.0
SEED = 20260718
R_EARTH_KM = 6378.137
R_MOON_KM = 1737.4
MARCMIN_PER_RAD = 60_000.0 * 180.0 / math.pi

FULL_LO, FULL_HI = 2415025.0, 2471180.0   # ~1900.0 .. 2053.77 TT
MOD_LO, MOD_HI = 2447893.0, 2469808.0     # 1990-01-01 .. 2050-01-01
Y2026_LO, Y2026_HI = 2461041.5, 2461406.5  # calendar 2026


def pick_targeted(ts, earth, moon, sun):
    """Hourly scan 2025.0-2027.5; targeted JD_TT epochs by category."""
    jd = np.arange(2460676.5, 2461589.5, 1.0 / 24.0)
    t = ts.tt_jd(jd)
    g = (moon - earth).at(t)
    dist = g.distance().km
    lat, lon, _ = g.ecliptic_latlon()
    beta = lat.degrees
    dec = g.radec()[1].degrees
    _, slon, _ = earth.at(t).observe(sun).apparent().ecliptic_latlon()
    elong = np.abs(((lon.degrees - slon.degrees + 180.0) % 360.0) - 180.0)

    def local_min(a):
        return np.where((a[1:-1] < a[:-2]) & (a[1:-1] < a[2:]))[0] + 1

    def local_max(a):
        return np.where((a[1:-1] > a[:-2]) & (a[1:-1] > a[2:]))[0] + 1

    out = {}
    peri = local_min(dist)
    out["perigee"] = jd[peri[np.argsort(dist[peri])[:8]]]
    apo = local_max(dist)
    out["apogee"] = jd[apo[np.argsort(dist[apo])[-8:]]]
    st = local_max(np.abs(dec))
    out["standstill"] = jd[st[np.argsort(np.abs(dec)[st])[-8:]]]
    nb = local_min(np.abs(beta))
    nb = nb[np.abs(beta)[nb] < 0.05]
    out["node"] = jd[nb[:8]]
    nm = local_min(elong)
    nm = nm[elong[nm] < 0.6]
    out["newmoon"] = jd[nm[:8]]
    fm = local_max(elong)
    fm = fm[elong[fm] > 179.4]
    out["fullmoon"] = jd[fm[:8]]
    return out


VEC_RE = re.compile(
    r"earth-fixed vector: \((-?\d+), (-?\d+), (-?\d+)\)/2\^30")
DIST_RE = re.compile(r"distance:\s+(\d+) km")
SDHP_RE = re.compile(
    r"SD:\s+[\d.]+' \((\d+) milli-arcmin\)\s+HP:\s+[\d.]+'"
    r" \((\d+) milli-arcmin\)")


def run_moon(binary, ut1_ms, ttm_ms):
    """One --moon invocation -> (unit vec, dist km, sd, hp marcmin)."""
    out = subprocess.run(
        [binary, "--moon", str(ut1_ms), str(ttm_ms)],
        capture_output=True, text=True, check=True).stdout
    vx, vy, vz = (int(g) for g in VEC_RE.search(out).groups())
    dist = int(DIST_RE.search(out).group(1))
    sd, hp = (int(g) for g in SDHP_RE.search(out).groups())
    q = float(1 << 30)
    v = np.array([vx, vy, vz], float) / q
    return v / np.linalg.norm(v), dist, sd, hp


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./sight_reduction"

    load = Loader("tools/skyfield-cache")
    ts = load.timescale()
    eph = load("de421.bsp")
    earth, moon, sun = eph["earth"], eph["moon"], eph["sun"]

    rng = np.random.default_rng(SEED)
    jd_list, cat_list = [], []

    def add(jds, name):
        jds = np.atleast_1d(np.asarray(jds, float))
        jd_list.append(jds)
        cat_list.extend([name] * len(jds))

    add(rng.uniform(FULL_LO, FULL_HI, 25000), "full")
    add(rng.uniform(MOD_LO, MOD_HI, 10000), "modern")
    add(rng.uniform(Y2026_LO, Y2026_HI, 2000), "y2026")
    for name, jds in pick_targeted(ts, earth, moon, sun).items():
        if len(jds):
            add(jds, name)
    jd = np.concatenate(jd_list)
    cat = np.array(cat_list)
    print("sweep: %d instants (%s)" % (len(jd), ", ".join(
        "%s %d" % (n, np.sum(cat == n)) for n in dict.fromkeys(cat_list))))

    t = ts.tt_jd(jd)
    print("computing DE421 truth...")
    x, y, z = earth.at(t).observe(moon).apparent().frame_xyz(itrs).au
    truth = np.stack([x, y, z], axis=1)
    truth /= np.linalg.norm(truth, axis=1)[:, None]
    truth_km = (moon - earth).at(t).distance().km

    tt_ms = np.rint((jd - J2000) * MS_PER_DAY).astype(np.int64)
    ut1_ms = np.rint((t.ut1 - J2000) * MS_PER_DAY).astype(np.int64)
    ttm = tt_ms - ut1_ms
    assert np.all(np.abs(ttm) <= 600_000), "Delta-T outside library domain"

    print("running %s --moon per instant..." % binary)
    results = [None] * len(jd)

    def work(i):
        results[i] = run_moon(binary, ut1_ms[i], ttm[i])

    with ThreadPoolExecutor() as pool:
        for n, _ in enumerate(pool.map(work, range(len(jd))), 1):
            if n % 5000 == 0:
                print("  %d / %d" % (n, len(jd)))

    ours = np.array([r[0] for r in results])
    ours_km = np.array([r[1] for r in results], float)
    ours_sd = np.array([r[2] for r in results], float)
    ours_hp = np.array([r[3] for r in results], float)

    dots = np.clip(np.sum(ours * truth, axis=1), -1.0, 1.0)
    err_dir = np.arccos(dots) * MARCMIN_PER_RAD
    err_km = np.abs(ours_km - truth_km)
    err_sd = np.abs(ours_sd -
                    np.arcsin(R_MOON_KM / truth_km) * MARCMIN_PER_RAD)
    err_hp = np.abs(ours_hp -
                    np.arcsin(R_EARTH_KM / truth_km) * MARCMIN_PER_RAD)

    print("\n%s vs Skyfield/DE421 apparent, %d instants" % (binary,
                                                            len(jd)))
    for name, m in [("full-span 1900-2053", cat == "full"),
                    ("modern 1990-2050", cat == "modern"),
                    ("ALL", np.ones(len(jd), bool))]:
        print("  %-20s dir worst %6.1f  p99 %6.1f  median %6.1f"
              "  [marcmin]" % (name, err_dir[m].max(),
                              np.percentile(err_dir[m], 99),
                              np.median(err_dir[m])))
    print("  distance worst %.1f km (whole-km print quantization"
          " included)" % err_km.max())
    print("  HP worst %.2f marcmin   SD worst %.2f marcmin"
          % (err_hp.max(), err_sd.max()))

    # The committed claim, enforced (astro_nav.h says "about 0.3'";
    # the README quotes worst ~0.27' / median ~0.10', ~13 km, HP ~2.4,
    # SD ~1.1). Bounds carry just enough headroom over the measured
    # values to absorb print quantization, not enough to let the model
    # quietly degrade: a trip means the ephemeris code, the kernel, or
    # the pinned environment changed -- fix the regression or
    # re-measure and update the documented claim alongside the gate.
    gates = [
        ("direction worst [marcmin]", float(err_dir.max()), 300.0),
        ("direction median [marcmin]", float(np.median(err_dir)), 120.0),
        ("distance worst [km]", float(err_km.max()), 15.0),
        ("HP worst [marcmin]", float(err_hp.max()), 3.0),
        ("SD worst [marcmin]", float(err_sd.max()), 1.5),
    ]
    failed = 0
    for name, value, bound in gates:
        ok = value <= bound
        failed += not ok
        print("  gate %-28s %8.2f <= %6.1f  %s"
              % (name, value, bound, "ok" if ok else "FAIL"))
    if failed:
        print("SWEEP FAIL: %d gate(s) exceeded" % failed)
        return 1
    print("SWEEP OK: all gates hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
