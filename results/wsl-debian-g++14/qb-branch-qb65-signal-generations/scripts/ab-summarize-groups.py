"""Summarise a multi-copy census: <dir>/<cell>@<tag>@<round>.json where tags are <group>_<copy> (ctl_1..ctl_4,
cand_1..cand_4: byte-identical binaries in different files, so different physical pages). Per cell: each copy's
median over rounds (the physical-placement spread shows as the spread across copies of ONE group), the group's
median of copy medians, and the delta between groups -- with, as its yardstick, the spread inside the control group."""
import json, glob, collections, statistics, sys, os

R = sys.argv[1]
d = collections.defaultdict(lambda: collections.defaultdict(list))
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
    d[cell][tag].append(j["summary"]["work_p50"] / j["work_units"])
for cell in sorted(d):
    groups = collections.defaultdict(dict)
    for tag, xs in d[cell].items():
        g, _, c = tag.partition("_")
        groups[g][c] = statistics.median(xs)
    print(cell)
    gm = {}
    for g in sorted(groups, key=lambda g: (g != "ctl", g)):
        meds = groups[g]
        gm[g] = statistics.median(meds.values())
        pooled = statistics.median([x for c in meds for x in d[cell][f"{g}_{c}"]])
        print(f"  {g:6s} copies {' '.join(f'{meds[c]:7.1f}' for c in sorted(meds))}   median-of-copies {gm[g]:7.1f}"
              f"   copy spread {min(meds.values()):.1f}..{max(meds.values()):.1f} ({(max(meds.values()) - min(meds.values())) / gm[g] * 100:.1f} %)"
              f"   pooled {pooled:7.1f}")
    if "ctl" in gm:
        for g in gm:
            if g != "ctl":
                print(f"  {g} vs ctl: {(gm[g] - gm['ctl']) / gm['ctl'] * 100:+.1f} % (median of copy medians)")
    print()
