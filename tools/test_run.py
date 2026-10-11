#!/usr/bin/env python3
"""Regression controls for the runner's result-file provenance.

The synthetic .exe is never executed: subprocess is replaced at the process boundary, so these
controls exercise the same runner on POSIX and Windows without building a benchmark or measuring
on a busy host.
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
                self.assertIn("| `qb` | yes |", rendered)
                self.assertIn("| `caf` | **FAILED** |", rendered)
                self.assertIn("current child aborted", rendered)

        # No surviving measurement means no unit can be inferred, but every failure stays visible.
        self.seed(run.failed_document("savina/ping-pong", "qb",
                                      {"cores": 1, "wait": 1}, "second child aborted"))
        rendered = report.render(self.out)
        self.assertIn("per repetition", rendered)
        self.assertEqual(rendered.count("**FAILED**"), 2)


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
