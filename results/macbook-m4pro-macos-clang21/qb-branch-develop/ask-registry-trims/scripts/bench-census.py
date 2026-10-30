#!/usr/bin/env python3
"""QB-1171: the dev/bench ask round trips read as a placement census -- per family directory (dev/bench/ab.py run
output: results/run-<n>-<ctl|cand>-<copy>-<binary>.json), each cell's median real_time per side over every run
(the per-run value is Google Benchmark's median aggregate over --reps), the ratio cand/ctl, a bootstrap 95 % CI
(4000 resamples, seed 1171), then one reading across the families by the x86 census's rules (ROBUST+/-, PLACEMENT,
LEANS+/-, LEVEL; + = trim 2 faster). Usage: bench-census.py <dir> <family-dir> [<family-dir> ...]"""
import glob
import json
import os
import random
import re
import statistics
import sys

sys.dont_write_bytecode = True


def load(d):
    vals = {}
    for f in glob.glob(os.path.join(d, "results", "run-*.json")):
        m = re.search(r"run-\d+-(ctl|cand)-\d+-", os.path.basename(f))
        for bm in json.load(open(f))["benchmarks"]:
            if bm.get("aggregate_name") == "median":
                vals.setdefault((bm["run_name"], m.group(1)), []).append(bm["real_time"])
    return vals


def main():
    root, fams = sys.argv[1], sys.argv[2:]
    rng = random.Random(1171)
    cells = {}
    for fam in fams:
        v = load(os.path.join(root, fam))
        for (cell, side) in v:
            if side != "ctl":
                continue
            a, b = v[(cell, "ctl")], v[(cell, "cand")]
            rs = sorted(statistics.median([rng.choice(b) for _ in b]) / statistics.median([rng.choice(a) for _ in a]) for _ in range(4000))
            cells.setdefault(cell, []).append((fam, statistics.median(b) / statistics.median(a), rs[100], rs[3899],
                                               statistics.median(a), statistics.median(b), len(a)))
    for cell in sorted(cells):
        v = cells[cell]
        faster = [x for x in v if x[3] < 1]
        slower = [x for x in v if x[2] > 1]
        ratios = [x[1] for x in v]
        if faster and slower:
            reading = "PLACEMENT"
        elif faster:
            reading = "ROBUST+" if all(r < 1 for r in ratios) else "LEANS+"
        elif slower:
            reading = "ROBUST-" if all(r > 1 for r in ratios) else "LEANS-"
        else:
            reading = "LEVEL"
        print(f"{cell}: median of {len(v)} family ratios {statistics.median(ratios):.3f}  {reading}")
        for fam, r, lo, hi, a, b, n in v:
            print(f"   {fam:<8} {a:8.3f} -> {b:8.3f} ms  ratio {r:.4f} [{lo:.4f}, {hi:.4f}]  n={n} runs a side")
    return 0


if __name__ == "__main__":
    sys.exit(main())
