#!/usr/bin/env python3
"""The roster cross-check: every spec has an implementation in every framework, both ways.

Three surfaces have to agree about WHAT this repository measures, and until this guard existed
nothing made them: the spec headers under `benchmarks/specs/qvospec/<suite>/`, the adapter
sources under `frameworks/<framework>/<suite>/`, and — when a results directory is given — the
documents `tools/run.py` wrote. A framework silently missing from a benchmark is invisible to
every one of them on its own: CMake globs whatever sources exist, `run.py` measures whatever
binaries it finds, and `report.py` renders whatever documents it reads. The trap is a table that
looks complete because nothing knew it was not.

    Forward   for every spec `<suite>/<bench>.h`, every framework has `<suite>/<bench>.cpp`,
              unless the framework's CMakeLists.txt DECLARES the omission with a reason
              (`# qvo-roster-omit: <suite>/<bench> [...] -- <why>`). An omission naming no spec
              is an error, so a retired benchmark cannot leave a stale excuse behind.
    Backward  every adapter source names a spec that exists, `#include`s exactly that spec header
              (so it cannot compute its own expected value), and the spec's `kId` is the path it
              lives at.
    Docs      every spec has its `benchmarks/<suite>/<bench>.md`, and every such page has a spec.
    Results   with --results: every (benchmark, framework) the roster expects has one document
              per configuration in `<results>/<bench-slug>/<framework>__<cfg>.json`, and every
              document there names a roster cell. A cell the adapter declared not applicable
              counts — it is a document; what does not count is silence.

Floors: at least 3 frameworks, 4 specs and 4 configurations are expected in scope, and an
under-floor run exits 2 (inconclusive), never 0. Exit 1 is a finding.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPECS = ROOT / "benchmarks" / "specs" / "qvospec"
FRAMEWORKS = ROOT / "frameworks"
DOCS = ROOT / "benchmarks"
CONFIGS = ("1c-spin", "1c-park", "2c-spin", "2c-park")

FLOOR_FRAMEWORKS = 3
FLOOR_SPECS = 4

OMIT_RE = re.compile(r"^\s*#\s*qvo-roster-omit:\s*(?P<what>.+?)\s+--\s+(?P<why>\S.*)$")


def specs() -> dict[str, Path]:
    """`suite/bench` -> header, from the directory layout alone."""
    found = {}
    for h in sorted(SPECS.glob("*/*.h")):
        found[f"{h.parent.name}/{h.stem}"] = h
    return found


def frameworks() -> dict[str, Path]:
    """Every directory under frameworks/ with a CMakeLists.txt is a framework in the field."""
    return {d.name: d for d in sorted(FRAMEWORKS.iterdir())
            if d.is_dir() and (d / "CMakeLists.txt").is_file()}


def omissions(fw_dir: Path, findings: list[str]) -> dict[str, str]:
    """Declared omissions, `suite/bench` -> reason, read from the framework's CMakeLists.txt."""
    out = {}
    for line in (fw_dir / "CMakeLists.txt").read_text(encoding="utf-8").splitlines():
        m = OMIT_RE.match(line)
        if not m:
            continue
        for what in m.group("what").split():
            if what in out:
                findings.append(f"{fw_dir.name}/CMakeLists.txt declares the omission of {what} "
                                "twice")
            out[what] = m.group("why").strip()
    return out


VARIANT_RE = re.compile(r'^\s*#include\s+"(?P<rel>[^"]+\.cpp)"\s*$', re.M)


def adapter_text(src: Path, findings: list[str]) -> str:
    """The adapter's text, with a `#include "<other>.cpp"` variant followed to its source.

    frameworks/caf-detached mirrors frameworks/caf's adapter by including the .cpp under a
    define: one source, two binaries, so the two cannot drift. The spec binding is then in the
    included file, and that is where it is checked; a variant including a file that is not an
    adapter of the SAME benchmark is a finding, since the binary would answer for a benchmark its
    directory does not name.
    """
    text = src.read_text(encoding="utf-8")
    for m in VARIANT_RE.finditer(text):
        target = (src.parent / m.group("rel")).resolve()
        if not target.is_file():
            findings.append(f"{src.relative_to(ROOT).as_posix()}: includes {m.group('rel')}, "
                            "which does not exist")
            continue
        if target.name != src.name or target.parent.name != src.parent.name:
            findings.append(f"{src.relative_to(ROOT).as_posix()}: a variant of "
                            f"{m.group('rel')}, whose benchmark is not this file's")
        text += "\n" + target.read_text(encoding="utf-8")
    return text


def check_tree() -> tuple[list[str], dict[str, Path], dict[str, Path], dict[str, dict[str, str]]]:
    findings: list[str] = []
    sp = specs()
    fws = frameworks()
    omitted: dict[str, dict[str, str]] = {}

    # Backward: each spec's kId is its own path, and each has a doc page.
    for bench, h in sp.items():
        text = h.read_text(encoding="utf-8")
        m = re.search(r'kId\s*=\s*"([^"]+)"', text)
        if not m:
            findings.append(f"{h.relative_to(ROOT).as_posix()}: no `kId` declared")
        elif m.group(1) != bench:
            findings.append(f"{h.relative_to(ROOT).as_posix()}: kId is {m.group(1)!r}, but the "
                            f"header lives at {bench!r}")
        doc = DOCS / f"{bench}.md"
        if not doc.is_file():
            findings.append(f"{bench}: no benchmark page at {doc.relative_to(ROOT).as_posix()}")
    for page in sorted(DOCS.glob("*/*.md")):
        if page.parent.name == "specs":
            continue
        bench = f"{page.parent.name}/{page.stem}"
        if bench not in sp:
            findings.append(f"{page.relative_to(ROOT).as_posix()}: a page for a benchmark with no "
                            "spec header")

    for fw, d in fws.items():
        om = omissions(d, findings)
        omitted[fw] = om
        for what in om:
            if what not in sp:
                findings.append(f"{fw}/CMakeLists.txt omits {what!r}, which is not a spec -- "
                                "a stale excuse; remove it")
        # Forward: a source per spec, or a declared omission.
        for bench, h in sp.items():
            src = d / f"{bench}.cpp"
            if src.is_file():
                if bench in om:
                    findings.append(f"{fw}: {bench} is both implemented "
                                    f"({src.relative_to(ROOT).as_posix()}) and declared omitted")
                text = adapter_text(src, findings)
                want = f"#include <qvospec/{bench}.h>"
                if want not in text:
                    findings.append(f"{src.relative_to(ROOT).as_posix()}: does not `{want}` -- "
                                    "an adapter that does not bind to its spec can compute its "
                                    "own expected value")
                others = [b for b in sp if b != bench and f"#include <qvospec/{b}.h>" in text]
                if others:
                    findings.append(f"{src.relative_to(ROOT).as_posix()}: also includes the "
                                    f"spec of {', '.join(others)}")
            elif bench not in om:
                findings.append(f"{fw}: no implementation of {bench} "
                                f"({src.relative_to(ROOT).as_posix()} missing) and no "
                                "`# qvo-roster-omit:` declaring why")
        # Backward: every source is a spec.
        for src in sorted(d.glob("*/*.cpp")):
            bench = f"{src.parent.name}/{src.stem}"
            if bench not in sp:
                findings.append(f"{src.relative_to(ROOT).as_posix()}: an adapter for "
                                f"{bench!r}, which has no spec header")
    return findings, sp, fws, omitted


def check_results(results: Path, sp: dict[str, Path], fws: dict[str, Path],
                  omitted: dict[str, dict[str, str]], only: set[str] | None) -> tuple[list[str], int]:
    """Every roster cell has a document, and every document is a roster cell."""
    findings: list[str] = []
    expected = set()
    for bench in sp:
        for fw in fws:
            if only and fw not in only:
                continue
            if bench in omitted.get(fw, {}):
                continue
            for cfg in CONFIGS:
                expected.add((bench, fw, cfg))
    present = set()
    for f in sorted(results.glob("*/*.json")):
        if f.name == "run.json":
            continue
        m = re.fullmatch(r"(?P<fw>[a-z0-9-]+)__(?P<cfg>[12]c-(?:spin|park))\.json", f.name)
        if not m:
            continue  # side-experiment files carry other names; report.py has its own rule
        slug = f.parent.name
        bench = next((b for b in sp if b.replace("/", "-") == slug), None)
        if bench is None:
            continue  # a side-experiment directory (report.py names those on stderr)
        cell = (bench, m.group("fw"), m.group("cfg"))
        present.add(cell)
        if cell not in expected and (not only or m.group("fw") in only):
            findings.append(f"{f.relative_to(ROOT).as_posix()}: a document for a cell the roster "
                            f"does not expect ({cell[1]} on {cell[0]}) -- an adapter that was "
                            "removed or declared omitted after it was measured?")
    for cell in sorted(expected - present):
        bench, fw, cfg = cell
        findings.append(f"{results.relative_to(ROOT).as_posix()}: no document for {fw} on "
                        f"{bench} at {cfg} -- the framework is silently missing from that table")
    return findings, len(expected)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", type=Path, action="append", default=[],
                    help="a results directory to cross-check against the roster (repeatable)")
    ap.add_argument("--only", default=None,
                    help="comma-separated frameworks the results directories are expected to "
                         "hold (a partial run); default: every framework")
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None

    findings, sp, fws, omitted = check_tree()
    n_om = sum(len(v) for v in omitted.values())
    print(f"check-roster: {len(sp)} specs x {len(fws)} frameworks, {n_om} declared omission(s)")
    for fw, om in omitted.items():
        for what, why in om.items():
            print(f"  omitted: {fw} on {what} -- {why}")

    cells = 0
    for r in args.results:
        if not r.is_dir():
            # A results directory named on the command line and absent is a finding, not an
            # inconclusive run: it is the shape of a host directory renamed or deleted while a
            # README marker, a REPORT.md or a CI invocation still names it.
            print(f"FAIL: {r.as_posix()}: not a directory")
            return 1
        f2, n = check_results(r.resolve(), sp, fws, omitted, only)
        findings += f2
        cells += n
        print(f"  results {r.as_posix()}: {n} roster cells expected")

    if len(fws) < FLOOR_FRAMEWORKS or len(sp) < FLOOR_SPECS:
        print(f"check-roster: INCONCLUSIVE -- {len(fws)} frameworks / {len(sp)} specs in scope, "
              f"floors {FLOOR_FRAMEWORKS} / {FLOOR_SPECS}; the roster is not the one this guard "
              "was written for", file=sys.stderr)
        return 2
    if args.results and cells == 0:
        print("check-roster: INCONCLUSIVE -- a results directory was given and zero cells were "
              "expected", file=sys.stderr)
        return 2
    if findings:
        for f in findings:
            print(f"FAIL: {f}")
        print(f"check-roster: {len(findings)} finding(s)")
        return 1
    print("check-roster: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
