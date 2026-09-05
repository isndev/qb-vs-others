#!/usr/bin/env python3
"""The launch census: the instrument for a bimodal cell.

A grid cell is one launch with N repetitions, and a two-core cell on a modern desktop is often
bimodal WITHIN a launch -- the per-repetition sequence alternates between two levels (which core
pair the OS handed out, which cache the lines settled in), and a 7-repetition median lands
wherever the majority fell. Comparing two such medians compares two coin flips.

docs/TUNING.md 9.11 is where this was learned. The census is the answer: launch the same
binaries, the same arguments, N times INTERLEAVED (A, B, A, B, ...), few repetitions per launch,
and read the DISTRIBUTION of per-launch medians -- median-of-medians, min, max -- instead of one
number. Interleaving is what makes host drift land on both sides.

Usage
-----
    python3 tools/launch-census.py --out results/<host>/x/launch-census --no-pin \\
        --bin cand=build/a/bin/qvo-qb-savina-ping-pong --bin ctrl=build/b/bin/qvo-qb-savina-ping-pong \\
        --config 2c-spin,2c-park --launches 12 --repetitions 3 --warmup 1
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

CONFIGS = {
    "1c-spin": {"cores": 1, "wait": 1},
    "1c-park": {"cores": 1, "wait": 0},
    "2c-spin": {"cores": 2, "wait": 1},
    "2c-park": {"cores": 2, "wait": 0},
}


def describe(exe: Path) -> dict:
    out = subprocess.run([str(exe), "--describe"], capture_output=True, text=True, timeout=60)
    if out.returncode != 0:
        sys.exit(f"launch-census.py: {exe} --describe failed")
    return json.loads(out.stdout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--bin", action="append", required=True, help="label=path, repeatable")
    ap.add_argument("--config", default="2c-spin,2c-park")
    ap.add_argument("--launches", type=int, default=10)
    ap.add_argument("--repetitions", type=int, default=3)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--cpus", default=None)
    ap.add_argument("--no-pin", action="store_true")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()
    if args.no_pin == bool(args.cpus):
        sys.exit("launch-census.py: say how to place the run: exactly one of --cpus / --no-pin")

    bins = []
    for it in args.bin:
        label, path = it.split("=", 1)
        p = Path(path)
        if not p.is_file():
            sys.exit(f"launch-census.py: {p} is not a file")
        bins.append((label, p, describe(p)))
    bench = {d["benchmark"] for _, _, d in bins}
    if len(bench) != 1:
        sys.exit(f"launch-census.py: the binaries are not the same benchmark: {bench}")
    bench = bench.pop()
    slug = bench.replace("/", "-")
    args.out.mkdir(parents=True, exist_ok=True)
    cfgs = [c for c in args.config.split(",") if c]

    # label -> cfg -> [per-launch median ns/unit]
    med: dict[str, dict[str, list[float]]] = {l: {c: [] for c in cfgs} for l, _, _ in bins}
    bad = 0
    for cfg in cfgs:
        params = CONFIGS[cfg]
        for i in range(1, args.launches + 1):
            for label, exe, _ in bins:
                dest = args.out / f"{label}__{slug}-{cfg}-launch{i}.json"
                cmd = [str(exe), "--repetitions", str(args.repetitions), "--warmup", str(args.warmup),
                       "--out", str(dest), "--param", f"cores={params['cores']}",
                       "--param", f"wait={params['wait']}"]
                cmd += ["--no-pin"] if args.no_pin else ["--cpus", args.cpus]
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
                if r.returncode != 0 or not dest.exists():
                    bad += 1
                    print(f"  {label:<12} {cfg} launch {i}: FAILED rc={r.returncode}")
                    continue
                d = json.loads(dest.read_text())
                if not d.get("verified"):
                    bad += 1
                    print(f"  {label:<12} {cfg} launch {i}: UNVERIFIED")
                    continue
                med[label][cfg].append(d["summary"]["work_p50"] / d["work_units"])

    print(f"\n{bench}: per-launch medians, ns per unit -- {args.launches} launches interleaved, "
          f"{args.repetitions} rep + {args.warmup} warmup each"
          + (", unpinned" if args.no_pin else f", cpus {args.cpus}"))
    for cfg in cfgs:
        print(f"\n  {cfg}")
        for label, _, _ in bins:
            xs = med[label][cfg]
            if not xs:
                print(f"    {label:<12} no verified launch")
                continue
            xs_s = sorted(xs)
            print(f"    {label:<12} median-of-medians {statistics.median(xs):8.1f}   "
                  f"min {xs_s[0]:8.1f}   max {xs_s[-1]:8.1f}   n={len(xs)}   "
                  f"[{' '.join(f'{x:.0f}' for x in xs)}]")
        if len(bins) == 2:
            a, b = bins[0][0], bins[1][0]
            xa, xb = med[a][cfg], med[b][cfg]
            if xa and xb:
                ma, mb = statistics.median(xa), statistics.median(xb)
                overlap = not (max(xa) < min(xb) or max(xb) < min(xa))
                print(f"    {a} vs {b}: {ma:.1f} vs {mb:.1f} ({(ma / mb - 1) * 100:+.1f}%)"
                      + (" -- distributions OVERLAP: no measurable difference" if overlap
                         else " -- distributions separate"))
    print(f"\nlaunch-census.py: {len(bins) * len(cfgs) * args.launches} launches, {bad} not verified"
          f" -> {args.out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
