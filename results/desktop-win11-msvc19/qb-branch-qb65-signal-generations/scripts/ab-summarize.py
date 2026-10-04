"""Summarise an interleaved ctl/cand census: <dir>/<bench>-<p1>-<p2>@<tag>@<round>.json -> per cell the median of the
rounds' work_p50/work_units (ns per unit), the delta, and both [min..max]. A second arg names a probe log
("<tag> <probe> <mode> ... ns_per_pass=X" lines) to summarise the same way."""
import json, glob, collections, statistics, sys, os, re

def census(R):
    d = collections.defaultdict(list)
    for f in sorted(glob.glob(os.path.join(R, "*.json"))):
        n = os.path.basename(f)[:-5]
        try:
            bench, tag, _ = n.split("@")
        except ValueError:
            continue
        j = json.load(open(f, encoding="utf-8"))
        if j.get("verified") is False:
            print("UNVERIFIED", n)
            continue
        d[(bench, tag)].append(j["summary"]["work_p50"] / j["work_units"])
    print(f"{'cell':30s} {'ctl':>8s} {'cand':>8s} {'delta':>7s}   ctl [min..max] / cand [min..max]  (ns per unit, median of rounds)")
    for bench in sorted({k[0] for k in d}):
        c = sorted(d[(bench, "ctl")]); k = sorted(d[(bench, "cand")])
        if not c or not k:
            continue
        mc = statistics.median(c); mk = statistics.median(k)
        print(f"{bench:30s} {mc:8.1f} {mk:8.1f} {(mk - mc) / mc * 100:+6.1f}%   [{c[0]:.1f}..{c[-1]:.1f}] / [{k[0]:.1f}..{k[-1]:.1f}]")

def probes(path):
    d = collections.defaultdict(list)
    for ln in open(path, encoding="utf-8", errors="replace"):
        p = ln.split()
        m = re.search(r"ns_per_(?:pass|trip)=([0-9.]+)", ln)
        if len(p) >= 3 and m:
            d[(p[1] + " " + p[2], p[0])].append(float(m.group(1)))
    print("%-16s %8s %8s %7s   ctl [min..max] / cand [min..max]" % ("probe", "ctl", "cand", "delta"))
    for probe in sorted({k[0] for k in d}):
        c = sorted(d[(probe, "ctl")]); k = sorted(d[(probe, "cand")])
        mc = statistics.median(c); mk = statistics.median(k)
        print("%-16s %8.2f %8.2f %+6.1f%%   [%.2f..%.2f] / [%.2f..%.2f]" % (probe, mc, mk, (mk - mc) / mc * 100, c[0], c[-1], k[0], k[-1]))

if __name__ == "__main__":
    census(sys.argv[1])
    if len(sys.argv) > 2:
        print()
        probes(sys.argv[2])
