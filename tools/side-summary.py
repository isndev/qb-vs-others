#!/usr/bin/env python3
"""side-summary.py -- the A/B reading of a launch census whose labels are `<side>-c<k>` (physical copies of two or more
sides, e.g. ctl-c1..3 and var-c1..3, as qb181-census-*.sh launch them). Per configuration and side: the pooled per-launch
medians (ns per unit = summary.work_p50 / work_units), their median and IQR, the copies' medians and band (half their
spread), and the two-cluster split with the share of launches in each mode; then every other side against the first with
a bootstrap 95 % CI of the ratio of pooled medians and a verdict against the wider copy band (SLOWER / FASTER only when
the CI excludes 1 AND the move exceeds the band). Also the share of launches in the low mode per side, when both sides
are bimodal around the same two centres: the "pass share" QB-1020 asked for.

  side-summary.py CENSUS_DIR [CENSUS_DIR ...]
"""
import glob
import json
import os
import random
import re
import statistics
import sys

NAME = re.compile(r"(?P<side>[a-z]+)-c(?P<copy>\d+)__(?P<slug>.+)-(?P<cfg>[12]c-(?:spin|park))-launch(?P<n>\d+)\.json$")
rng = random.Random(7102)


def split2(xs):
    if len(xs) < 4:
        return None
    lo, hi = min(xs), max(xs)
    for _ in range(50):
        a = [x for x in xs if abs(x - lo) <= abs(x - hi)]
        b = [x for x in xs if abs(x - lo) > abs(x - hi)]
        if not a or not b:
            return None
        nlo, nhi = statistics.mean(a), statistics.mean(b)
        if nlo == lo and nhi == hi:
            break
        lo, hi = nlo, nhi
    within = statistics.pstdev(a + [x - (hi - lo) for x in b])
    sep = (hi - lo) / within if within > 0 else float("inf")
    return lo, hi, len(a), len(b), sep


def boot(x, y, iters=4000):
    rs = sorted(statistics.median([rng.choice(y) for _ in y]) / statistics.median([rng.choice(x) for _ in x]) for _ in range(iters))
    return rs[int(0.025 * iters)], rs[int(0.975 * iters)]


for d in sys.argv[1:]:
    cells = {}  # cfg -> side -> copy -> [ns]
    bench = None
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        m = NAME.search(os.path.basename(f))
        if not m:
            continue
        doc = json.load(open(f, encoding="utf-8"))
        if not doc.get("verified"):
            continue
        bench = doc.get("benchmark", bench)
        cells.setdefault(m.group("cfg"), {}).setdefault(m.group("side"), {}).setdefault(int(m.group("copy")), []).append(
            doc["summary"]["work_p50"] / doc["work_units"])
    print(f"\n== {os.path.basename(d.rstrip('/'))}  ({bench})")
    for cfg, sides in sorted(cells.items()):
        print(f"  {cfg}")
        stats = {}
        for side, copies in sides.items():
            pooled = sorted(x for xs in copies.values() for x in xs)
            cm = [statistics.median(copies[k]) for k in sorted(copies)]
            band = (max(cm) - min(cm)) / statistics.median(cm) * 100 / 2 if len(cm) > 1 else 0.0
            q = statistics.quantiles(pooled, n=4)
            s = split2(pooled)
            modes = ""
            if s and s[4] >= 3 and s[2] >= 3 and s[3] >= 3:
                modes = f"  modes {s[0]:.1f} x{s[2]} / {s[1]:.1f} x{s[3]} (+{(s[1] / s[0] - 1) * 100:.1f} %, low-mode share {s[2] / len(pooled) * 100:.0f} %)"
            stats[side] = (statistics.median(pooled), pooled, band, s)
            print(f"    {side:<4} median {statistics.median(pooled):9.1f}  IQR [{q[0]:.1f}, {q[2]:.1f}]  min {pooled[0]:.1f}  max {pooled[-1]:.1f}  n={len(pooled)}"
                  f"  copies {' '.join(f'{x:.1f}' for x in cm)}  band +-{band:.2f}%{modes}")
        names = list(stats)
        for cand in names[1:]:
            ctl = names[0]
            ratio = stats[cand][0] / stats[ctl][0]
            lo, hi = boot(stats[ctl][1], stats[cand][1])
            band = max(stats[ctl][2], stats[cand][2])
            verdict = "SLOWER" if lo > 1 and (ratio - 1) * 100 > band else "FASTER" if hi < 1 and (1 - ratio) * 100 > band else "LEVEL"
            print(f"    {cand}/{ctl}: ratio {ratio:.4f}  CI95 [{lo:.4f}, {hi:.4f}]  copy band +-{band:.2f}%  -> {verdict}")
