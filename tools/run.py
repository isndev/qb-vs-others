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
import math
import os
import platform
import subprocess
import sys
import tempfile
import uuid
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


def atomic_json(path: Path, doc: dict) -> None:
    """Publish complete JSON in one replacement, on the destination's filesystem.

    Close the temporary handle before replacing: Windows does not permit an open temporary file
    to be renamed, and readers must never see a partly written result or manifest.
    """
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(doc, indent=2) + "\n")
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def failed_document(benchmark: str, framework: str, params: dict, reason: str) -> dict:
    """The current attempt failed; it cannot leave an older timing or n/a in a published cell."""
    return {"schema": "qvo/result/1", "benchmark": benchmark, "framework": framework,
            "verified": False, "failures": [reason], "params": params,
            "work_ns": [], "summary": {}}


def read_attempt(path: Path, benchmark: str, framework: str, params: dict,
                 returncode: int, repetitions: int, warmup: int) -> tuple[dict | None, str]:
    """Accept only a document this invocation could have produced for this exact cell."""
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as e:
        return None, f"no complete result document ({e})"
    if not isinstance(doc, dict) or doc.get("schema") != "qvo/result/1":
        return None, "invalid result schema"
    if doc.get("benchmark") != benchmark or doc.get("framework") != framework:
        return None, "result identity differs from the requested cell"
    recorded = doc.get("params")
    if not isinstance(recorded, dict) or any(type(recorded.get(k)) is not int or
                                              recorded[k] != v for k, v in params.items()):
        return None, "result parameters differ from the requested configuration"
    if returncode == 3:
        reason = doc.get("not_applicable")
        if (doc.get("verified") is not False or not isinstance(reason, str) or not reason.strip()
                or doc.get("work_ns") != [] or doc.get("summary") != {}
                or doc.get("total_ns", []) != [] or doc.get("outside_window_ns", []) != []):
            return None, "exit 3 without a valid not_applicable document"
    elif returncode in (0, 1):
        if doc.get("repetitions") != repetitions or doc.get("warmup") != warmup:
            return None, "result repetition count differs from this invocation"
        if doc.get("not_applicable") is not None:
            return None, "measured exit carries a not_applicable verdict"
        samples = doc.get("work_ns")
        if returncode == 0:
            if (doc.get("verified") is not True or not isinstance(samples, list)
                    or len(samples) != repetitions or doc.get("failures") not in (None, [])
                    or not isinstance(doc.get("summary"), dict)
                    or not isinstance(doc["summary"].get("work_p50"), (int, float))
                    or not math.isfinite(doc["summary"]["work_p50"])
                    or any(type(v) not in (int, float) or not math.isfinite(v) for v in samples)):
                return None, "exit 0 without a fully verified sample"
        elif (doc.get("verified") is not False
              or not isinstance(doc.get("failures"), list) or not doc["failures"]
              or samples != [] or doc.get("summary") != {}
              or doc.get("total_ns", []) != [] or doc.get("outside_window_ns", []) != []):
            return None, "exit 1 emitted a timing or lacked a failure reason"
    else:
        return None, f"child exited {returncode}"
    return doc, ""


def measure_cells(args: argparse.Namespace, cells: list[tuple[str, str, Path]],
                  only: set[str] | None, benches: set[str] | None,
                  cfg_filter: set[str] | None, cpus: str, manifest: dict) -> tuple[int, int, int]:
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

            # The child never sees the canonical path. A fresh attempt file makes an old n/a or
            # timing impossible to mistake for this launch's document, including on Windows where
            # a process abort may report the same exit code as not_applicable().
            fd, attempt_name = tempfile.mkstemp(prefix=f".{framework}__{cfg_name}.",
                                                 suffix=".tmp", dir=dest_dir)
            os.close(fd)  # Windows: the child cannot reopen a handle still held by Python.
            attempt = Path(attempt_name)
            cmd = [str(exe), "--repetitions", str(args.repetitions),
                   "--warmup", str(args.warmup), "--out", str(attempt)]
            cmd += ["--no-pin"] if args.no_pin else ["--cpus", cpus]
            for k, v in params.items():
                cmd += ["--param", f"{k}={v}"]

            total += 1
            label = f"{benchmark:<24} {framework:<12} {cfg_name:<8}"
            try:
                try:
                    r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    r = None
                    status = "timeout"
                    reason = f"timed out after {args.timeout}s"
                except OSError as e:
                    r = None
                    status = "failed"
                    reason = f"could not launch benchmark ({e})"
                else:
                    status = "failed"
                    if r.returncode in (0, 1, 3):
                        doc, reason = read_attempt(attempt, benchmark, framework, params,
                                                   r.returncode, args.repetitions, args.warmup)
                        if doc is not None:
                            os.replace(attempt, dest)
                            if r.returncode == 3:
                                status = "n/a"
                                why = doc["not_applicable"]
                                not_applicable += 1
                                print(f"  {label} n/a  ({why[:70]}{'...' if len(why) > 70 else ''})")
                                manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                                          "config": cfg_name, "status": status,
                                                          "reason": why})
                                continue
                            status = "ok" if r.returncode == 0 else "unverified"
                            if status == "unverified":
                                failed += 1
                            p50 = doc.get("summary", {}).get("work_p50", 0)
                            print(f"  {label} {'ok ' if status == 'ok' else 'UNVERIFIED'} "
                                  f"p50={p50:,.0f} ns")
                            manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                                      "config": cfg_name, "status": status})
                            continue
                    else:
                        reason = f"child exited {r.returncode}"

                # A failed current attempt must publish a current failure document. Leaving the
                # old file would let report.py show its timing despite run.json saying failed.
                failed += 1
                atomic_json(dest, failed_document(benchmark, framework, params, reason))
                if status == "timeout":
                    print(f"  {label} TIMEOUT after {args.timeout}s")
                else:
                    rc = r.returncode if r is not None else "launch"
                    print(f"  {label} FAILED rc={rc}: {reason}")
                    if r is not None:
                        for line in (r.stderr or r.stdout).strip().splitlines()[-3:]:
                            print(f"      {line}")
                manifest["cells"].append({"benchmark": benchmark, "framework": framework,
                                          "config": cfg_name, "status": status})
            finally:
                attempt.unlink(missing_ok=True)
    return total, failed, not_applicable


def preflight_manifest(path: Path, manifest: dict, partial: bool) -> dict | None:
    """Read the latest manifest and refuse an unsafe partial merge before any cell changes."""
    try:
        prev = json.loads(path.read_text())
    except FileNotFoundError:
        return None
    if prev.get("status") == "in_progress":
        sys.exit(f"run.py: {path} records an interrupted run; use a new --out directory, or "
                 "discard the entire old output directory before rerunning")
    if partial:
        for key in ("host", "platform", "cpus", "repetitions", "warmup"):
            if prev.get(key) != manifest[key]:
                sys.exit(f"run.py: refusing to merge into {path}: {key} differs "
                         f"({prev.get(key)!r} recorded, {manifest[key]!r} now). A partial "
                         "re-run must repeat the recorded conditions, or go to a new --out.")
    return prev


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
    only = set(args.only.split(",")) if args.only else None
    benches = set(args.benchmark.split(",")) if args.benchmark else None
    cfg_filter = set(args.config.split(",")) if args.config else None

    manifest = {
        "schema": "qvo/run/1",
        # Distinguish two completed attempts even when their status/cell lists are identical.
        # report.py compares manifest bytes around its scan and render to catch that ABA race.
        "run_id": uuid.uuid4().hex,
        "host": platform.node(),
        "platform": platform.platform(),
        "cpus": cpus,
        "repetitions": args.repetitions,
        "warmup": args.warmup,
        "cells": [],
    }
    # A filtered run (--only / --benchmark / --config) is a partial re-measurement into a results
    # directory that already describes a whole field, and overwriting its manifest would erase
    # the cells that were not re-run. Merge instead -- but only under the SAME conditions: a
    # partial run on another host, CPU set or repetition count is a different experiment, and
    # mixing it into an existing table is exactly the kind of quiet edit this tool exists to
    # make impossible. Refuse loudly rather than merge.
    rj = args.out / "run.json"
    partial = bool(only or benches or cfg_filter)
    preflight_manifest(rj, manifest, partial)  # Usual incompatible case makes no file changes.

    args.out.mkdir(parents=True, exist_ok=True)
    lock = args.out / ".run.lock"
    try:
        lock.mkdir()  # atomic on both POSIX and Windows; two runners may not share --out.
    except FileExistsError:
        sys.exit(f"run.py: {lock} exists -- another run may be active; refusing to share --out")
    try:
        # Another runner may have completed between the first preflight and lock acquisition.
        # This snapshot, taken while holding the lock, owns the later partial-merge decision.
        prev = preflight_manifest(rj, manifest, partial)
        # --describe launches each binary. Keep even discovery inside the output lock so a second
        # runner cannot perturb a measurement already writing to this directory.
        cells = discover(args.build)
        if not cells:
            sys.exit("run.py: no benchmark binaries found -- refusing to write an empty result set")
        # A mixed filter such as --only qb,caf must not silently keep an old CAF document when
        # only qb was discovered in this build. Validate every named token, not just the final
        # non-empty intersection, before publishing the in-progress marker or touching a cell.
        for flag, requested, available in (
            ("only", only, {f for f, _, _ in cells}),
            ("benchmark", benches, {b for _, b, _ in cells}),
            ("config", cfg_filter, {name for name, _ in CONFIGS}),
        ):
            if requested:
                unknown = requested - available
                if unknown:
                    sys.exit(f"run.py: --{flag} names unknown value(s): "
                             f"{', '.join(sorted(unknown))}")
        selected = [(f, b, exe) for f, b, exe in cells
                    if (not only or f in only) and (not benches or b in benches)]
        configs = [name for name, _ in CONFIGS if not cfg_filter or name in cfg_filter]
        if not selected or not configs:
            sys.exit("run.py: no cells selected by --only/--benchmark/--config -- refusing "
                     "to complete an empty run")
        current_keys = {(f, b, cfg) for f, b, _ in selected for cfg in configs}
        if prev:
            previous_selected = {
                (c["framework"], c["benchmark"], c["config"])
                for c in prev.get("cells", [])
                if (not only or c["framework"] in only)
                and (not benches or c["benchmark"] in benches)
                and (not cfg_filter or c["config"] in cfg_filter)
            }
            missing_prior = previous_selected - current_keys
            if missing_prior:
                sys.exit(f"run.py: previous selected cell has no current binary: "
                         f"{sorted(missing_prior)} -- use a new --out for this smaller roster")
        if only is not None and benches is not None:
            # Both axes were named by the caller. An absent pair is an unfulfilled request, even
            # when some other named pair can run. With either axis implicit, use the supported
            # discovered union; framework-specific omissions are not fabricated as cells.
            available_pairs = {(f, b) for f, b, _ in cells}
            missing_pairs = {(f, b) for f in only for b in benches} - available_pairs
            if missing_pairs:
                sys.exit(f"run.py: explicit framework/benchmark pair has no binary: "
                         f"{sorted(missing_pairs)}")

        frameworks = sorted({f for f, _, _ in cells})
        print(f"run.py: {len(cells)} binaries, frameworks: {', '.join(frameworks)}")
        if args.no_pin:
            print(f"run.py: UNPINNED (--no-pin), {args.repetitions} repetitions + {args.warmup} warmup"
                  " -- every result carries pinned:false")
        else:
            print(f"run.py: pinning to CPUs {cpus}, {args.repetitions} repetitions + {args.warmup} warmup")
        # Refuse a partial field silently. A table missing a framework is a table that reads as if
        # that framework did not exist, and this is the easiest way to publish a flattering one.
        if only is None and len(frameworks) < 2:
            sys.exit("run.py: fewer than two frameworks were built -- a comparison needs a field")

        # The marker is published before the first cell. A killed runner can leave a mixture of
        # old and new cells, but report.py refuses the directory until a complete run replaces it.
        atomic_json(rj, {**manifest, "status": "in_progress"})
        total, failed, not_applicable = measure_cells(args, cells, only, benches, cfg_filter,
                                                       cpus, manifest)
        if prev and partial:
            rerun = {(c["benchmark"], c["framework"], c["config"]) for c in manifest["cells"]}
            kept = [c for c in prev.get("cells", [])
                    if (c["benchmark"], c["framework"], c["config"]) not in rerun]
            manifest["cells"] = kept + manifest["cells"]
            manifest["merged_partial_runs"] = prev.get("merged_partial_runs", 0) + 1
        manifest["status"] = "complete"
        atomic_json(rj, manifest)
    finally:
        lock.rmdir()
    print(f"\nrun.py: {total} cells, {failed} not verified, {not_applicable} not applicable "
          f"-> {args.out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
