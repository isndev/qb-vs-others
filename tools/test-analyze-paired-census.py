#!/usr/bin/env python3
"""Synthetic positive and negative controls; starts no benchmark process."""

import hashlib
import json
import runpy
import tempfile
import unittest
from pathlib import Path

analyze = runpy.run_path(str(Path(__file__).with_name("analyze-paired-census.py")))["analyze"]


class PairedCensusControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.results = self.root / "results"
        self.results.mkdir()
        self.quiet = self.root / "quiet.json"
        self.binaries = {}
        for side in ("control", "candidate"):
            path = self.root / side / "qb-vs-others/build/qb922/bin/qvo-qb-savina-counting"
            path.parent.mkdir(parents=True)
            path.write_bytes(side.encode())
            self.binaries[side] = path
        self.manifest = self.root / "binaries.sha256"
        self.manifest.write_text("".join(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path}\n"
            for path in self.binaries.values()))
        self.save_quiet({"verdict": "sampled_quiet", "command_exit_code": 0,
                         "contamination": [], "samples": 12, "selected_cpus": [8, 9],
                         "runner_restoration": {"verdict": "restored"},
                         "command": ["python3", "tools/launch-census.py", "--out", str(self.results),
                                     "--config", "2c-park", "--launches", "12", "--alternate-order",
                                     "--repetitions", "3", "--warmup", "1", "--cpus", "8,9",
                                     "--bin", f"control={self.binaries['control']}",
                                     "--bin", f"candidate={self.binaries['candidate']}"]})
        rows = []
        for launch in range(1, 13):
            order = ["control", "candidate"] if launch % 2 else ["candidate", "control"]
            rows.append({"config": "2c-park", "launch": launch, "order": order})
            for side in order:
                ns = [100.0, 110.0, 120.0] if side == "control" else [90.0, 99.0, 108.0]
                self.save_result(side, launch, {"schema": "qvo/result/1", "benchmark": "savina/counting",
                    "verified": True, "failures": [], "repetitions": 3, "warmup": 1, "work_ns": ns,
                    "work_units": 1, "work_unit": "message", "summary": {"work_p50": ns[1]},
                    "expected_checksum": 42, "expected_messages": 100,
                    "params": {"cores": 2, "wait": 0, "messages": 100},
                    "pinned": True, "cpus": "8,9", "env": {"compiler": "GNU",
                    "compiler_version": "14.2.0", "build_type": "Release", "cxx_flags": "-O3 -DNDEBUG"}})
        self.order = self.results / "launch-order.jsonl"
        self.order.write_text("".join(json.dumps(row) + "\n" for row in rows))

    def save_quiet(self, doc):
        self.quiet.write_text(json.dumps(doc))

    def save_result(self, side, launch, doc):
        (self.results / f"{side}__savina-counting-2c-park-launch{launch}.json").write_text(json.dumps(doc))

    def change_result(self, side, launch, key, value):
        path = self.results / f"{side}__savina-counting-2c-park-launch{launch}.json"
        doc = json.loads(path.read_text())
        doc[key] = value
        path.write_text(json.dumps(doc))

    def check(self, with_manifest=False):
        return analyze(self.results, self.quiet, "2c-park", 12, "8,9",
                       self.manifest if with_manifest else None, with_manifest)

    def test_verified_balanced_pair_and_bootstrap(self):
        report = self.check(with_manifest=True)
        self.assertAlmostEqual(report["paired_delta_percent_p50"], -10.0)
        self.assertEqual(report["paired_delta_percent_iqr"], 0)
        for bound in report["paired_delta_percent_bootstrap_95"]:
            self.assertAlmostEqual(bound, -10.0)
        self.assertTrue(report["screening_only"])
        self.assertTrue(report["binary_hashes_checked"])

    def test_rejects_swapped_or_same_checkout_labels(self):
        doc = json.loads(self.quiet.read_text())
        command = doc["command"]
        slots = [i + 1 for i, arg in enumerate(command[:-1]) if arg == "--bin"]
        command[slots[0]], command[slots[1]] = command[slots[1]].replace("candidate=", "control="), command[slots[0]].replace("control=", "candidate=")
        self.save_quiet(doc)
        with self.assertRaisesRegex(ValueError, "maps control to another checkout"):
            self.check()
        command[slots[0]] = f"control={self.binaries['control']}"
        command[slots[1]] = f"candidate={self.binaries['control']}"
        self.save_quiet(doc)
        with self.assertRaisesRegex(ValueError, "maps candidate to another checkout"):
            self.check()

    def test_rejects_missing_or_changed_binary_manifest(self):
        self.manifest.write_text(f"{hashlib.sha256(self.binaries['control'].read_bytes()).hexdigest()}  {self.binaries['control']}\n")
        with self.assertRaisesRegex(ValueError, "omits candidate"):
            self.check(with_manifest=True)
        self.manifest.write_text("".join(f"{'0' * 64}  {path}\n" for path in self.binaries.values()))
        with self.assertRaisesRegex(ValueError, "control binary hash differs"):
            self.check(with_manifest=True)

    def test_rejects_unbalanced_order(self):
        rows = [json.loads(line) for line in self.order.read_text().splitlines()]
        rows[1]["order"] = ["control", "candidate"]
        self.order.write_text("".join(json.dumps(row) + "\n" for row in rows))
        with self.assertRaisesRegex(ValueError, "balanced AB/BA"):
            self.check()

    def test_rejects_wrong_checksum_and_unverified_result(self):
        self.change_result("candidate", 1, "expected_checksum", 43)
        with self.assertRaisesRegex(ValueError, "expected_checksum differs"):
            self.check()
        self.change_result("candidate", 1, "verified", False)
        with self.assertRaisesRegex(ValueError, "failed correctness"):
            self.check()

    def test_rejects_contamination_and_failed_restoration(self):
        doc = json.loads(self.quiet.read_text())
        doc["verdict"] = "contaminated"
        self.save_quiet(doc)
        with self.assertRaisesRegex(ValueError, "quiet monitor rejected"):
            self.check()
        doc["verdict"] = "sampled_quiet"
        doc["runner_restoration"]["verdict"] = "restore_failed"
        self.save_quiet(doc)
        with self.assertRaisesRegex(ValueError, "not restored"):
            self.check()

    def test_rejects_partial_or_wrong_sample_count(self):
        (self.results / "candidate__savina-counting-2c-park-launch2.json").unlink()
        with self.assertRaisesRegex(ValueError, "missing or duplicate"):
            self.check()
        self.change_result("control", 1, "work_ns", [100.0, 110.0])
        with self.assertRaisesRegex(ValueError, "wrong sample count"):
            self.check()


if __name__ == "__main__":
    unittest.main()
