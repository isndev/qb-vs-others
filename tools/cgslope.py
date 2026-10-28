#!/usr/bin/env python3
"""cgslope.py -- per-function SELF cost split into intercept and per-responder slope, two sides, from two cgselfdiff JSON dumps
(one at N = n1 responders, one at N = n2).  slope = (c(n2) - c(n1)) / (n2 - n1), intercept = c(n1) - n1 * slope, per side;
printed: delta slope (B - A), the two slopes, delta intercept, the two intercepts, the function.  Sorted by |delta slope|.
  cgslope.py --n 1 4 same1.json same4.json [--min 2]
"""
import argparse, json
ap = argparse.ArgumentParser()
ap.add_argument("--n", nargs=2, type=int, required=True)
ap.add_argument("files", nargs=2)
ap.add_argument("--min", type=float, default=2.0)
ap.add_argument("--width", type=int, default=120)
a = ap.parse_args()
d1 = json.load(open(a.files[0])); d2 = json.load(open(a.files[1])); n1, n2 = a.n
rows = []
for fn in set(d1) | set(d2):
    c1 = d1.get(fn, [0.0, 0.0]); c2 = d2.get(fn, [0.0, 0.0])
    sl = [(c2[i] - c1[i]) / (n2 - n1) for i in (0, 1)]
    ic = [c1[i] - n1 * sl[i] for i in (0, 1)]
    if max(abs(sl[1] - sl[0]), abs(ic[1] - ic[0])) >= a.min:
        rows.append((sl[1] - sl[0], sl[0], sl[1], ic[1] - ic[0], ic[0], ic[1], fn))
rows.sort(key=lambda r: -(abs(r[0]) * 2 + abs(r[3])))
print(f"{'dSlope':>8} {'slopeA':>8} {'slopeB':>8} | {'dIcpt':>8} {'icptA':>8} {'icptB':>8}  function (self Ir: intercept + slope x N)")
tsa = tsb = tia = tib = 0.0
for r in rows:
    print(f"{r[0]:>+8.1f} {r[1]:>8.1f} {r[2]:>8.1f} | {r[3]:>+8.1f} {r[4]:>8.1f} {r[5]:>8.1f}  {r[6][:a.width]}")
for fn in set(d1) | set(d2):
    c1 = d1.get(fn, [0.0, 0.0]); c2 = d2.get(fn, [0.0, 0.0])
    sa = (c2[0] - c1[0]) / (n2 - n1); sb = (c2[1] - c1[1]) / (n2 - n1)
    tsa += sa; tsb += sb; tia += c1[0] - n1 * sa; tib += c1[1] - n1 * sb
print(f"{tsb - tsa:>+8.1f} {tsa:>8.1f} {tsb:>8.1f} | {tib - tia:>+8.1f} {tia:>8.1f} {tib:>8.1f}  TOTAL over every annotated function")
