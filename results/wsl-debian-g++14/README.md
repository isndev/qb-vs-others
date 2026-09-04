# wsl-debian-g++14

WSL2 (Hyper-V guest) — Debian 13, g++ 14.2.0, `-O3 -DNDEBUG`, i9-12900K, pinned to vCPUs 0 and 2,
**5 repetitions + 1 warmup**. Every one of the **84 cells** across the five `savina-*/`
directories was measured in one quiet session on 2026-09-04 (15:50–16:05 UTC, the Windows side
idle throughout — its own session started at 16:09 UTC), and the candidate qb branch's 20 cells
followed at 16:05 UTC in the same session. `run.json` carries `merged_partial_runs`: the field was
brought to single-session provenance framework by framework. qb is the shipped v3.1.0
(`eac739ff`), built clean under `~/qvo/shipped`.

`REPORT.md` beside this file is `tools/report.py`'s render of this directory and
`tools/check-report.py` fails if it drifts. `docs/TUNING.md` §6 is the reading guide, §8 for the
`caf-detached` row and the qb idle-floor experiment, §9 for the four newer shapes.

| directory | what it is |
|---|---|
| `savina-ping-pong/` | **20 cells** — 18 verified + 2 declared `n/a` (`caf-detached` has no spin mode). |
| `savina-counting/`, `savina-thread-ring/`, `savina-fork-join/`, `savina-big/` | **16 cells** each, all verified; `caf-detached` declares itself omitted from these four (`frameworks/caf-detached/CMakeLists.txt`). |
| `qb-branch-perf-core-hot-path/L-ba051409/` | **the candidate**: qb at `perf/core-hot-path` `ba051409` (six commits over 3.1.0) through the same adapters, all five benchmarks — README.md's `framework=qb` grid for this host. |
| `qb-branch-perf-core-hot-path/` (the loose files) | earlier side experiments at `39992047`, NOT rendered by the report: the four ping-pong cells with the shipped build beside them, the axis-K A/B, the `idlespin*` files (§8.2). |

**Caveat that applies to every `2c-park` cell of a benchmark that crosses a core per message:**
a futex wake of a thread parked on another vCPU costs ~12 µs under WSL2, so the raw
condition-variable floor is **25.47 µs per ping-pong round trip** and **13.01 µs per ring hop**
(`baseline__2c-park` in each), and any framework that truly parks across cores measures the
hypervisor, not itself: qb 3.1.0 26.78 µs / 13.41 µs, SObjectizer 26.53 µs on ping-pong,
`caf-detached` in its slow mode 25.81 µs. The pooled `caf` row (290 / 141 ns) is below that floor
because it never crosses a core. counting, fork-join and big do not collapse — their consumers
never run dry — and every `1c-*` and `2c-spin` cell is unaffected.

**`caf-detached__2c-park` is bimodal** — 1 of 5 repetitions at ~3.6 µs, 4 at ~25.8 µs in this
document, 3 of 5 fast on the previous session's — and the report marks it so instead of quoting
its median. See `frameworks/caf-detached/README.md`.
