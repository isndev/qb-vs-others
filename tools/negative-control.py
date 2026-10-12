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

# Floors. Raise them in the same commit that adds a control; a count that goes DOWN is a control
# that stopped firing, which is indistinguishable from a guard that stopped guarding.
FLOOR_CAUGHT = 21
FLOOR_CONFIRMED = 10

# The deadline controls (section I): a planted hang must end inside DEADLINE_S plus this slack --
# process start and the document write, generous for a loaded host -- and the battery gives up on
# the child at HANG_WAIT_S, which is a MISSED, never a hang of the battery itself.
DEADLINE_S = 2
DEADLINE_SLACK_S = 30
HANG_WAIT_S = 120

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


def comparable(doc: dict) -> dict:
    """The document minus what legitimately differs between two launches: the clock (env.utc)
    and the timings. Everything else must be byte-for-byte the same."""
    d = json.loads(json.dumps(doc))
    d.get("env", {}).pop("utc", None)
    for key in ("work_ns", "total_ns", "outside_window_ns"):
        d[key] = len(d.get(key, []))
    d["summary"] = sorted(d.get("summary", {}))
    return d


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
