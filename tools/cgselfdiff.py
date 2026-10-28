#!/usr/bin/env python3
"""cgselfdiff.py -- the SELF (exclusive) instruction cost per iteration, per function, differenced two ways.

For each side, two callgrind runs at a small and a large iteration count: self Ir per iteration of a function is
(large - small) / iters (the process start and stop cancel out).  Then the two sides are joined on a normalised function
name and the per-iteration delta (side B - side A) is printed, largest first, with the two per-iteration values.

  cgselfdiff.py --iters N --a A_small.out A_large.out --b B_small.out B_large.out [--min 5] [--map OLD=NEW ...]

Inputs are the raw callgrind output files; callgrind_annotate (threshold 100, no inclusive) does the reading.
"""
import argparse
import re
import subprocess
import sys

LINE = re.compile(r"^\s*([\d,]+)\s+\(\s*[\d.]+%\)\s+(?:\[[^\]]*\]\s+)?(\S.*)$")


def annotate(path):
    out = subprocess.run(["callgrind_annotate", "--threshold=100", path], capture_output=True, text=True).stdout
    costs = {}
    body = False
    for ln in out.splitlines():
        if ln.startswith("Ir") and "file:function" in ln:
            body = True
            continue
        if not body:
            continue
        m = LINE.match(ln)
        if not m:
            continue
        ir = int(m.group(1).replace(",", ""))
        fn = m.group(2).strip()
        fn = re.sub(r"^\S*?:", "", fn, count=1) if fn.startswith("???:") else fn
        fn = re.sub(r"\s*\[[^\]]*\]$", "", fn)  # callgrind_annotate's trailing [binary path]
        costs[fn] = costs.get(fn, 0) + ir
    return costs


def per_iter(small, large, iters):
    a, b = annotate(small), annotate(large)
    return {fn: (b.get(fn, 0) - a.get(fn, 0)) / iters for fn in set(a) | set(b)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, required=True)
    ap.add_argument("--a", nargs=2, required=True)
    ap.add_argument("--b", nargs=2, required=True)
    ap.add_argument("--min", type=float, default=3.0, help="hide |delta| below this many Ir/iter")
    ap.add_argument("--map", nargs="*", default=[], help="OLD=NEW substring rewrites applied to both sides' names")
    ap.add_argument("--width", type=int, default=150)
    ap.add_argument("--json", help="also dump {function: [A, B]} per-iteration self costs (every function) to this file")
    args = ap.parse_args()
    maps = [m.split("=", 1) for m in args.map]

    def norm(fn):
        for old, new in maps:
            fn = fn.replace(old, new)
        return fn

    def fold(d):
        out = {}
        for fn, v in d.items():
            k = norm(fn)
            out[k] = out.get(k, 0.0) + v
        return out

    A = fold(per_iter(args.a[0], args.a[1], args.iters))
    B = fold(per_iter(args.b[0], args.b[1], args.iters))
    if args.json:
        import json
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({fn: [A.get(fn, 0.0), B.get(fn, 0.0)] for fn in set(A) | set(B)}, f, indent=0, sort_keys=True)
    rows = []
    for fn in set(A) | set(B):
        a, b = A.get(fn, 0.0), B.get(fn, 0.0)
        if abs(b - a) >= args.min:
            rows.append((b - a, a, b, fn))
    rows.sort(key=lambda r: -abs(r[0]))
    print(f"{'delta':>9} {'A':>9} {'B':>9}  function (self Ir per iteration)")
    for d, a, b, fn in rows:
        print(f"{d:>+9.1f} {a:>9.1f} {b:>9.1f}  {fn[: args.width]}")
    ta, tb = sum(A.values()), sum(B.values())
    print(f"{tb - ta:>+9.1f} {ta:>9.1f} {tb:>9.1f}  TOTAL (sum of self costs over the annotated functions)")


if __name__ == "__main__":
    sys.exit(main())
