#!/usr/bin/env python3
"""scatter-ab.py -- the per-responder slope of qb::ask_any on a warm engine, two or more qb trees against each other, the
way METHOD.md section 8 asks for (Huly QB-1153): qvoprobe-ask-any-scatter (tools/probes/ask-any-scatter.cpp, docs/TUNING.md
sections 24 and 26) built against a control tree (the first --side: shipped 3.2.1) and one or more candidates (shipped
3.3.0, the branch tip), run from COPIES physical copies of each binary, every (side, copy) once per round for every N in a
SEEDED SHUFFLED order, the process pinned to the first N + 1 CPUs of --pool before the launch (Windows: this driver's own
affinity mask, inherited by the child; Linux: taskset), so that qb's core i lands on a CPU of its own -- a cross run needs
responders + 1 CPUs nothing else uses. On macOS nothing pins (FAIRNESS.md 1.4): pass --no-pin, the only form there, and
the launches are recorded unpinned; a bimodal cell is then read with tools/launch-census.py, as every 2c cell is on the M4.

  scatter-ab.py --out DIR --side v321=EXE --side tip=EXE [--side v330=EXE ...] --pool 0,2,4,6,8,10,12,14,16,18,20
                --responders 1,2,3,4,5,6,7 [--mode cross] [--asks 200000] [--copies 3] [--rounds 8] [--seed 7101]
                [--quiet 30] [--tag TEXT] [--no-pin]

Every launch's line (`responders=N asks=M  X ns/ask_any  (checksum C)`) is parsed and its exit status checked: a non-zero
exit, an unparsable line or a checksum the probe refused is a FUNCTIONAL RED -- the campaign stops, writes RED.txt and
exits 3. Raw launches go to launches.tsv (never aggregates only); summary.txt / summary.json give, per N, each side's
median over rounds x copies, the copies' medians (the A/A band is their spread), each candidate's ratio of medians over
the control with a bootstrap 95 % CI and a verdict against the A/A band, and the per-responder slope of each side (least
squares of the median ns/ask over N) with each candidate's delta.
"""
import argparse
import ctypes
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import time

sys.dont_write_bytecode = True
EXE = ".exe" if os.name == "nt" else ""
LINE = re.compile(r"responders=(\d+) asks=(\d+)\s+([0-9.]+) ns/ask_any\s+\(checksum (\d+)\)")


def set_affinity(cpus):
    """Pin THIS process (the children inherit it). Returns the previous mask on Windows (to restore), None on Linux."""
    if os.name != "nt":
        return None
    k = ctypes.windll.kernel32
    # the pseudo-handle is a 64-bit -1: without restype/argtypes ctypes truncates it (abrun.py's shape)
    k.GetCurrentProcess.restype = ctypes.c_void_p
    k.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    h = k.GetCurrentProcess()
    proc = ctypes.c_size_t()
    sysm = ctypes.c_size_t()
    if not k.GetProcessAffinityMask(h, ctypes.byref(proc), ctypes.byref(sysm)):
        sys.exit(f"GetProcessAffinityMask failed: {ctypes.GetLastError()}")
    mask = 0
    for c in cpus:
        mask |= 1 << c
    if not k.SetProcessAffinityMask(h, ctypes.c_size_t(mask)):
        sys.exit(f"SetProcessAffinityMask({mask:#x}) failed: {ctypes.GetLastError()}")
    return sysm.value


def restore_affinity(prev):
    if os.name == "nt" and prev is not None:
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = ctypes.c_void_p
        k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        k.SetProcessAffinityMask(k.GetCurrentProcess(), ctypes.c_size_t(prev))


def launch(exe, mode, n, asks, cpus, timeout, pin=True):
    argv = [exe, mode, str(n), str(asks)]
    if pin and os.name != "nt":
        argv = ["taskset", "-c", ",".join(str(c) for c in cpus)] + argv
    prev = set_affinity(cpus) if pin else None
    t0 = time.monotonic()
    try:
        r = subprocess.run(argv, cwd=os.path.dirname(exe), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        restore_affinity(prev)
        return None, "TIMEOUT", "", time.monotonic() - t0
    restore_affinity(prev)
    m = LINE.search(r.stdout or "")
    if r.returncode != 0 or not m:
        return None, r.returncode, (r.stderr or r.stdout or "").strip().splitlines()[-1:] or [""], time.monotonic() - t0
    if int(m.group(1)) != n or int(m.group(2)) != asks:
        return None, "MISMATCH", [m.group(0)], time.monotonic() - t0
    return float(m.group(3)), 0, "", time.monotonic() - t0


def bootstrap_ratio(a, b, rng, iters=4000):
    """95 % CI of median(b) / median(a) by paired-free bootstrap over the two samples."""
    rs = []
    for _ in range(iters):
        sa = [rng.choice(a) for _ in a]
        sb = [rng.choice(b) for _ in b]
        rs.append(statistics.median(sb) / statistics.median(sa))
    rs.sort()
    return rs[int(0.025 * iters)], rs[int(0.975 * iters)]


def fit(points):
    n = len(points)
    sx = sum(x for x, _ in points); sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points); sxy = sum(x * y for x, y in points)
    b = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    a = (sy - b * sx) / n
    return a, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--side", action="append", required=True, help="name=path to qvoprobe-ask-any-scatter")
    ap.add_argument("--pool", default="", help="CPUs in launch order; a run at N takes the first N + 1 (required unless --no-pin)")
    ap.add_argument("--responders", required=True)
    ap.add_argument("--mode", default="cross", choices=["cross", "same"])
    ap.add_argument("--asks", type=int, default=200000)
    ap.add_argument("--copies", type=int, default=3)
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--seed", type=int, default=7101)
    ap.add_argument("--quiet", type=int, default=30)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--tag", default="")
    ap.add_argument("--no-pin", action="store_true",
                    help="launch every process unpinned (recorded as pinned:false); the only form on macOS, where "
                         "the affinity API pins nothing (FAIRNESS.md 1.4) and the driver refuses --pool")
    a = ap.parse_args()
    sides = dict(s.split("=", 1) for s in a.side)
    if len(sides) < 2:
        sys.exit("at least two --side are needed: the first is the control, the others the candidates")
    for name, p in sides.items():
        if not os.path.isfile(p):
            sys.exit(f"side {name}: no binary {p}")
    pin = not a.no_pin
    if sys.platform == "darwin" and pin:
        sys.exit("macOS pins nothing: run with --no-pin (FAIRNESS.md 1.4), and read a bimodal cell with tools/launch-census.py")
    pool = [int(c) for c in a.pool.split(",") if c]
    ns = [int(x) for x in a.responders.split(",") if x]
    if pin and not pool:
        sys.exit("--pool is required unless --no-pin")
    if pin and a.mode == "cross" and max(ns) + 1 > len(pool):
        sys.exit(f"--pool has {len(pool)} CPUs, a cross run at N={max(ns)} needs {max(ns) + 1}")
    if not pin:
        pool = []  # nothing is pinned: every launch records its cpus as "unpinned"
    os.makedirs(a.out, exist_ok=True)
    # physical copies: separate files => separate page placement
    copies = {}
    for name, p in sides.items():
        for c in range(1, a.copies + 1):
            d = os.path.join(a.out, "copies", f"{name}-{c}")
            os.makedirs(d, exist_ok=True)
            dst = os.path.join(d, os.path.basename(p))
            shutil.copy2(p, dst)
            # the DLLs the Windows build deploys beside the probe (qb-core is static there; vcpkg's OpenSSL may not be)
            for f in os.listdir(os.path.dirname(p)):
                if f.lower().endswith(".dll"):
                    shutil.copy2(os.path.join(os.path.dirname(p), f), os.path.join(d, f))
            copies[(name, c)] = dst
    meta = {"seed": a.seed, "rounds": a.rounds, "copies": a.copies, "asks": a.asks, "mode": a.mode, "pool": pool,
            "pinned": pin, "platform": sys.platform, "responders": ns, "sides": sides, "quiet": a.quiet, "tag": a.tag,
            "host": os.uname().nodename if hasattr(os, "uname") else os.environ.get("COMPUTERNAME", "?")}
    json.dump(meta, open(os.path.join(a.out, "ab-meta.json"), "w"), indent=1)
    rng = random.Random(a.seed)
    tsv = open(os.path.join(a.out, "launches.tsv"), "w")
    tsv.write("n\tround\tN\tside\tcopy\tcpus\tns_per_ask\trc\twall_s\n")
    # smoke: every copy once at the smallest N (also warms the page cache for the copies)
    for (name, c), exe in copies.items():
        v, rc, err, dt = launch(exe, a.mode, ns[0], min(a.asks, 20000), pool[: ns[0] + 1], a.timeout, pin)
        if v is None:
            open(os.path.join(a.out, "RED.txt"), "w").write(f"smoke {name}-{c}: rc={rc} {err}\n")
            print(f"RED smoke {name}-{c}: rc={rc} {err}")
            return 3
    print(f"== {a.tag or 'scatter-ab'}: sides {list(sides)}, N={ns}, mode {a.mode}, asks {a.asks}, {a.copies} copies x {a.rounds} rounds, seed {a.seed}, {'pool ' + str(pool) if pin else 'UNPINNED'}", flush=True)
    if a.quiet:
        time.sleep(a.quiet)
    samples = {}  # (N, side) -> list of ns; per copy too
    percopy = {}  # (N, side, copy) -> list
    k = 0
    for rnd in range(1, a.rounds + 1):
        order = [(n, name, c) for n in ns for name in sides for c in range(1, a.copies + 1)]
        rng.shuffle(order)
        for n, name, c in order:
            cpus = (pool[: n + 1] if a.mode == "cross" else pool[:1]) if pin else []
            v, rc, err, dt = launch(copies[(name, c)], a.mode, n, a.asks, cpus, a.timeout, pin)
            k += 1
            tsv.write(f"{k}\t{rnd}\t{n}\t{name}\t{c}\t{','.join(map(str, cpus)) if pin else 'unpinned'}\t{'' if v is None else f'{v:.1f}'}\t{rc}\t{dt:.2f}\n"); tsv.flush()
            if v is None:
                open(os.path.join(a.out, "RED.txt"), "w").write(f"launch {k} round {rnd} N={n} {name}-{c}: rc={rc} {err}\n")
                print(f"RED launch {k}: N={n} {name}-{c} rc={rc} {err}")
                return 3
            samples.setdefault((n, name), []).append(v)
            percopy.setdefault((n, name, c), []).append(v)
        print(f"   round {rnd}/{a.rounds} done {time.strftime('%H:%M:%S')}", flush=True)
    # summary
    names = list(sides)
    ctl, cands = names[0], names[1:]
    out_rows = []
    lines = [f"campaign {a.out}: seed {a.seed}, rounds {a.rounds}, copies {a.copies}, asks {a.asks}, mode {a.mode}, {'pool ' + str(pool) if pin else 'UNPINNED (the macOS form)'}; control {ctl}, candidates {cands}",
             f"ns per ask_any (median over rounds x copies; copies' medians; A/A band = spread of the copies' medians)"]
    brng = random.Random(a.seed + 1)
    for n in ns:
        row = {"N": n}
        lines.append(f"N={n} responders ({a.mode}, {('CPUs ' + str(pool[: n + 1] if a.mode == 'cross' else pool[:1])) if pin else 'unpinned'})")
        for name in names:
            s = samples[(n, name)]
            cm = [statistics.median(percopy[(n, name, c)]) for c in range(1, a.copies + 1)]
            band = (max(cm) - min(cm)) / statistics.median(cm) * 100 / 2
            row[name] = {"median": statistics.median(s), "n": len(s), "copies": cm, "aa_pct": band,
                         "iqr": [statistics.quantiles(s, n=4)[0], statistics.quantiles(s, n=4)[2]]}
            lines.append(f"   {name:<5} {statistics.median(s):9.1f} ns  IQR [{row[name]['iqr'][0]:.1f}, {row[name]['iqr'][1]:.1f}]  n={len(s)}  copies {' '.join(f'{x:.1f}' for x in cm)}  A/A +-{band:.2f}%")
        row["ratio"] = {}
        for cand in cands:
            lo, hi = bootstrap_ratio(samples[(n, ctl)], samples[(n, cand)], brng)
            ratio = row[cand]["median"] / row[ctl]["median"]
            band = max(row[ctl]["aa_pct"], row[cand]["aa_pct"])
            verdict = "FASTER" if hi < 1 and (1 - ratio) * 100 > band else "SLOWER" if lo > 1 and (ratio - 1) * 100 > band else "LEVEL"
            row["ratio"][cand] = {"over": ctl, "value": ratio, "ci95": [lo, hi], "aa_band_pct": band, "verdict": verdict}
            lines.append(f"   ratio {cand}/{ctl} {ratio:.4f}  CI95 [{lo:.4f}, {hi:.4f}]  A/A band +-{band:.2f}%  -> {verdict}")
        out_rows.append(row)
    if len(ns) >= 3:
        for name in names:
            pts = [(n, samples[(n, name)] and statistics.median(samples[(n, name)])) for n in ns]
            a0, b0 = fit(pts)
            lines.append(f"slope {name:<5}: {b0:.1f} ns per responder, intercept {a0:.1f} ns  (least squares over N={ns})")
            out_rows.append({"slope": name, "per_responder_ns": b0, "intercept_ns": a0})
        for cand in cands:
            pc = [(n, statistics.median(samples[(n, ctl)])) for n in ns]
            pd = [(n, statistics.median(samples[(n, cand)])) for n in ns]
            _, bc = fit(pc); _, bd = fit(pd)
            lines.append(f"slope delta ({cand} - {ctl}): {bd - bc:+.1f} ns per responder ({(bd / bc - 1) * 100:+.1f} %)")
            out_rows.append({"slope_delta": cand, "over": ctl, "per_responder_ns": bd - bc, "pct": (bd / bc - 1) * 100})
            # per N: the per-responder delta read directly, (cand - ctl) / N
            lines.append(f"per-N delta of the medians ({cand} - {ctl}), and per responder: " + "  ".join(
                f"N={n}: {statistics.median(samples[(n, cand)]) - statistics.median(samples[(n, ctl)]):+.1f} ns ({(statistics.median(samples[(n, cand)]) - statistics.median(samples[(n, ctl)])) / n:+.2f}/resp)" for n in ns))
    txt = "\n".join(lines) + "\n"
    open(os.path.join(a.out, "summary.txt"), "w").write(txt)
    json.dump(out_rows, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    print(txt)
    shutil.rmtree(os.path.join(a.out, "copies"), ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
