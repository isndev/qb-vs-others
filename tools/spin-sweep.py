#!/usr/bin/env python3
"""Run the two established spin sweeps with fresh, retained attempts (QB-877/852).

The shell entry points retain their historical workloads and order. A populated --out is never
reused; send tee's log to a neighbouring path, since tee creates its file before this admission.
This helper owns collection only: run.read_attempt owns result validation, report.work_unit owns
normalisation, and neither the shared runner nor historical measurements are rewritten.
"""
from __future__ import annotations

import argparse
import math
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from run import atomic_json, failed_document, read_attempt
from report import work_unit

PARAMS = {"messages": 1000000, "cores": 2, "wait": 1}
CPUS = "0,2"
SWEEP_ENV = ("QVO_SO_SPIN_WAIT_US", "QVO_CAF_AGGRESSIVE_POLL", "QVO_CAF_STEAL_INTERVAL")
SO_BUDGETS = (
    ("profile-10s", None), ("simple-lock", "0"), ("wait1us", "1"), ("wait10us", "10"),
    ("wait100us", "100"), ("wait1ms-default", "1000"), ("wait10ms", "10000"),
    ("wait100ms", "100000"), ("wait10s-b", "10000000"), ("profile-10s-b", None),
)
CAF_BUDGETS = (
    ("poll100-steal10", "100", "10"), ("poll1e3-steal10", "1000", "10"),
    ("poll1e4-steal10", "10000", "10"), ("poll1e5-steal10", "100000", "10"),
    ("poll1e6-steal10", "1000000", "10"), ("poll1e4-steal1", "10000", "1"),
    ("poll1e4-steal100", "10000", "100"), ("poll1e4-steal1e3", "10000", "1000"),
    ("poll1e6-steal1e6", "1000000", "1000000"), ("poll100-steal10-b", "100", "10"),
)


def cells(mode: str, binary: Path):
    if mode == "so":
        ext = ".exe" if (binary / "qvo-sobjectizer-savina-ping-pong.exe").is_file() else ""
        for shape in ("ping-pong", "counting"):
            for label, budget in SO_BUDGETS:
                # No budget means inherit the environment, exactly as the historical wrapper did.
                overrides = {} if budget is None else {"QVO_SO_SPIN_WAIT_US": budget}
                yield (f"{shape}-{label}", f"savina/{shape}", "sobjectizer",
                       binary / f"qvo-sobjectizer-savina-{shape}{ext}", overrides)
    else:
        for label, poll, steal in CAF_BUDGETS:
            yield (label, "savina/ping-pong", "caf", binary,
                   {"QVO_CAF_AGGRESSIVE_POLL": poll, "QVO_CAF_STEAL_INTERVAL": steal})


def sweep_metadata_error(doc: dict, benchmark: str, repetitions: int) -> str:
    """The measured sweeps' stricter display contract, after the common result validator."""
    if type(doc.get("repetitions")) is not int or type(doc.get("warmup")) is not int:
        return "repetition metadata must use integers"
    if doc.get("pinned") is not True or doc.get("cpus") != CPUS:
        return "result CPU set or pinning differs from this invocation"

    def duration(value):
        return type(value) in (int, float) and math.isfinite(value) and value >= 0

    for field in ("work_ns", "total_ns", "outside_window_ns"):
        if field not in doc:
            continue
        values = doc[field]
        if (not isinstance(values, list) or len(values) != repetitions
                or any(not duration(value) for value in values)):
            return f"invalid {field} timings"
    for field in ("work_min", "work_p50", "work_p99", "work_iqr"):
        if field in doc["summary"] and not duration(doc["summary"][field]):
            return f"invalid summary {field}"
    if "work_units" in doc:
        if type(doc["work_units"]) is not int or doc["work_units"] <= 0:
            return "work_units must be a positive integer"
        if not isinstance(doc.get("work_unit"), str) or not doc["work_unit"].strip():
            return "work_unit must name the measured unit"
    elif (benchmark != "savina/ping-pong"
          or type(doc.get("expected_messages")) is not int or doc["expected_messages"] <= 0):
        return "missing valid work units"
    expected_unit = "round trip" if benchmark == "savina/ping-pong" else "message"
    if "work_unit" in doc and doc["work_unit"] != expected_unit:
        return "invalid work_unit"
    unit, count = work_unit(doc)
    if unit != expected_unit or count != PARAMS["messages"]:
        return "result work unit differs from the sweep specification"
    return ""


def measure(cell, out: Path, attempts: Path, repetitions: int, timeout: float,
            manifest: dict) -> bool:
    label, benchmark, framework, exe, overrides = cell
    raw = Path(tempfile.mkdtemp(prefix=f"{label}-", dir=attempts))
    attempt = raw / "result.json"  # Does not exist before this invocation.
    cmd = [str(exe), "--repetitions", str(repetitions), "--warmup", "2", "--cpus", CPUS]
    for key, value in PARAMS.items():
        cmd += ["--param", f"{key}={value}"]
    cmd += ["--out", str(attempt)]
    env = os.environ.copy()
    env.update(overrides)
    record = {"label": label, "benchmark": benchmark, "framework": framework,
              "status": "running", "attempt": str(raw.relative_to(out)), "command": cmd,
              "environment": {key: env[key] for key in SWEEP_ENV if key in env}}
    manifest["cells"].append(record)
    atomic_json(out / "run.json", manifest)  # A killed process leaves its current attempt visible.
    rc = None
    doc = None
    try:
        with (raw / "stdout.txt").open("wb") as stdout, (raw / "stderr.txt").open("wb") as stderr:
            result = subprocess.run(cmd, stdout=stdout, stderr=stderr, env=env, timeout=timeout)
        rc = result.returncode
        doc, reason = read_attempt(attempt, benchmark, framework, PARAMS, rc, repetitions, 2)
        if doc is not None and rc != 0:
            reason = ("not applicable: " + doc["not_applicable"] if rc == 3
                      else "unverified: " + str(doc["failures"]))
        if doc is not None and rc == 0:
            reason = sweep_metadata_error(doc, benchmark, repetitions)
            if not reason:
                unit, count = work_unit(doc)
                samples = sorted(doc["work_ns"])
                sweep = any(isinstance(c, str) and c.startswith("SWEEP DOCUMENT")
                            for c in doc.get("caveats", []))
                line = (f"{label:<28} verified=True sweep-caveat={sweep} "
                        f"p50={statistics.median(samples) / count:7.1f}  "
                        f"min={samples[0] / count:7.1f}  max={samples[-1] / count:7.1f} ns/{unit}")
                atomic_json(out / f"{label}.json", doc)
                record.update(status="ok", returncode=rc)
                atomic_json(out / "run.json", manifest)
                print(line, flush=True)
                return True
    except subprocess.TimeoutExpired:
        reason = f"timed out after {timeout:g}s"
    except Exception as error:
        # Malformed metadata, launch failure or decoding must yield a visible failed attempt,
        # never a traceback followed by a later shell command's success. Keep the raw files.
        reason = f"{type(error).__name__}: {error}"
    record.update(status="failed", returncode=rc, reason=reason)
    atomic_json(out / f"{label}.json", failed_document(benchmark, framework, PARAMS, reason))
    atomic_json(out / "run.json", manifest)
    print(f"{label:<28} FAILED rc={rc}: {reason[:300]}", file=sys.stderr, flush=True)
    return False


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("so", "caf"))
    parser.add_argument("binary", type=Path, help="SO binary directory, or CAF executable")
    parser.add_argument("out", type=Path, help="new or empty campaign directory")
    parser.add_argument("--timeout", type=float, default=1800,
                        help="outer process bound in seconds (default: run.py's 1800)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    # Keep '.' and './exe' anchored to this invocation, not an executable found on PATH.
    binary = args.binary.resolve()
    # tempfile returns absolute paths; normalise the campaign before deriving relative receipts.
    out = args.out.resolve()
    lock = out / ".run.lock"
    try:
        if out.exists() and any(out.iterdir()):
            raise ValueError(f"{out} is populated; use a new directory (keep tee's log beside it)")
        out.mkdir(parents=True, exist_ok=True)
        lock.mkdir()  # Same atomic lock as run.py, covering admission and every launch.
    except (OSError, ValueError) as error:
        print(f"spin-sweep.py: {type(error).__name__}: {str(error)[:300]}", file=sys.stderr)
        return 2

    manifest = None
    exit_code = 2
    try:
        # A competing process may have completed after the first check. Own the lock before
        # making any campaign files, and never erase a file another campaign left behind.
        if any(path != lock for path in out.iterdir()):
            raise ValueError(f"{out} became populated; use a new directory")
        attempts = out / ".attempts"
        attempts.mkdir()
        repetitions = 7 if args.mode == "so" else 5
        manifest = {"schema": "qvo/sweep/1", "mode": args.mode, "status": "in_progress",
                    "cpus": CPUS, "params": PARAMS, "repetitions": repetitions, "warmup": 2,
                    "timeout": args.timeout, "cells": []}
        atomic_json(out / "run.json", manifest)
        failed = 0
        for cell in cells(args.mode, binary):
            if not measure(cell, out, attempts, repetitions, args.timeout, manifest):
                failed += 1
        manifest.update(status="failed" if failed else "complete", failed=failed)
        atomic_json(out / "run.json", manifest)
        exit_code = 1 if failed else 0
    except (KeyboardInterrupt, Exception) as error:
        if manifest is not None:
            manifest.update(status="interrupted", reason=f"{type(error).__name__}: {error}")
            if manifest["cells"] and manifest["cells"][-1]["status"] == "running":
                current = manifest["cells"][-1]
                current.update(status="interrupted", reason=manifest["reason"])
                try:
                    atomic_json(out / f"{current['label']}.json", failed_document(
                        current["benchmark"], current["framework"], PARAMS, manifest["reason"]))
                except OSError:
                    pass  # The in-progress manifest still prevents a success claim after an I/O failure.
            try:
                atomic_json(out / "run.json", manifest)
            except OSError:
                pass
        print(f"spin-sweep.py: {type(error).__name__}: {str(error)[:300]}", file=sys.stderr)
        exit_code = 130 if isinstance(error, KeyboardInterrupt) else 2
    finally:
        try:
            lock.rmdir()  # Only this invocation's lock; never remove result or diagnostic files.
        except OSError as error:
            print(f"spin-sweep.py: cannot release {lock}: {str(error)[:300]}", file=sys.stderr)
            exit_code = 2
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
