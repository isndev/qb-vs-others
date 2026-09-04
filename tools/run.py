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
    ap.add_argument("--only", default=None, help="comma-separated framework subset")
    ap.add_argument("--benchmark", default=None, help="comma-separated benchmark subset")
    ap.add_argument("--config", default=None, help="comma-separated configuration subset")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()

    cpus = args.cpus or default_cpus()
    cells = discover(args.build)
    if not cells:
        sys.exit("run.py: no benchmark binaries found -- refusing to write an empty result set")

    only = set(args.only.split(",")) if args.only else None
    benches = set(args.benchmark.split(",")) if args.benchmark else None
    cfg_filter = set(args.config.split(",")) if args.config else None

    frameworks = sorted({f for f, _, _ in cells})
    print(f"run.py: {len(cells)} binaries, frameworks: {', '.join(frameworks)}")
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

    total = failed = 0
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
                   "--warmup", str(args.warmup), "--cpus", cpus, "--out", str(dest)]
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

    (args.out / "run.json").write_text(json.dumps(manifest, indent=2))
    print(f"\nrun.py: {total} cells, {failed} not verified -> {args.out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
