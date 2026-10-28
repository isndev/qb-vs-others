#!/usr/bin/env python3
"""regime-summary.py -- read a launch census (tools/launch-census.py's documents) per family, configuration and PHYSICAL
COPY, and say what kind of spread each cell has (Huly QB-1020, the x86 half):

  * per copy: the per-launch medians (ns per unit = summary.work_p50 / work_units, the census's own figure), their median,
    min, max, IQR, and a two-cluster split (1-D k-means from the two extremes): the two centres, their counts, and the
    separation = (centre gap) / (pooled within-cluster spread); a cell is called BIMODAL when the separation is >= 3 and
    each cluster holds >= 3 of the launches, UNIMODAL otherwise;
  * per configuration: the copies' medians and their spread (the copy band, as an A/A), and the pooled distribution;
  * the verdict per configuration: "launch" when every copy is bimodal with centres within 2 % of each other across
    copies (the regime is a property of the launch), "copy" when the copies are unimodal but their medians sit apart by
    more than their own IQR (the regime is a property of the copy), "flat" when neither.

  regime-summary.py CENSUS_DIR [CENSUS_DIR ...]      (one directory per family, holding <label>__<slug>-<cfg>-launchN.json)
"""
import glob
import json
import os
import re
import statistics
import sys

NAME = re.compile(r"(?P<label>[^_]+)__(?P<slug>.+)-(?P<cfg>[12]c-(?:spin|park))-launch(?P<n>\d+)\.json$")


def split2(xs):
    """1-D two-means from the extremes; returns (lo_centre, hi_centre, lo_count, hi_count, separation)."""
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
    within = statistics.pstdev(a + [x - (hi - lo) for x in b]) if len(a) + len(b) > 1 else 0.0
    sep = (hi - lo) / within if within > 0 else float("inf")
    return lo, hi, len(a), len(b), sep


def quartiles(xs):
    q = statistics.quantiles(xs, n=4)
    return q[0], q[2]


for d in sys.argv[1:]:
    cells = {}  # cfg -> label -> [per-launch ns/unit]
    bench = None
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        m = NAME.search(os.path.basename(f))
        if not m:
            continue
        doc = json.load(open(f, encoding="utf-8"))
        if not doc.get("verified"):
            continue
        bench = doc.get("benchmark", bench)
        cells.setdefault(m.group("cfg"), {}).setdefault(m.group("label"), []).append(doc["summary"]["work_p50"] / doc["work_units"])
    print(f"\n== {os.path.basename(d.rstrip('/'))}  ({bench}, {sum(len(v) for c in cells.values() for v in c.values())} verified launches)")
    for cfg, copies in sorted(cells.items()):
        print(f"  {cfg}")
        modal = {}
        for label, xs in sorted(copies.items()):
            xs = sorted(xs)
            q1, q3 = quartiles(xs)
            s = split2(xs)
            if s and s[4] >= 3 and s[2] >= 3 and s[3] >= 3:
                kind = "BIMODAL"
                detail = f"modes {s[0]:.1f} x{s[2]} / {s[1]:.1f} x{s[3]}  (+{(s[1] / s[0] - 1) * 100:.1f} %, separation {s[4]:.1f})"
            else:
                kind = "unimodal"
                detail = f"(largest gap {max(b - a for a, b in zip(xs, xs[1:])):.1f} ns)"
            modal[label] = (kind, s, statistics.median(xs), q1, q3)
            print(f"    {label:<4} median {statistics.median(xs):9.1f}  IQR [{q1:.1f}, {q3:.1f}]  min {xs[0]:.1f}  max {xs[-1]:.1f}  n={len(xs)}  {kind} {detail}")
        meds = [v[2] for v in modal.values()]
        band = (max(meds) - min(meds)) / statistics.median(meds) * 100 / 2 if len(meds) > 1 else 0.0
        pooled = sorted(x for xs in copies.values() for x in xs)
        pq1, pq3 = quartiles(pooled)
        all_bimodal = all(v[0] == "BIMODAL" for v in modal.values())
        centres_agree = all_bimodal and max(abs(v[1][0] - w[1][0]) / w[1][0] for v in modal.values() for w in modal.values()) < 0.02
        copies_apart = (not any(v[0] == "BIMODAL" for v in modal.values())) and len(meds) > 1 and (max(meds) - min(meds)) > max(v[4] - v[3] for v in modal.values())
        verdict = "LAUNCH regime (two modes inside every copy, the same two across copies)" if centres_agree else \
                  "COPY regime (unimodal copies sitting apart by more than their IQR)" if copies_apart else \
                  "flat (no two modes, copies within their IQR)" if not any(v[0] == "BIMODAL" for v in modal.values()) else \
                  "mixed (two modes in some copies only)"
        print(f"    pooled median {statistics.median(pooled):9.1f}  IQR [{pq1:.1f}, {pq3:.1f}] ({(pq3 - pq1) / statistics.median(pooled) * 100:.1f} % wide)  copy band +-{band:.2f}%  -> {verdict}")
