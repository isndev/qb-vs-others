#!/usr/bin/env python3
"""Check and summarize one monitored, balanced two-binary launch census.

Reads existing launch-census.py output. It never starts a benchmark or writes a file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import re
import statistics
from pathlib import Path

CONFIGS = {"1c-spin", "1c-park", "2c-spin", "2c-park"}


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def quartiles(values: list[float]) -> tuple[float, float]:
    q = statistics.quantiles(values, n=4, method="inclusive")
    return q[0], q[2]


def analyze(results: Path, quiet_path: Path, config: str, launches: int, cpus: str,
            binary_manifest: Path | None = None, require_binaries: bool = False) -> dict:
    require(config in CONFIGS, f"unknown config: {config}")
    require(launches >= 2 and launches % 2 == 0, "launches must be even and >= 2")
    quiet = json.loads(quiet_path.read_text(encoding="utf-8"))
    require(quiet.get("verdict") == "sampled_quiet", "quiet monitor rejected this census")
    require(quiet.get("command_exit_code") == 0, "monitored command failed")
    require(quiet.get("runner_restoration", {}).get("verdict") == "restored", "runner affinity was not restored")
    require(quiet.get("contamination") == [] and quiet.get("samples", 0) > 0,
            "quiet monitor has no clean samples")
    require(quiet.get("selected_cpus") == [int(cpu) for cpu in cpus.split(",")],
            "quiet monitor watched other CPUs")
    command = quiet.get("command", [])
    require(any(str(x).endswith("launch-census.py") for x in command), "quiet summary names another command")
    # Downloaded artifacts have a different parent directory from the original runner.
    require("--out" in command and Path(command[command.index("--out") + 1]).name == results.name,
            "quiet summary names another output directory")
    require("--config" in command and config in command[command.index("--config") + 1].split(","),
            "quiet summary has wrong --config")
    for flag, value in (("--launches", str(launches)),
                        ("--repetitions", "3"), ("--warmup", "1"), ("--cpus", cpus)):
        require(flag in command and command[command.index(flag) + 1] == value,
                f"quiet summary has wrong {flag}")
    require("--alternate-order" in command, "quiet summary did not run balanced order")

    rows = [json.loads(line) for line in (results / "launch-order.jsonl").read_text(encoding="utf-8").splitlines()]
    rows = [row for row in rows if row.get("config") == config]
    require(len(rows) == launches, f"expected {launches} launch-order rows, got {len(rows)}")
    observed = {"control": [], "candidate": []}
    paired = []
    by_order = {"control-first": [], "candidate-first": []}
    benchmark = None
    expected_work = None
    expected_files = set()
    for launch, row in enumerate(rows, 1):
        order = ["control", "candidate"] if launch % 2 else ["candidate", "control"]
        require(row == {"config": config, "launch": launch, "order": order},
                f"launch {launch}: order/config is not balanced AB/BA")
        docs = {}
        for side in order:
            matches = list(results.glob(f"{side}__*-{config}-launch{launch}.json"))
            require(len(matches) == 1, f"launch {launch}: missing or duplicate {side} result")
            expected_files.add(matches[0])
            doc = json.loads(matches[0].read_text(encoding="utf-8"))
            require(doc.get("schema") == "qvo/result/1" and doc.get("verified") is True
                    and doc.get("failures") == [], f"launch {launch}: {side} failed correctness")
            require(doc.get("repetitions") == 3 and doc.get("warmup") == 1
                    and len(doc.get("work_ns", [])) == 3, f"launch {launch}: {side} has wrong sample count")
            require(doc.get("pinned") is True and doc.get("cpus") == cpus,
                    f"launch {launch}: {side} has wrong CPU placement")
            require(doc.get("work_units", 0) > 0 and doc["summary"]["work_p50"] == statistics.median(doc["work_ns"]),
                    f"launch {launch}: {side} has wrong work summary")
            require(doc.get("params", {}).get("cores") == int(config[0])
                    and doc["params"].get("wait") == int(config.endswith("spin")),
                    f"launch {launch}: {side} has wrong workload config")
            benchmark = benchmark or doc["benchmark"]
            require(doc["benchmark"] == benchmark and matches[0].name.startswith(f"{side}__{benchmark.replace('/', '-')}-"),
                    f"launch {launch}: {side} names another benchmark")
            docs[side] = doc
            observed[side].append(doc["summary"]["work_p50"] / doc["work_units"])
        for key in ("expected_checksum", "expected_messages", "work_unit", "work_units", "params"):
            require(docs["control"][key] == docs["candidate"][key],
                    f"launch {launch}: control/candidate {key} differs")
        current_work = tuple(docs["control"][key] for key in
                             ("expected_checksum", "expected_messages", "work_unit", "work_units"))
        expected_work = expected_work or current_work
        require(current_work == expected_work, f"launch {launch}: workload changed across launches")
        for key in ("compiler", "compiler_version", "build_type", "cxx_flags"):
            require(docs["control"]["env"][key] == docs["candidate"]["env"][key],
                    f"launch {launch}: control/candidate {key} differs")
        delta = 100 * (observed["candidate"][-1] / observed["control"][-1] - 1)
        paired.append(delta)
        by_order["control-first" if launch % 2 else "candidate-first"].append(delta)

    actual_files = set(results.glob(f"*__*-{config}-launch*.json"))
    require(actual_files == expected_files, "result directory contains missing or extra launch files")
    bin_args = [command[i + 1] for i, item in enumerate(command[:-1]) if item == "--bin"]
    require(len(bin_args) == 2, "quiet summary must name exactly two binaries")
    binaries = {}
    for arg in bin_args:
        require("=" in arg, "quiet summary has an unlabeled binary")
        label, path = arg.split("=", 1)
        require(label in ("control", "candidate") and label not in binaries,
                "quiet summary has duplicate or unknown binary labels")
        expected = (label, "qb-vs-others", "build", "qb922", "bin",
                    f"qvo-qb-{benchmark.replace('/', '-')}")
        require(Path(path).parts[-len(expected):] == expected,
                f"quiet summary maps {label} to another checkout")
        binaries[label] = path
    require(set(binaries) == {"control", "candidate"}, "quiet summary omitted a binary label")
    require(not require_binaries or binary_manifest is not None,
            "binary hash verification requires --binary-manifest")
    hashes_checked = False
    if binary_manifest is not None:
        recorded = {}
        for line in binary_manifest.read_text(encoding="utf-8").splitlines():
            fields = line.split(maxsplit=1)
            require(len(fields) == 2 and re.fullmatch(r"[0-9a-f]{64}", fields[0]) is not None,
                    "binary manifest has a malformed SHA-256 row")
            path = fields[1].lstrip(" *")
            require(path not in recorded, "binary manifest repeats a path")
            recorded[path] = fields[0]
        for label, path in binaries.items():
            require(path in recorded, f"binary manifest omits {label} checkout")
            if require_binaries:
                require(Path(path).is_file(), f"{label} binary is unavailable for hash verification")
                require(hashlib.sha256(Path(path).read_bytes()).hexdigest() == recorded[path],
                        f"{label} binary hash differs from manifest")
        hashes_checked = require_binaries
    rng = random.Random(37860027295)
    bootstrap = sorted(statistics.median(rng.choices(paired, k=launches)) for _ in range(10_000))
    q1, q3 = quartiles(paired)
    cq1, cq3 = quartiles(observed["control"])
    bq1, bq3 = quartiles(observed["candidate"])
    median = statistics.median(paired)
    lo, hi = bootstrap[249], bootstrap[9749]
    return {
        "benchmark": benchmark, "config": config, "launches_per_side": launches,
        "control_ns_per_unit_p50": statistics.median(observed["control"]),
        "control_ns_per_unit_iqr": cq3 - cq1,
        "control_ns_per_unit_range": [min(observed["control"]), max(observed["control"])],
        "candidate_ns_per_unit_p50": statistics.median(observed["candidate"]),
        "candidate_ns_per_unit_iqr": bq3 - bq1,
        "candidate_ns_per_unit_range": [min(observed["candidate"]), max(observed["candidate"])],
        "paired_delta_percent_p50": median, "paired_delta_percent_iqr": q3 - q1,
        "paired_delta_percent_bootstrap_95": [lo, hi],
        "control_first_delta_percent_p50": statistics.median(by_order["control-first"]),
        "candidate_first_delta_percent_p50": statistics.median(by_order["candidate-first"]),
        "candidate_faster_pairs": sum(delta < 0 for delta in paired),
        "binary_hashes_checked": hashes_checked,
        "screening_threshold_percent": 5.6,
        "screening_only": abs(median) > 5.6 and lo * hi > 0
        and statistics.median(by_order["control-first"]) * statistics.median(by_order["candidate-first"]) > 0,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--results", required=True, type=Path)
    ap.add_argument("--quiet-summary", required=True, type=Path)
    ap.add_argument("--config", required=True, choices=sorted(CONFIGS))
    ap.add_argument("--launches", type=int, default=12)
    ap.add_argument("--cpus", required=True)
    ap.add_argument("--binary-manifest", type=Path,
                    help="pre-run sha256sum file binding both checkout paths to their binaries")
    ap.add_argument("--require-binaries", action="store_true",
                    help="rehash both local binaries and fail if they are unavailable or changed")
    args = ap.parse_args()
    try:
        print(json.dumps(analyze(args.results, args.quiet_summary, args.config, args.launches, args.cpus,
                                 args.binary_manifest, args.require_binaries),
                         indent=2, sort_keys=True))
    except (KeyError, ValueError, OSError, json.JSONDecodeError) as error:
        ap.exit(1, f"analyze-paired-census.py: {error}\n")


if __name__ == "__main__":
    main()
