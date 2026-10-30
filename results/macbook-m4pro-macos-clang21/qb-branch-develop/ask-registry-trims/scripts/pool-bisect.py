#!/usr/bin/env python3
"""QB-1171 point 4: pool the three N = 1 bisection passes (bisect-n1-{A,B,C}-{same,cross}/launches.tsv): each side's
median over its 72 launches, the per-pass medians, and the ratio over shipped 3.3.0 with a bootstrap 95 % CI
(4000 resamples, seed 1171). Usage: pool-bisect.py <ask-registry-trims dir>  (writes nothing; bisect-n1-pooled.txt
is its stdout)."""
import csv
import random
import statistics
import sys

sys.dont_write_bytecode = True
R = sys.argv[1]
print("QB-1171 point 4 -- the N = 1 bisection on the M4 Pro, three passes pooled (A seed 7101, B 7202, C 7303; 3 copies x 8 rounds a pass, 200 000 asks a launch, --no-pin)")
print("ns per ask_any: each side's median over 72 launches; per-pass medians; the ratio over shipped 3.3.0 with a bootstrap 95 % CI (4000 resamples, seed 1171)")
rng = random.Random(1171)
for mode in ("same", "cross"):
    data, per = {}, {}
    for p in "ABC":
        for row in csv.DictReader(open(f"{R}/bisect-n1-{p}-{mode}/launches.tsv"), delimiter="\t"):
            data.setdefault(row["side"], []).append(float(row["ns_per_ask"]))
            per.setdefault((row["side"], p), []).append(float(row["ns_per_ask"]))
    print(f"{mode} N = 1")
    ctl = data["v330"]
    for s in ("v330", "c16f", "b67a", "tip"):
        v = data[s]
        m = statistics.median(v)
        line = f"   {s:<5} {m:7.1f} ns  n={len(v)}  passes " + " / ".join(f"{statistics.median(per[(s, p)]):.1f}" for p in "ABC")
        if s != "v330":
            rs = sorted(statistics.median([rng.choice(v) for _ in v]) / statistics.median([rng.choice(ctl) for _ in ctl]) for _ in range(4000))
            line += f"   ratio {m / statistics.median(ctl):.4f} CI95 [{rs[100]:.4f}, {rs[3899]:.4f}]"
        print(line)
