#!/usr/bin/env python3
"""Controls for the actual actor-memory probes; no accepted performance evidence.

--self-test uses explicitly synthetic JSON and no processes.
--bin DIR --out NEW_DIR executes only small correctness populations and preserves raw outputs.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys

FRAMEWORKS = ("qb", "caf", "sobjectizer", "baseline")


class Rejected(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Rejected(message)


def integer(value):
    return type(value) is int and 0 <= value <= 2**64 - 1


def load(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate field: {key}")
            result[key] = value
        return result
    def nonfinite(value):
        raise Rejected(f"nonfinite JSON: {value}")
    try:
        return json.loads(raw, object_pairs_hook=unique, parse_constant=nonfinite)
    except (UnicodeError, ValueError) as error:
        raise Rejected(f"invalid memory JSON: {error}") from error


def validate(doc, framework, actors, cores):
    require(isinstance(doc, dict), "memory document must be an object")
    require(doc.get("schema") == "qvo-actor-memory/1" and doc.get("verified") is True, "unverified memory document")
    require(doc.get("framework") == framework, "framework mismatch")
    require(integer(doc.get("actors")) and doc["actors"] == actors
            and integer(doc.get("cores")) and doc["cores"] == cores, "population parameters mismatch")
    require(doc.get("role") == ("floor" if framework == "baseline" else "framework"), "framework/floor role mismatch")
    require(doc.get("pinned") is False and doc.get("profile") == "park", "placement/profile mismatch")
    for field in ("framework_version", "compiler", "compiler_version", "build_type", "cxx_flags", "cxx_flags_scope", "caveat"):
        require(isinstance(doc.get(field), str) and bool(doc[field]), f"missing provenance: {field}")
    samples = doc.get("samples")
    require(isinstance(samples, list) and len(samples) == 3, "three memory phases required")
    metric = None
    expected = ((0, 0, 0, 0, 0), (actors, actors, actors, 0, cores), (actors, actors, 0, actors, cores))
    for row, phase, population in zip(samples, ("before", "live", "destroyed"), expected):
        require(isinstance(row, dict) and row.get("phase") == phase, "memory phase order mismatch")
        require(integer(row.get("rss_kib")) and integer(row.get("secondary_kib")), "invalid memory value")
        require(row.get("secondary_metric") in ("private_commit", "rss_high_water"), "invalid secondary metric")
        metric = metric or row["secondary_metric"]
        require(row["secondary_metric"] == metric, "secondary metric drift")
        for key, value in zip(("born", "ready", "alive", "destroyed", "infrastructure_ready"), population):
            require(integer(row.get(key)) and row[key] == value, f"population mismatch: {phase}/{key}")
        region = row.get("qb_global_regions")
        if framework == "qb":
            require(isinstance(region, dict) and set(region) == {"cached_bytes", "lent_bytes", "peak_bytes"}
                    and all(integer(value) for value in region.values()), "invalid global region counters")
        else:
            require(region is None, "qb counters attributed to another framework")
    arenas = doc.get("qb_thread_arenas")
    if framework == "qb":
        require(isinstance(arenas, dict), "missing owner-thread arena snapshots")
        require(arenas.get("snapshot_stage") == "sentinel_readiness_after_startup_init", "arena snapshot stage mismatch")
        require(arenas.get("scope") == "whole_owner_thread_pooled_arena", "arena scope mismatch")
        require(integer(arenas.get("infrastructure_actors")) and arenas["infrastructure_actors"] == cores,
                "arena infrastructure count mismatch")
        rows = arenas.get("cores")
        require(isinstance(rows, list) and len(rows) == cores, "arena core coverage mismatch")
        for core, arena in enumerate(rows):
            require(isinstance(arena, dict) and integer(arena.get("core")) and arena["core"] == core,
                    "arena core identity mismatch")
            require(integer(arena.get("infrastructure_actors")) and arena["infrastructure_actors"] == 1,
                    "arena per-core infrastructure mismatch")
            require(all(integer(arena.get(key)) and arena[key] > 0 for key in ("live_blocks", "chunks", "chunk_bytes")),
                    "invalid pooled arena counters")
            # Deliberately no equality to N + infrastructure: these are whole-arena counters.
    else:
        require(arenas is None, "qb arena counters attributed to another framework")
    require(integer(doc.get("object_size_bytes")) and doc["object_size_bytes"] > 0, "invalid concrete object size")
    require(doc.get("subscription_storage_bytes") is None and doc.get("oninit_frame_bytes") is None,
            "unproved internal attribution")
    require(not any(key in doc for key in ("samples_work_ns", "work_ns", "bytes_per_actor", "rss_per_actor")),
            "memory probe must not fabricate timing or per-actor attribution")
    return doc


def fixture(actors=2, framework="qb", cores=1):
    # Synthetic parser fixture only. These values are deliberately not measurements.
    doc = {"schema": "qvo-actor-memory/1", "verified": True, "framework": framework,
           "framework_version": "synthetic", "role": "floor" if framework == "baseline" else "framework",
           "actors": actors, "cores": cores, "pinned": False, "profile": "park", "compiler": "synthetic",
           "compiler_version": "fixture", "build_type": "fixture", "cxx_flags": "synthetic flags",
           "cxx_flags_scope": "fixture", "caveat": "synthetic, no performance claim", "object_size_bytes": 16,
           "subscription_storage_bytes": None, "oninit_frame_bytes": None, "samples": []}
    for phase, counts in zip(("before", "live", "destroyed"), ((0, 0, 0, 0, 0),
                             (actors, actors, actors, 0, cores), (actors, actors, 0, actors, cores))):
        row = {"phase": phase, "rss_kib": 0, "secondary_kib": 0, "secondary_metric": "rss_high_water",
               "qb_global_regions": {"cached_bytes": 0, "lent_bytes": 0, "peak_bytes": 0} if framework == "qb" else None}
        row.update(zip(("born", "ready", "alive", "destroyed", "infrastructure_ready"), counts))
        doc["samples"].append(row)
    doc["qb_thread_arenas"] = {
        "snapshot_stage": "sentinel_readiness_after_startup_init", "scope": "whole_owner_thread_pooled_arena",
        "infrastructure_actors": cores,
        # Extra live blocks intentionally differ from actors + infrastructure.
        "cores": [{"core": core, "live_blocks": 7 + core, "chunks": 1, "chunk_bytes": 65536,
                   "infrastructure_actors": 1} for core in range(cores)]
    } if framework == "qb" else None
    return doc


def self_test():
    for actors, framework, cores in ((0, "qb", 1), (2, "qb", 1), (2, "qb", 2), (2, "baseline", 1)):
        validate(load(json.dumps(fixture(actors, framework, cores))), framework, actors, cores)
        print(f"CONFIRMED synthetic complete population {framework}/{actors}/{cores}, including valid zero RSS and whole-arena counts")
    plants = (
        ("missing-live-actor", lambda d: d["samples"][1].update(alive=1), "live/alive"),
        ("constructed-not-ready", lambda d: d["samples"][1].update(ready=1), "live/ready"),
        ("early-destruction", lambda d: d["samples"][1].update(destroyed=1), "live/destroyed"),
        ("missing-destruction", lambda d: d["samples"][2].update(destroyed=1), "destroyed/destroyed"),
        ("missing-memory", lambda d: d["samples"][1].pop("rss_kib"), "memory value"),
        ("negative-memory", lambda d: d["samples"][1].update(rss_kib=-1), "memory value"),
        ("boolean-memory", lambda d: d["samples"][1].update(rss_kib=True), "memory value"),
        ("metric-drift", lambda d: d["samples"][1].update(secondary_metric="private_commit"), "metric drift"),
        ("wrong-identity", lambda d: d.update(framework="caf"), "framework mismatch"),
        ("wrong-population", lambda d: d.update(actors=1), "parameters mismatch"),
        ("arena-missing", lambda d: d.pop("qb_thread_arenas"), "missing owner-thread"),
        ("arena-missing-core", lambda d: d["qb_thread_arenas"]["cores"].clear(), "core coverage"),
        ("arena-wrong-core", lambda d: d["qb_thread_arenas"]["cores"][0].update(core=1), "core identity"),
        ("arena-wrong-stage", lambda d: d["qb_thread_arenas"].update(snapshot_stage="live_rss"), "snapshot stage"),
        ("arena-wrong-scope", lambda d: d["qb_thread_arenas"].update(scope="per_actor_rss"), "arena scope"),
        ("arena-no-infrastructure", lambda d: d["qb_thread_arenas"].update(infrastructure_actors=0), "infrastructure count"),
        ("arena-zero-from-controller", lambda d: d["qb_thread_arenas"]["cores"][0].update(live_blocks=0), "pooled arena counters"),
        ("arena-negative-bytes", lambda d: d["qb_thread_arenas"]["cores"][0].update(chunk_bytes=-1), "pooled arena counters"),
    )
    original = fixture()
    for name, mutate, diagnostic in plants:
        planted = copy.deepcopy(original)
        mutate(planted)
        reread = load(json.dumps(planted))
        require(reread != original, f"plant did not land: {name}")
        try:
            validate(reread, "qb", 2, 1)
        except Rejected as error:
            require(diagnostic in str(error), f"wrong diagnostic: {name}: {error}")
            print(f"CAUGHT {name}: {error}")
        else:
            raise Rejected(f"plant not rejected: {name}")


    duplicate = load(json.dumps(fixture(2, "qb", 2)))
    duplicate["qb_thread_arenas"]["cores"][1]["core"] = 0
    reread = load(json.dumps(duplicate))
    require(len(reread["qb_thread_arenas"]["cores"]) == 2
            and [row["core"] for row in reread["qb_thread_arenas"]["cores"]] == [0, 0],
            "duplicate core plant did not land")
    try:
        validate(reread, "qb", 2, 2)
    except Rejected as error:
        require("arena core identity mismatch" in str(error), "wrong duplicate core diagnostic")
        print(f"CAUGHT arena-duplicate-core-with-complete-length: {error}")
    else:
        raise Rejected("duplicate arena core not rejected")


def process_controls(args):
    args.out.mkdir(parents=True, exist_ok=False)
    extension = ".exe" if os.name == "nt" else ""
    for framework in args.frameworks:
        for mode, actors in (("zero", 0), ("live", 3), ("missing", 3)):
            prefix = "qvoctl-actor-memory-missing-" if mode == "missing" else "qvoprobe-actor-memory-"
            binary = (args.bin / (prefix + framework + extension)).resolve(strict=True)
            command = [str(binary), "--actors", str(actors), "--cores", str(args.cores), "--timeout-ms", "1000" if mode == "missing" else "10000"]
            name = f"{framework}-{mode}"
            try:
                result = subprocess.run(command, cwd=binary.parent, capture_output=True, timeout=20)
            except subprocess.TimeoutExpired as error:
                (args.out / f"{name}.stdout").write_bytes(error.stdout or b"")
                (args.out / f"{name}.stderr").write_bytes(error.stderr or b"")
                (args.out / f"{name}.process.json").write_text(json.dumps(
                    {"argv": command, "returncode": None, "state": "timed_out", "performance_accepted": False}, indent=2) + "\n")
                raise Rejected(f"{name}: process timeout, raw streams preserved") from error
            (args.out / f"{name}.stdout").write_bytes(result.stdout)
            (args.out / f"{name}.stderr").write_bytes(result.stderr)
            (args.out / f"{name}.process.json").write_text(json.dumps({"argv": command, "returncode": result.returncode,
                                                                      "performance_accepted": False}, indent=2) + "\n")
            doc = load(result.stdout)
            require(isinstance(doc, dict), f"{name}: memory JSON must be an object")
            if mode == "missing":
                require(result.returncode == 1 and doc.get("verified") is False and doc.get("samples") == [],
                        f"{name}: missing actor did not refuse memory")
                require(result.stderr.count(b"actor-memory plant-fired: omitted requested identity 2") == 1,
                        f"{name}: omission plant was not confirmed")
                require(doc.get("framework") == framework and "population not fully alive and ready" in doc.get("error", "")
                        and b"population not fully alive and ready" in result.stderr, f"{name}: wrong refusal diagnostic")
                print(f"CAUGHT {name}: real adapter omitted one actor, no accepted memory")
            else:
                require(result.returncode == 0, f"{name}: nonzero exit {result.returncode}")
                validate(doc, framework, actors, args.cores)
                print(f"CONFIRMED {name}: exact live and destroyed population; memory not accepted as performance evidence")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--bin", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--cores", type=int, choices=(1, 2), default=1, help="configured budget for the small process controls")
    ap.add_argument("--frameworks", choices=FRAMEWORKS, nargs="+", default=list(FRAMEWORKS))
    args = ap.parse_args()
    try:
        if args.self_test:
            require(args.bin is None and args.out is None, "self-test takes no binary/output paths")
            self_test()
        else:
            require(args.bin is not None and args.out is not None, "require --bin and fresh --out")
            require(len(args.frameworks) == len(set(args.frameworks)), "duplicate framework")
            process_controls(args)
        return 0
    except (Rejected, OSError, subprocess.TimeoutExpired) as error:
        print(f"actor-memory-control: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
