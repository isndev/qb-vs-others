#!/usr/bin/env python3
"""Assert that no hand-written figure has drifted from the JSON it claims to summarise.

Two surfaces carry numbers, and they are guarded differently.

  1. THE GENERATED REPORTS. `REPORT.md` at the root and `results/<host>/REPORT.md` under every
     host are `tools/report.py` output and nothing else. This guard re-renders each in memory and
     fails on ANY byte of difference -- the same shape as qb's `gen-llms-txt.py --check` -- so an
     edited number, an edited sentence, or a report left stale after a re-run is a red check,
     not a discrepancy for a reader to notice. The root report is the PRIMARY host's.

  2. THE TRANSCRIBED TABLES. README.md summarises the reports by hand, and a summary that is
     typed can be typed wrong or left behind. Every such table is preceded by a marker naming
     the results it summarises:

         <!-- check-report: results/<host> [benchmark=<suite/name>] [cfg=<Nc-spin|park>] -->
         | configuration | fastest | second | floor | the rest |
         | 1 core, spin  | **qb 114 ns** | SObjectizer 182 ns | 2 ns | CAF 483 ns . qb 1.60x |

     Each cell is split on the middle dot; each item is `<framework> <figure>` -- a per-unit
     time, which must equal `report.py`'s own formatting of that cell's median, or a `<n>x`
     multiple, which must equal the runner-up's median over the named (fastest) framework's to
     two decimals -- report.py's own "N.NNx faster than" sentence, no other denominator. A row's
     first cell is a configuration (`1 core, spin`) or a benchmark (`counting`), whichever the
     marker did not fix. A marker may instead fix `framework=<fw>`: then the rows are benchmarks,
     the COLUMNS are configurations, and every item is that framework's figure for its
     (row, column) cell -- the shape of a one-framework grid such as a candidate build measured
     beside the shipped one. Items about a BIMODAL cell, `n/a` cells and dashes are skipped: they
     are prose about a distribution, not a figure. A table whose header starts with `configuration`
     or `benchmark` and carries NO marker is a finding, so a new hand-written table cannot opt out
     by omission.

     docs/TUNING.md is deliberately NOT parsed: its figures come from side experiments
     (`qb-branch-perf-core-hot-path/`, `caf-spin-sweep/`, ad-hoc A/B runs) whose documents are not
     cells of a published table, and a guard that pretended to verify them would verify nothing.
     Its subsections name the directory each number came from; that is its provenance.

Floors: at least 2 marked tables and 16 figures verified, else exit 2 (inconclusive). Exit 1 is a
finding; 0 is clean. `--write` regenerates every report in place (the reports only -- README.md
is edited by a person, and this guard tells that person what to fix).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402  (tools/report.py -- the renderer itself, so the two cannot disagree)

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
PRIMARY = "desktop-win11-msvc19"  # the host whose report is the root REPORT.md

FLOOR_TABLES = 2
FLOOR_FIGURES = 16

NAMES = {
    "qb": "qb", "caf": "caf", "caf-detached": "caf-detached", "sobjectizer": "sobjectizer",
    "floor": report.FLOOR, "baseline": report.FLOOR,
}
CFG_LABELS = {
    "1 core, spin": "1c-spin", "1 core, park": "1c-park",
    "2 cores, spin": "2c-spin", "2 cores, park": "2c-park",
}
MARKER_RE = re.compile(r"<!--\s*check-report:\s*(?P<dir>\S+)(?P<kv>(?:\s+\w+=\S+)*)\s*-->")
ITEM_RE = re.compile(r"^(?:(?P<name>[A-Za-z][\w-]*)\s+)?~?(?P<num>\d[\d,]*(?:\.\d+)?)\s*"
                     r"(?P<unit>ns|us|µs|ms|×|x)$")


def hosts() -> list[Path]:
    return sorted(d for d in RESULTS.iterdir() if d.is_dir() and (d / "run.json").is_file())


def check_reports(write: bool) -> tuple[list[str], int]:
    findings = []
    n = 0
    pairs = [(h, h / "REPORT.md") for h in hosts()]
    pairs.append((RESULTS / PRIMARY, ROOT / "REPORT.md"))
    for results, path in pairs:
        if not results.is_dir():
            findings.append(f"{path.relative_to(ROOT).as_posix()}: its results directory "
                            f"{results.relative_to(ROOT).as_posix()} does not exist")
            continue
        want = report.render(results, path)
        n += 1
        if write:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(want)
            print(f"  wrote {path.relative_to(ROOT).as_posix()}")
            continue
        if not path.is_file():
            findings.append(f"{path.relative_to(ROOT).as_posix()}: missing -- run with --write")
            continue
        have = path.read_bytes()
        if have != want.encode("utf-8"):
            hl, wl = have.decode("utf-8", "replace").splitlines(), want.splitlines()
            at = next((i for i, (a, b) in enumerate(zip(hl, wl)) if a != b),
                      min(len(hl), len(wl)))
            findings.append(f"{path.relative_to(ROOT).as_posix()}: differs from "
                            f"`report.py --results {results.relative_to(ROOT).as_posix()}` at "
                            f"line {at + 1} (have {len(hl)} lines, want {len(wl)}) -- edited by "
                            "hand, or stale after a re-run; --write regenerates it")
        else:
            print(f"  {path.relative_to(ROOT).as_posix()}: byte-identical to the render")
    return findings, n


def strip_md(cell: str) -> str:
    cell = re.sub(r"\*\([^)]*\)\*", "", cell)  # *(a parenthetical note)*
    cell = cell.replace("**", "").replace("`", "")
    return cell.strip()


def check_tables(doc: Path, cells_by_dir: dict) -> tuple[list[str], int, int]:
    """Verify every marked table in `doc`; a `configuration`/`benchmark` table with no marker is
    a finding. Returns (findings, tables checked, figures verified)."""
    findings = []
    lines = doc.read_text(encoding="utf-8").splitlines()
    tables = figures = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        m = MARKER_RE.search(line)
        header_at = None
        if m:
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            if j >= len(lines) or not lines[j].startswith("|"):
                findings.append(f"{doc.name}:{i + 1}: check-report marker with no table under it")
                i += 1
                continue
            header_at = j
        elif line.startswith("|"):
            first = strip_md(line.strip("|").split("|")[0]).lower()
            if first in ("configuration", "benchmark"):
                findings.append(f"{doc.name}:{i + 1}: a `{first}` table with no "
                                "`<!-- check-report: ... -->` marker -- its figures are verified "
                                "by nobody")
                while i < len(lines) and lines[i].startswith("|"):
                    i += 1
                continue
        if header_at is None:
            i += 1
            continue

        fixed = dict(kv.split("=", 1) for kv in m.group("kv").split())
        grid_fw = NAMES.get(fixed.get("framework", "").lower()) if "framework" in fixed else None
        if "framework" in fixed and grid_fw is None:
            findings.append(f"{doc.name}:{i + 1}: marker fixes framework="
                            f"{fixed['framework']!r}, which names no known framework")
            i = header_at
            continue
        rdir = ROOT / m.group("dir")
        if rdir not in cells_by_dir:
            if not rdir.is_dir():
                findings.append(f"{doc.name}:{i + 1}: marker names {m.group('dir')}, which does "
                                "not exist")
                i = header_at
                continue
            cells_by_dir[rdir] = report.load(rdir)
        cells = cells_by_dir[rdir]
        tables += 1

        header = [strip_md(c) for c in lines[header_at].strip().strip("|").split("|")]
        row_kind = header[0].lower()
        k = header_at + 1
        if k < len(lines) and re.fullmatch(r"\|[-:| ]+\|?", lines[k].strip()):
            k += 1
        while k < len(lines) and lines[k].startswith("|"):
            row = [c for c in lines[k].strip().strip("|").split("|")]
            key = strip_md(row[0])
            bench = fixed.get("benchmark")
            cfg = fixed.get("cfg")
            if grid_fw and row_kind != "benchmark":
                findings.append(f"{doc.name}:{header_at + 1}: a framework= grid whose rows are "
                                f"{header[0]!r}, not `benchmark`")
                break
            if row_kind == "configuration":
                cfg = CFG_LABELS.get(key, key)
            elif row_kind == "benchmark":
                bench = key if "/" in key else f"savina/{key}"
            else:
                findings.append(f"{doc.name}:{header_at + 1}: a marked table whose first column "
                                f"is {header[0]!r}, not `configuration` or `benchmark`")
                break
            if not bench or (not cfg and not grid_fw):
                findings.append(f"{doc.name}:{k + 1}: row {key!r} fixes neither the benchmark "
                                "nor the configuration (the marker must carry the other)")
                k += 1
                continue
            for col, cell in zip(header[1:], row[1:]):
                text = strip_md(cell)
                if not text or text in ("—", "-", "n/a"):
                    continue
                col_cfg = cfg
                if grid_fw:
                    col_cfg = CFG_LABELS.get(col)
                    if col_cfg is None:
                        findings.append(f"{doc.name}:{header_at + 1}: a framework= grid whose "
                                        f"column {col!r} is not a configuration")
                        continue
                fws = cells.get(bench, {}).get(col_cfg)
                if fws is None:
                    findings.append(f"{doc.name}:{k + 1}: no results for {bench} / {col_cfg} "
                                    f"under {m.group('dir')}")
                    continue
                ranked = sorted((f for f, d in fws.items()
                                 if f != report.FLOOR and d.get("verified")),
                                key=lambda f: report.per_op(fws[f]))
                for item in re.split(r"\s+·\s+", text):
                    item = item.strip()
                    if not item or "bimodal" in item or "no measurable difference" in item:
                        continue
                    im = ITEM_RE.match(item)
                    if not im:
                        findings.append(f"{doc.name}:{k + 1}: cannot read {item!r} as "
                                        "`<framework> <figure>`")
                        continue
                    if grid_fw:
                        # Every item in a framework= grid is that framework's; a name is
                        # allowed only when it is the same one.
                        fw = grid_fw if im.group("name") in (None, fixed["framework"]) else None
                    else:
                        name = (im.group("name") or col).lower()
                        if name == "floor":
                            fw = report.FLOOR
                        else:
                            fw = NAMES.get(name)
                    if fw is None:
                        findings.append(f"{doc.name}:{k + 1}: {item!r} names no known framework")
                        continue
                    d = fws.get(fw)
                    if d is None or not d.get("verified"):
                        findings.append(f"{doc.name}:{k + 1}: {item!r} but {fw} has no verified "
                                        f"document for {bench} / {col_cfg}")
                        continue
                    unit = im.group("unit")
                    if unit in ("×", "x"):
                        # `qb 1.60x` is report.py's own "X is N.NNx faster than Y" sentence:
                        # the named framework must be the row's fastest verified framework and
                        # N its runner-up's median over its own, so the multiple cannot be
                        # quietly recomputed against a more flattering denominator.
                        if fw != ranked[0]:
                            findings.append(f"{doc.name}:{k + 1}: {item!r} but the fastest "
                                            f"verified framework for {bench} / {col_cfg} is "
                                            f"{ranked[0]}")
                            continue
                        if len(ranked) < 2:
                            findings.append(f"{doc.name}:{k + 1}: {item!r} with nothing to be "
                                            "faster than")
                            continue
                        want = f"{report.per_op(fws[ranked[1]]) / report.per_op(d):.2f}"
                        have = im.group("num")
                    else:
                        want = report.fmt_ns(report.per_op(d)).replace(",", "")
                        have = f"{im.group('num').replace(',', '')} {unit.replace('µs', 'us')}"
                    if have != want:
                        findings.append(f"{doc.name}:{k + 1}: {item!r} -- the results say "
                                        f"{fw} = {want} for {bench} / {col_cfg}")
                    else:
                        figures += 1
            k += 1
        i = k
    return findings, tables, figures


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")  # a cp1252 console mangles the middle dot
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="regenerate every REPORT.md in place")
    args = ap.parse_args()

    findings, n_reports = check_reports(args.write)
    cells_by_dir: dict = {}
    tables = figures = 0
    for doc in (ROOT / "README.md",):
        f, t, g = check_tables(doc, cells_by_dir)
        findings += f
        tables += t
        figures += g
    print(f"check-report: {n_reports} report(s) rendered, {tables} transcribed table(s), "
          f"{figures} figure(s) verified")
    if findings:
        for f in findings:
            print(f"FAIL: {f}")
        print(f"check-report: {len(findings)} finding(s)")
        return 1
    if tables < FLOOR_TABLES or figures < FLOOR_FIGURES:
        print(f"check-report: INCONCLUSIVE -- {tables} tables / {figures} figures, floors "
              f"{FLOOR_TABLES} / {FLOOR_FIGURES}", file=sys.stderr)
        return 2
    print("check-report: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
