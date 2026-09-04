#!/usr/bin/env python3
"""Render a results directory as Markdown.

Every figure this repository publishes comes from here. Nothing is typed by hand, which is what
makes `tools/check-report.py` able to assert that no hand-written number has crept in.

Two rules are enforced by this renderer rather than left to the reader's charity:

  1. A framework whose run did not VERIFY is shown as failed, never omitted. An omitted row reads
     as if the framework had not been measured.

  2. A difference smaller than the two distributions' overlap is printed as "no measurable
     difference", not as a percentage. Two medians divided by each other will always produce a
     number; whether that number means anything is a separate question, and this is where it gets
     asked.

  3. A cell the adapter declared NOT APPLICABLE (harness exit 3, `not_applicable` in the JSON) is
     shown as `n/a` with its reason, in the row where the number would have been. It is a third
     verdict: not a failure, and not a measurement of something else under this label.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# The floor is not a framework and is never ranked against one. It is printed in its own row,
# below a rule, because its job is to bound the field rather than to join it.
FLOOR = "baseline"


def load(results: Path) -> dict:
    """Read `<results>/<benchmark-slug>/<framework>__<config>.json` -- that shape and no other.

    tools/run.py writes each document one level down, in a directory NAMED for the benchmark the
    document declares. A directory that does not match is a side experiment (an A/B directory, a
    re-measurement against a branch) whose documents carry the SAME framework and parameter keys
    as the published cells; walked indiscriminately, they would silently replace or be replaced
    by the published number depending on nothing but sort order. Measured before this rule
    existed: the branch A/B files loaded first and lost to the shipped cells on the alphabet
    alone. Such directories are named on stderr and left out; a key that still arrives twice is
    a hard stop, not a last-writer-wins.
    """
    cells = {}
    origin = {}
    side = set()
    for f in sorted(results.glob("*/*.json")):
        if f.name == "run.json":
            continue
        try:
            d = json.loads(f.read_text())
        except json.JSONDecodeError:
            print(f"report.py: {f} is not JSON -- skipped", file=sys.stderr)
            continue
        if d.get("schema") != "qvo/result/1":
            continue
        bench = d.get("benchmark", "?")
        fw = d.get("framework", "?")
        if f.parent.name != bench.replace("/", "-"):
            if f.parent not in side:
                side.add(f.parent)
                print(f"report.py: {f.parent} is not a benchmark directory (its documents "
                      f"declare {bench!r}) -- a side experiment, not rendered", file=sys.stderr)
            continue
        # The configuration key is derived from the parameters that were actually recorded, not
        # from the file name, so a renamed file cannot silently move a number into another column.
        p = d.get("params", {})
        cfg = f"{p.get('cores', '?')}c-{'spin' if p.get('wait') else 'park'}"
        key = (bench, cfg, fw)
        if key in origin:
            sys.exit(f"report.py: {f} and {origin[key]} both describe {fw} / {bench} / {cfg} "
                     "-- two documents for one cell; refusing to pick one")
        origin[key] = f
        cells.setdefault(bench, {}).setdefault(cfg, {})[fw] = d
    return cells


def fmt_ns(v: float) -> str:
    if v <= 0:
        return "-"
    if v >= 1e6:
        return f"{v / 1e6:,.2f} ms"
    if v >= 1e3:
        return f"{v / 1e3:,.2f} us"
    return f"{v:,.0f} ns"


def work_unit(d: dict) -> tuple[str, float]:
    """(name, count) of the unit a repetition's time is divided by.

    Declared ONCE per benchmark, in its spec header, and written into every framework's document
    by the harness (`Spec::work_unit` / `Spec::work_units`), so a table's denominator is the same
    for every row and is never inferred here from the benchmark's name. Documents written before
    the field existed carry `expected_messages` only; those are the ping-pong documents, whose
    unit was the round trip (two messages), and that is the only fallback this function knows.
    A document with neither is reported per repetition, and the column header says so.
    """
    n = d.get("work_units", 0)
    if n:
        return d.get("work_unit", "unit"), float(n)
    msgs = d.get("expected_messages", 0)
    if msgs and d.get("benchmark") == "savina/ping-pong":
        return "round trip", msgs / 2
    return "repetition", 1.0


def per_op(d: dict) -> float:
    """Nanoseconds per unit of benchmark work, so numbers stay comparable across parameters."""
    p50 = d.get("summary", {}).get("work_p50", 0)
    if not p50:
        return 0.0
    return p50 / work_unit(d)[1]


def modes(d: dict) -> tuple | None:
    """Two clusters of repetitions when the sample is BIMODAL, else None.

    A median is a statement about one distribution. Some cells here are not one: a cross-core
    park on Windows lands either at ~0.9 us or at ~10.6 us per round trip and stays there for the
    whole 10 s repetition, and WSL2 does the same at ~3.3 us / ~25.7 us. The median of such a
    sample is whichever mode won the coin toss THIS run -- on WSL2 the caf-detached 2c-park cell
    measured 25.8 us on one run and 4.1 us on the next, from the same binary on the same idle
    host. The test is deliberately crude: sort the repetitions, split at the widest gap, and
    call it bimodal when the upper cluster starts at more than twice the lower cluster's end.
    Ordinary jitter does not open a 2x gap; a 10x one is a different mechanism, not noise.
    Returns ((count, median), (count, median)) for the lower and upper cluster, per round trip.
    """
    xs = sorted(d.get("work_ns", []))
    if len(xs) < 3:
        return None
    gaps = [(xs[i + 1] / xs[i] if xs[i] > 0 else 1.0, i) for i in range(len(xs) - 1)]
    ratio, at = max(gaps)
    if ratio < 2.0:
        return None
    lo, hi = xs[:at + 1], xs[at + 1:]
    units = work_unit(d)[1]
    per = lambda v: v / units
    med = lambda c: per(c[len(c) // 2]) if len(c) % 2 else per((c[len(c) // 2 - 1] + c[len(c) // 2]) / 2)
    return ((len(lo), med(lo)), (len(hi), med(hi)))


def overlapping(a: dict, b: dict) -> bool:
    """True when the two samples' [min, p99] ranges overlap.

    Deliberately conservative. A stricter test (a bootstrap, or a Mann-Whitney U) would call more
    differences significant; this one errs towards saying "we cannot tell", which is the correct
    direction of error for a comparison whose author has a stake in the outcome.
    """
    sa, sb = a.get("summary", {}), b.get("summary", {})
    a_lo, a_hi = sa.get("work_min", 0), sa.get("work_p99", 0)
    b_lo, b_hi = sb.get("work_min", 0), sb.get("work_p99", 0)
    if not all((a_lo, a_hi, b_lo, b_hi)):
        return True
    return a_lo <= b_hi and b_lo <= a_hi


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=None,
                    help="write the report here (UTF-8, LF) instead of stdout. Prefer this on "
                         "Windows: a shell redirection writes CRLF and, from PowerShell, a BOM, "
                         "and tools/check-report.py compares bytes")
    args = ap.parse_args()

    cells = load(args.results)
    if not cells:
        sys.exit("report.py: no results found -- refusing to render an empty report")

    run = {}
    rj = args.results / "run.json"
    if rj.exists():
        run = json.loads(rj.read_text())

    out = []
    out.append("# Results\n")
    out.append("> Generated by `tools/report.py`. Do not edit: every figure here is regenerated "
               "from the JSON in `results/`, and `tools/check-report.py` fails if a hand-written "
               "number appears.\n")
    out.append("Read [FAIRNESS.md](../FAIRNESS.md) before reading any table below. In particular: "
               "these numbers measure message plumbing, not applications, and the spin/park axis "
               "moves them further than any architectural difference they show.\n")

    if run:
        out.append("## Run\n")
        out.append(f"- host: `{run.get('host', '?')}`")
        out.append(f"- platform: `{run.get('platform', '?')}`")
        out.append(f"- pinned CPUs: `{run.get('cpus', '?')}`")
        out.append(f"- repetitions: {run.get('repetitions', '?')} "
                   f"(+{run.get('warmup', '?')} warmup), one process per cell\n")

    # Environment, taken from the results themselves rather than from the runner, so it describes
    # the binaries that produced the numbers.
    env_seen = {}
    for bench in cells.values():
        for cfg in bench.values():
            for d in cfg.values():
                e = d.get("env", {})
                key = (e.get("compiler"), e.get("compiler_version"), e.get("cxx_flags"))
                env_seen.setdefault(key, set()).add(d.get("framework"))
    if len(env_seen) > 1:
        out.append("> **WARNING — the field was not built by one toolchain.** The frameworks below "
                   "were compiled by more than one compiler build, so part of any difference "
                   "shown may be the compiler rather than the framework. Rebuild everything in "
                   "one pass before quoting these figures.\n")
        for (comp, ver, flags), fws in env_seen.items():
            out.append(f"> - `{comp} {ver}` `{flags}` -> {', '.join(sorted(fws))}")
        out.append("")
    elif env_seen:
        (comp, ver, flags), _ = next(iter(env_seen.items()))
        out.append(f"- toolchain: `{comp} {ver}`, flags `{flags}` — identical for every framework\n")

    versions = {}
    for bench in cells.values():
        for cfg in bench.values():
            for fw, d in cfg.items():
                versions[fw] = d.get("framework_version", "?")
    out.append("## Versions under test\n")
    out.append("| framework | version |")
    out.append("|---|---|")
    for fw in sorted(versions):
        note = " *(not a framework — the floor)*" if fw == FLOOR else ""
        out.append(f"| `{fw}` | {versions[fw]}{note} |")
    out.append("")

    for bench in sorted(cells):
        out.append(f"## {bench}\n")
        for cfg in sorted(cells[bench]):
            fws = cells[bench][cfg]
            out.append(f"### {cfg}\n")
            units = {work_unit(d)[0] for d in fws.values() if not d.get("not_applicable")}
            if len(units) > 1:
                sys.exit(f"report.py: {bench} {cfg}: the documents disagree on the work unit "
                         f"({sorted(units)}) -- one table cannot divide its rows by different "
                         "things; re-run the cell whose adapter predates the spec's declaration")
            unit = next(iter(units)) if units else "repetition"
            out.append(f"| framework | verified | median | per {unit} | IQR | p99 |")
            out.append("|---|---|---:|---:|---:|---:|")

            ranked = [f for f in fws if f != FLOOR]
            ranked.sort(key=lambda f: per_op(fws[f]) or float("inf"))
            for fw in ranked + ([FLOOR] if FLOOR in fws else []):
                d = fws[fw]
                s = d.get("summary", {})
                if d.get("not_applicable"):
                    out.append(f"| `{fw}` | n/a | — | — | — | — |")
                    out.append(f"| | <sub>{d['not_applicable']}</sub> | | | | |")
                    continue
                if not d.get("verified"):
                    why = "; ".join(d.get("failures", [])) or "did not verify"
                    out.append(f"| `{fw}` | **FAILED** | — | — | — | — |")
                    out.append(f"| | <sub>{why}</sub> | | | | |")
                    continue
                if fw == FLOOR:
                    out.append("| | | | | | |")
                out.append(f"| `{fw}`{' *(floor)*' if fw == FLOOR else ''} | yes | "
                           f"{fmt_ns(s.get('work_p50', 0))} | {fmt_ns(per_op(d))} | "
                           f"{fmt_ns(s.get('work_iqr', 0))} | {fmt_ns(s.get('work_p99', 0))} |")
                m = modes(d)
                if m:
                    (nlo, vlo), (nhi, vhi) = m
                    out.append(f"| | <sub>**bimodal**: {nlo} of {nlo + nhi} repetitions at "
                               f"~{fmt_ns(vlo)} per {unit}, {nhi} at ~{fmt_ns(vhi)}. The "
                               "median above is whichever mode won this run; quote both, never "
                               "the median</sub> | | | | |")
            out.append("")

            verified = [f for f in ranked if fws[f].get("verified")]
            if len(verified) >= 2:
                best, second = verified[0], verified[1]
                if modes(fws[best]) or modes(fws[second]):
                    out.append(f"**No ordering is claimed between `{best}` and `{second}`** — "
                               "one of them is bimodal, and a ratio of two medians where one "
                               "median is a coin toss is not a result.\n")
                elif overlapping(fws[best], fws[second]):
                    out.append(f"**`{best}` and `{second}` show no measurable difference here** — "
                               "their sample ranges overlap, so the ordering above is not a "
                               "result.\n")
                else:
                    ratio = per_op(fws[second]) / per_op(fws[best])
                    out.append(f"`{best}` is **{ratio:.2f}x** faster than `{second}` in this "
                               "configuration.\n")
                if FLOOR in fws and fws[FLOOR].get("verified"):
                    fr = per_op(fws[best]) / per_op(fws[FLOOR])
                    if fr < 1.0:
                        # A framework below the floor is not beating raw threads: it is not
                        # doing what the floor does (here, typically, crossing a core and waking
                        # a parked thread). Saying "0.01x the floor" would read as a triumph.
                        out.append(f"The fastest framework sits **below the floor** "
                                   f"({fr:.2f}x) — which means it is not paying the cost the "
                                   "floor measures, not that it beats raw threads at it; its "
                                   "caveats below say what it does instead.\n")
                    else:
                        out.append(f"The fastest framework costs **{fr:.2f}x the floor** — that "
                                   "multiple is what being a framework costs on this "
                                   "workload.\n")

            caveats = {}
            for fw, d in fws.items():
                for c in d.get("caveats", []):
                    caveats.setdefault(c, []).append(fw)
            if caveats:
                out.append("<details><summary>Caveats recorded by the implementations "
                           "themselves</summary>\n")
                for c, owners in caveats.items():
                    out.append(f"- *({', '.join(sorted(owners))})* {c}")
                out.append("\n</details>\n")

    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
