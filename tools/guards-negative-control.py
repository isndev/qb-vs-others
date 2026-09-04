#!/usr/bin/env python3
"""Plant a defect at a time and assert the two DOCUMENT guards reject it.

`tools/negative-control.py` proves the harness rejects a wrong answer. This battery proves the
guards over the repository's OWN documents do: `tools/check-roster.py` (every spec has an adapter
in every framework, both ways, and every roster cell has a result document) and
`tools/check-report.py` (every REPORT.md is byte-identical to its render, every transcribed
README figure equals the JSON it summarises). A guard nobody has watched reject anything is a
claim, and the defects it exists for -- a framework silently missing from a table, a number typed
once and never re-read -- are precisely the ones that look like a passing run.

Same three verdicts as the harness battery:

    CAUGHT     a planted defect was rejected (exit 1), or an emptied scope refused to pass (exit 2)
    CONFIRMED  a shape that must NOT be reported was accepted (exit 0)
    MISSED     a planted defect was accepted, or a legitimate shape rejected

Every control plants in a `mkdtemp` COPY of the checkout (build/, .git/ and vcpkg_installed/
excluded) and runs the guard from inside that copy, so the guards' own `ROOT` is the sandbox; the
real checkout is hashed before and after, and any difference is a failure of this battery,
whatever the controls said. A control that plants nothing -- the needle it edits is absent -- is a
MISSED, not a CAUGHT: the guard was right to find nothing, and the control was wrong.

    python3 tools/guards-negative-control.py [--results results/<host>]

The CONFIRMED controls need a results directory that IS complete for the roster and a README
whose figures ARE current; the default is the WSL host, the first one measured for all five
benchmarks in one session. Floors are counted, and a run below them fails.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXCLUDE = {"build", ".git", "vcpkg_installed", "__pycache__", "alt"}

FLOOR_CAUGHT = 33
FLOOR_CONFIRMED = 3

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


# ----------------------------------------------------------------------------- sandbox

def tree_digest(root: Path) -> str:
    """One digest over every file the guards read or the controls edit, in the REAL checkout."""
    h = hashlib.sha256()
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root)
        if any(part in EXCLUDE or part.endswith(".log") for part in rel.parts):
            continue
        if p.is_file():
            h.update(rel.as_posix().encode())
            h.update(p.read_bytes())
    return h.hexdigest()


class Sandbox:
    """A fresh copy of the checkout per control; nothing here can reach the real tree."""

    def __init__(self, base: Path):
        self.base = base

    def __enter__(self) -> Path:
        self.dir = Path(tempfile.mkdtemp(prefix="qvo-guards-"))
        self.root = self.dir / "repo"
        shutil.copytree(self.base, self.root,
                        ignore=shutil.ignore_patterns(*EXCLUDE, "*.log"))
        return self.root

    def __exit__(self, *_):
        shutil.rmtree(self.dir, ignore_errors=True)


def run_guard(root: Path, name: str, *args: str) -> tuple[int, str]:
    p = subprocess.run([sys.executable, str(root / "tools" / name), *args],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       cwd=root, timeout=300)
    return p.returncode, p.stdout + p.stderr


def edit(path: Path, old: str, new: str, count: int = 1) -> bool:
    """Replace `old` in `path`; False if the needle is absent (the plant did not land)."""
    text = path.read_text(encoding="utf-8")
    if old not in text:
        return False
    path.write_text(text.replace(old, new, count), encoding="utf-8", newline="\n")
    return True


def expect(kind: str, rc: int, out: str, what: str, want_rc: int, needle: str = "") -> None:
    """kind='CAUGHT': the guard must exit want_rc (1 or 2) and, if given, print `needle`.
    kind='CONFIRMED': the guard must exit 0."""
    tail = "\n".join(out.strip().splitlines()[-4:])
    if kind == "CONFIRMED":
        if rc == 0:
            verdict("CONFIRMED", what)
        else:
            verdict("MISSED", what, f"exit {rc} on a legitimate shape:\n{tail}")
        return
    if rc != want_rc:
        verdict("MISSED", what, f"exit {rc}, wanted {want_rc}:\n{tail}")
    elif needle and needle not in out:
        verdict("MISSED", what, f"exit {rc} but for another reason -- {needle!r} not printed:\n{tail}")
    else:
        verdict("CAUGHT", what)


def botched(what: str, why: str) -> None:
    verdict("MISSED", what, f"the plant did not land: {why}")


# ----------------------------------------------------------------------------- roster controls

def roster_controls(base: Path, results: str) -> None:
    print("check-roster.py")
    R = "check-roster.py"
    res_args = ("--results", results)

    with Sandbox(base) as s:
        rc, out = run_guard(s, R, *res_args)
        expect("CONFIRMED", rc, out, f"clean tree + complete {results} is OK "
               "(the caf-detached variant include and the side-experiment directories included)", 0)

    with Sandbox(base) as s:
        (s / "frameworks/sobjectizer/savina/counting.cpp").unlink()
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "an adapter deleted with no declared omission", 1,
               "no implementation of savina/counting")

    with Sandbox(base) as s:
        p = s / "frameworks/caf-detached/CMakeLists.txt"
        p.write_text(p.read_text(encoding="utf-8")
                     + "# qvo-roster-omit: savina/retired -- it was measured once\n",
                     encoding="utf-8", newline="\n")
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "an omission naming a spec that does not exist (a stale excuse)", 1,
               "not a spec")

    with Sandbox(base) as s:
        p = s / "frameworks/caf-detached/CMakeLists.txt"
        p.write_text(p.read_text(encoding="utf-8")
                     + "# qvo-roster-omit: savina/ping-pong -- declared AND implemented\n",
                     encoding="utf-8", newline="\n")
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "a benchmark both implemented and declared omitted", 1,
               "both implemented")

    with Sandbox(base) as s:
        p = s / "frameworks/qb/savina/thread-ring.cpp"
        if not edit(p, "#include <qvospec/savina/thread-ring.h>", "// spec include removed"):
            botched("an adapter that does not include its spec", "no spec include in qb thread-ring")
        else:
            rc, out = run_guard(s, R)
            expect("CAUGHT", rc, out, "an adapter that does not include its spec "
                   "(it could compute its own expected value)", 1, "does not `#include <qvospec/")

    with Sandbox(base) as s:
        p = s / "frameworks/qb/savina/thread-ring.cpp"
        if not edit(p, "#include <qvospec/savina/thread-ring.h>",
                    "#include <qvospec/savina/thread-ring.h>\n#include <qvospec/savina/counting.h>"):
            botched("an adapter including a second spec", "no spec include in qb thread-ring")
        else:
            rc, out = run_guard(s, R)
            expect("CAUGHT", rc, out, "an adapter that also includes another benchmark's spec", 1,
                   "also includes the spec of savina/counting")

    with Sandbox(base) as s:
        p = s / "benchmarks/specs/qvospec/savina/big.h"
        if not edit(p, 'kId = "savina/big"', 'kId = "savina/large"'):
            botched("a spec whose kId is not its path", "kId literal not found in big.h")
        else:
            rc, out = run_guard(s, R)
            expect("CAUGHT", rc, out, "a spec whose kId disagrees with the path it lives at", 1,
                   "kId is 'savina/large'")

    with Sandbox(base) as s:
        (s / "benchmarks/savina/fork-join.md").unlink()
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "a spec with no benchmark page", 1, "no benchmark page")

    with Sandbox(base) as s:
        (s / "benchmarks/savina/ghost.md").write_text("# savina/ghost\n", encoding="utf-8")
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "a benchmark page with no spec header", 1, "no spec header")

    with Sandbox(base) as s:
        (s / "frameworks/qb/savina/ghost.cpp").write_text("// an adapter for nothing\n",
                                                          encoding="utf-8")
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "an adapter for a benchmark with no spec", 1,
               "which has no spec header")

    with Sandbox(base) as s:
        p = s / "frameworks/caf-detached/savina/ping-pong.cpp"
        if not edit(p, '#include "../../caf/savina/ping-pong.cpp"',
                    '#include "../../caf/savina/big.cpp"'):
            botched("a variant including another benchmark's adapter", "variant include not found")
        else:
            rc, out = run_guard(s, R)
            expect("CAUGHT", rc, out, "a variant that includes the adapter of a DIFFERENT benchmark "
                   "(the binary would answer for a benchmark its directory does not name)", 1,
                   "whose benchmark is not this file's")

    with Sandbox(base) as s:
        docs = sorted((s / results / "savina-counting").glob("qb__*.json"))
        if not docs:
            botched("a result document missing from a roster cell", f"no qb counting docs in {results}")
        else:
            docs[0].unlink()
            rc, out = run_guard(s, R, *res_args)
            expect("CAUGHT", rc, out, "one result document missing from a roster cell "
                   "(a framework silently absent from one row of one table)", 1,
                   "no document for qb on savina/counting")

    with Sandbox(base) as s:
        src = s / results / "savina-counting" / "qb__1c-spin.json"
        if not src.is_file():
            botched("a document for a framework the roster does not know", f"{src.name} absent")
        else:
            shutil.copy(src, src.parent / "ghost__1c-spin.json")
            rc, out = run_guard(s, R, *res_args)
            expect("CAUGHT", rc, out, "a document for a framework the roster does not know", 1,
                   "a cell the roster does not expect")

    with Sandbox(base) as s:
        p = s / "frameworks/caf-detached/CMakeLists.txt"
        if not edit(p, "# qvo-roster-omit: savina/counting", "# qvo-roster-omit: savina/big savina/counting"):
            botched("a duplicated omission", "the caf-detached omission line was not found")
        else:
            rc, out = run_guard(s, R)
            expect("CAUGHT", rc, out, "the same omission declared twice", 1, "twice")

    with Sandbox(base) as s:
        for h in (s / "benchmarks/specs/qvospec/savina").glob("*.h"):
            if h.stem != "ping-pong":
                h.unlink()
        for d in (s / "benchmarks/savina").glob("*.md"):
            if d.stem != "ping-pong":
                d.unlink()
        for f in (s / "frameworks").glob("*/savina/*.cpp"):
            if f.stem != "ping-pong":
                f.unlink()
        p = s / "frameworks/caf-detached/CMakeLists.txt"
        p.write_text("\n".join(l for l in p.read_text(encoding="utf-8").splitlines()
                               if "qvo-roster-omit" not in l) + "\n", encoding="utf-8", newline="\n")
        rc, out = run_guard(s, R)
        expect("CAUGHT", rc, out, "a roster shrunk below the spec floor is INCONCLUSIVE (exit 2), "
               "never a pass", 2, "INCONCLUSIVE")

    with Sandbox(base) as s:
        rc, out = run_guard(s, R, "--results", "results/does-not-exist")
        expect("CAUGHT", rc, out, "a results directory that does not exist", 1, "not a directory")

    with Sandbox(base) as s:
        rc, out = run_guard(s, R, *res_args, "--only", "nobody")
        expect("CAUGHT", rc, out, "--only naming no framework expects zero cells: INCONCLUSIVE, "
               "not a pass", 2, "zero cells")


# ----------------------------------------------------------------------------- report controls

def first_marked_row(readme: Path, results: str) -> tuple[int, str] | None:
    """(line index, line) of the first data row under the marker naming `results`."""
    lines = readme.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if f"check-report: {results}" in line:
            j = i + 1
            while j < len(lines) and not lines[j].startswith("|"):
                j += 1
            j += 2  # header + separator
            if j < len(lines) and lines[j].startswith("|"):
                return j, lines[j]
    return None


def rewrite_line(readme: Path, idx: int, new: str) -> None:
    lines = readme.read_text(encoding="utf-8").splitlines()
    lines[idx] = new
    readme.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def report_controls(base: Path, results: str) -> None:
    print("check-report.py")
    P = "check-report.py"
    host = Path(results).name

    with Sandbox(base) as s:
        rc, out = run_guard(s, P)
        expect("CONFIRMED", rc, out, "every REPORT.md byte-identical to its render and every "
               "transcribed README figure equal to its JSON", 0)

    with Sandbox(base) as s:
        rc, out = run_guard(s, P, "--write")
        rc2, out2 = run_guard(s, P)
        if rc != 0:
            verdict("MISSED", "--write regenerates every report", f"--write exited {rc}")
        else:
            expect("CONFIRMED", rc2, out2, "--write regenerates reports the check then accepts "
                   "(the renderer is deterministic)", 0)

    with Sandbox(base) as s:
        rep = s / "REPORT.md"
        # report.py prints nanoseconds with no decimals (`308 ns`) and microseconds with two;
        # the first run of this battery against a real report found this control written for a
        # `N.NN ns` the renderer never emits, so it planted nothing -- a MISSED, as it should be.
        m = re.search(r"\b(\d+) ns\b", rep.read_text(encoding="utf-8"))
        if not m:
            botched("a figure edited in the generated REPORT.md", "no `N ns` figure in REPORT.md")
        else:
            edit(rep, m.group(0), f"{int(m.group(1)) + 1} ns")
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "one figure edited by hand in the generated REPORT.md", 1,
                   "REPORT.md: differs from")

    with Sandbox(base) as s:
        (s / results / "REPORT.md").unlink()
        rc, out = run_guard(s, P)
        expect("CAUGHT", rc, out, "a per-host REPORT.md missing", 1, "missing -- run with --write")

    with Sandbox(base) as s:
        rep = s / "REPORT.md"
        rep.write_text(rep.read_text(encoding="utf-8") + "\nOne more sentence.\n",
                       encoding="utf-8", newline="\n")
        rc, out = run_guard(s, P)
        expect("CAUGHT", rc, out, "a sentence appended to a generated report (not only figures)", 1,
               "REPORT.md: differs from")

    with Sandbox(base) as s:
        # A JSON re-measured with the report and README left behind: both surfaces go stale.
        doc = s / results / "savina-ping-pong" / "qb__1c-spin.json"
        if not doc.is_file():
            botched("a re-measured JSON with stale documents", f"{doc.name} absent")
        else:
            d = json.loads(doc.read_text(encoding="utf-8"))
            d["work_ns"] = [int(x * 1.5) for x in d["work_ns"]]
            d["summary"] = {k: (v * 1.5 if isinstance(v, (int, float)) else v)
                            for k, v in d["summary"].items()}
            doc.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
            rc, out = run_guard(s, P)
            ok = rc == 1 and "REPORT.md: differs" in out and "the results say qb =" in out
            if ok:
                verdict("CAUGHT", "a JSON re-measured with REPORT.md and README.md left behind: "
                        "BOTH the report and the transcribed figure are reported stale")
            else:
                verdict("MISSED", "a JSON re-measured with stale documents",
                        f"exit {rc}:\n" + "\n".join(out.splitlines()[-5:]))

    readme = base / "README.md"
    row = first_marked_row(readme, results)
    if row is None:
        botched("README transcription controls", f"no marked table for {results} in README.md")
        return
    idx, line = row

    m = re.search(r"\b(\d+) ns\b", line)
    with Sandbox(base) as s:
        if not m:
            botched("a transcribed time off by one", "no `N ns` figure in the first marked row")
        else:
            rewrite_line(s / "README.md", idx, line.replace(m.group(0), f"{int(m.group(1)) + 1} ns", 1))
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a transcribed README time off by one nanosecond", 1,
                   "the results say")

    with Sandbox(base) as s:
        if not m:
            botched("a transcribed time in the wrong unit", "no `N ns` figure in the first marked row")
        else:
            rewrite_line(s / "README.md", idx, line.replace(m.group(0), f"{m.group(1)} us", 1))
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a transcribed README time in the wrong unit (ns typed as us)",
                   1, "the results say")

    mx = re.search(r"\b([A-Za-z-]+) (\d+\.\d\d)[x×]", line)
    with Sandbox(base) as s:
        if not mx:
            botched("a ratio off by a hundredth", "no `<fw> N.NNx` item in the first marked row")
        else:
            new = line.replace(mx.group(0), f"{mx.group(1)} {float(mx.group(2)) + 0.01:.2f}x", 1)
            rewrite_line(s / "README.md", idx, new)
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a transcribed ratio off by a hundredth", 1, "the results say")

    with Sandbox(base) as s:
        if not mx:
            botched("a ratio attributed to the wrong framework", "no ratio item in the first marked row")
        else:
            other = "CAF" if mx.group(1).lower() != "caf" else "qb"
            new = line.replace(mx.group(0), f"{other} {mx.group(2)}x", 1)
            rewrite_line(s / "README.md", idx, new)
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a ratio attributed to a framework that is not the row's "
                   "fastest (a more flattering denominator)", 1, "but the fastest verified framework")

    with Sandbox(base) as s:
        cells = line.split("|")
        cells[2] = " Seastar 1 ns · " + cells[2].strip() + " "
        rewrite_line(s / "README.md", idx, "|".join(cells))
        rc, out = run_guard(s, P)
        expect("CAUGHT", rc, out, "a figure for a framework with no document in that cell", 1,
               "no known framework")

    with Sandbox(base) as s:
        p = s / "README.md"
        if not edit(p, f"<!-- check-report: {results}", f"<!-- checked: {results}"):
            botched("a table whose marker was removed", "marker not found")
        else:
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a configuration table whose marker was removed -- its "
                   "figures are verified by nobody", 1, "with no `<!-- check-report:")

    with Sandbox(base) as s:
        p = s / "README.md"
        if not edit(p, f"<!-- check-report: {results}", "<!-- check-report: results/nowhere"):
            botched("a marker naming a results directory that does not exist", "marker not found")
        else:
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a marker naming a results directory that does not exist", 1,
                   "which does not exist")

    with Sandbox(base) as s:
        p = s / "README.md"
        if not edit(p, f"<!-- check-report: {results} benchmark=savina/ping-pong -->",
                    f"<!-- check-report: {results} benchmark=savina/ghost -->"):
            botched("a marker naming a benchmark with no results", "ping-pong marker not found")
        else:
            rc, out = run_guard(s, P)
            expect("CAUGHT", rc, out, "a marker naming a benchmark that has no results there", 1,
                   "no results for savina/ghost")

    with Sandbox(base) as s:
        p = s / "README.md"
        text = p.read_text(encoding="utf-8")
        text = re.sub(r"<!-- check-report:[^\n]*-->\n(?:\|[^\n]*\n)+", "", text)
        p.write_text(text, encoding="utf-8", newline="\n")
        rc, out = run_guard(s, P)
        expect("CAUGHT", rc, out, "every marked table removed from README.md: INCONCLUSIVE "
               "(exit 2), never a pass over zero figures", 2, "INCONCLUSIVE")

    # The one-framework GRID (`framework=qb`, rows = benchmarks, columns = configurations): the
    # shape the candidate branch is transcribed in. Its figures are verified against the grid's
    # OWN directory, so a candidate number cannot borrow a shipped cell's document, or the reverse.
    grid = first_marked_row(readme, f"{results}/qb-branch-perf-core-hot-path")
    if grid is None:
        botched("framework= grid controls", "no `framework=` grid under the branch directory")
    else:
        gidx, gline = grid
        gm = re.search(r"\b(\d+) ns\b", gline)
        with Sandbox(base) as s:
            if not gm:
                botched("a grid figure off by one", "no `N ns` figure in the grid's first row")
            else:
                rewrite_line(s / "README.md", gidx,
                             gline.replace(gm.group(0), f"{int(gm.group(1)) + 1} ns", 1))
                rc, out = run_guard(s, P)
                expect("CAUGHT", rc, out, "a candidate-branch grid figure off by one nanosecond, "
                       "verified against the grid's own directory", 1, "the results say")
        with Sandbox(base) as s:
            if not gm:
                botched("a grid item naming another framework", "no figure in the grid's first row")
            else:
                rewrite_line(s / "README.md", gidx, gline.replace(gm.group(0), f"CAF {gm.group(0)}", 1))
                rc, out = run_guard(s, P)
                expect("CAUGHT", rc, out, "a grid item naming a framework the marker did not fix",
                       1, "names no known framework")
        with Sandbox(base) as s:
            p = s / "README.md"
            text = p.read_text(encoding="utf-8")
            marker = f"<!-- check-report: {results}/qb-branch-perf-core-hot-path"
            at = text.find(marker)
            if at < 0:
                botched("a grid column that is not a configuration", "grid marker not found")
            else:
                head = text.find("| 1 core, spin |", at)
                if head < 0:
                    botched("a grid column that is not a configuration", "grid header not found")
                else:
                    text = text[:head] + "| 1 core, warm |" + text[head + len("| 1 core, spin |"):]
                    p.write_text(text, encoding="utf-8", newline="\n")
                    rc, out = run_guard(s, P)
                    expect("CAUGHT", rc, out, "a grid column header that is not a configuration",
                           1, "is not a configuration")

    with Sandbox(base) as s:
        # The primary host's results gone: the root REPORT.md has nothing to be rendered from.
        shutil.rmtree(s / "results" / "desktop-b67osn6-win-msvc", ignore_errors=True)
        rc, out = run_guard(s, P)
        expect("CAUGHT", rc, out, "the primary host's results directory absent", 1,
               "does not exist")


# ----------------------------------------------------------------------------- main

def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results/wsl-debian-g++14",
                    help="a results directory complete for the roster, whose README table is "
                         "current (the CONFIRMED controls' subject)")
    args = ap.parse_args()
    if not (ROOT / args.results / "run.json").is_file():
        print(f"guards-negative-control: {args.results} has no run.json", file=sys.stderr)
        return 2

    before = tree_digest(ROOT)
    roster_controls(ROOT, args.results)
    report_controls(ROOT, args.results)
    after = tree_digest(ROOT)

    print(f"\n    CAUGHT={caught} CONFIRMED={confirmed} MISSED={missed}")
    if before != after:
        print("guards-negative-control: the REAL checkout changed during the run -- a control "
              "wrote outside its sandbox, or someone else edited the tree meanwhile", file=sys.stderr)
        return 1
    print("    real checkout: unchanged (hashed before and after)")
    if missed:
        return 1
    if caught < FLOOR_CAUGHT or confirmed < FLOOR_CONFIRMED:
        print(f"guards-negative-control: below floors ({FLOOR_CAUGHT}/{FLOOR_CONFIRMED}) -- a "
              "control stopped firing", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
