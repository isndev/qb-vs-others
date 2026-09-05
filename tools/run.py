#!/usr/bin/env python3
"""Run the benchmark matrix and write one verified JSON result per cell.

The matrix is (framework x benchmark x configuration). Every cell runs in its OWN process: no two
frameworks ever share an address space, an allocator arena or a warmed cache line, because a
benchmark that measures two runtimes in one process measures their interference.

This script REFUSES to record an unverified run. A cell whose checksum did not match, or whose
pinning was refused, is written to results/ with `verified:false` and is excluded from every
table by tools/report.py -- it is kept rather than deleted so that a reader can see a framework
failed rather than wonder why it is missing.

Usage
-----
    python3 tools/run.py --build build/win-release --out results/<host-id>
    python3 tools/run.py --build build/win-release --out results/x --only qb,caf --repetitions 11
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------------------------
# The configuration matrix
#
# Every configuration is measured for every framework. There is no per-framework subset: a table
# with a cell missing for one framework is a table a reader cannot compare across.
# ---------------------------------------------------------------------------------------------

CONFIGS = [
    # name          params
    ("2c-spin", {"cores": 2, "wait": 1}),
    ("2c-park", {"cores": 2, "wait": 0}),
    ("1c-spin", {"cores": 1, "wait": 1}),
    ("1c-park", {"cores": 1, "wait": 0}),
]


def discover(build_dir: Path) -> list[tuple[str, str, Path]]:
    """Find every benchmark binary and ask it what it is.

    The identity comes from the binary's own --describe, never from its filename: the CMake
    derivation guarantees they agree, and asking the binary is what makes that a check rather than
    an assumption.
    """
    bindir = build_dir / "bin"
    if not bindir.is_dir():
        sys.exit(f"run.py: no bin/ under {build_dir} -- build first")

    found = []
    for exe in sorted(bindir.iterdir()):
        if exe.suffix.lower() not in (".exe", "") or not exe.is_file():
            continue
        if not exe.name.startswith("qvo-"):
            continue
        try:
            out = subprocess.run([str(exe), "--describe"], capture_output=True, text=True,
                                 timeout=60)
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f"run.py: {exe.name} could not be described ({e}) -- SKIPPED", file=sys.stderr)
            continue
        if out.returncode != 0:
            print(f"run.py: {exe.name} --describe exited {out.returncode} -- SKIPPED",
                  file=sys.stderr)
            continue
        try:
            d = json.loads(out.stdout)
        except json.JSONDecodeError:
            print(f"run.py: {exe.name} --describe emitted no JSON -- SKIPPED", file=sys.stderr)
            continue
        found.append((d["framework"], d["benchmark"], exe))
    return found


def default_cpus() -> str:
    """One CPU per PHYSICAL performance core, hyperthread siblings excluded.

    Why this default and not "all of them": the primary host is a hybrid 8P+8E part. A thread that
    lands on an efficiency core runs at roughly half the clock, and a thread that lands on the
    hyperthread sibling of a busy core shares its execution units. Either one moves a result by
    more than the differences this repository reports.

    On Linux the topology is read from sysfs. On Windows there is no equally cheap query from
    Python, so the conventional Intel enumeration (physical cores first, siblings interleaved) is
    assumed and the CHOICE IS RECORDED IN THE RESULT so a reader can reject it.
    """
    if sys.platform.startswith("linux"):
        seen, cpus = set(), []
        base = Path("/sys/devices/system/cpu")
        for d in sorted(base.glob("cpu[0-9]*"), key=lambda p: int(p.name[3:])):
            core_id = d / "topology" / "core_id"
            pkg = d / "topology" / "physical_package_id"
            if not core_id.exists():
                continue
            key = (pkg.read_text().strip() if pkg.exists() else "0", core_id.read_text().strip())
            if key in seen:
                continue
            seen.add(key)
            cpus.append(int(d.name[3:]))
        if cpus:
            return ",".join(str(c) for c in cpus[:8])
    # Windows / fallback: even-numbered logical CPUs are the first thread of each physical core on
    # the conventional Intel enumeration, and on a 12th-gen part the P-cores come first.
    return "0,2,4,6,8,10,12,14"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--repetitions", type=int, default=11)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--cpus", default=None,
                    help="affinity set (default: one CPU per physical performance core)")
    ap.add_argument("--no-pin", action="store_true",
                    help="run every cell unpinned (the harness records pinned:false). The only "
                         "way to measure on a platform with no verified affinity API -- macOS "
                         "-- where the harness refuses --cpus by design (FAIRNESS.md 1.4)")
    ap.add_argument("--only", default=None, help="comma-separated framework subset")
    ap.add_argument("--benchmark", default=None, help="comma-separated benchmark subset")
    ap.add_argument("--config", default=None, help="comma-separated configuration subset")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()

    if args.no_pin and args.cpus:
        sys.exit("run.py: --no-pin and --cpus are contradictory -- pick one")
    # "unpinned" is recorded where a CPU list would be, so the manifest's own field says the run
    # was not pinned and a partial re-run cannot silently merge pinned and unpinned cells.
    cpus = "unpinned" if args.no_pin else (args.cpus or default_cpus())
    cells = discover(args.build)
    if not cells:
        sys.exit("run.py: no benchmark binaries found -- refusing to write an empty result set")

    only = set(args.only.split(",")) if args.only else None
    benches = set(args.benchmark.split(",")) if args.benchmark else None
    cfg_filter = set(args.config.split(",")) if args.config else None

    frameworks = sorted({f for f, _, _ in cells})
    print(f"run.py: {len(cells)} binaries, frameworks: {', '.join(frameworks)}")
    if args.no_pin:
        print(f"run.py: UNPINNED (--no-pin), {args.repetitions} repetitions + {args.warmup} warmup"
              " -- every result carries pinned:false")
    else:
        print(f"run.py: pinning to CPUs {cpus}, {args.repetitions} repetitions + {args.warmup} warmup")

    # Refuse a partial field silently. A table missing a framework is a table that reads as if
    # that framework did not exist, and this is the single easiest way to publish a flattering one.
    if only is None and len(frameworks) < 2:
        sys.exit("run.py: fewer than two frameworks were built -- a comparison needs a field")

    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": "qvo/run/1",
        "host": platform.node(),
        "platform": platform.platform(),
        "cpus": cpus,
        "repetitions": args.repetitions,
        "warmup": args.warmup,
        "cells": [],
    }

    total = failed = not_applicable = 0
    for framework, benchmark, exe in cells:
        if only and framework not in only:
            continue
        if benches and benchmark not in benches:
            continue
        for cfg_name, params in CONFIGS:
            if cfg_filter and cfg_name not in cfg_filter:
                continue
            slug = benchmark.replace("/", "-")
            dest_dir = args.out / slug
            dest_dir.mkdir(parents=True, exist_ok=True)
            dest = dest_dir / f"{framework}__{cfg_name}.json"

            cmd = [str(exe), "--repetitions", str(args.repetitions),
                   "--warmup", str(args.warmup), "--out", str(dest)]
            cmd += ["--no-pin"] if args.no_pin else ["--cpus", cpus]
            for k, v in params.items():
                cmd += ["--param", f"{k}={v}"]

            total += 1
            label = f"{benchmark:<24} {framework:<12} {cfg_name:<8}"
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                failed += 1
                print(f"  {label} TIMEOUT after {args.timeout}s")
                dest.write_text(json.dumps({
                    "schema": "qvo/result/1", "benchmark": benchmark, "framework": framework,
                    "verified": False, "failures": [f"timed out after {args.timeout}s"],
                    "params": params, "work_ns": [], "summary": {}}, indent=2))
                manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                          "config": cfg_name, "status": "timeout"})
                continue

            # Exit 3 is the harness's third verdict (harness.h: qvo::not_applicable): the adapter
            # cannot express this configuration and wrote a document saying why. It is neither a
            # defect nor a pass, so it is counted on its own line and never hidden in `failed`.
            if r.returncode == 3 and dest.exists():
                doc = json.loads(dest.read_text())
                why = doc.get("not_applicable") or "no reason recorded"
                if not doc.get("not_applicable"):
                    # A 3 without a reason is a contract violation, not a verdict.
                    failed += 1
                    print(f"  {label} FAILED rc=3 without a not_applicable reason")
                    manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                              "config": cfg_name, "status": "failed"})
                    continue
                not_applicable += 1
                print(f"  {label} n/a  ({why[:70]}{'...' if len(why) > 70 else ''})")
                manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                          "config": cfg_name, "status": "n/a", "reason": why})
                continue

            if r.returncode != 0 or not dest.exists():
                failed += 1
                tail = (r.stderr or r.stdout).strip().splitlines()[-3:]
                print(f"  {label} FAILED rc={r.returncode}")
                for line in tail:
                    print(f"      {line}")
                manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                          "config": cfg_name, "status": "failed"})
                continue

            doc = json.loads(dest.read_text())
            p50 = doc.get("summary", {}).get("work_p50", 0)
            ok = doc.get("verified", False)
            if not ok:
                failed += 1
            print(f"  {label} {'ok ' if ok else 'UNVERIFIED'} p50={p50:,.0f} ns")
            manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                      "config": cfg_name,
                                      "status": "ok" if ok else "unverified"})

    # A filtered run (--only / --benchmark / --config) is a partial re-measurement into a results
    # directory that already describes a whole field, and overwriting its manifest would erase
    # the cells that were not re-run. Merge instead -- but only under the SAME conditions: a
    # partial run on another host, CPU set or repetition count is a different experiment, and
    # mixing it into an existing table is exactly the kind of quiet edit this tool exists to
    # make impossible. Refuse loudly rather than merge.
    rj = args.out / "run.json"
    if rj.exists() and (only or benches or cfg_filter):
        prev = json.loads(rj.read_text())
        for key in ("host", "platform", "cpus", "repetitions", "warmup"):
            if prev.get(key) != manifest[key]:
                sys.exit(f"run.py: refusing to merge into {rj}: {key} differs "
                         f"({prev.get(key)!r} recorded, {manifest[key]!r} now). A partial "
                         "re-run must repeat the recorded conditions, or go to a new --out.")
        rerun = {(c["benchmark"], c["framework"], c["config"]) for c in manifest["cells"]}
        kept = [c for c in prev.get("cells", [])
                if (c["benchmark"], c["framework"], c["config"]) not in rerun]
        manifest["cells"] = kept + manifest["cells"]
        manifest["merged_partial_runs"] = prev.get("merged_partial_runs", 0) + 1
    # newline="\n": text mode on Windows would write CRLF, and a manifest is the same bytes on
    # every host or it is two manifests.
    with open(rj, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(manifest, indent=2) + "\n")
    print(f"\nrun.py: {total} cells, {failed} not verified, {not_applicable} not applicable "
          f"-> {args.out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
