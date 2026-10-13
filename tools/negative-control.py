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
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run as runner  # noqa: E402  (tools/run.py: its own acceptance rule judges the documents)

# Complete merged census measured on macOS arm64 with CLT 21 (2026-10-13):
# 151 CAUGHT / 43 CONFIRMED / 0 MISSED, 194 unique executed cases.
# Floors below were read from the successful --census-out export, not branch arithmetic.
# Windows/WSL validation of the merged candidate remains required.
FLOOR_CAUGHT = 151
FLOOR_CONFIRMED = 43

# The deadline controls (section I): a planted hang must end inside DEADLINE_S plus this slack --
# process start and the document write, generous for a loaded host -- and the battery gives up on
# the child at HANG_WAIT_S, which is a MISSED, never a hang of the battery itself.
DEADLINE_S = 2
DEADLINE_SLACK_S = 30
HANG_WAIT_S = 120

caught = confirmed = missed = 0
case_group = "A-K"
case_sequence = 0
case_verdicts: list[dict] = []


def set_case_group(name: str) -> None:
    global case_group, case_sequence
    case_group, case_sequence = name, 0


def verdict(kind: str, what: str, detail: str = "") -> None:
    global caught, confirmed, missed, case_sequence
    if kind == "CAUGHT":
        caught += 1
    elif kind == "CONFIRMED":
        confirmed += 1
    else:
        missed += 1
    case_sequence += 1
    case_id = f"{case_group}-{case_sequence:03d}"
    case_verdicts.append({"case_id": case_id, "group": case_group, "verdict": kind,
                          "what": what, "detail": detail})
    line = f"  {kind:<10} [{case_id}] {what}"
    if detail:
        line += f"\n             {detail}"
    print(line)


def run_subject(exe: Path, plant: str, extra: list[str] | None = None,
                messages: int = 20000000, repetitions: int = 1,
                warmup: int = 0, params: list[str] | None = None) -> tuple[int, dict | None, str]:
    """Run a control subject with one planted defect. Returns (rc, result_json_or_None, stderr).
    `params` replaces the ping-pong subject's (`messages`, cores=1, wait=1) for another subject."""
    env = dict(os.environ)
    env["QVO_CONTROL_PLANT"] = plant
    if params is None:
        params = [f"messages={messages}", "cores=1", "wait=1"]
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "r.json"
        cmd = [str(exe), "--repetitions", str(repetitions), "--warmup", str(warmup),
               "--out", str(out)]
        for kv in params:
            cmd += ["--param", kv]
        cmd += extra or []
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=900)
        doc = None
        if out.exists():
            try:
                doc = json.loads(out.read_text())
            except json.JSONDecodeError:
                doc = None
        return p.returncode, doc, p.stderr


def expect_rejected(exe: Path, plant: str, what: str, messages: int = 20000000,
                    because: str | None = None, params: list[str] | None = None) -> None:
    """A planted defect: exit non-zero, a failure document with a reason and no timing -- and,
    when `because` is given, a first reason containing it: the check that is MEANT to see the
    defect saw it, not another one by accident."""
    rc, doc, _ = run_subject(exe, plant, messages=messages, params=params)
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
    if because is not None and because not in doc["failures"][0]:
        verdict("MISSED", what, f"rejected, but not by the check meant to see it ({because!r}): "
                                f"{doc['failures'][0][:100]}")
        return
    verdict("CAUGHT", what, doc["failures"][0][:110])


def run_deadline(exe: Path, plant: str, deadline: str | None, repetitions: int = 1,
                 warmup: int = 0) -> tuple[int | None, dict | None, str, float, str]:
    """Run the subject, unpinned, with `--deadline <deadline>` (none when None).

    Returns (rc -- None when the battery itself had to kill the child --, document, stderr,
    elapsed seconds, tools/run.py's verdict on the document: "" when it would publish it)."""
    env = dict(os.environ)
    env["QVO_CONTROL_PLANT"] = plant
    params = {"messages": 1000, "cores": 1, "wait": 1}
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "r.json"
        cmd = [str(exe), "--repetitions", str(repetitions), "--warmup", str(warmup), "--no-pin",
               "--out", str(out)]
        for k, v in params.items():
            cmd += ["--param", f"{k}={v}"]
        if deadline is not None:
            cmd += ["--deadline", deadline]
        t0 = time.monotonic()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=HANG_WAIT_S)
        except subprocess.TimeoutExpired:
            return None, None, "", time.monotonic() - t0, "the battery killed the child"
        elapsed = time.monotonic() - t0
        doc = None
        if out.exists():
            try:
                doc = json.loads(out.read_text())
            except json.JSONDecodeError:
                doc = None
        if p.returncode in (0, 1, 3):
            _, accepted = runner.read_attempt(out, "control/ping-pong", "control", params,
                                              p.returncode, repetitions, warmup)
        else:
            accepted = f"exit {p.returncode}"
        return p.returncode, doc, p.stderr, elapsed, accepted


def expect_deadline(exe: Path, plant: str, what: str, repetitions: int, warmup: int,
                    want_failures: list[str]) -> None:
    """A planted hang: exit 1 inside the deadline (plus slack), a failure document whose reasons
    START WITH `want_failures` in order -- the deadline's own last -- no timing, and a document
    tools/run.py publishes as an unverified cell."""
    rc, doc, err, elapsed, accepted = run_deadline(exe, plant, str(DEADLINE_S), repetitions, warmup)
    fails = (doc or {}).get("failures") or []
    if rc is None:
        verdict("MISSED", what, f"still running after {HANG_WAIT_S} s -- the deadline never fired")
    elif rc != 1 or doc is None:
        verdict("MISSED", what, f"rc={rc}, document={'none' if doc is None else 'present'}, "
                                f"err={err.strip()[:100]}")
    elif not (DEADLINE_S <= elapsed < DEADLINE_S + DEADLINE_SLACK_S):
        verdict("MISSED", what, f"ended after {elapsed:.1f} s, deadline {DEADLINE_S} s")
    elif (doc.get("verified") is not False or len(fails) != len(want_failures)
          or not all(f.startswith(w) for f, w in zip(fails, want_failures))):
        verdict("MISSED", what, f"failures={fails}")
    elif any(doc.get(key) != [] for key in ("work_ns", "total_ns", "outside_window_ns")) \
            or doc.get("summary") != {}:
        verdict("MISSED", what, "a blocked run's document carries a timing")
    elif accepted:
        verdict("MISSED", what, f"tools/run.py would not publish the document: {accepted}")
    else:
        verdict("CAUGHT", what, f"{fails[-1][:100]} (exit 1 after {elapsed:.1f} s)")


def expect_declared_failure(exe: Path, plant: str, what: str, repetitions: int, warmup: int,
                            want_failures: list[str], deadline: str = "60") -> None:
    """A failure the body declares through qvo::fail() (Huly QB-1018): exit 1 -- never the
    not-applicable 3, nor std::abort's 0xC0000409 or SIGABRT -- a failure document whose reasons
    START WITH `want_failures` in order, no timing and no n/a, and a document tools/run.py
    publishes as an unverified cell. Under a 60 s deadline by default, so a fail() that returned
    or blocked reads as the deadline's failure -- a MISSED -- never as a hang of the battery; with
    `--deadline 0` the battery's own HANG_WAIT_S is the bound."""
    rc, doc, err, elapsed, accepted = run_deadline(exe, plant, deadline, repetitions, warmup)
    fails = (doc or {}).get("failures") or []
    if rc is None:
        verdict("MISSED", what, f"still running after {HANG_WAIT_S} s -- fail() never ended it")
    elif rc != 1 or doc is None:
        verdict("MISSED", what, f"rc={rc}, document={'none' if doc is None else 'present'}, "
                                f"err={err.strip()[:100]}")
    elif (doc.get("verified") is not False or len(fails) != len(want_failures)
          or not all(f.startswith(w) for f, w in zip(fails, want_failures))):
        verdict("MISSED", what, f"failures={fails}")
    elif any(doc.get(key) != [] for key in ("work_ns", "total_ns", "outside_window_ns")) \
            or doc.get("summary") != {} or doc.get("not_applicable") is not None:
        verdict("MISSED", what, "a declared failure's document carries a timing or an n/a")
    elif accepted:
        verdict("MISSED", what, f"tools/run.py would not publish the document: {accepted}")
    else:
        verdict("CAUGHT", what, f"{fails[-1][:100]} (exit 1 after {elapsed:.1f} s)")


def expect_blocked(exe: Path, *, plant: str, what: str, benchmark: str, params: list[str]) -> None:
    """A planted loss in a shape that waits for the end of its protocol: the run blocks, and the
    harness's deadline must end it -- exit 1 inside DEADLINE_S plus the slack, one failure naming
    the deadline, no timing, and a document tools/run.py publishes as an unverified cell."""
    env = dict(os.environ)
    env["QVO_CONTROL_PLANT"] = plant
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "r.json"
        cmd = [str(exe), "--repetitions", "1", "--warmup", "0", "--no-pin", "--out", str(out),
               "--deadline", str(DEADLINE_S)]
        for kv in params:
            cmd += ["--param", kv]
        t0 = time.monotonic()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=HANG_WAIT_S)
        except subprocess.TimeoutExpired:
            verdict("MISSED", what, f"still running after {HANG_WAIT_S} s -- the deadline never fired")
            return
        elapsed = time.monotonic() - t0
        doc = None
        if out.exists():
            try:
                doc = json.loads(out.read_text())
            except json.JSONDecodeError:
                doc = None
        accepted = f"exit {p.returncode}"
        if p.returncode == 1:
            given = {k: int(v) for k, v in (kv.split("=", 1) for kv in params)}
            _, accepted = runner.read_attempt(out, benchmark, "control", given, 1, 1, 0)
    fails = (doc or {}).get("failures") or []
    want = f"repetition 0: deadline exceeded after {DEADLINE_S} s"
    if p.returncode != 1 or doc is None:
        verdict("MISSED", what, f"rc={p.returncode}, document={'none' if doc is None else 'present'}, "
                                f"err={p.stderr.strip()[:100]}")
    elif not (DEADLINE_S <= elapsed < DEADLINE_S + DEADLINE_SLACK_S):
        verdict("MISSED", what, f"ended after {elapsed:.1f} s, deadline {DEADLINE_S} s")
    elif doc.get("verified") is not False or len(fails) != 1 or not fails[0].startswith(want):
        verdict("MISSED", what, f"failures={fails}")
    elif any(doc.get(key) != [] for key in ("work_ns", "total_ns", "outside_window_ns")) \
            or doc.get("summary") != {}:
        verdict("MISSED", what, "a blocked run's document carries a timing")
    elif accepted:
        verdict("MISSED", what, f"tools/run.py would not publish the document: {accepted}")
    else:
        verdict("CAUGHT", what, f"{fails[0][:100]} (exit 1 after {elapsed:.1f} s)")


def expect_refused(exe: Path, *, params: list[str], what: str, needle: str, plant: str = "none") -> None:
    """A parameter outside the spec's domain: exit 2, the fatal code, with no document, before
    anything is built."""
    rc, doc, err = run_subject(exe, plant, params=params, extra=["--no-pin"])
    if rc == 2 and doc is None and needle in err:
        verdict("CAUGHT", what)
    else:
        verdict("MISSED", what, f"rc={rc}, document={'none' if doc is None else 'present'}, "
                                f"err={err.strip()[:100]}")


def expect_consistent(exe: Path, *, plant: str, what: str, params: list[str], said: str) -> None:
    """A CONSISTENT fault: the subject says on stderr that every count is the closed form's, and
    the checksum must reject it anyway."""
    rc, doc, err = run_subject(exe, plant, params=params, extra=["--no-pin"])
    fails = (doc or {}).get("failures") or []
    if (rc == 1 and doc and doc.get("verified") is False and fails
            and fails[0].startswith("repetition 0: checksum ") and not doc.get("work_ns")
            and said in err):
        verdict("CAUGHT", what, fails[0][:100])
    else:
        verdict("MISSED", what, f"rc={rc}, failures={fails}, err={err.strip()[-160:]}")


def expect_correct(exe: Path, *, what: str, params: list[str], messages: int) -> None:
    """The positive control of a shape's subject: it verifies, with the messages it counted."""
    rc, doc, err = run_subject(exe, "none", params=params, extra=["--no-pin"])
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1 \
            and doc.get("expected_messages") == messages:
        verdict("CONFIRMED", what)
    else:
        verdict("MISSED", what + " -- REJECTED",
                f"rc={rc}, failures={(doc or {}).get('failures')}, err={err.strip()[:100]}")


def expect_rejected_saying(exe: Path, *, plant: str, what: str, params: list[str], because: str,
                           said: str) -> None:
    """A planted defect that ONE check alone can see: rejected by the check meant to see it (the
    first failure starts with `because`), with no timing, AND the subject's own report on stderr
    saying `said` -- the other checks it carries would have let the defect through."""
    rc, doc, err = run_subject(exe, plant, params=params, extra=["--no-pin"])
    fails = (doc or {}).get("failures") or []
    if (rc == 1 and doc and doc.get("verified") is False and fails
            and fails[0].startswith(because) and not doc.get("work_ns") and said in err):
        verdict("CAUGHT", what, fails[0][:100])
    else:
        verdict("MISSED", what, f"rc={rc}, failures={fails}, err={err.strip()[-200:]}")


def run_shape(exe: Path, *, plant: str, benchmark: str, params: dict,
              deadline: str | None = None,
              framework: str = "control") -> tuple[int | None, dict | None, str, float, str]:
    """Run a shape subject or actual adapter control, unpinned, with its own identity and params.

    Returns (rc -- None when the battery had to kill the child --, document, stderr, elapsed
    seconds, tools/run.py's verdict on the document: "" when it would publish it)."""
    env = dict(os.environ)
    env["QVO_CONTROL_PLANT"] = plant
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "r.json"
        cmd = [str(exe), "--repetitions", "1", "--warmup", "0", "--no-pin", "--out", str(out)]
        for k, v in params.items():
            cmd += ["--param", f"{k}={v}"]
        if deadline is not None:
            cmd += ["--deadline", deadline]
        t0 = time.monotonic()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=HANG_WAIT_S)
        except subprocess.TimeoutExpired:
            return None, None, "", time.monotonic() - t0, "the battery killed the child"
        elapsed = time.monotonic() - t0
        doc = None
        if out.exists():
            try:
                doc = json.loads(out.read_text())
            except json.JSONDecodeError:
                doc = None
        if p.returncode in (0, 1, 3):
            _, accepted = runner.read_attempt(out, benchmark, framework, params, p.returncode, 1, 0)
        else:
            accepted = f"exit {p.returncode}"
        return p.returncode, doc, p.stderr, elapsed, accepted


def shape_controls(exe: Path, *, benchmark: str, params: dict, expected_messages: int,
                   rejected: list[tuple[str, str, str]], blind: tuple[str, str, list[str]],
                   refused: tuple[dict, str], observed: str | None = None) -> None:
    """The battery of one shape's subject (sections T and U):

    * `none` and `shuffle` must verify -- the second delivering every message in a seeded random
      order: the answer does not depend on the interleaving;
    * every (plant, what, needle) of `rejected` must be rejected by the check whose first failure
      reason contains `needle`;
    * `lost` must end at the deadline with a failure document tools/run.py publishes;
    * the `blind` plant must be rejected by the checksum while the subject says on stderr that
      every line of its third element still holds -- the counts and the final answer a check
      without the witness would have compared;
    * the `refused` parameters must be refused (exit 2, no document) by the spec before anything
      runs."""
    for plant, what in (("none", "a correct run verifies"),
                        ("shuffle", "every message delivered in a seeded random order verifies -- "
                                    "the answer does not depend on the interleaving")):
        rc, doc, err, _, accepted = run_shape(exe,
                                        plant=plant,
                                        benchmark=benchmark,
                                        params=params)
        ok = (rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1
              and doc.get("expected_messages") == expected_messages and not accepted)
        if ok and observed is not None:
            ok = observed in (doc.get("observed") or {})
        if ok:
            verdict("CONFIRMED", what)
        else:
            verdict("MISSED", f"{what}: REJECTED",
                    f"rc={rc}, failures={(doc or {}).get('failures')}, err={err.strip()[-120:]}, "
                    f"run.py: {accepted or 'accepted'}")
    for plant, what, needle in rejected:
        rc, doc, err, _, accepted = run_shape(exe,
                                        plant=plant,
                                        benchmark=benchmark,
                                        params=params,
                                        deadline="60")
        fails = (doc or {}).get("failures") or []
        if (rc == 1 and doc and doc.get("verified") is False and fails and needle in fails[0]
                and not doc.get("work_ns") and not accepted):
            verdict("CAUGHT", what, fails[0][:110])
        else:
            verdict("MISSED", what, f"rc={rc}, failures={fails}, err={err.strip()[-120:]}, "
                                    f"run.py: {accepted or 'accepted'}")
    rc, doc, err, elapsed, accepted = run_shape(exe,
                                          plant="lost",
                                          benchmark=benchmark,
                                          params=params,
                                          deadline=str(DEADLINE_S))
    fails = (doc or {}).get("failures") or []
    if (rc == 1 and doc and len(fails) == 1
            and fails[0].startswith(f"repetition 0: deadline exceeded after {DEADLINE_S} s")
            and DEADLINE_S <= elapsed < DEADLINE_S + DEADLINE_SLACK_S and not doc.get("work_ns")
            and not accepted):
        verdict("CAUGHT", "a message lost: the protocol never ends, and the deadline fails it",
                f"{fails[0][:90]} (exit 1 after {elapsed:.1f} s)")
    else:
        verdict("MISSED", "a lost message did not end at the deadline",
                f"rc={rc}, failures={fails}, elapsed={elapsed:.1f}, run.py: {accepted or 'accepted'}")
    plant, what, still = blind
    rc, doc, err, _, accepted = run_shape(exe,
                                    plant=plant,
                                    benchmark=benchmark,
                                    params=params,
                                    deadline="60")
    fails = (doc or {}).get("failures") or []
    if (rc == 1 and doc and fails and fails[0].startswith("repetition 0: checksum ")
            and not doc.get("work_ns") and not accepted and all(s in err for s in still)):
        verdict("CAUGHT", what, fails[0][:100])
    else:
        verdict("MISSED", what, f"rc={rc}, failures={fails}, err={err.strip()[-200:]}")
    bad, needle = refused
    rc, doc, err, _, _ = run_shape(exe,
                             plant="none",
                             benchmark=benchmark,
                             params={**params, **bad})
    if rc == 2 and doc is None and needle in err:
        verdict("CAUGHT", f"parameters outside the spec's domain ({bad}) are refused (exit 2) "
                          "before anything runs")
    else:
        verdict("MISSED", f"parameters outside the spec's domain ({bad}) were not refused",
                f"rc={rc}, document={'none' if doc is None else 'present'}, err={err.strip()[:100]}")


def adapter_controls(exe: Path, *, framework: str, benchmark: str, params: dict,
                     expected_messages: int, rejected: list[tuple[str, str, str]],
                     blind: tuple[str, str, list[str]], observed: str | None = None,
                     blind_params: dict | None = None) -> None:
    """C1 plants compiled into the actual qb/floor adapter sources, never normal qvo-* binaries.

    Every fault must emit its one-shot plant marker and fail through its intended check. The
    checksum-only witness also proves that counts and final values still pass independent
    reference checks. The unmodified adapter is the positive control; shuffled deliveries remain
    the separate synthetic subject's positive control.
    """
    rc, doc, err, _, accepted = run_shape(exe, plant="none", framework=framework,
                                         benchmark=benchmark, params=params, deadline="60")
    ok = (rc == 0 and doc and doc.get("verified") is True
          and len(doc.get("work_ns", [])) == 1
          and doc.get("expected_messages") == expected_messages and not accepted
          and "control plant fired:" not in err)
    if ok and observed is not None:
        ok = observed in (doc.get("observed") or {})
    if ok:
        verdict("CONFIRMED", f"{framework} actual {benchmark} adapter verifies unmodified")
    else:
        verdict("MISSED", f"{framework} actual {benchmark} positive was rejected",
                f"rc={rc}, failures={(doc or {}).get('failures')}, "
                f"err={err.strip()[-160:]}, run.py: {accepted or 'accepted'}")

    faults = [(plant, what, needle, []) for plant, what, needle in rejected]
    plant, what, still = blind
    faults.append((plant, what, "repetition 0: checksum ", still))
    faults.append(("lost", "a lost message leaves the protocol waiting until its deadline",
                   f"repetition 0: deadline exceeded after {DEADLINE_S} s", []))
    for plant, what, needle, still in faults:
        deadline = str(DEADLINE_S) if plant == "lost" else "60"
        rc, doc, err, elapsed, accepted = run_shape(
            exe, plant=plant, framework=framework, benchmark=benchmark,
            params=(blind_params if plant == blind[0] and blind_params is not None else params),
            deadline=deadline)
        fails = (doc or {}).get("failures") or []
        landed = err.count(f"control plant fired: {plant}\n") == 1
        no_timing = (doc is not None
                     and all(doc.get(key) == [] for key in
                             ("work_ns", "total_ns", "outside_window_ns"))
                     and doc.get("summary") == {} and doc.get("not_applicable") is None)
        ok = (rc == 1 and doc and doc.get("verified") is False and len(fails) == 1
              and needle in fails[0] and landed and no_timing and not accepted
              and all(line in err for line in still))
        if plant == "lost":
            ok = ok and DEADLINE_S <= elapsed < DEADLINE_S + DEADLINE_SLACK_S
        if ok:
            verdict("CAUGHT", f"{framework} actual {benchmark}: {what}", fails[0][:110])
        else:
            verdict("MISSED", f"{framework} actual {benchmark}: {what}",
                    f"rc={rc}, planted={landed}, failures={fails}, elapsed={elapsed:.1f}, "
                    f"err={err.strip()[-200:]}, run.py: {accepted or 'accepted'}")


def comparable(doc: dict) -> dict:
    """The document minus what legitimately differs between two launches: the clock (env.utc)
    and the timings. Everything else must be byte-for-byte the same."""
    d = json.loads(json.dumps(doc))
    d.get("env", {}).pop("utc", None)
    for key in ("work_ns", "total_ns", "outside_window_ns"):
        d[key] = len(d.get(key, []))
    d["summary"] = sorted(d.get("summary", {}))
    return d


def mesh_controls(fixed: Path, reentrant: Path) -> None:
    """A functional owner-dispatch control, separate from the timed qvospec harness subjects.

    The compile-only mutant bypasses the Mesh dispatch guard. Both versions deliver every
    submitted message; only the fixed owner preserves non-reentrancy and FIFO. These binaries
    emit a functional JSON verdict with no timing fields and cannot enter the runner's roster.
    """
    set_case_group("mesh")
    for workers, owner in ((1, 0), (2, 0), (2, 1)):
        for mode in ("spin", "park"):
            for exe, valid, code, kind in ((fixed, True, 0, "CONFIRMED"),
                                           (reentrant, False, 1, "CAUGHT")):
                what = f"Mesh owner dispatch {workers} worker(s), active owner {owner}, {mode}, "
                what += "fixed preserves FIFO" if valid else "reentrant mutant violates FIFO"
                try:
                    process = subprocess.run([str(exe), str(workers), mode, str(owner)],
                                             capture_output=True, text=True, timeout=60)
                    doc = json.loads(process.stdout)
                except (subprocess.TimeoutExpired, json.JSONDecodeError) as error:
                    verdict("MISSED", what, str(error))
                    continue
                ok = (process.returncode == code and doc.get("valid") is valid
                      and doc.get("received") == 65537 and doc.get("submitted") is True
                      and doc.get("no_reentry_and_fifo") is valid
                      and not any(key in doc for key in
                                  ("work_ns", "total_ns", "outside_window_ns", "summary")))
                if ok:
                    verdict(kind, what, "65 537 messages delivered; no timing fields")
                else:
                    verdict("MISSED", what,
                            f"rc={process.returncode}, document={doc}, "
                            f"err={process.stderr.strip()[-160:]}")

    # Cross-worker sender 1 to owner 0: submitted is read only after run() joins
    # worker 1, never concurrently in the receiving handler.
    for mode in ("spin", "park"):
        what = f"Mesh cross-worker FIFO, sender 1 to owner 0, {mode}"
        process = subprocess.run([str(fixed), "2", mode, "cross"], capture_output=True, text=True, timeout=60)
        try:
            doc = json.loads(process.stdout)
        except json.JSONDecodeError:
            verdict("MISSED", what, "invalid functional JSON")
            continue
        if (process.returncode == 0 and doc.get("valid") is True and doc.get("received") == 65537
                and doc.get("submitted") is True and doc.get("no_reentry_and_fifo") is True
                and not any(k in doc for k in ("work_ns", "total_ns", "outside_window_ns", "summary"))):
            verdict("CONFIRMED", what, "all 65 537 messages delivered in sender order; no timing")
        else:
            verdict("MISSED", what, f"rc={process.returncode}, doc={doc}, stderr={process.stderr[-160:]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--census-out", type=Path,
                    help="write the completed execution's verdict identities and actual counts")
    args = ap.parse_args()

    def subject(name: str) -> Path:
        exe = args.build / "bin" / name
        if not exe.exists():
            exe = exe.with_suffix(".exe")
        if not exe.exists():
            sys.exit(f"negative-control: {exe} not found -- build the QVO_BUILD_CONTROLS targets "
                     "first")
        return exe

    exe = subject("qvoctl-control-subject")
    ring = subject("qvoctl-thread-ring-subject")
    radix = subject("qvoctl-radixsort-subject")
    bank = subject("qvoctl-filterbank-subject")
    sieve = subject("qvoctl-sieve-subject")
    trap = subject("qvoctl-trapezoid-subject")
    pip = subject("qvoctl-piprecision-subject")
    rmm = subject("qvoctl-recmatmul-subject")
    qs = subject("qvoctl-quicksort-subject")
    bs = subject("qvoctl-bitonicsort-subject")
    uct = subject("qvoctl-uct-subject")
    facloc = subject("qvoctl-facloc-subject")

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
    # A count the body does not report asserts nothing; the harness used to skip the comparison
    # when Answer::messages was 0 (Huly QB-1017).
    expect_rejected(exe, "no-count", "correct checksum, no message count against a spec that "
                    "declares one", messages=1000000, because="reported no message count")

    print("\n== E. an unmeasured window ==")
    expect_rejected(exe, "no-window", "the body never marked its workload window", messages=1000000)

    print("\n== E2. an asserted lower bound on an observation ==")
    # The work a checksum cannot see: an adapter that skips a minimum the semantics requires (a
    # search that never hands its frontier back) reaches the right answer anyway. The spec's
    # Spec::observed_at_least makes that minimum an assertion; these two plants must be rejected.
    expect_rejected(exe, "observe-below", "an observation one below its asserted lower bound",
                    messages=1000000)
    expect_rejected(exe, "observe-missing", "an observation the spec bounds was never reported",
                    messages=1000000)

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

    # Observations are REPORTED, not asserted: a value above its bound and a name with no bound
    # must verify, and both must reach the document the report renders beside the cell.
    rc, doc, _ = run_subject(exe, "observe-extra", messages=1000)
    obs = (doc or {}).get("observed", {})
    if (rc == 0 and doc and doc.get("verified") and obs.get("extra", {}).get("samples") == [42]
            and obs.get("rounds", {}).get("samples") == [1005]
            and doc.get("observed_at_least") == {"rounds": 1000}):
        verdict("CONFIRMED", "an observation above its bound and an unbounded one verify, and "
                             "both are written beside the cell")
    else:
        verdict("MISSED", "an observation above its bound, or an unbounded one, was rejected or "
                          "not recorded", f"rc={rc}, observed={obs}")

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

    print(f"\n== I. the harness deadline -- a run that never ends FAILS, it does not run forever ==")
    # A lost message in a shape that waits for the end of its protocol does not move the
    # checksum: the body never returns (Huly QB-991). The harness's own deadline turns that into a
    # failure document and exit 1; tools/run.py --timeout is only the outer bound.
    expect_deadline(exe, "hang", "a repetition that never returns (a lost message) ends at its "
                    "deadline with a failure document", 1, 0,
                    [f"repetition 0: deadline exceeded after {DEADLINE_S} s"])
    expect_deadline(exe, "hang-teardown", "a teardown that never returns, after a warmup and a "
                    "verified repetition, publishes no partial timing", 2, 1,
                    [f"repetition 2: deadline exceeded after {DEADLINE_S} s"])
    expect_deadline(exe, "fail-then-hang", "a wrong checksum then a hang: the deadline's document "
                    "keeps the earlier failure", 2, 0,
                    ["repetition 0: checksum ", f"repetition 1: deadline exceeded after {DEADLINE_S} s"])

    refused = []
    for bad in ("-1", "abc", "nan", "2s"):
        rc, _, err, _, _ = run_deadline(exe, "none", bad)
        if rc != 2 or "--deadline expects" not in err:
            refused.append(f"{bad!r}: rc={rc}")
    if refused:
        verdict("MISSED", "a --deadline that is not a number of seconds >= 0 was accepted",
                "; ".join(refused))
    else:
        verdict("CAUGHT", "a --deadline that is not a number of seconds >= 0 is refused "
                          "(-1, abc, nan, 2s: exit 2) -- 0 disables it, so a typo must not")

    rc, doc, err, elapsed, accepted = run_deadline(exe, "none", "60", repetitions=2, warmup=1)
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 2 \
            and not accepted:
        verdict("CONFIRMED", "a correct run inside its deadline verifies (no false positive)")
    else:
        verdict("MISSED", "a correct run inside its deadline was rejected",
                f"rc={rc}, err={err.strip()[:100]}, run.py: {accepted or 'accepted'}")

    rc, doc_off, err, _, _ = run_deadline(exe, "none", "0", repetitions=2, warmup=1)
    if rc == 0 and doc_off and doc_off.get("verified") is True:
        verdict("CONFIRMED", "--deadline 0 runs without a watchdog and verifies")
    else:
        verdict("MISSED", "--deadline 0 was rejected", f"rc={rc}, err={err.strip()[:100]}")

    rc_on, doc_on, err, _, _ = run_deadline(exe, "none", None, repetitions=2, warmup=1)
    if rc_on == 0 and doc_on and doc_off and comparable(doc_on) == comparable(doc_off):
        verdict("CONFIRMED", "the watchdog changes nothing in a correct run's document: the "
                             "default deadline and --deadline 0 agree outside the clock and the "
                             "timings")
    else:
        verdict("MISSED", "the document of a correct run differs with the watchdog armed",
                f"rc={rc_on}, err={err.strip()[:100]}")

    print("\n== J. a failure the body declares -- exit 1 and a document, never std::abort ==")
    # A protocol violation a spec or an adapter detects (concdict's answer after the last request,
    # barber's (n+1)-th Next) cannot wait for a checksum. It used to std::abort(): no document,
    # and on Windows 0xC0000409 -- or 3, the not-applicable code, under another abort behaviour
    # (Huly QB-1018). qvo::fail() ends it like the deadline does, from whichever thread saw it.
    declared = "planted protocol violation on "
    expect_declared_failure(exe, "fail-body", "the body's own thread declares a failure inside its "
                            "window", 1, 0, [f"repetition 0: {declared}the body's thread"])
    expect_declared_failure(exe, "fail-thread", "another thread declares it while the body waits "
                            "(an actor's handler, the body in its join)", 2, 1,
                            [f"repetition 0: {declared}another thread"])
    expect_declared_failure(exe, "fail-thread", "the same with the watchdog off (--deadline 0): "
                            "the repetition is still named and documented", 1, 0,
                            [f"repetition 0: {declared}another thread"], deadline="0")
    expect_declared_failure(exe, "fail-after-failure", "a wrong checksum then a declared failure: "
                            "the document keeps both, in order", 2, 0,
                            ["repetition 0: checksum ", f"repetition 1: {declared}another thread"])
    rc, doc, err, _, _ = run_deadline(exe, "refuse-thread", "60")
    if rc == 2 and doc is None and "qvo: FATAL: planted refusal on another thread" in err:
        verdict("CAUGHT", "a refusal from another thread exits 2 -- the fatal code, never 3 -- "
                          "with no document")
    else:
        verdict("MISSED", "a refusal did not exit 2 without a document",
                f"rc={rc}, document={'none' if doc is None else 'present'}, "
                f"err={err.strip()[:100]}")

    print("\n== K. savina/thread-ring -- every actor's tally (count, sum of countdowns) against "
          "the closed form ==")
    # Until Huly QB-1017 the ring folded mix(remaining) into the token and the count was
    # declared, so a token delivered twice, to the wrong ring actor, or forwarded untouched
    # verified. The thread-ring subject tallies through the spec's own Tally and reduces through
    # its checksum() and delivered(), like every adapter.
    ring_params = ["actors=100", "hops=100000", "cores=1", "wait=1"]
    rc, doc, err = run_subject(ring, "none", params=ring_params, extra=["--no-pin"])
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1 \
            and doc.get("expected_messages") == 100001:
        verdict("CONFIRMED", "a correct ring verifies: every actor's count and sum as the closed "
                             "form says, 100 001 deliveries counted")
    else:
        verdict("MISSED", "a correct ring was REJECTED",
                f"rc={rc}, failures={(doc or {}).get('failures')}, err={err.strip()[:100]}")
    for plant, what in (
        ("duplicate", "the last token delivered twice -- its copy carries the right countdown"),
        ("misroute", "one hop to the wrong ring actor"),
        ("untouched", "one actor forwards the token without handling it"),
        ("never-touched", "the ring routes around one actor on every lap -- its tally is empty"),
    ):
        expect_rejected(ring, plant, what, because="checksum ", params=ring_params)
    # The CONSISTENT misroute: every hop two actors on, on a ring of 101 with 101 000 hops. Every
    # actor receives exactly the 1 000 tokens it should -- a per-actor count passes it, and the
    # subject says so on stderr -- and each receives another actor's countdowns: the sums fail it.
    stride_params = ["actors=101", "hops=101000", "cores=1", "wait=1"]
    rc, doc, err = run_subject(ring, "stride-2", params=stride_params, extra=["--no-pin"])
    fails = (doc or {}).get("failures") or []
    if (rc == 1 and doc and doc.get("verified") is False and fails
            and fails[0].startswith("repetition 0: checksum ") and not doc.get("work_ns")
            and "every count as the closed form says: yes" in err):
        verdict("CAUGHT", "a consistent misroute (stride 2 on an odd ring): every count right, "
                          "the sums reject it", fails[0][:100])
    else:
        verdict("MISSED", "a consistent misroute was not rejected by the sums with every count "
                          "right", f"rc={rc}, failures={fails}, err={err.strip()[-160:]}")
    rc, doc, err = run_subject(ring, "none", params=["actors=100", "hops=0", "cores=1", "wait=1"],
                               extra=["--no-pin"])
    if rc == 2 and doc is None and "qvo: FATAL: savina/thread-ring: need" in err:
        verdict("CAUGHT", "a ring of 0 hops is refused (exit 2) before anything runs -- its token "
                          "would count down from 2^64")
    else:
        verdict("MISSED", "a ring of 0 hops was not refused",
                f"rc={rc}, document={'none' if doc is None else 'present'}, "
                f"err={err.strip()[:100]}")

    set_case_group("L")
    print("\n== L. savina/radixsort -- every stage's tally and the sorted sequence, position by "
          "position ==")
    # The pipeline on one thread through the spec's Stage and Validation (Huly QB-246). A stage
    # receives exactly `values` values: one more fails the run as a protocol violation; one fewer
    # leaves it waiting, and the deadline ends the run.
    radix_params = ["values=1000", "bits=8", "seed=7", "cores=1", "wait=1"]
    expect_correct(radix,
        what="a correct pipeline verifies: every stage's tally and the sequence, "
                   "9 001 deliveries counted",
        params=radix_params,
        messages=9001)
    expect_rejected(radix, "duplicate", "a value delivered twice to the next stage",
                    because="protocol violated", params=radix_params)
    expect_blocked(radix,
        plant="lost",
        what="a value lost between two stages blocks the chain until the "
                   "deadline",
        benchmark="control/radixsort",
        params=radix_params)
    expect_blocked(radix,
        plant="skip",
        what="a value sent past the next stage (a misroute) blocks the stage "
                   "it skipped",
        benchmark="control/radixsort",
        params=radix_params)
    for plant, what in (
        ("untouched", "a held value passed on at once (the stage's work not done)"),
        ("routed-around", "the chain routed around its first stage (never-touched work)"),
        ("reversed", "the last stage flushing what it held in reverse (no longer stable)"),
    ):
        expect_rejected(radix, plant, what, because="checksum ", params=radix_params)
    expect_consistent(radix,
        plant="swapped",
        what="stages 0 and 1 wired in each other's place: every stage "
                      "tally right, the sequence rejects it",
        params=radix_params,
        said="every stage tally as the closed form says: yes")
    expect_refused(radix,
        params=["values=0", "cores=1", "wait=1"],
        what="a pipeline of 0 values is refused "
                   "(exit 2) before anything runs",
        needle="qvo: FATAL: savina/radixsort: need")

    set_case_group("M")
    print("\n== M. savina/filterbank -- every actor's tally and the sink's sums, bit for bit ==")
    bank_params = ["simulations=300", "columns=16", "channels=3", "cores=1", "wait=1"]
    expect_correct(bank,
        what="a correct filter bank verifies: every actor's tally and the sequence, "
                   "8 354 deliveries counted",
        params=bank_params,
        messages=8354)
    for plant, what in (
        ("duplicate", "a delay's output delivered twice to its FIR"),
        ("lost", "a sampler's output lost"),
        ("misroute", "a value tagged with another bank's channel"),
        ("untouched", "a sampler keeping a value it should zero (its work not done)"),
        ("routed-around", "a bank's second FIR routed around (never-touched work)"),
    ):
        expect_rejected(bank, plant, what, because="checksum ", params=bank_params)
    expect_consistent(bank,
        plant="swapped",
        what="two banks filtering with each other's coefficients: every "
                      "count right, the sums reject it",
        params=bank_params,
        said="every count as the closed form says: yes")
    expect_blocked(bank,
        plant="lost-exit",
        what="a bank's Exit lost blocks the integrator until the deadline",
        benchmark="control/filterbank",
        params=bank_params)
    expect_refused(bank,
        params=["channels=1", "cores=1", "wait=1"],
        what="a bank of 1 channel is refused "
                   "(exit 2) before anything runs",
        needle="qvo: FATAL: savina/filterbank: need")

    set_case_group("N")
    print("\n== N. savina/sieve -- every filter's tally and the primes it holds, position by "
          "position ==")
    sieve_params = ["limit=1000", "local=3", "cores=1", "wait=1"]
    expect_correct(sieve,
        what="a correct growing chain verifies: 56 filters, every tally and every "
                   "prime, 5 251 deliveries counted",
        params=sieve_params,
        messages=5251)
    for plant, what in (
        ("duplicate", "a candidate delivered twice"),
        ("lost", "a prime candidate lost"),
        ("skip", "a candidate sent past the next filter (a misroute)"),
        ("early-exit", "the Exit overtaking the last candidates"),
    ):
        expect_rejected(sieve, plant, what, because="checksum ", params=sieve_params)
    # A composite kept as a prime is one prime too many: with 168 primes below 1 000 and 3 per
    # filter the chain needs exactly 56 filters, and the 57th creation fails the run.
    for plant, what in (
        ("reorder", "a composite ahead of its smallest factor, kept as a prime: one filter too many"),
        ("untouched", "a filter passing candidates on untested (never-touched work): composites "
                      "kept as primes, filters too many"),
        ("grow-early", "a filter created one prime early: more filters than the primes allow"),
    ):
        expect_rejected(sieve, plant, what, because="protocol violated", params=sieve_params)
    expect_blocked(sieve,
        plant="lost-exit",
        what="a filter's Exit lost blocks the chain until the deadline",
        benchmark="control/sieve",
        params=sieve_params)
    expect_refused(sieve,
        params=["local=0", "cores=1", "wait=1"],
        what="a filter holding 0 primes is refused "
                   "(exit 2) before anything runs",
        needle="qvo: FATAL: savina/sieve: need")

    set_case_group("O")
    print("\n== O. savina/trapezoid -- the master's records folded in worker order, every worker's "
          "count, the answer's bits ==")
    # Savina's master folds the areas in arrival order, so its answer moves in the last bits from
    # run to run; the spec's master keeps one record per worker and folds them in worker order,
    # through one compiled kernel (Huly QB-246). The subject integrates through the spec's own
    # integrate(), records through Record and folds through fold(), like every adapter.
    trap_params = ["pieces=100000", "workers=10", "cores=1", "wait=1"]
    for plant, what in (
        ("none", "a correct fan-out verifies: every area to the bit, 20 deliveries counted"),
        ("reverse", "the results folded in REVERSE arrival order verify -- no arrival order moves "
                    "a bit of the answer"),
    ):
        rc, doc, err = run_subject(trap, plant, params=trap_params, extra=["--no-pin"])
        if rc == 0 and doc and doc.get("verified") is True and doc.get("expected_messages") == 20:
            verdict("CONFIRMED", what)
        else:
            verdict("MISSED", what, f"rc={rc}, failures={(doc or {}).get('failures')}, "
                                    f"err={err.strip()[:100]}")
    for plant, what in (
        ("duplicate-work", "one work message delivered twice -- two results, twice the area"),
        ("duplicate-result", "one result delivered twice -- the master's count reached one real "
                             "result early"),
        ("misroute", "one worker's range delivered to another -- every message count right"),
        ("untouched", "one worker answers without integrating"),
        ("wrong-range", "one worker integrates its range shifted by one trapezoid"),
    ):
        expect_rejected(trap, plant, what, because="checksum ", params=trap_params)
    expect_refused(trap,
        what="fewer trapezoids than workers is refused (exit 2) before anything runs",
        params=["pieces=5", "workers=10", "cores=1", "wait=1"],
        needle="qvo: FATAL: savina/trapezoid: need")

    # Derive this platform's adjacent finite boundary through the actual subject's config,
    # reference and one-piece body. No hand-written cross-libm boundary is a source of truth.
    lo, hi = 1, (1 << 20) - 1
    boundary_valid = True
    while lo < hi:
        mid = lo + (hi - lo + 1) // 2
        probe_params = ["pieces=1", "workers=1", f"left={mid - 1}", f"right={mid}", "cores=1", "wait=1"]
        rc, doc, err = run_subject(trap, "none", params=probe_params, extra=["--no-pin"])
        if rc == 0 and doc and doc.get("verified") is True:
            lo = mid
        elif rc == 2 and doc is None and "finite binary64 integrand envelope" in err:
            hi = mid - 1
        else:
            verdict("MISSED", "the finite trapezoid boundary cannot be derived from this subject",
                    f"right={mid}, rc={rc}, document={doc}, err={err.strip()[:160]}")
            boundary_valid = False
            break
    if boundary_valid:
        right = lo
        for what, left, pieces, workers in (
            ("largest admitted finite interval verifies", right - 1, 1, 1),
            ("widest admitted finite interval verifies", 0, 1, 1),
            ("rounded finite partitions verify", 0, 13, 5),
        ):
            params = [f"pieces={pieces}", f"workers={workers}", f"left={left}", f"right={right}", "cores=1", "wait=1"]
            rc, doc, err = run_subject(trap, "none", params=params, extra=["--no-pin"])
            if rc == 0 and doc and doc.get("verified") is True and doc.get("expected_messages") == 2 * workers:
                verdict("CONFIRMED", what, f"native right boundary={right}")
            else:
                verdict("MISSED", what, f"rc={rc}, document={doc}, err={err.strip()[:160]}")
        expect_refused(trap, params=["pieces=1", "workers=1", f"left={right}", f"right={right + 1}", "cores=1", "wait=1"],
                       what="the adjacent nonfinite endpoint is refused before a measurement",
                       needle="finite binary64 integrand envelope")
    expect_refused(trap, params=["pieces=1", "workers=1", "left=300000", "right=300001", "cores=1", "wait=1"],
                   what="the formerly accepted infinite trapezoid interval is refused before a measurement",
                   needle="finite binary64 integrand envelope")
    expect_refused(trap, params=["pieces=1", "workers=1", "left=1", "right=5", "cores=1", "wait=1"],
                   what="a nondefault rounding mode is refused before reference calculation",
                   needle="requires round-to-nearest mode", plant="round-down")

    set_case_group("P")
    print("\n== P. savina/piprecision -- the exact integer sum of the terms, every term index "
          "answered once, the terms handed out reported ==")
    # Savina's terms are BigDecimals at the scale; the spec computes them exactly on their unscaled
    # integers, so the sum is an integer sum and the race past the first term at the tolerance
    # (every later term rounds to 0) never reaches the answer (Huly QB-246). The subject computes
    # through the spec's own bbp_term() and folds through Fold, like every adapter.
    pip_params = ["precision=1000", "workers=20", "cores=1", "wait=1"]
    for plant, what in (
        ("none", "a correct run verifies: the sum to the limb, every index answered once"),
        ("shuffled", "deliveries drawn at random verify -- another number of terms handed out, "
                     "the same answer"),
    ):
        rc, doc, err = run_subject(pip, plant, params=pip_params, extra=["--no-pin"])
        if (rc == 0 and doc and doc.get("verified") is True
                and "terms" in (doc.get("observed") or {})):
            verdict("CONFIRMED", what)
        else:
            verdict("MISSED", what, f"rc={rc}, failures={(doc or {}).get('failures')}, "
                                    f"err={err.strip()[:100]}")
    for plant, what in (
        ("duplicate-answer", "one answer delivered twice"),
        ("untouched", "one term answered without being computed"),
        ("wrong-term", "one term answered with the next term's value"),
    ):
        expect_rejected(pip, plant, what, because="checksum ", params=pip_params)
    expect_rejected_saying(pip,
        plant="duplicate-zero",
        what="an answer of value 0 delivered twice: the sum "
                           "unchanged, the per-index count rejects it",
        params=pip_params,
        because="repetition 0: checksum ",
        said="the duplicated answer is 0: yes")
    expect_rejected_saying(pip,
        plant="skip-zero",
        what="a term past k1 answered 0 without being computed: "
                           "the value right, the proof of work rejects it",
        params=pip_params,
        because="repetition 0: checksum ",
        said="the skipped term's value is 0: yes")
    expect_rejected(pip, "phantom", "an answer for a term never handed out is a declared protocol "
                    "failure (qvo::fail)", because="savina/piprecision: protocol violated",
                    params=pip_params)
    expect_refused(pip,
        what="a precision of 0 is refused (exit 2) before anything runs",
        params=["precision=0", "workers=20", "cores=1", "wait=1"],
        needle="qvo: FATAL: savina/piprecision: need")

    set_case_group("Q")
    print("\n== Q. savina/recmatmul -- C against its closed form, every worker's tally against "
          "the task tree, the master's counts ==")
    # Savina's data (A[i][k] = i, B[k][j] = j) makes every inner range give the same product, so a
    # task with the wrong inner corners passes Savina's own check; the spec's data does not, and
    # every entry of C is an exact integer, so no order of the products moves a bit (Huly QB-246).
    # The subject splits, routes and multiplies through the spec's own functions, like every
    # adapter, with a mailbox per sender.
    rmm_params = ["length=64", "threshold=64", "workers=20", "cores=1", "wait=1"]
    for plant, what in (
        ("none", "a correct decomposition verifies: C to the bit, every tally, 1 754 deliveries "
                 "counted"),
        ("shuffled", "deliveries interleaved at random, each sender's order kept, verify -- no "
                     "order of the products moves a bit of C"),
    ):
        rc, doc, err = run_subject(rmm, plant, params=rmm_params, extra=["--no-pin"])
        if (rc == 0 and doc and doc.get("verified") is True
                and doc.get("expected_messages") == 1754):
            verdict("CONFIRMED", what)
        else:
            verdict("MISSED", what, f"rc={rc}, failures={(doc or {}).get('failures')}, "
                                    f"err={err.strip()[:100]}")
    for plant, what in (
        ("duplicate", "one leaf delivered twice -- its product added twice"),
        ("lost-child", "one created task lost on its way to the master -- the run still ENDS"),
        ("untouched", "one leaf answered with a done and no product"),
    ):
        expect_rejected(rmm, plant, what, because="checksum ", params=rmm_params)
    expect_rejected_saying(rmm,
        plant="misroute",
        what="one leaf routed to the wrong worker: C right, the "
                           "tallies reject it",
        params=rmm_params,
        because="repetition 0: checksum ",
        said="every entry as the closed form says: yes")
    expect_rejected_saying(rmm,
        plant="wrong-inner",
        what="one leaf multiplying the other half of the inner "
                           "index: Savina's own data would accept it, the spec's rejects it",
        params=rmm_params,
        because="repetition 0: checksum ",
        said="Savina's data would accept this run: yes")
    expect_refused(rmm,
        what="a dimension that is not a power of two is refused (exit 2) before "
                   "anything runs",
        params=["length=100", "cores=1", "wait=1"],
        needle="qvo: FATAL: savina/recmatmul: need")

    set_case_group("R")
    print("\n== R. savina/quicksort -- the driver's sequence (length, multiset, descents) and the "
          "tree's Sorts and Results against the sequential reference ==")
    # Huly QB-246. The subject runs the decomposition on one thread through the spec's own split(),
    # sort_leaf(), join(), measure() and answer(), and says on stderr whether its output is the
    # sorted input -- which is how inline-split shows the case an output check alone misses.
    qs_params = ["n=100000", "threshold=2048", "cores=1", "wait=1"]
    rc, doc, err = run_subject(qs, "none", params=qs_params, extra=["--no-pin"])
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1 \
            and doc.get("expected_messages") == 406 \
            and "the output is the sorted input: yes" in err:
        verdict("CONFIRMED", "a correct decomposition verifies: the sorted input, 203 actors, "
                             "406 Sorts and Results counted")
    else:
        verdict("MISSED", "a correct decomposition was REJECTED",
                f"rc={rc}, failures={(doc or {}).get('failures')}, "
                f"messages={(doc or {}).get('expected_messages')}, err={err.strip()[:100]}")
    for plant, what in (
        ("dup-result", "a Result handled twice by its parent -- its values joined twice"),
        ("wrong-side", "a Result joined on the wrong side -- every value present, out of order"),
        ("unsorted-leaf", "a leaf answers without its sequential sort"),
    ):
        expect_rejected(qs, plant, what, because="checksum ", params=qs_params)
    rc, doc, err = run_subject(qs, "inline-split", params=qs_params, extra=["--no-pin"])
    fails = (doc or {}).get("failures") or []
    if (rc == 1 and doc and doc.get("verified") is False and fails
            and fails[0].startswith("repetition 0: checksum ") and not doc.get("work_ns")
            and "the output is the sorted input: yes" in err):
        verdict("CAUGHT", "an actor that should split sorts its sequence itself: the RIGHT "
                          "output with fewer actors -- the counts reject it", fails[0][:100])
    else:
        verdict("MISSED", "the right output reached by less work was not rejected by the counts "
                          "with the output right",
                f"rc={rc}, failures={fails}, err={err.strip()[-160:]}")
    expect_blocked(qs,
        plant="lost-result",
        what="a Result never delivered blocks its parent: the deadline "
                                      "fails the run",
        benchmark='control/quicksort',
        params=qs_params)
    rc, doc, err = run_subject(qs, "none", params=["n=100", "threshold=2048", "cores=1", "wait=1"],
                               extra=["--no-pin"])
    if rc == 2 and doc is None and "qvo: FATAL: savina/quicksort: need" in err:
        verdict("CAUGHT", "fewer values than the threshold is refused (exit 2) before anything "
                          "runs -- the root would never split, its two seeds never be asked")
    else:
        verdict("MISSED", "n below the threshold was not refused",
                f"rc={rc}, document={'none' if doc is None else 'present'}, "
                f"err={err.strip()[:100]}")

    set_case_group("S")
    print("\n== S. savina/bitonicsort -- every actor's tally (messages, sum of values) and the "
          "sink's sequence against the reference network ==")
    # Huly QB-246. The subject runs the spec's topology() on one thread through its own Splitter,
    # Comparator and Joiner and reduces through its checksum(); the spec's explain() line says
    # whether every actor's COUNT is the reference's and how many descents the output has -- which
    # is how stride shows the case a per-actor count misses, and swap the case the output misses.
    bs_params = ["n=256", "cores=1", "wait=1"]
    rc, doc, err = run_subject(bs, "none", params=bs_params, extra=["--no-pin"])
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1 \
            and doc.get("expected_messages") == 87924:
        verdict("CONFIRMED", "a correct network verifies: every actor's tally as the reference "
                             "says, the sorted input, 87 924 deliveries counted")
    else:
        verdict("MISSED", "a correct network was REJECTED",
                f"rc={rc}, failures={(doc or {}).get('failures')}, "
                f"messages={(doc or {}).get('expected_messages')}, err={err.strip()[:100]}")
    for plant, what in (
        ("dup-sink", "the last value delivered to the sink twice"),
        ("untouched", "a compare-exchange passes its values on without its handler running"),
        ("lost-value", "a value dropped by a splitter -- the Exit wave still ends the run, short"),
        ("dup-exit", "a joiner sends an Exit too many"),
    ):
        expect_rejected(bs, plant, what, because="checksum ", params=bs_params)
    for plant, what, needle in (
        ("stride", "a CONSISTENT misroute (a partition splitter pairing neighbours): every count "
                   "right, the sums reject it", "every count as the reference says: yes"),
        ("swap", "a compare-exchange that orders its pair the wrong way: the network still sorts, "
                 "only the downstream tallies reject it", " 0 descents"),
    ):
        rc, doc, err = run_subject(bs, plant, params=bs_params, extra=["--no-pin"])
        fails = (doc or {}).get("failures") or []
        if (rc == 1 and doc and doc.get("verified") is False and fails
                and fails[0].startswith("repetition 0: checksum ") and not doc.get("work_ns")
                and needle in err):
            verdict("CAUGHT", what, fails[0][:100])
        else:
            verdict("MISSED", f"{plant}: not rejected by the tallies with {needle.strip()!r}",
                    f"rc={rc}, failures={fails}, err={err.strip()[-200:]}")
    expect_rejected(bs, "dup-data", "a stamped value handled twice by a joiner: its lane "
                                    "overflows, a named failure (qvo::fail)",
                    because="savina/bitonicsort: a joiner received more values", params=bs_params)
    expect_blocked(bs,
        plant="lost-exit",
        what="a compare-exchange's Exit never delivered: its joiner never "
                                    "closes, the deadline fails the run",
        benchmark='control/bitonicsort',
        params=bs_params)
    rc, doc, err = run_subject(bs, "none", params=["n=1000", "cores=1", "wait=1"],
                               extra=["--no-pin"])
    if rc == 2 and doc is None and "qvo: FATAL: savina/bitonicsort: need" in err:
        verdict("CAUGHT", "n not a power of two is refused (exit 2) before anything runs")
    else:
        verdict("MISSED", "n not a power of two was not refused",
                f"rc={rc}, document={'none' if doc is None else 'present'}, "
                f"err={err.strip()[:100]}")

    set_case_group("T")
    print("\n== T. savina/apsp -- every block's phases, tiles and witness against the reference ==")
    # The grid simulated on one thread through the spec's own Block (Huly QB-247). A tile names its
    # generation, a block holds the two its neighbours can be at, and every phase folds its tile's
    # sum into a witness: the final matrix alone cannot see a thrown-away phase.
    shape_controls(subject("qvoctl-apsp-subject"),
        benchmark="control/apsp",
        params={"nodes": 300, "block": 50, "weight": 100, "form": 0, "cores": 1, "wait": 1},
        expected_messages=108072,
        rejected=[("duplicate", "a tile delivered twice", "twice"),
         ("misroute", "a tile delivered outside its sender's row and column",
          "not in its row or its column"),
         ("misroute-row", "a tile delivered to another block of its sender's row", "twice"),
         ("far-ahead", "a tile tagged two generations ahead", "one generation ahead, never more"),
         ("early", "a phase computed one tile short", "a phase that ends early")],
        blind=("skip-change", "a phase thrown away (282 distances): every count and the final matrix "
                        "right, the witness rejects it",
         ["every block's phases and tiles as the closed form says: yes",
          "the final matrix as the reference says: yes"]),
        refused=({"block": 7}, "qvo: FATAL: savina/apsp: need"),
        observed="ahead")

    for framework in ("qb", "baseline"):
        set_case_group(f"T-{framework}")
        adapter_controls(
            subject(f"qvoctl-{framework}-apsp"), framework=framework, benchmark="savina/apsp",
            params={"nodes": 300, "block": 50, "weight": 100, "form": 0, "cores": 1, "wait": 1},
            expected_messages=108072,
            rejected=[("duplicate", "a tile delivered twice", "twice"),
                      ("misroute", "a tile delivered outside its sender's row and column",
                       "not in its row or its column"),
                      ("misroute-row", "a tile delivered to another block in its sender's row",
                       "twice"),
                      ("far-ahead", "a tile tagged two generations ahead",
                       "one generation ahead, never more"),
                      ("early", "a phase computed one tile short", "a phase that ends early")],
            blind=("skip-change", "a changed phase discarded: counts and the final matrix right",
                   ["every block's phases and tiles as the closed form says: yes",
                    "the final matrix as the reference says: yes"]), observed="ahead")

    set_case_group("U")
    print("\n== U. savina/sor -- every cell's updates, receipts and witness against the reference ==")
    # The grid and its two owners simulated on one thread through the spec's own Cell and Owner
    # (Huly QB-247), at Savina's own size: fixed-point values, so the answer is exact in any order,
    # and a witness per cell, because the relaxation erases an error before the end.
    shape_controls(subject("qvoctl-sor-subject"),
        benchmark="control/sor",
        params={"size": 20, "iterations": 100, "cores": 1, "wait": 1},
        expected_messages=152042,
        rejected=[("duplicate", "a value delivered twice", "twice"),
         ("misroute", "a value delivered to a cell that is not its sender's neighbour",
          "not its neighbour"),
         ("wrong-round", "a value tagged with the next generation", "waiting for generation")],
        blind=("off-by-one", "a value one unit off at one update: every count and every final value "
                       "right, the witnesses reject it",
         ["every cell's updates and receipts as the closed form says: yes",
          "every final value as the reference says: yes"]),
        refused=({"size": 1}, "qvo: FATAL: savina/sor: need"))

    for framework in ("qb", "baseline"):
        set_case_group(f"U-{framework}")
        adapter_controls(
            subject(f"qvoctl-{framework}-sor"), framework=framework, benchmark="savina/sor",
            params={"size": 20, "iterations": 100, "cores": 1, "wait": 1},
            expected_messages=152042,
            rejected=[("duplicate", "a value delivered twice", "twice"),
                      ("misroute", "a value delivered outside its sender's neighbours",
                       "not its neighbour"),
                      ("wrong-round", "a value tagged with the next generation",
                       "waiting for generation")],
            blind=("off-by-16", "a published value 16 fixed-point units off: counts and final values right",
                   ["every cell's updates and receipts as the closed form says: yes",
                    "every final value as the reference says: yes"]),
            blind_params={"size": 4, "iterations": 100, "cores": 1, "wait": 1})

    set_case_group("V")
    print("\n== V. savina/uct -- the tree, node by node: every node's report against the plan ==")
    # The tree grows inside the window, by grants the root decides from each node's identity and
    # a cap the spec places before the run (Huly QB-247). The subject runs the protocol on one
    # thread through the spec's decide(), Tally and checksum(), the adapters' protocol checks
    # included. A fault the sums see must fail the checksum; a fault the protocol sees first must
    # fail as a protocol violation; a delivery left uncounted must fail the message count.
    uct_params = ["nodes=2000", "cores=1", "wait=1"]
    rc, doc, err = run_subject(uct, "none", params=uct_params, extra=["--no-pin"])
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1 \
            and doc.get("expected_messages", 0) > 0:
        verdict("CONFIRMED", "a correct tree verifies: every node's report as the plan says, "
                             f"{doc.get('expected_messages')} deliveries counted")
    else:
        verdict("MISSED", "a correct tree was REJECTED",
                f"rc={rc}, failures={(doc or {}).get('failures')}, err={err.strip()[:100]}")
    for plant, what, because, params in (
        ("misroute-grant", "a grant delivered to a sibling the plan never grants", "checksum ",
         uct_params),
        ("wrong-comp", "a grant carrying another computation size", "checksum ", uct_params),
        ("lost-update", "an UpdateGrant lost -- it blocks nothing", "checksum ", uct_params),
        ("skip-work", "a traversal that skips its busy work", "checksum ",
         ["nodes=2000", "work=1", "cores=1", "wait=1"]),
        ("untouched", "a leaf that reports without its handler counting the Traverse",
         "delivered ", uct_params),
        ("duplicate-grant", "a grant delivered twice", "protocol violated", uct_params),
        ("duplicate-request", "a request delivered twice", "protocol violated", uct_params),
        ("duplicate-traverse", "a Traverse delivered twice", "protocol violated", uct_params),
    ):
        expect_rejected(uct, plant, what, because=because, params=params)
    rc, doc, err = run_subject(uct, "none", params=["nodes=5", "cores=1", "wait=1"],
                               extra=["--no-pin"])
    if rc == 2 and doc is None and "qvo: FATAL: savina/uct: need" in err:
        verdict("CAUGHT", "a cap below the root's first generation is refused (exit 2) before "
                          "anything runs")
    else:
        verdict("MISSED", "a cap below the root's first generation was not refused",
                f"rc={rc}, document={'none' if doc is None else 'present'}, "
                f"err={err.strip()[:100]}")

    set_case_group("W")
    print("\n== W. savina/facloc -- the quadtree: every quadrant's ConfirmExit against the "
          "reference ==")
    # The quadtree grows inside the window where the customers fall; the spec's reference fixes
    # which quadrants split and which customers each of the others holds (Huly QB-247).
    fl_params = ["points=20000", "cores=1", "wait=1"]
    rc, doc, err = run_subject(facloc, "none", params=fl_params, extra=["--no-pin"])
    if rc == 0 and doc and doc.get("verified") is True and len(doc.get("work_ns", [])) == 1 \
            and doc.get("expected_messages", 0) > 0:
        verdict("CONFIRMED", "a correct quadtree verifies: every quadrant's ConfirmExit as the "
                             f"reference says, {doc.get('expected_messages')} deliveries counted")
    else:
        verdict("MISSED", "a correct quadtree was REJECTED",
                f"rc={rc}, failures={(doc or {}).get('failures')}, err={err.strip()[:100]}")
    for plant, what, because in (
        ("misroute-customer", "a customer routed to a sibling of the child whose region holds it",
         "checksum "),
        ("duplicate-customer", "a customer delivered twice below the root", "checksum "),
        ("lost-customer", "a customer lost below the root -- the producer's flow control sees "
                          "only the root", "checksum "),
        ("no-split", "a quadrant that never splits, whatever its cost says", "checksum "),
        ("lost-facility", "a Facility notification lost on its way up", "checksum "),
        ("untouched", "a quadrant that passes a customer on without counting it", "delivered "),
        ("duplicate-exit", "a RequestExit delivered twice", "protocol violated"),
    ):
        expect_rejected(facloc, plant, what, because=because, params=fl_params)
    rc, doc, err = run_subject(facloc, "none", params=["points=0", "cores=1", "wait=1"],
                               extra=["--no-pin"])
    if rc == 2 and doc is None and "qvo: FATAL: savina/facloc: need" in err:
        verdict("CAUGHT", "a run of no customer is refused (exit 2) before anything runs")
    else:
        verdict("MISSED", "a run of no customer was not refused",
                f"rc={rc}, document={'none' if doc is None else 'present'}, "
                f"err={err.strip()[:100]}")

    print("\n== Mesh owner dispatch: functional causal control, no timed workload ==")
    mesh_controls(subject("qvoctl-mesh-dispatch-subject"),
                  subject("qvoctl-mesh-dispatch-reentrant"))

    print(f"\n== census ==\n  CAUGHT={caught} CONFIRMED={confirmed} MISSED={missed} "
          f"(floors: CAUGHT>={FLOOR_CAUGHT} CONFIRMED>={FLOOR_CONFIRMED})")

    if args.census_out is not None:
        groups = {}
        for case in case_verdicts:
            counts = groups.setdefault(case["group"], {"CAUGHT": 0, "CONFIRMED": 0, "MISSED": 0})
            counts[case["verdict"]] += 1
        args.census_out.parent.mkdir(parents=True, exist_ok=True)
        args.census_out.write_text(json.dumps({
            "complete": True,
            "successful": missed == 0 and caught >= FLOOR_CAUGHT and confirmed >= FLOOR_CONFIRMED,
            "executed_verdicts": len(case_verdicts),
            "counts": {"CAUGHT": caught, "CONFIRMED": confirmed, "MISSED": missed},
            "recorded_base_floors": {"CAUGHT": FLOOR_CAUGHT, "CONFIRMED": FLOOR_CONFIRMED},
            "groups": groups,
            "cases": case_verdicts,
        }, indent=2) + "\n")

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
