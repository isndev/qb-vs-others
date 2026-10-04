"""Summarise an interleaved census of any number of tags: <dir>/<cell>@<tag>@<round>.json -> per cell and tag the
median of the rounds' work_p50/work_units (ns per unit), [min..max], the delta against 'ctl', and the count of
rounds in which the tag was slower than ctl in the SAME round (a paired sign count)."""
import json, glob, collections, statistics, sys, os

R = sys.argv[1]
d = collections.defaultdict(dict)
for f in sorted(glob.glob(os.path.join(R, "*.json"))):
    n = os.path.basename(f)[:-5]
    try:
        cell, tag, rnd = n.split("@")
    except ValueError:
        continue
    j = json.load(open(f, encoding="utf-8"))
    if j.get("verified") is False:
        print("UNVERIFIED", n)
        continue
    d[(cell, tag)][int(rnd)] = j["summary"]["work_p50"] / j["work_units"]
tags = sorted({k[1] for k in d}, key=lambda t: (t != "ctl", t))
for cell in sorted({k[0] for k in d}):
    base = d.get((cell, "ctl"), {})
    mc = statistics.median(base.values()) if base else None
    for tag in tags:
        v = d.get((cell, tag))
        if not v:
            continue
        xs = sorted(v.values()); m = statistics.median(xs)
        delta = f"{(m - mc) / mc * 100:+6.1f}%" if mc and tag != "ctl" else "       "
        paired = ""
        if tag != "ctl" and base:
            common = [r for r in v if r in base]
            slower = sum(1 for r in common if v[r] > base[r])
            paired = f"  slower than ctl in {slower}/{len(common)} rounds"
        print(f"{cell:28s} {tag:6s} {m:8.1f} {delta}  [{xs[0]:.1f}..{xs[-1]:.1f}]{paired}")
    print()
