#!/usr/bin/env python3
"""QB-1171 placement census on the M4, read the way the pilot's x86 census was (Huly QB-1171, 2026-10-28 13:20):
per cell, every family's ratio trim2/develop with its bootstrap CI, the median of the families' ratios and ONE reading:
  ROBUST+ / ROBUST-  every family on the same side of 1 and at least one CI excluding 1 (+ = faster, - = slower)
  PLACEMENT          CIs excluding 1 on BOTH sides
  LEANS+ / LEANS-    CIs excluding 1 on one side only, but a family's ratio on the other side
  LEVEL              no CI excludes 1
Usage: census.py <results-dir> <family> [<family> ...]   (reads <dir>/<family>-{same,cross}/summary.json)"""
import json
import statistics
import sys
from pathlib import Path

sys.dont_write_bytecode = True


def main():
    root, fams = Path(sys.argv[1]), sys.argv[2:]
    cells = {}
    for fam in fams:
        for mode in ("same", "cross"):
            p = root / f"{fam}-{mode}" / "summary.json"
            if not p.exists():
                print(f"missing {p}")
                continue
            for row in json.loads(p.read_text()):
                if "N" not in row or "ratio" not in row:
                    continue
                r = row["ratio"]["trim2"]
                cells.setdefault((mode, row["N"]), []).append(
                    (fam, r["value"], r["ci95"][0], r["ci95"][1], row["develop"]["median"], row["trim2"]["median"], r["verdict"]))
    print(f"{'cell':<10} {'median':>7}  reading     families (ratio [CI95]  develop -> trim2 ns, scatter-ab verdict)")
    out = []
    for (mode, n) in sorted(cells, key=lambda c: (c[0] != "same", c[1])):
        v = cells[(mode, n)]
        ratios = [x[1] for x in v]
        faster = [x for x in v if x[3] < 1]   # CI wholly below 1: trim2 faster
        slower = [x for x in v if x[2] > 1]   # CI wholly above 1: trim2 slower
        if faster and slower:
            reading = "PLACEMENT"
        elif faster:
            reading = "ROBUST+" if all(r < 1 for r in ratios) else "LEANS+"
        elif slower:
            reading = "ROBUST-" if all(r > 1 for r in ratios) else "LEANS-"
        else:
            reading = "LEVEL"
        med = statistics.median(ratios)
        fam_txt = "; ".join(f"{f} {r:.3f} [{lo:.3f}, {hi:.3f}] {d:.1f}->{t:.1f} {vd}" for f, r, lo, hi, d, t, vd in v)
        print(f"{mode + ' ' + str(n):<10} {med:7.3f}  {reading:<10}  {fam_txt}")
        out.append({"mode": mode, "N": n, "median_ratio": med, "reading": reading, "families": [
            {"family": f, "ratio": r, "ci95": [lo, hi], "develop_ns": d, "trim2_ns": t, "verdict": vd} for f, r, lo, hi, d, t, vd in v]})
    (root / "census.json").write_text(json.dumps(out, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
