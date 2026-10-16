#!/usr/bin/env python3
"""Regression controls for collection provenance, units and rendered distributions.

Runner and census controls replace subprocess at the process boundary and work on POSIX and
Windows. Sweep controls run the real Bash entry points against tiny Python producers on POSIX;
they explicitly skip native Windows. Result-contract and renderer controls need no shell.
No control builds or executes a framework benchmark or measures performance.
"""

from __future__ import annotations

import contextlib
import io
import importlib.util
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run
import report

_census_spec = importlib.util.spec_from_file_location(
    "launch_census", Path(__file__).with_name("launch-census.py"))
assert _census_spec is not None and _census_spec.loader is not None
census = importlib.util.module_from_spec(_census_spec)
_census_spec.loader.exec_module(census)


class RunnerProvenanceTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="qvo-run-control-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.bindir = self.root / "build" / "bin"
        self.bindir.mkdir(parents=True)
        (self.bindir / "qvo-fake.exe").touch()
        self.out = self.root / "results"
        self.dest = self.out / "savina-ping-pong" / "qb__1c-spin.json"
        self.dest.parent.mkdir(parents=True)
        self.launches = 0

    @staticmethod
    def doc(*, verified: bool, reason: str | None = None) -> dict:
        result = {
            "schema": "qvo/result/1", "benchmark": "savina/ping-pong", "framework": "qb",
            "framework_version": "test", "verified": verified,
            "params": {"cores": 1, "wait": 1}, "repetitions": 1, "warmup": 0,
            "pinned": False, "cpus": "", "env": {"host": platform.node()},
            "work_ns": [42.0] if verified else [],
            "summary": {"work_p50": 42.0} if verified else {},
        }
        if reason is not None:
            result["not_applicable"] = reason
        if not verified and reason is None:
            result["failures"] = ["current attempt failed"]
        return result

    def seed(self, doc: dict) -> bytes:
        old = (json.dumps(doc, indent=2) + "\n").encode()
        self.dest.write_bytes(old)
        return old

    def invoke(self, behavior, *, config: str | None = "1c-spin", only: str | None = "qb",
               benchmark: str | None = "savina/ping-pong"):
        args = ["run.py", "--build", str(self.root / "build"), "--out", str(self.out),
                "--no-pin", "--repetitions", "1", "--warmup", "0"]
        for flag, value in (("only", only), ("benchmark", benchmark), ("config", config)):
            if value is not None:
                args += [f"--{flag}", value]

        def child(cmd, **_kwargs):
            if "--describe" in cmd:
                description = {"framework": "qb", "benchmark": "savina/ping-pong", "params": {}}
                return subprocess.CompletedProcess(cmd, 0, json.dumps(description), "")
            self.launches += 1
            return behavior(cmd)

        with patch.object(sys, "argv", args), patch.object(run.platform, "platform", return_value="test-platform"), \
                patch.object(run.subprocess, "run", side_effect=child):
            with contextlib.redirect_stdout(io.StringIO()):
                return run.main()

    def test_stale_not_applicable_cannot_validate_exit_three_without_document(self) -> None:
        self.seed(self.doc(verified=False, reason="old n/a"))
        rc = self.invoke(lambda cmd: subprocess.CompletedProcess(cmd, 3, "", "aborted"))
        current = json.loads(self.dest.read_text())
        manifest = json.loads((self.out / "run.json").read_text())
        self.assertEqual(rc, 1)
        self.assertFalse(current["verified"])
        self.assertNotIn("not_applicable", current)
        self.assertEqual(current["work_ns"], [])
        self.assertEqual(manifest["cells"][0]["status"], "failed")

    def test_stale_timing_is_replaced_after_failed_attempt(self) -> None:
        for outcome in (1, 2, "timeout"):
            with self.subTest(outcome=outcome):
                self.seed(self.doc(verified=True))

                def behavior(cmd):
                    if outcome == "timeout":
                        raise subprocess.TimeoutExpired(cmd, 1)
                    return subprocess.CompletedProcess(cmd, outcome, "", "aborted")

                rc = self.invoke(behavior)
                current = json.loads(self.dest.read_text())
                self.assertEqual(rc, 1)
                self.assertFalse(current["verified"])
                self.assertEqual(current["work_ns"], [])
                self.assertEqual(current["summary"], {})

    def test_incompatible_partial_run_changes_nothing(self) -> None:
        before_cell = self.seed(self.doc(verified=True))
        prior = {"schema": "qvo/run/1", "host": platform.node(),
                 "platform": "test-platform", "cpus": "different",
                 "repetitions": 1, "warmup": 0, "cells": []}
        manifest = self.out / "run.json"
        before_manifest = (json.dumps(prior) + "\n").encode()
        manifest.write_bytes(before_manifest)

        def behavior(cmd):
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        with self.assertRaises(SystemExit):
            self.invoke(behavior)
        self.assertEqual(self.launches, 0)
        self.assertEqual(self.dest.read_bytes(), before_cell)
        self.assertEqual(manifest.read_bytes(), before_manifest)

    def test_child_output_is_staged_beside_the_destination(self) -> None:
        seen = []

        def behavior(cmd):
            target = Path(cmd[cmd.index("--out") + 1])
            seen.append(target)
            target.write_text(json.dumps(self.doc(verified=True)))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        rc = self.invoke(behavior)
        self.assertEqual(rc, 0)
        self.assertEqual(len(seen), 1)
        self.assertNotEqual(seen[0], self.dest)
        self.assertEqual(seen[0].parent, self.dest.parent)
        self.assertFalse(seen[0].exists())
        self.assertTrue(json.loads(self.dest.read_text())["verified"])

    def test_fresh_not_applicable_is_accepted(self) -> None:
        def behavior(cmd):
            Path(cmd[cmd.index("--out") + 1]).write_text(
                json.dumps(self.doc(verified=False, reason="no spin mode")))
            return subprocess.CompletedProcess(cmd, 3, "", "")

        rc = self.invoke(behavior)
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(self.dest.read_text())["not_applicable"], "no spin mode")
        self.assertEqual(json.loads((self.out / "run.json").read_text())["cells"][0]["status"], "n/a")

    def test_wrong_identity_is_not_published(self) -> None:
        self.seed(self.doc(verified=True))

        def behavior(cmd):
            wrong = self.doc(verified=True)
            wrong["benchmark"] = "savina/counting"
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(wrong))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        rc = self.invoke(behavior)
        self.assertEqual(rc, 1)
        self.assertFalse(json.loads(self.dest.read_text())["verified"])

    def test_wrong_parameters_or_verdict_are_not_published(self) -> None:
        for change in ("params", "verdict"):
            with self.subTest(change=change):
                self.seed(self.doc(verified=True))

                def behavior(cmd):
                    wrong = self.doc(verified=False, reason="no spin mode")
                    if change == "params":
                        wrong["params"]["wait"] = 0
                    else:
                        wrong["verified"] = True
                    Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(wrong))
                    return subprocess.CompletedProcess(cmd, 3, "", "")

                self.assertEqual(self.invoke(behavior), 1)
                current = json.loads(self.dest.read_text())
                self.assertFalse(current["verified"])
                self.assertNotIn("not_applicable", current)

    def test_exit_one_requires_a_valid_failure_document(self) -> None:
        for schema in ("qvo/result/1", "wrong/schema"):
            with self.subTest(schema=schema):
                self.seed(self.doc(verified=True))

                def behavior(cmd):
                    failure = self.doc(verified=False)
                    failure["schema"] = schema
                    Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(failure))
                    return subprocess.CompletedProcess(cmd, 1, "", "")

                self.assertEqual(self.invoke(behavior), 1)
                current = json.loads(self.dest.read_text())
                self.assertFalse(current["verified"])
                self.assertEqual(current["work_ns"], [])
                if schema == "qvo/result/1":
                    self.assertEqual(current["failures"], ["current attempt failed"])
                    self.assertEqual(json.loads((self.out / "run.json").read_text())
                                     ["cells"][0]["status"], "unverified")
                else:
                    self.assertIn("schema", current["failures"][0])

    def test_interrupted_run_marker_blocks_report(self) -> None:
        self.seed(self.doc(verified=True))

        def interrupted(_cmd):
            raise RuntimeError("runner interrupted after publishing the marker")

        with self.assertRaises(RuntimeError):
            self.invoke(interrupted)
        self.assertEqual(json.loads((self.out / "run.json").read_text())["status"],
                         "in_progress")
        with self.assertRaises(SystemExit) as cm:
            report.load(self.out)
        self.assertIn("incomplete run", str(cm.exception))

    def test_failed_atomic_cell_replacement_keeps_report_blocked(self) -> None:
        self.seed(self.doc(verified=True))
        replace = os.replace

        def fail_cell_replacement(source, target):
            if Path(target) == self.dest:
                raise OSError("simulated replacement failure")
            return replace(source, target)

        def behavior(cmd):
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        with patch.object(run.os, "replace", side_effect=fail_cell_replacement):
            with self.assertRaises(OSError):
                self.invoke(behavior)
        self.assertEqual(json.loads((self.out / "run.json").read_text())["status"],
                         "in_progress")
        self.assertTrue(json.loads(self.dest.read_text())["verified"])
        with self.assertRaises(SystemExit):
            report.load(self.out)

    def test_compatible_partial_run_keeps_other_cells(self) -> None:
        prior = {"schema": "qvo/run/1", "host": platform.node(),
                 "platform": "test-platform", "cpus": "unpinned",
                 "repetitions": 1, "warmup": 0,
                 "cells": [{"benchmark": "savina/counting", "framework": "caf",
                            "config": "2c-park", "status": "ok"}]}
        (self.out / "run.json").write_text(json.dumps(prior))
        previous_doc = self.doc(verified=True)
        previous_doc.update(benchmark="savina/counting", framework="caf",
                            params={"cores": 2, "wait": 0})
        previous_path = self.out / "savina-counting" / "caf__2c-park.json"
        previous_path.parent.mkdir()
        previous_path.write_text(json.dumps(previous_doc))

        def behavior(cmd):
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        self.assertEqual(self.invoke(behavior), 0)
        manifest = json.loads((self.out / "run.json").read_text())
        self.assertEqual(manifest["status"], "complete")
        self.assertEqual(manifest["merged_partial_runs"], 1)
        self.assertEqual(len(manifest["cells"]), 2)
        self.assertEqual(manifest["cells"][0], prior["cells"][0])
        self.assertEqual(len(report.load(self.out)), 2)

    def test_second_runner_merges_manifest_written_before_its_lock(self) -> None:
        for initial_manifest in (True, False):
            with self.subTest(initial_manifest=initial_manifest):
                shutil.rmtree(self.out)
                self.dest.parent.mkdir(parents=True)
                if initial_manifest:
                    prior = {"schema": "qvo/run/1", "host": platform.node(),
                             "platform": "test-platform", "cpus": "unpinned",
                             "repetitions": 1, "warmup": 0, "cells": []}
                    (self.out / "run.json").write_text(json.dumps(prior))
                mkdir = Path.mkdir
                first_finished = False

                def first_child(cmd):
                    result = self.doc(verified=True)
                    result["params"] = {"cores": 2, "wait": 0}
                    Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(result))
                    return subprocess.CompletedProcess(cmd, 0, "", "")

                def second_child(cmd):
                    Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
                    return subprocess.CompletedProcess(cmd, 0, "", "")

                def interleave_at_lock(path, *args, **kwargs):
                    nonlocal first_finished
                    if path == self.out / ".run.lock" and not first_finished:
                        first_finished = True
                        self.assertEqual(self.invoke(first_child, config="2c-park"), 0)
                    return mkdir(path, *args, **kwargs)

                # B reaches lock.mkdir after the old code snapshotted run.json (or its absence).
                # A then finishes under the hook. B must merge A's cell after acquiring the lock.
                with patch.object(Path, "mkdir", new=interleave_at_lock):
                    self.assertEqual(self.invoke(second_child), 0)
                self.assertTrue(first_finished)
                manifest = json.loads((self.out / "run.json").read_text())
                configs = {cell["config"] for cell in manifest["cells"]}
                self.assertEqual(configs, {"1c-spin", "2c-park"})
                self.assertEqual(set(report.load(self.out)["savina/ping-pong"]), configs)

    def test_existing_lock_refuses_concurrent_runner(self) -> None:
        (self.out / ".run.lock").mkdir()

        def behavior(_cmd):
            self.fail("a locked run must not launch a benchmark")

        with self.assertRaises(SystemExit) as cm:
            self.invoke(behavior)
        self.assertIn("another run may be active", str(cm.exception))
        self.assertEqual(self.launches, 0)
        self.assertFalse((self.out / "run.json").exists())

    def test_discovery_runs_only_while_output_lock_is_held(self) -> None:
        discover = run.discover
        called = False

        def observe(build_dir):
            nonlocal called
            called = True
            self.assertTrue((self.out / ".run.lock").is_dir())
            return discover(build_dir)

        def behavior(cmd):
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        with patch.object(run, "discover", side_effect=observe):
            self.assertEqual(self.invoke(behavior), 0)
        self.assertTrue(called)

    def test_existing_output_lock_prevents_even_discovery(self) -> None:
        (self.out / ".run.lock").mkdir()
        with patch.object(run, "discover", side_effect=AssertionError("discovery ran")):
            with self.assertRaises(SystemExit) as cm:
                self.invoke(lambda _cmd: self.fail("benchmark launched"))
        self.assertIn("another run may be active", str(cm.exception))
        self.assertEqual(self.launches, 0)
        self.assertFalse((self.out / "run.json").exists())

    def test_empty_selected_roster_cannot_complete_a_run(self) -> None:
        prior = {"schema": "qvo/run/1", "host": platform.node(),
                 "platform": "test-platform", "cpus": "unpinned",
                 "repetitions": 1, "warmup": 0,
                 "cells": [{"benchmark": "savina/ping-pong", "framework": "qb",
                            "config": "1c-spin", "status": "ok"}]}
        for existing in (False, True):
            for filters in ({"only": "ghost"}, {"benchmark": "savina/ghost"},
                            {"config": "ghost"}):
                with self.subTest(existing=existing, filters=filters):
                    before_cell = self.seed(self.doc(verified=True)) if existing else None
                    if not existing:
                        self.dest.unlink(missing_ok=True)
                    manifest = self.out / "run.json"
                    before_manifest = (json.dumps(prior) + "\n").encode() if existing else None
                    if existing:
                        manifest.write_bytes(before_manifest)
                    else:
                        manifest.unlink(missing_ok=True)

                    def behavior(_cmd):
                        self.fail("empty selection launched a benchmark")

                    with self.assertRaises(SystemExit) as cm:
                        self.invoke(behavior, **filters)
                    self.assertIn("unknown", str(cm.exception))
                    self.assertEqual(self.launches, 0)
                    self.assertEqual(manifest.read_bytes() if manifest.exists() else None,
                                     before_manifest)
                    self.assertEqual(self.dest.read_bytes() if self.dest.exists() else None,
                                     before_cell)
                    self.assertFalse((self.out / ".run.lock").exists())

    def test_known_filters_with_empty_intersection_change_nothing(self) -> None:
        before_cell = self.seed(self.doc(verified=True))
        fake_caf = self.bindir / "qvo-caf.exe"
        fake_caf.touch()
        discovered = [("qb", "savina/ping-pong", self.bindir / "qvo-fake.exe"),
                      ("caf", "savina/counting", fake_caf)]
        with patch.object(run, "discover", return_value=discovered):
            with self.assertRaises(SystemExit) as cm:
                self.invoke(lambda _cmd: self.fail("empty intersection launched a benchmark"),
                            benchmark="savina/counting")
        self.assertIn("no cells selected", str(cm.exception))
        self.assertEqual(self.launches, 0)
        self.assertEqual(self.dest.read_bytes(), before_cell)
        self.assertFalse((self.out / "run.json").exists())
        self.assertFalse((self.out / ".run.lock").exists())

    def test_mixed_valid_and_missing_filter_token_changes_nothing(self) -> None:
        caf_path = self.dest.parent / "caf__1c-spin.json"
        caf = self.doc(verified=True)
        caf["framework"] = "caf"
        prior = {"schema": "qvo/run/1", "status": "complete", "host": platform.node(),
                 "platform": "test-platform", "cpus": "unpinned",
                 "repetitions": 1, "warmup": 0,
                 "cells": [{"benchmark": "savina/ping-pong", "framework": framework,
                            "config": "1c-spin", "status": "ok"} for framework in ("qb", "caf")]}
        manifest = self.out / "run.json"
        for filters in ({"only": "qb,caf"},
                        {"benchmark": "savina/ping-pong,savina/ghost"},
                        {"config": "1c-spin,ghost"}):
            with self.subTest(filters=filters):
                self.launches = 0
                before_qb = self.seed(self.doc(verified=True))
                before_caf = (json.dumps(caf) + "\n").encode()
                caf_path.write_bytes(before_caf)
                before_manifest = (json.dumps(prior) + "\n").encode()
                manifest.write_bytes(before_manifest)

                def behavior(cmd):
                    Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
                    return subprocess.CompletedProcess(cmd, 0, "", "")

                with self.assertRaises(SystemExit) as cm:
                    self.invoke(behavior, **filters)
                self.assertIn("unknown", str(cm.exception))
                self.assertEqual(self.launches, 0)
                self.assertEqual(self.dest.read_bytes(), before_qb)
                self.assertEqual(caf_path.read_bytes(), before_caf)
                self.assertEqual(manifest.read_bytes(), before_manifest)
                self.assertFalse((self.out / ".run.lock").exists())

    def test_selected_prior_cell_without_current_binary_is_refused(self) -> None:
        fake_caf = self.bindir / "qvo-caf.exe"
        fake_caf.touch()
        discovered = [("qb", "savina/ping-pong", self.bindir / "qvo-fake.exe"),
                      ("caf", "savina/counting", fake_caf)]
        before_qb = self.seed(self.doc(verified=True))
        caf_path = self.dest.parent / "caf__1c-spin.json"
        caf = self.doc(verified=True)
        caf["framework"] = "caf"
        before_caf = (json.dumps(caf) + "\n").encode()
        caf_path.write_bytes(before_caf)
        prior = {"schema": "qvo/run/1", "status": "complete", "host": platform.node(),
                 "platform": "test-platform", "cpus": "unpinned",
                 "repetitions": 1, "warmup": 0,
                 "cells": [{"benchmark": "savina/ping-pong", "framework": framework,
                            "config": "1c-spin", "status": "ok"} for framework in ("qb", "caf")]}
        manifest = self.out / "run.json"
        before_manifest = (json.dumps(prior) + "\n").encode()
        manifest.write_bytes(before_manifest)

        with patch.object(run, "discover", return_value=discovered):
            with self.assertRaises(SystemExit) as cm:
                self.invoke(lambda _cmd: self.fail("partial field launched a measurement"),
                            only="qb,caf", benchmark="savina/ping-pong")
        self.assertIn("previous selected cell", str(cm.exception))
        self.assertEqual(self.launches, 0)
        self.assertEqual(self.dest.read_bytes(), before_qb)
        self.assertEqual(caf_path.read_bytes(), before_caf)
        self.assertEqual(manifest.read_bytes(), before_manifest)
        self.assertFalse((self.out / ".run.lock").exists())

    def test_explicit_framework_benchmark_pair_must_exist_without_prior_manifest(self) -> None:
        fake_caf = self.bindir / "qvo-caf.exe"
        fake_caf.touch()
        discovered = [("qb", "savina/ping-pong", self.bindir / "qvo-fake.exe"),
                      ("caf", "savina/counting", fake_caf)]
        with patch.object(run, "discover", return_value=discovered):
            with self.assertRaises(SystemExit) as cm:
                self.invoke(lambda _cmd: self.fail("unsupported explicit pair was measured"),
                            only="qb,caf", benchmark="savina/ping-pong")
        self.assertIn("explicit framework/benchmark pair", str(cm.exception))
        self.assertEqual(self.launches, 0)
        self.assertFalse((self.out / "run.json").exists())
        self.assertFalse((self.out / ".run.lock").exists())

    def test_implicit_unsupported_pairs_do_not_block_supported_union(self) -> None:
        fake_caf = self.bindir / "qvo-caf.exe"
        fake_caf.touch()
        discovered = [("qb", "savina/ping-pong", self.bindir / "qvo-fake.exe"),
                      ("caf", "savina/counting", fake_caf)]

        def behavior(cmd):
            result = self.doc(verified=True)
            if Path(cmd[0]) == fake_caf:
                result.update(framework="caf", benchmark="savina/counting")
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(result))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        with patch.object(run, "discover", return_value=discovered):
            self.assertEqual(self.invoke(behavior, only="qb,caf", benchmark=None), 0)
        manifest = json.loads((self.out / "run.json").read_text())
        self.assertEqual({(cell["framework"], cell["benchmark"]) for cell in manifest["cells"]},
                         {("qb", "savina/ping-pong"), ("caf", "savina/counting")})
        self.assertEqual(self.launches, 2)

    def test_prior_cell_outside_framework_filter_is_preserved(self) -> None:
        caf_path = self.dest.parent / "caf__1c-spin.json"
        caf = self.doc(verified=True)
        caf["framework"] = "caf"
        before_caf = (json.dumps(caf) + "\n").encode()
        caf_path.write_bytes(before_caf)
        prior = {"schema": "qvo/run/1", "status": "complete", "host": platform.node(),
                 "platform": "test-platform", "cpus": "unpinned",
                 "repetitions": 1, "warmup": 0,
                 "cells": [{"benchmark": "savina/ping-pong", "framework": "caf",
                            "config": "1c-spin", "status": "ok"}]}
        (self.out / "run.json").write_text(json.dumps(prior))

        def behavior(cmd):
            Path(cmd[cmd.index("--out") + 1]).write_text(json.dumps(self.doc(verified=True)))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        self.assertEqual(self.invoke(behavior, only="qb", benchmark="savina/ping-pong"), 0)
        manifest = json.loads((self.out / "run.json").read_text())
        self.assertEqual({cell["framework"] for cell in manifest["cells"]}, {"qb", "caf"})
        self.assertEqual(caf_path.read_bytes(), before_caf)
        self.assertEqual(set(report.load(self.out)["savina/ping-pong"]["1c-spin"]),
                         {"qb", "caf"})

    def test_incomplete_manifest_requires_a_new_or_cleared_directory(self) -> None:
        before_cell = self.seed(self.doc(verified=True))
        manifest = self.out / "run.json"
        prior = {"schema": "qvo/run/1", "status": "in_progress", "cells": []}
        before_manifest = (json.dumps(prior) + "\n").encode()
        manifest.write_bytes(before_manifest)

        with self.assertRaises(SystemExit) as cm:
            self.invoke(lambda _cmd: self.fail("incomplete run must not launch"))
        self.assertIn("interrupted run", str(cm.exception))
        self.assertEqual(self.launches, 0)
        self.assertEqual(self.dest.read_bytes(), before_cell)
        self.assertEqual(manifest.read_bytes(), before_manifest)
        with self.assertRaises(SystemExit):
            report.load(self.out)

    def test_report_refuses_canonical_cell_absent_from_complete_manifest(self) -> None:
        self.seed(self.doc(verified=True))
        obsolete = self.dest.parent / "caf__1c-spin.json"
        stale = self.doc(verified=True)
        stale["framework"] = "caf"
        obsolete.write_text(json.dumps(stale))
        manifest = {"schema": "qvo/run/1", "status": "complete",
                    "cells": [{"benchmark": "savina/ping-pong", "framework": "qb",
                               "config": "1c-spin", "status": "ok"}]}
        (self.out / "run.json").write_text(json.dumps(manifest))
        with self.assertRaises(SystemExit) as cm:
            report.load(self.out)
        self.assertIn("absent from run.json", str(cm.exception))

    def test_report_rejects_manifest_changed_while_loading_cells(self) -> None:
        self.seed(self.doc(verified=True))
        manifest = self.out / "run.json"
        prior = {"schema": "qvo/run/1", "status": "complete", "cells": [
            {"benchmark": "savina/ping-pong", "framework": "qb",
             "config": "1c-spin", "status": "ok"}]}
        manifest.write_text(json.dumps(prior))
        read_text = Path.read_text
        changed = False

        def change_during_cell_read(path, *args, **kwargs):
            nonlocal changed
            if path == self.dest and not changed:
                changed = True
                manifest.write_text(json.dumps({**prior, "status": "in_progress"}))
                newer = self.doc(verified=True)
                newer["summary"]["work_p50"] = 99.0
                self.dest.write_text(json.dumps(newer))
            return read_text(path, *args, **kwargs)

        with patch.object(Path, "read_text", new=change_during_cell_read):
            with self.assertRaises(SystemExit) as cm:
                report.render(self.out)
        self.assertTrue(changed)
        self.assertIn("changed during", str(cm.exception))

    def test_report_rejects_manifest_changed_after_load_before_render(self) -> None:
        self.seed(self.doc(verified=True))
        manifest = self.out / "run.json"
        prior = {"schema": "qvo/run/1", "status": "complete", "run_id": "old", "cells": [
            {"benchmark": "savina/ping-pong", "framework": "qb",
             "config": "1c-spin", "status": "ok"}]}
        manifest.write_text(json.dumps(prior))
        load = report.load

        def change_after_load(*args, **kwargs):
            cells = load(*args, **kwargs)
            manifest.write_text(json.dumps({**prior, "run_id": "new"}))
            return cells

        with patch.object(report, "load", side_effect=change_after_load):
            with self.assertRaises(SystemExit) as cm:
                report.render(self.out)
        self.assertIn("changed during", str(cm.exception))

    def test_report_without_manifest_remains_supported(self) -> None:
        self.seed(self.doc(verified=True))
        self.assertIn("savina/ping-pong", report.load(self.out))
        self.assertIn("savina/ping-pong", report.render(self.out))

    def test_report_keeps_measured_unit_when_other_framework_failed(self) -> None:
        failed = run.failed_document("savina/ping-pong", "caf",
                                     {"cores": 1, "wait": 1}, "current child aborted")
        (self.dest.parent / "caf__1c-spin.json").write_text(json.dumps(failed))
        for unit in ("round trip", "message", "transfer"):
            with self.subTest(unit=unit):
                measured = self.doc(verified=True)
                measured.update(work_unit=unit, work_units=2)
                self.seed(measured)
                rendered = report.render(self.out)
                self.assertIn(f"per {unit}", rendered)
                self.assertIn("| `qb` test | yes |", rendered)
                self.assertIn("| `caf` | **FAILED** |", rendered)  # a runner failure: no version
                self.assertIn("current child aborted", rendered)

        # No surviving measurement means no unit can be inferred, but every failure stays visible.
        self.seed(run.failed_document("savina/ping-pong", "qb",
                                      {"cores": 1, "wait": 1}, "second child aborted"))
        rendered = report.render(self.out)
        self.assertIn("per repetition", rendered)
        self.assertEqual(rendered.count("**FAILED**"), 2)

    def test_report_names_each_cells_own_version(self) -> None:
        """QB-1011: a directory holding cells of two builds renders both versions, each on the rows
        its own documents produced. The renderer used to keep ONE version per framework -- the last
        document read -- so a planted document of another version vanished whenever it was not read
        last; it is planted in both positions here, so either order is a witness."""
        counting = self.out / "savina-counting" / "qb__1c-spin.json"
        counting.parent.mkdir()
        for planted_in in ("savina/counting", "savina/ping-pong"):  # read first, read last
            with self.subTest(planted_in=planted_in):
                for bench, path in (("savina/counting", counting), ("savina/ping-pong", self.dest)):
                    d = self.doc(verified=True)
                    d.update(benchmark=bench,
                             framework_version="9.9.9-planted" if bench == planted_in else "3.2.1")
                    path.write_text(json.dumps(d))
                rendered = report.render(self.out)
                # The defect itself, whatever the table's shape: both versions reach the render.
                self.assertIn("9.9.9-planted", rendered)
                self.assertIn("3.2.1", rendered)
                head, rest = rendered.split("## savina/counting\n", 1)
                first, second = rest.split("## savina/ping-pong\n", 1)
                sections = {"savina/counting": first, "savina/ping-pong": second}
                self.assertIn("| `qb` | 3.2.1 | 1 |", head)
                self.assertIn("| `qb` | 9.9.9-planted | 1 |", head)
                self.assertIn("More than one version of `qb`", head)
                for bench, text in sections.items():
                    mine, other = (("9.9.9-planted", "3.2.1") if bench == planted_in
                                   else ("3.2.1", "9.9.9-planted"))
                    self.assertIn(f"| `qb` {mine} | yes |", text)
                    self.assertNotIn(other, text)

    def test_report_with_one_version_per_framework_carries_no_mixed_warning(self) -> None:
        self.seed(self.doc(verified=True))
        caf = self.doc(verified=True)
        caf.update(framework="caf", framework_version="1.1.0")
        (self.dest.parent / "caf__1c-spin.json").write_text(json.dumps(caf))
        rendered = report.render(self.out)
        self.assertNotIn("More than one version", rendered)
        self.assertIn("| `caf` | 1.1.0 | 1 |", rendered)
        self.assertIn("| `qb` | test | 1 |", rendered)


class SweepResultContractTest(unittest.TestCase):
    """The existing process-boundary validator remains testable without Bash (QB-877)."""

    def test_sweep_result_requires_the_requested_identity_parameters_and_success(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "attempt.json"
            for framework, repetitions in (("sobjectizer", 7), ("caf", 5)):
                for fault in (None, "benchmark", "framework", "cores", "wait", "messages",
                              "verified", "repetitions", "warmup", "exit", "missing", "malformed"):
                    with self.subTest(framework=framework, fault=fault):
                        doc = RunnerProvenanceTest.doc(verified=True)
                        params = {"cores": 2, "wait": 1, "messages": 1000000}
                        doc.update(framework=framework, params=dict(params), repetitions=repetitions,
                                   warmup=2, work_ns=[111000000.0] * repetitions,
                                   summary={"work_p50": 111000000.0})
                        if fault in ("benchmark", "framework"):
                            doc[fault] = "wrong"
                        elif fault in params:
                            doc["params"][fault] += 1
                        elif fault == "verified":
                            doc["verified"] = False
                        elif fault in ("repetitions", "warmup"):
                            doc[fault] += 1
                        path.write_text("{" if fault == "malformed" else json.dumps(doc), encoding="utf-8")
                        if fault == "missing":
                            path.unlink()
                        accepted, reason = run.read_attempt(
                            path, "savina/ping-pong", framework, params,
                            42 if fault == "exit" else 0, repetitions, 2)
                        if fault is None:
                            self.assertEqual(accepted, doc)
                            self.assertEqual(reason, "")
                        else:
                            self.assertIsNone(accepted)
                            self.assertTrue(reason)


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"),
                     "real sweep entry points need POSIX Bash; portable result/report controls still run")
class SweepEntrypointTest(unittest.TestCase):
    """QB-877/852: execute the shipped shell scripts, never a copied loop or a real benchmark."""

    PRODUCER = '''#!/usr/bin/env python3
import json, os, pathlib, statistics, sys
args = sys.argv[1:]
params = {}
for i, arg in enumerate(args):
    if arg == "--param":
        key, value = args[i + 1].split("=", 1)
        params[key] = int(value)
name = pathlib.Path(sys.argv[0]).name
framework = "caf" if name.startswith("qvo-caf-") else "sobjectizer"
shape = "counting" if "counting" in name else "ping-pong"
out = pathlib.Path(args[args.index("--out") + 1])
repetitions = int(args[args.index("--repetitions") + 1])
warmup = int(args[args.index("--warmup") + 1])
log = pathlib.Path(os.environ["QVO_TEST_LAUNCHES"])
first = not log.exists()
fault = os.environ["QVO_TEST_FIRST_FAULT"] if first else "valid"
with log.open("a", encoding="utf-8") as f:
    f.write(json.dumps({"framework": framework, "shape": shape, "out": str(out),
                        "params": params, "fault": fault}) + "\\n")
if fault in ("exit", "missing"):
    sys.exit(42 if fault == "exit" else 0)
if fault == "malformed":
    out.write_text("{", encoding="utf-8")
    sys.exit(0)
samples = [111000000.0] * repetitions
doc = {"schema": "qvo/result/1", "benchmark": "savina/" + shape,
       "framework": framework, "framework_version": "fixture", "verified": True,
       "params": params, "repetitions": repetitions, "warmup": warmup,
       "work_unit": "round trip" if shape == "ping-pong" else "message",
       "work_units": params["messages"],
       "expected_messages": params["messages"] * (2 if shape == "ping-pong" else 1),
       "work_ns": samples, "summary": {"work_min": min(samples),
       "work_p50": statistics.median(samples), "work_p99": max(samples), "work_iqr": 0},
       "failures": [], "caveats": [], "pinned": True, "cpus": "0,2"}
if fault in ("benchmark", "framework"):
    doc[fault] = "wrong"
elif fault in ("cores", "wait", "messages"):
    doc["params"][fault] += 1
elif fault == "verified":
    doc["verified"] = False
    doc["failures"] = ["fixture did not verify"]
out.write_text(json.dumps(doc), encoding="utf-8")
'''

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="qvo sweep controls ")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.repo = Path(__file__).resolve().parent.parent
        self.invocations = 0

    def invoke(self, framework: str, fault: str = "valid", *, retained: bool = False,
               relative_binary: bool = False):
        self.invocations += 1
        cwd = self.root / str(self.invocations)
        cwd.mkdir()
        shim = cwd / "interpreters"
        shim.mkdir()
        (shim / "python3").symlink_to(sys.executable)
        if framework == "sobjectizer":
            bindir, out = cwd / "fake binaries", cwd / "sweep results"
            script = self.repo / "tools/so-sweep.sh"
            argv = [str(bindir), str(out)]
            names = ["qvo-sobjectizer-savina-ping-pong", "qvo-sobjectizer-savina-counting"]
            first_name = "ping-pong-profile-10s.json"
        else:
            # Preserve CAF's historical default cwd interface, without rewriting the script.
            bindir, out = cwd / "build/final/bin", cwd / "build/caf-sweep"
            script = self.repo / "results/desktop-win11-msvc19/caf-spin-sweep/caf-sweep.sh"
            argv, names, first_name = [], ["qvo-caf-savina-ping-pong.exe"], "poll100-steal10.json"
        if relative_binary:
            bindir = cwd
            argv = ["." if framework == "sobjectizer" else "./" + names[0], str(out)]
        else:
            bindir.mkdir(parents=True)
        for name in names:
            exe = bindir / name
            exe.write_text(self.PRODUCER, encoding="utf-8")
            exe.chmod(0o755)
        dest = out / first_name
        old = None
        if retained:
            out.mkdir(parents=True)
            previous = RunnerProvenanceTest.doc(verified=True)
            repetitions = 7 if framework == "sobjectizer" else 5
            previous.update(framework=framework, repetitions=repetitions, warmup=2,
                            params={"messages": 1000000, "cores": 2, "wait": 1},
                            work_ns=[777000000.0] * repetitions,
                            summary={"work_min": 777000000.0, "work_p50": 777000000.0,
                                     "work_p99": 777000000.0, "work_iqr": 0},
                            work_unit="round trip", work_units=1000000,
                            expected_messages=2000000, pinned=True, cpus="0,2",
                            retained_marker="old-only")
            old = (json.dumps(previous) + "\n").encode()
            dest.write_bytes(old)
        launches = cwd / "launches.jsonl"
        # Never let a basename find the fixture through PATH: './exe' must mean this cwd.
        env = dict(os.environ, PATH=str(shim) + os.pathsep + os.defpath,
                   QVO_TEST_FIRST_FAULT=fault, QVO_TEST_LAUNCHES=str(launches),
                   PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([shutil.which("bash"), str(script), *argv], cwd=cwd, env=env,
                                capture_output=True, text=True, check=False, timeout=60)
        calls = [json.loads(line) for line in launches.read_text().splitlines()] if launches.exists() else []
        return result, dest, old, calls, out

    def test_failed_relaunch_cannot_relabel_retained_json(self) -> None:
        for framework in ("sobjectizer", "caf"):
            with self.subTest(framework=framework):
                result, dest, old, _calls, _out = self.invoke(framework, "exit", retained=True)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(dest.read_bytes(), old)
                self.assertNotIn("777.0", result.stdout)

    def test_fresh_failure_cannot_be_hidden_by_later_success(self) -> None:
        for framework in ("sobjectizer", "caf"):
            with self.subTest(framework=framework):
                result, _dest, _old, calls, _out = self.invoke(framework, "exit")
                self.assertTrue(calls, result.stdout + result.stderr)
                self.assertEqual(calls[0]["fault"], "exit")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_malformed_or_unverified_result_is_rejected(self) -> None:
        for framework in ("sobjectizer", "caf"):
            for fault in ("missing", "malformed", "verified"):
                with self.subTest(framework=framework, fault=fault):
                    result, _dest, _old, calls, _out = self.invoke(framework, fault)
                    self.assertTrue(calls, result.stdout + result.stderr)
                    self.assertEqual(calls[0]["fault"], fault)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_wrong_identity_or_parameters_are_rejected(self) -> None:
        for framework in ("sobjectizer", "caf"):
            for fault in ("benchmark", "framework", "cores", "wait", "messages"):
                with self.subTest(framework=framework, fault=fault):
                    result, _dest, _old, calls, _out = self.invoke(framework, fault)
                    self.assertTrue(calls, result.stdout + result.stderr)
                    self.assertEqual(calls[0]["fault"], fault)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_successful_sweeps_keep_every_requested_cell(self) -> None:
        for framework, expected in (("sobjectizer", 20), ("caf", 10)):
            with self.subTest(framework=framework):
                result, _dest, _old, calls, out = self.invoke(framework)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(calls), expected)
                docs = [json.loads(path.read_text()) for path in out.glob("*.json")]
                measured = [doc for doc in docs if doc.get("schema") == "qvo/result/1"]
                self.assertEqual(len(measured), expected)
                self.assertTrue(all(doc["verified"] and doc["framework"] == framework for doc in measured))
                self.assertTrue(all(call["params"] == {"messages": 1000000, "cores": 2, "wait": 1}
                                    for call in calls))

    def test_relative_binary_arguments_execute_the_selected_local_producer(self) -> None:
        for framework, expected in (("sobjectizer", 20), ("caf", 10)):
            with self.subTest(framework=framework):
                result, _dest, _old, calls, out = self.invoke(framework, relative_binary=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(calls), expected)
                manifest = json.loads((out / "run.json").read_text())
                self.assertEqual(manifest["status"], "complete")
                self.assertTrue(all(Path(cell["command"][0]).is_absolute()
                                    for cell in manifest["cells"]))

    def test_ping_pong_and_counting_use_their_declared_work_units(self) -> None:
        result, _dest, _old, _calls, _out = self.invoke("sobjectizer")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for shape, unit in (("ping-pong", "round trip"), ("counting", "message")):
            rows = [line for line in result.stdout.splitlines() if line.startswith(shape + "-")]
            self.assertEqual(len(rows), 10, result.stdout)
            for line in rows:
                self.assertRegex(line, r"p50=\s*111\.0\b")
                self.assertIn("ns/" + unit, line)


class SweepHelperTest(unittest.TestCase):
    """Portable collection controls: real validation and files, only the child process replaced."""

    @classmethod
    def setUpClass(cls) -> None:
        spec = importlib.util.spec_from_file_location(
            "spin_sweep_control", Path(__file__).with_name("spin-sweep.py"))
        assert spec is not None and spec.loader is not None
        cls.sweep = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.sweep)

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="qvo-sweep-helper-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.invocations = 0

    @staticmethod
    def document(mode: str, index: int) -> dict:
        doc = RunnerProvenanceTest.doc(verified=True)
        shape = "counting" if mode == "so" and index >= 10 else "ping-pong"
        repetitions = 7 if mode == "so" else 5
        doc.update(framework="sobjectizer" if mode == "so" else "caf",
                   benchmark="savina/" + shape,
                   params={"messages": 1000000, "cores": 2, "wait": 1},
                   repetitions=repetitions, warmup=2, pinned=True, cpus="0,2",
                   work_unit="message" if shape == "counting" else "round trip",
                   work_units=1000000, expected_messages=1000000 if shape == "counting" else 2000000,
                   work_ns=[111000000.0] * repetitions,
                   total_ns=[112000000.0] * repetitions,
                   outside_window_ns=[1000000.0] * repetitions,
                   summary={"work_min": 111000000.0, "work_p50": 111000000.0,
                            "work_p99": 111000000.0, "work_iqr": 0}, failures=[], caveats=[])
        return doc

    def invoke(self, mode="caf", behavior=None, *, out=None):
        self.invocations += 1
        out = out or self.root / f"out-{self.invocations}"
        binary = self.root / "binaries" if mode == "so" else self.root / "qvo-caf-savina-ping-pong.exe"
        calls = []
        # Explicit inherited values detect accidental environment clearing as well as a wrong
        # override. Profile cells historically inherit QVO_SO_SPIN_WAIT_US; budget cells set it.
        inherited = {"QVO_TEST_KEEP": "untouched", "QVO_SO_SPIN_WAIT_US": "77",
                     "QVO_CAF_AGGRESSIVE_POLL": "88", "QVO_CAF_STEAL_INTERVAL": "99"}

        def child(cmd, *, stdout, stderr, env, timeout):
            index = len(calls)
            attempt = Path(cmd[cmd.index("--out") + 1])
            self.assertFalse(attempt.exists(), "each launch must receive a fresh result path")
            call = {"command": list(cmd), "environment": dict(env), "timeout": timeout,
                    "attempt": attempt}
            calls.append(call)
            stdout.write(b"fixture stdout\n")
            stderr.write(b"fixture stderr\n")
            doc = self.document(mode, index)
            try:
                if behavior is not None:
                    return behavior(attempt, doc, index, cmd)
                attempt.write_text(json.dumps(doc), encoding="utf-8")
                return subprocess.CompletedProcess(cmd, 0)
            finally:
                call["raw_bytes"] = attempt.read_bytes() if attempt.exists() else None

        captured, errors = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, inherited, clear=True), \
                patch.object(self.sweep.subprocess, "run", side_effect=child), \
                contextlib.redirect_stdout(captured), contextlib.redirect_stderr(errors):
            rc = self.sweep.main([mode, str(binary), str(out), "--timeout", "0.25"])
        return rc, out, captured.getvalue(), errors.getvalue(), calls, inherited

    def assert_failed_first_attempt(self, result) -> None:
        rc, out, _stdout, stderr, calls, _inherited = result
        self.assertEqual(rc, 1, stderr)
        manifest = json.loads((out / "run.json").read_text())
        self.assertEqual(manifest["status"], "failed")
        first = manifest["cells"][0]
        self.assertEqual(first["status"], "failed")
        canonical = json.loads((out / f"{first['label']}.json").read_text())
        self.assertIs(canonical["verified"], False)
        self.assertEqual(canonical["work_ns"], [])
        self.assertEqual(canonical["summary"], {})
        raw = out / first["attempt"]
        self.assertEqual((raw / "stderr.txt").read_bytes(), b"fixture stderr\n")
        self.assertEqual((raw / "stdout.txt").read_bytes(), b"fixture stdout\n")
        if calls[0]["raw_bytes"] is None:
            self.assertFalse((raw / "result.json").exists())
        else:
            self.assertEqual((raw / "result.json").read_bytes(), calls[0]["raw_bytes"])
        self.assertFalse((out / ".run.lock").exists())

    def test_so_arguments_order_and_environment_match_the_established_matrix(self) -> None:
        # Independent list of the historical experiment, not imported from cells/SO_BUDGETS.
        expected = [
            ("ping-pong-profile-10s", "ping-pong", "77"),
            ("ping-pong-simple-lock", "ping-pong", "0"),
            ("ping-pong-wait1us", "ping-pong", "1"),
            ("ping-pong-wait10us", "ping-pong", "10"),
            ("ping-pong-wait100us", "ping-pong", "100"),
            ("ping-pong-wait1ms-default", "ping-pong", "1000"),
            ("ping-pong-wait10ms", "ping-pong", "10000"),
            ("ping-pong-wait100ms", "ping-pong", "100000"),
            ("ping-pong-wait10s-b", "ping-pong", "10000000"),
            ("ping-pong-profile-10s-b", "ping-pong", "77"),
            ("counting-profile-10s", "counting", "77"),
            ("counting-simple-lock", "counting", "0"),
            ("counting-wait1us", "counting", "1"),
            ("counting-wait10us", "counting", "10"),
            ("counting-wait100us", "counting", "100"),
            ("counting-wait1ms-default", "counting", "1000"),
            ("counting-wait10ms", "counting", "10000"),
            ("counting-wait100ms", "counting", "100000"),
            ("counting-wait10s-b", "counting", "10000000"),
            ("counting-profile-10s-b", "counting", "77"),
        ]
        rc, out, _stdout, stderr, calls, inherited = self.invoke("so")
        self.assertEqual(rc, 0, stderr)
        manifest = json.loads((out / "run.json").read_text())
        self.assertEqual(manifest["status"], "complete")
        self.assertEqual([cell["label"] for cell in manifest["cells"]], [e[0] for e in expected])
        self.assertEqual(len(calls), len(expected))
        for (label, shape, budget), call in zip(expected, calls):
            with self.subTest(label=label):
                self.assertEqual(call["command"], [
                    str((self.root / "binaries" / f"qvo-sobjectizer-savina-{shape}").resolve()),
                    "--repetitions", "7", "--warmup", "2", "--cpus", "0,2",
                    "--param", "messages=1000000", "--param", "cores=2", "--param", "wait=1",
                    "--out", str(call["attempt"])])
                self.assertEqual(call["environment"], dict(inherited, QVO_SO_SPIN_WAIT_US=budget))
                self.assertEqual(call["timeout"], 0.25)
                self.assertEqual(call["attempt"].read_bytes(), call["raw_bytes"])
                self.assertTrue(json.loads((out / f"{label}.json").read_text())["verified"])

    def test_caf_arguments_order_and_environment_match_the_established_matrix(self) -> None:
        expected = [
            ("poll100-steal10", "100", "10"), ("poll1e3-steal10", "1000", "10"),
            ("poll1e4-steal10", "10000", "10"), ("poll1e5-steal10", "100000", "10"),
            ("poll1e6-steal10", "1000000", "10"), ("poll1e4-steal1", "10000", "1"),
            ("poll1e4-steal100", "10000", "100"), ("poll1e4-steal1e3", "10000", "1000"),
            ("poll1e6-steal1e6", "1000000", "1000000"), ("poll100-steal10-b", "100", "10"),
        ]
        rc, out, _stdout, stderr, calls, inherited = self.invoke()
        self.assertEqual(rc, 0, stderr)
        manifest = json.loads((out / "run.json").read_text())
        self.assertEqual([cell["label"] for cell in manifest["cells"]], [e[0] for e in expected])
        self.assertEqual(len(calls), len(expected))
        for (label, poll, steal), call in zip(expected, calls):
            with self.subTest(label=label):
                self.assertEqual(call["command"], [
                    str((self.root / "qvo-caf-savina-ping-pong.exe").resolve()),
                    "--repetitions", "5", "--warmup", "2", "--cpus", "0,2",
                    "--param", "messages=1000000", "--param", "cores=2", "--param", "wait=1",
                    "--out", str(call["attempt"])])
                self.assertEqual(call["environment"], dict(
                    inherited, QVO_CAF_AGGRESSIVE_POLL=poll, QVO_CAF_STEAL_INTERVAL=steal))
                self.assertEqual(call["timeout"], 0.25)

    def test_pin_cpu_and_work_unit_mismatches_never_promote_a_timing(self) -> None:
        for field, value in (("pinned", False), ("pinned", 1), ("cpus", "2,0"),
                             ("work_unit", "message"), ("work_units", 2000000)):
            with self.subTest(field=field, value=value):
                def behavior(attempt, doc, index, cmd):
                    if index == 0:
                        doc[field] = value
                    attempt.write_text(json.dumps(doc), encoding="utf-8")
                    return subprocess.CompletedProcess(cmd, 0)
                self.assert_failed_first_attempt(self.invoke(behavior=behavior))

    def test_boolean_negative_and_nonfinite_numbers_are_not_timings_or_units(self) -> None:
        for field in ("work_ns", "total_ns", "outside_window_ns", "work_p50", "work_units"):
            for value in (True, -1, float("nan"), float("inf")):
                with self.subTest(field=field, value=value):
                    def behavior(attempt, doc, index, cmd):
                        if index == 0:
                            if field in ("work_ns", "total_ns", "outside_window_ns"):
                                doc[field][0] = value
                            elif field == "work_p50":
                                doc["summary"][field] = value
                            else:
                                doc[field] = value
                        attempt.write_text(json.dumps(doc), encoding="utf-8")
                        return subprocess.CompletedProcess(cmd, 0)
                    self.assert_failed_first_attempt(self.invoke(behavior=behavior))

    def test_legacy_caf_expected_messages_accepts_absent_optional_metadata(self) -> None:
        def legacy(attempt, doc, index, cmd):
            for field in ("work_unit", "work_units", "total_ns", "outside_window_ns", "caveats"):
                doc.pop(field)
            if index % 2:
                doc.pop("failures")
            else:
                doc["failures"] = None
            # Older files have the checksum/message contract, before explicit work-unit fields.
            doc["summary"] = {"work_p50": 111000000.0}
            attempt.write_text(json.dumps(doc), encoding="utf-8")
            return subprocess.CompletedProcess(cmd, 0)

        rc, out, stdout, stderr, calls, _inherited = self.invoke(behavior=legacy)
        self.assertEqual(rc, 0, stderr)
        self.assertEqual(len(calls), 10)
        self.assertEqual(stdout.count("ns/round trip"), 10)
        self.assertIn("111.0", stdout)
        doc = json.loads((out / "poll100-steal10.json").read_text())
        self.assertEqual(doc["expected_messages"], 2000000)
        self.assertNotIn("work_units", doc)
        self.assertNotIn("work_min", doc["summary"])

    def test_legacy_caf_requires_the_expected_round_trip_denominator(self) -> None:
        for messages in (True, 0, 1000000, 2000001):
            with self.subTest(expected_messages=messages):
                def legacy(attempt, doc, index, cmd):
                    if index == 0:
                        doc.pop("work_unit")
                        doc.pop("work_units")
                        doc["expected_messages"] = messages
                    attempt.write_text(json.dumps(doc), encoding="utf-8")
                    return subprocess.CompletedProcess(cmd, 0)
                self.assert_failed_first_attempt(self.invoke(behavior=legacy))

    def test_timeout_and_launch_error_keep_raw_output_and_diagnostics(self) -> None:
        for fault in ("timeout", "launch"):
            with self.subTest(fault=fault):
                def behavior(attempt, doc, index, cmd):
                    if index == 0:
                        if fault == "timeout":
                            attempt.write_bytes(b'{"unfinished":')
                            raise subprocess.TimeoutExpired(cmd, 0.25)
                        raise OSError("fixture launch failed")
                    attempt.write_text(json.dumps(doc), encoding="utf-8")
                    return subprocess.CompletedProcess(cmd, 0)
                result = self.invoke(behavior=behavior)
                self.assert_failed_first_attempt(result)
                self.assertEqual(len(result[4]), 10)
                self.assertIn("timed out" if fault == "timeout" else "fixture launch failed", result[3])

    def test_interrupt_records_incomplete_campaign_and_preserves_the_last_attempt(self) -> None:
        def interrupted(attempt, _doc, _index, _cmd):
            attempt.write_bytes(b'{"interrupted":')
            raise KeyboardInterrupt("fixture interrupted")

        rc, out, _stdout, stderr, calls, _inherited = self.invoke(behavior=interrupted)
        self.assertEqual(rc, 130, stderr)
        self.assertEqual(len(calls), 1)
        manifest = json.loads((out / "run.json").read_text())
        self.assertEqual(manifest["status"], "interrupted")
        self.assertEqual(manifest["cells"][0]["status"], "interrupted")
        raw = out / manifest["cells"][0]["attempt"]
        self.assertEqual((raw / "result.json").read_bytes(), b'{"interrupted":')
        self.assertEqual((raw / "stderr.txt").read_bytes(), b"fixture stderr\n")
        self.assertEqual((raw / "stdout.txt").read_bytes(), b"fixture stdout\n")
        self.assertIs(json.loads((out / "poll100-steal10.json").read_text())["verified"], False)
        self.assertFalse((out / ".run.lock").exists())

    def test_populated_output_and_another_owners_lock_refuse_every_launch(self) -> None:
        for existing in ("old.json", ".run.lock/owner.txt"):
            with self.subTest(existing=existing):
                out = self.root / f"retained-{self.invocations}"
                retained = out / existing
                retained.parent.mkdir(parents=True)
                retained.write_bytes(b"prior campaign bytes\n")
                before = sorted(str(p.relative_to(out)) for p in out.rglob("*"))
                rc, _out, _stdout, stderr, calls, _inherited = self.invoke(out=out)
                self.assertEqual(rc, 2, stderr)
                self.assertEqual(calls, [])
                self.assertEqual(retained.read_bytes(), b"prior campaign bytes\n")
                self.assertEqual(sorted(str(p.relative_to(out)) for p in out.rglob("*")), before)

    def test_files_appearing_while_admitting_a_campaign_are_not_overwritten(self) -> None:
        out = self.root / "admission-race"
        mkdir = Path.mkdir
        planted = []

        def raced(path, *args, **kwargs):
            result = mkdir(path, *args, **kwargs)
            if path.resolve() == (out / ".run.lock").resolve():
                (out / "prior.json").write_bytes(b"another campaign finished\n")
                planted.append(path)
            return result

        with patch.object(Path, "mkdir", new=raced):
            rc, _out, _stdout, stderr, calls, _inherited = self.invoke(out=out)
        self.assertEqual(len(planted), 1, "the admission-race fixture must actually land")
        self.assertEqual(rc, 2, stderr)
        self.assertEqual(calls, [])
        self.assertEqual((out / "prior.json").read_bytes(), b"another campaign finished\n")
        self.assertFalse((out / ".run.lock").exists())
        self.assertFalse((out / "run.json").exists())


class ReportDistributionTest(unittest.TestCase):
    """QB-883/884: semantic oracles independent of a renderer-generated golden file."""

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="qvo-report-distribution-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def write(self, doc: dict) -> None:
        dest = self.root / doc["benchmark"].replace("/", "-") / f"{doc['framework']}.json"
        dest.parent.mkdir(exist_ok=True)
        dest.write_text(json.dumps(doc), encoding="utf-8")

    @staticmethod
    def measured(framework: str, samples: list[float], summary: dict) -> dict:
        doc = RunnerProvenanceTest.doc(verified=True)
        doc.update(framework=framework, repetitions=len(samples), work_ns=samples,
                   summary=summary, work_unit="round trip", work_units=10)
        return doc

    def uniform_field(self) -> None:
        # These are durations in ns, ten work units each. All three statistics differ, and
        # values after normalization differ too: rendering the median as min cannot pass.
        for framework, offset in (("qb", 0), ("caf", 200), ("baseline", 100)):
            self.write(self.measured(framework, [100 + offset, 110 + offset, 120 + offset,
                                                130 + offset, 140 + offset],
                                     {"work_min": 100 + offset, "work_p50": 120 + offset,
                                      "work_p99": 139.6 + offset, "work_iqr": 20}))

    def saved_fib(self) -> None:
        source = Path(__file__).resolve().parent.parent / "results/desktop-win11-msvc19/savina-fib"
        dest = self.root / "savina-fib"
        dest.mkdir()
        for framework in ("baseline", "qb", "caf"):
            path = source / f"{framework}__2c-spin.json"
            # Copy retained bytes into the sandbox; no rewrite of a historical input.
            (dest / path.name).write_bytes(path.read_bytes())

    def table(self, rendered: str, config: str = "1c-spin") -> list[list[str]]:
        section = rendered.split(f"### {config}\n", 1)[1]
        rows = []
        for line in section.lstrip().splitlines():
            if not line.startswith("|"):
                break
            rows.append([cell.strip() for cell in line.strip("|").split("|")])
        self.assertTrue(rows)
        return rows

    def minimum_column(self, header: list[str]) -> int:
        columns = [i for i, name in enumerate(header) if name == "minimum"]
        self.assertEqual(len(columns), 1, header)
        return columns[0]

    def assert_no_floor_ranking(self, rendered: str) -> None:
        # Ignore JSON-authored caveats and table values; inspect conclusions from the renderer.
        conclusions = "\n".join(line for line in rendered.split("<details>", 1)[0].splitlines()
                                if not line.startswith("|"))
        self.assertNotIn("below the floor", conclusions)
        self.assertNotRegex(conclusions, r"\d+(?:\.\d+)?x(?:\*\*)? the floor")
        self.assertNotRegex(conclusions, r"floor[^\n]*\d+(?:\.\d+)?x")

    def test_saved_bimodal_floor_cannot_produce_a_unique_ratio(self) -> None:
        self.saved_fib()
        rendered = report.render(self.root)
        self.assertIn("**bimodal**", rendered)
        self.assertIn("`baseline`", rendered)
        self.assert_no_floor_ranking(rendered)

    def test_each_bimodal_side_suppresses_floor_ordering(self) -> None:
        for bimodal in ("baseline", "qb"):
            with self.subTest(bimodal=bimodal):
                self.uniform_field()
                self.write(self.measured(bimodal, [100, 110, 120, 900, 950],
                                         {"work_min": 100, "work_p50": 120,
                                          "work_p99": 948, "work_iqr": 790}))
                rendered = report.render(self.root)
                self.assertIn("**bimodal**", rendered)
                self.assert_no_floor_ranking(rendered)
                if bimodal == "qb":
                    self.assertNotRegex(rendered, r"`qb` is \*\*\d+(?:\.\d+)?x\*\* faster")

    def test_unimodal_field_retains_framework_and_floor_comparisons(self) -> None:
        self.uniform_field()
        rendered = report.render(self.root)
        self.assertNotIn("**bimodal**", rendered)
        self.assertIn("`qb` is **2.67x** faster than `caf`", rendered)
        self.assertIn("below the floor", rendered)
        self.assertIn("0.55x", rendered)
        self.write(self.measured("baseline", [50, 55, 60, 65, 70],
                                 {"work_min": 50, "work_p50": 60, "work_p99": 69.8, "work_iqr": 10}))
        self.assertIn("**2.00x the floor**", report.render(self.root))

    def test_bimodal_second_framework_does_not_make_best_the_fastest(self) -> None:
        self.uniform_field()
        self.write(self.measured("caf", [90, 100, 300, 310, 320],
                                 {"work_min": 90, "work_p50": 300, "work_p99": 319.6,
                                  "work_iqr": 210}))
        rendered = report.render(self.root)
        self.assertIn("**No ordering is claimed between `qb` and `caf`**", rendered)
        self.assertIn("below the floor", rendered)
        self.assertIn("0.55x", rendered)
        self.assertNotIn("fastest framework", rendered.lower())

    def test_absent_failed_or_unavailable_floor_does_not_produce_a_comparison(self) -> None:
        for state in ("absent", "failed", "unavailable"):
            with self.subTest(state=state):
                self.uniform_field()
                floor = self.root / "savina-ping-pong/baseline.json"
                if state == "absent":
                    floor.unlink()
                else:
                    doc = RunnerProvenanceTest.doc(
                        verified=False, reason="fixture floor unavailable" if state == "unavailable" else None)
                    doc["framework"] = "baseline"
                    self.write(doc)
                rendered = report.render(self.root)
                self.assert_no_floor_ranking(rendered)
                self.assertIn("`qb` is **2.67x** faster than `caf`", rendered)

    def test_minimum_uses_total_duration_and_not_median_or_normalized_value(self) -> None:
        self.uniform_field()
        rows = self.table(report.render(self.root))
        header, minimum = rows[0], self.minimum_column(rows[0])
        qb = next(row for row in rows[2:] if row[0].startswith("`qb`"))
        self.assertEqual(qb[minimum], "100 ns")
        self.assertEqual(qb[header.index("median")], "120 ns")
        self.assertEqual(qb[header.index("p99")], "140 ns")
        self.assertEqual(qb[header.index("per round trip")], "12 ns")

    def test_saved_minimum_is_read_from_its_own_summary(self) -> None:
        self.saved_fib()
        rows = self.table(report.render(self.root), "2c-spin")
        header, minimum = rows[0], self.minimum_column(rows[0])
        floor = next(row for row in rows[2:] if row[0].startswith("`baseline`"))
        # Independent expectations from the retained JSON: 2,526,500 / 25,557,600 / 53,856,084 ns.
        self.assertEqual(floor[minimum], "2.53 ms")
        self.assertEqual(floor[header.index("median")], "25.56 ms")
        self.assertEqual(floor[header.index("p99")], "53.86 ms")

    def test_missing_minimum_is_unavailable_not_zero_or_reconstructed(self) -> None:
        self.uniform_field()
        doc = self.measured("qb", [100, 110, 120, 130, 140],
                            {"work_p50": 120, "work_p99": 139.6, "work_iqr": 20})
        self.write(doc)
        rows = self.table(report.render(self.root))
        minimum = self.minimum_column(rows[0])
        qb = next(row for row in rows[2:] if row[0].startswith("`qb`"))
        self.assertEqual(qb[minimum], "—")
        self.assertEqual(qb[rows[0].index("median")], "120 ns")

    def test_minimum_column_aligns_failed_na_bimodal_and_observation_rows(self) -> None:
        self.uniform_field()
        bimodal = self.measured("qb", [100, 110, 120, 900, 950],
                                {"work_min": 100, "work_p50": 120, "work_p99": 948, "work_iqr": 790})
        bimodal["observed"] = {"visits": {"samples": [2, 3, 4], "min": 2, "p50": 3, "max": 4}}
        self.write(bimodal)
        failed = RunnerProvenanceTest.doc(verified=False)
        failed["framework"] = "caf"
        self.write(failed)
        unavailable = RunnerProvenanceTest.doc(verified=False, reason="fixture has no spin profile")
        unavailable["framework"] = "sobjectizer"
        self.write(unavailable)
        rendered = report.render(self.root)
        rows = self.table(rendered)
        minimum = self.minimum_column(rows[0])
        self.assertEqual(len(rows[0]), 7)
        for row in rows[1:]:
            self.assertEqual(len(row), len(rows[0]), row)
        for framework, verdict in (("caf", "**FAILED**"), ("sobjectizer", "n/a")):
            row = next(row for row in rows[2:] if row[0].startswith(f"`{framework}`"))
            self.assertEqual(row[1], verdict)
            self.assertEqual(row[minimum], "—")
        for text in ("current attempt failed", "fixture has no spin profile", "**bimodal**",
                     "**observed, not asserted**"):
            self.assertIn(text, rendered)


class LaunchCensusPlanTest(unittest.TestCase):
    def test_seeded_family_executes_the_recorded_plan(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            bins = [(label, root / label) for label in ("a0", "a1", "b0")]
            for _, path in bins:
                path.touch()
            prior_plan = None
            for attempt in (0, 1):
                out = root / f"out-{attempt}"
                observed = []

                def child(cmd, **_kwargs):
                    if "--describe" in cmd:
                        return subprocess.CompletedProcess(cmd, 0, json.dumps({"benchmark": "control/ping-pong"}), "")
                    observed.append(Path(cmd[0]).name)
                    dest = Path(cmd[cmd.index("--out") + 1])
                    dest.write_text(json.dumps({"verified": True, "work_units": 1,
                                               "summary": {"work_p50": 42.0}}))
                    return subprocess.CompletedProcess(cmd, 0, "", "")

                argv = ["launch-census.py", "--out", str(out), "--config", "1c-spin,2c-park",
                        "--launches", "3", "--shuffle-seed", "990", "--no-pin"]
                for label, path in bins:
                    argv += ["--bin", f"{label}={path}"]
                with patch.object(sys, "argv", argv), patch.object(census.subprocess, "run", side_effect=child), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(census.main(), 0)
                plan = [json.loads(line) for line in (out / "launch-order.jsonl").read_text().splitlines()]
                self.assertEqual(observed, [label for row in plan for label in row["order"]])
                self.assertEqual(len(observed), 18)
                self.assertTrue(all(set(row["order"]) == {"a0", "a1", "b0"} for row in plan))
                self.assertGreater(len({tuple(row["order"]) for row in plan}), 1)
                if prior_plan is not None:
                    self.assertEqual(plan, prior_plan)
                prior_plan = plan

    def test_empty_census_is_rejected_before_any_child(self) -> None:
        with patch.object(sys, "argv", ["launch-census.py", "--out", "unused", "--bin", "a=unused",
                                        "--launches", "0", "--no-pin"]), \
                patch.object(census.subprocess, "run") as child:
            with self.assertRaisesRegex(SystemExit, "must be positive"):
                census.main()
            child.assert_not_called()


if __name__ == "__main__":
    unittest.main()
