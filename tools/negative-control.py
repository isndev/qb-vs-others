#!/usr/bin/env python3
"""Plant a defect at a time and assert the harness REJECTS it.

FAIRNESS.md section 0 says a framework that drops one message in ten million produces no timing at
all. That is a claim about code, and until something has been watched being rejected it is only a
claim. This is the battery that makes it a measurement.

Every control prints one of three verdicts, the same vocabulary the guard batteries of qb's development tree use:

    CAUGHT     a planted defect was rejected -- the guard did its job
    CONFIRMED  a shape that must NOT be reported was accepted -- no false positive
    MISSED     a planted defect was accepted -- the guard is not guarding

A run with any MISSED fails. So does a run whose CAUGHT/CONFIRMED counts fall below the floors
below: a battery that silently stops planting anything looks exactly like a battery that passes.

    python3 tools/negative-control.py --build build/final
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Floors. Raise them in the same commit that adds a control; a count that goes DOWN is a control
# that stopped firing, which is indistinguishable from a guard that stopped guarding.
FLOOR_CAUGHT = 10
FLOOR_CONFIRMED = 6

caught = confirmed = missed = 0


def verdict(kind: str, what: str, detail: str = "") -> None:
    global caught, confirmed, missed
    if kind == "CAUGHT":
        caught += 1
    elif kind == "CONFIRMED":
        confirmed += 1
    else:
        missed += 1
    line = f"  {kind:<10} {what}"
    if detail:
        line += f"\n             {detail}"
    print(line)


def run_subject(exe: Path, plant: str, extra: list[str] | None = None,
                messages: int = 20000000, repetitions: int = 1,
                warmup: int = 0) -> tuple[int, dict | None, str]:
    """Run the control subject with one planted defect. Returns (rc, result_json_or_None, stderr)."""
    env = dict(os.environ)
    env["QVO_CONTROL_PLANT"] = plant
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "r.json"
        cmd = [str(exe), "--repetitions", str(repetitions), "--warmup", str(warmup),
               "--out", str(out),
               "--param", f"messages={messages}", "--param", "cores=1", "--param", "wait=1"]
        cmd += extra or []
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=900)
        doc = None
        if out.exists():
            try:
                doc = json.loads(out.read_text())
            except json.JSONDecodeError:
                doc = None
        return p.returncode, doc, p.stderr


def expect_rejected(exe: Path, plant: str, what: str, messages: int = 20000000) -> None:
    rc, doc, _ = run_subject(exe, plant, messages=messages)
    if rc == 0:
        verdict("MISSED", what, "the harness exited 0 -- the defect was accepted")
        return
    if doc is None:
        verdict("MISSED", what, "no result file was written, so the rejection cannot be attributed")
        return
    if doc.get("verified") is not False:
        verdict("MISSED", what, "exited non-zero but the result is not marked verified:false")
        return
    if not doc.get("failures"):
        verdict("MISSED", what, "marked unverified but named no reason")
        return
    if doc.get("work_ns"):
        verdict("MISSED", what,
                "REPORTED A TIMING for a run that failed verification -- FAIRNESS.md 0 says a "
                "failing framework produces no timing at all")
        return
    verdict("CAUGHT", what, doc["failures"][0][:110])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, type=Path)
    args = ap.parse_args()

    exe = args.build / "bin" / "qvoctl-control-subject"
    if not exe.exists():
        exe = exe.with_suffix(".exe")
    if not exe.exists():
        sys.exit(f"negative-control: {exe} not found -- build the QVO_BUILD_CONTROLS target first")

    print("== A. the positive control: an undefective subject must PASS ==")
    rc, doc, err = run_subject(exe, "none")
    if rc == 0 and doc and doc.get("verified") and doc.get("work_ns"):
        verdict("CONFIRMED", "a correct subject verifies and reports a timing")
    else:
        verdict("MISSED", "a correct subject was REJECTED",
                "every control below is meaningless: a battery that rejects everything is not a "
                f"working battery (rc={rc}, err={err.strip()[:100]})")

    # --out is a native path on Windows. Replacing an existing document under a directory whose
    # name is outside the active ANSI code page checks both the wide argument and wide rename path.
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "résultats-日本語" / "cell.json"
        out.parent.mkdir()
        out.write_text('{"stale": true}')
        cmd = [str(exe), "--repetitions", "1", "--warmup", "0", "--no-pin",
               "--out", str(out), "--param", "messages=1000", "--param", "cores=1",
               "--param", "wait=1"]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        try:
            fresh = json.loads(out.read_text())
        except (OSError, json.JSONDecodeError):
            fresh = None
        if (p.returncode == 0 and fresh and fresh.get("verified") is True
                and len(fresh.get("work_ns", [])) == 1
                and not list(out.parent.glob("*.qvo-tmp-*"))):
            verdict("CONFIRMED", "a Unicode --out path atomically replaces an older document")
        else:
            verdict("MISSED", "a Unicode --out path could not replace an older document",
                    f"rc={p.returncode}, err={p.stderr.strip()[:100]}")

    print("\n== B. message loss -- the defect the checksum exists for ==")
    expect_rejected(exe, "drop-rare", "one message dropped in 10^7 (20M messages, 2 losses)")
    expect_rejected(exe, "drop-one", "exactly one message dropped in the whole run",
                    messages=1000000)

    print("\n== C. duplication -- why the reduction is a sum and not an XOR ==")
    expect_rejected(exe, "duplicate", "one message delivered twice", messages=1000000)
    # An XOR-based checksum would be UNCHANGED by a duplicate. This assertion is what stops the
    # reduction being 'simplified' back to an XOR by someone who has not read ping-pong.md.
    print("             (an XOR reduction would be unchanged by this and the control would MISS)")

    print("\n== D. a wrong answer, and a right answer reached by the wrong amount of work ==")
    expect_rejected(exe, "wrong-answer", "checksum off by one", messages=1000000)
    expect_rejected(exe, "short-count", "correct checksum, wrong message count", messages=1000000)

    print("\n== E. an unmeasured window ==")
    expect_rejected(exe, "no-window", "the body never marked its workload window", messages=1000000)

    print("\n== F. pinning -- a refused pin must abort, never report ==")
    rc, doc, err = run_subject(exe, "none", extra=["--cpus", "4095"], messages=1000)
    if rc == 0:
        verdict("MISSED", "a pin that cannot be applied was accepted",
                "on a hybrid CPU an unpinned run is not a measurement (FAIRNESS.md 1.4)")
    elif "pinning" in err.lower() or "affinity" in err.lower():
        verdict("CAUGHT", "an impossible CPU set aborts the run", err.strip().splitlines()[0][:110])
    else:
        verdict("CAUGHT", "an impossible CPU set aborts the run", f"rc={rc}")

    print("\n== G. shapes that must NOT be reported (false-positive set) ==")
    rc, doc, _ = run_subject(exe, "none", extra=["--no-pin"], messages=1000)
    if rc == 0 and doc and doc.get("verified") and doc.get("pinned") is False:
        verdict("CONFIRMED", "--no-pin runs and records pinned:false rather than failing")
    else:
        verdict("MISSED", "--no-pin was rejected", "it is a documented escape hatch, not a defect")

    rc, doc, _ = run_subject(exe, "none", messages=1000)
    if rc == 0 and doc and doc.get("verified"):
        verdict("CONFIRMED", "a tiny run (1000 messages) still verifies")
    else:
        verdict("MISSED", "a small message count was rejected")

    # An undeclared parameter must be refused rather than silently defaulted to zero -- a
    # benchmark quietly running with messages=0 would 'verify' against an expected value of 0.
    rc, doc, err = run_subject(exe, "none", extra=["--param", "nonexistent=5"], messages=1000)
    if rc != 0:
        verdict("CONFIRMED", "an undeclared --param is refused rather than silently ignored")
    else:
        verdict("MISSED", "an undeclared --param was accepted",
                "a typo in a parameter name would then run a different benchmark than intended")

    print("\n== H. one defective repetition invalidates the whole run ==")
    for plant, warmup, failed_rep, what in (
        ("fail-second", 0, 1, "success then failure publishes no partial timing"),
        ("fail-first", 0, 0, "failure then success publishes no partial timing"),
        ("fail-first", 1, 0, "a defective warmup invalidates later measured successes"),
    ):
        rc, doc, err = run_subject(exe, plant, messages=1000,
                                  repetitions=2, warmup=warmup, extra=["--no-pin"])
        if (rc == 1 and doc and doc.get("verified") is False
                and doc.get("repetitions") == 2 and doc.get("warmup") == warmup
                and len(doc.get("failures", [])) == 1
                and doc["failures"][0].startswith(f"repetition {failed_rep}: checksum ")
                and all(doc.get(key) == [] for key in
                        ("work_ns", "total_ns", "outside_window_ns"))
                and doc.get("summary") == {}):
            verdict("CAUGHT", what, doc["failures"][0])
        else:
            verdict("MISSED", what, f"rc={rc}, doc={doc}, err={err.strip()[:100]}")

    rc, doc, err = run_subject(exe, "none", messages=1000, repetitions=2,
                              warmup=1, extra=["--no-pin"])
    if (rc == 0 and doc and doc.get("verified") is True and doc.get("failures") == []
            and doc.get("repetitions") == 2 and doc.get("warmup") == 1
            and all(len(doc.get(key, [])) == 2 for key in
                    ("work_ns", "total_ns", "outside_window_ns"))
            and set(doc.get("summary", {})) ==
            {"work_min", "work_p50", "work_p99", "work_iqr"}):
        verdict("CONFIRMED", "a correct warmup and two repetitions retain every sample")
    else:
        verdict("MISSED", "a correct multi-repetition run lost samples",
                f"rc={rc}, doc={doc}, err={err.strip()[:100]}")

    print(f"\n== census ==\n  CAUGHT={caught} CONFIRMED={confirmed} MISSED={missed} "
          f"(floors: CAUGHT>={FLOOR_CAUGHT} CONFIRMED>={FLOOR_CONFIRMED})")

    if missed:
        print("\nnegative-control: FAILED -- a planted defect was accepted")
        return 1
    if caught < FLOOR_CAUGHT or confirmed < FLOOR_CONFIRMED:
        print("\nnegative-control: FAILED -- below the recorded floor. A battery that stopped "
              "planting looks exactly like one that passes; if controls were deliberately "
              "removed, lower the floor in the same commit.")
        return 2
    print("\nnegative-control: ALL GREEN -- the verifier has been watched rejecting every "
          "planted defect")
    return 0


if __name__ == "__main__":
    sys.exit(main())
