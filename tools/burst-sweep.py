#!/usr/bin/env python3
"""The burst sweep: one benchmark, one configuration, the burst size swept -- for N binaries at once.

docs/TUNING.md 9.11 is the finding this instrument exists for: `savina/counting` at one core
measures the growth of qb's event pipe, not its dispatch, and the only way to see that is to sweep
the producer's burst from cache-resident (2 000 messages) to the published 1 M and beyond. The
sweep was first run by hand on two hosts; this is that procedure written down so a third host runs
the SAME one.

What it guarantees that a shell loop does not:

  * SEVERAL BINARIES, INTERLEAVED PER BURST. A candidate and its control are launched back to
    back at each burst size, so the host's drift over a 10-minute sweep lands on both, not on
    whichever ran second. FAIRNESS.md 1.4: a comparison is shipped and candidate in one session.
  * ONE PROCESS PER CELL, like tools/run.py: fresh address space, fresh page mapping every time.
  * THE FILE NAMES THE RESULTS DIRECTORIES ALREADY USE: `<label>__counting-<config>-<burst>.json`,
    so results/*/qb-branch-*/burst-sweep/ on every host reads the same way.
  * PAGE FAULTS ON macOS. `perf stat` does not exist there; `/usr/bin/time -l` reports the
    process's page faults and reclaims, and 9.11's mechanism is a page-fault count (291 vs
    230 942 at 1 M). `--faults` wraps each launch and writes `<same-name>.faults.txt` beside the
    document, and the table prints the count. Linux gets `perf stat -e page-faults` when present.
  * REFUSES an unverified document like run.py does: a checksum that did not match is a cell with
    no timing, printed as such.

Usage
-----
    python3 tools/burst-sweep.py --out results/<host>/qb-branch-x/burst-sweep \\
        --bin qb-segmented=build/cand/bin/qvo-qb-savina-counting \\
        --bin qb-230c5035=build/f5c/bin/qvo-qb-savina-counting \\
        --bursts 2000,10000,30000,100000,300000,1000000,4000000 \\
        --repetitions 7 --warmup 2 --no-pin --faults

    # the reference frameworks at the two published bursts only
    python3 tools/burst-sweep.py --out ... --bin caf=build/cand/bin/qvo-caf-savina-counting \\
        --bin sobjectizer=... --bin baseline=... --bursts 30000,1000000 ...
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

CONFIGS = {
    "1c-spin": {"cores": 1, "wait": 1},
    "1c-park": {"cores": 1, "wait": 0},
    "2c-spin": {"cores": 2, "wait": 1},
    "2c-park": {"cores": 2, "wait": 0},
}


def parse_bins(items: list[str]) -> list[tuple[str, Path]]:
    out = []
    for it in items:
        if "=" not in it:
            sys.exit(f"burst-sweep.py: --bin expects label=path, got {it!r}")
        label, path = it.split("=", 1)
        p = Path(path)
        if not p.is_file():
            sys.exit(f"burst-sweep.py: {p} is not a file")
        out.append((label, p))
    return out


def fault_wrapper() -> list[str] | None:
    """The command prefix that counts page faults on this platform, or None."""
    if sys.platform == "darwin" and Path("/usr/bin/time").exists():
        return ["/usr/bin/time", "-l"]
    if sys.platform.startswith("linux") and shutil.which("perf"):
        return ["perf", "stat", "-e", "page-faults,minor-faults,major-faults", "-x", ","]
    return None


def parse_faults(stderr: str) -> dict[str, int]:
    """Pull the page-fault figures out of the wrapper's report, whichever wrapper it was."""
    got: dict[str, int] = {}
    for line in stderr.splitlines():
        # macOS `/usr/bin/time -l`:  "        290  page faults" / "     12345  page reclaims"
        m = re.match(r"\s*(\d+)\s+(page faults|page reclaims|involuntary context switches|"
                     r"voluntary context switches|maximum resident set size)", line)
        if m:
            got[m.group(2).replace(" ", "_")] = int(m.group(1))
            continue
        # linux `perf stat -x,`:  "291,,page-faults,..."
        m = re.match(r"(\d+),[^,]*,(page-faults|minor-faults|major-faults)", line)
        if m:
            got[m.group(2).replace("-", "_")] = int(m.group(1))
    return got


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--bin", action="append", required=True,
                    help="label=path, repeatable; every binary is launched at every burst, "
                         "interleaved")
    ap.add_argument("--bursts", default="2000,10000,30000,100000,300000,1000000,4000000")
    ap.add_argument("--config", default="1c-spin", choices=sorted(CONFIGS))
    ap.add_argument("--repetitions", type=int, default=7)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--cpus", default=None, help="affinity set (Linux/Windows)")
    ap.add_argument("--no-pin", action="store_true",
                    help="run unpinned (macOS has no verified pinning; the harness refuses --cpus)")
    ap.add_argument("--faults", action="store_true",
                    help="count page faults per launch (/usr/bin/time -l on macOS, perf stat on Linux)")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()

    if args.no_pin and args.cpus:
        sys.exit("burst-sweep.py: --no-pin and --cpus are contradictory -- pick one")
    if not args.no_pin and not args.cpus:
        sys.exit("burst-sweep.py: say how to place the run: --cpus <set> or --no-pin")

    bins = parse_bins(args.bin)
    bursts = [int(b) for b in args.bursts.split(",") if b]
    params = CONFIGS[args.config]
    wrapper = fault_wrapper() if args.faults else None
    if args.faults and wrapper is None:
        sys.exit("burst-sweep.py: --faults asked but no page-fault counter on this platform")

    args.out.mkdir(parents=True, exist_ok=True)
    placement = "--no-pin" if args.no_pin else f"--cpus {args.cpus}"
    print(f"burst-sweep.py: {len(bins)} binaries x {len(bursts)} bursts, {args.config}, "
          f"{args.repetitions} rep + {args.warmup} warmup, {placement}"
          + (", faults counted" if wrapper else ""))

    # label -> burst -> (ns_per_msg, verified, faults)
    table: dict[str, dict[int, tuple[float, bool, int | None]]] = {l: {} for l, _ in bins}
    bad = 0
    for burst in bursts:
        for label, exe in bins:
            dest = args.out / f"{label}__counting-{args.config}-{burst}.json"
            cmd = [str(exe), "--repetitions", str(args.repetitions), "--warmup", str(args.warmup),
                   "--out", str(dest),
                   "--param", f"messages={burst}",
                   "--param", f"cores={params['cores']}",
                   "--param", f"wait={params['wait']}"]
            cmd += ["--no-pin"] if args.no_pin else ["--cpus", args.cpus]
            if wrapper:
                cmd = wrapper + cmd
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                bad += 1
                print(f"  {label:<14} {burst:>9,}  TIMEOUT after {args.timeout}s")
                continue
            faults = None
            if wrapper:
                f = parse_faults(r.stderr)
                # macOS `time -l` splits the count: "page reclaims" are the minor faults (a fresh
                # page zero-filled or reused -- the number 9.11 is about), "page faults" only the
                # major ones (paged in from disk: the binary itself, ~20 on a cold launch, 1 warm).
                # Linux perf reports the total as page-faults, minor first.
                if sys.platform == "darwin":
                    faults = f.get("page_reclaims")
                else:
                    faults = f.get("page_faults", f.get("minor_faults"))
                (args.out / f"{label}__counting-{args.config}-{burst}.faults.txt").write_text(
                    r.stderr, encoding="utf-8")
            if r.returncode != 0 or not dest.exists():
                bad += 1
                tail = (r.stderr or r.stdout).strip().splitlines()[-2:]
                print(f"  {label:<14} {burst:>9,}  FAILED rc={r.returncode}  " + " | ".join(tail))
                continue
            doc = json.loads(dest.read_text())
            ok = bool(doc.get("verified"))
            p50 = float(doc.get("summary", {}).get("work_p50", 0.0))
            ns_per_msg = p50 / burst if burst else 0.0
            if not ok:
                bad += 1
            table[label][burst] = (ns_per_msg, ok, faults)
            print(f"  {label:<14} {burst:>9,}  {'ok ' if ok else 'UNVERIFIED'}  "
                  f"{ns_per_msg:7.2f} ns/msg"
                  + (f"  faults={faults:,}" if faults is not None else ""))

    # The table, ns per message p50, one row per burst, one column per binary -- 9.11's shape.
    print("\nns per message, p50:")
    head = f"{'burst':>10}" + "".join(f" {l:>16}" for l, _ in bins)
    print(head)
    for burst in bursts:
        row = f"{burst:>10,}"
        for label, _ in bins:
            cell = table[label].get(burst)
            row += f" {'-':>16}" if cell is None else (
                f" {cell[0]:>15.2f}{'' if cell[1] else '!'}")
        print(row)
    if wrapper:
        print("\npage faults, whole process (warmup + repetitions):")
        print(head)
        for burst in bursts:
            row = f"{burst:>10,}"
            for label, _ in bins:
                cell = table[label].get(burst)
                row += f" {'-':>16}" if cell is None or cell[2] is None else f" {cell[2]:>16,}"
            print(row)
    print(f"\nburst-sweep.py: {len(bins) * len(bursts)} launches, {bad} not verified -> {args.out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
