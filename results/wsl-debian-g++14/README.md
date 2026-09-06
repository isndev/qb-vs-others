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
| `qb-branch-perf-core-hot-path/M-f5c20eeb/` | **the previous candidate** (README.md's grid until `perf/event-pipe-segmented` superseded it): qb at `perf/core-hot-path` `f5c20eeb` (eight commits over 3.1.0, axes I and M included) through the same adapters, all five benchmarks, 5 repetitions — README.md's `framework=qb` grid for this host. `M-f5c20eeb-shipped-3.1.0/` beside it is v3.1.0 measured minutes earlier in the same session, the control. |
| `qb-branch-perf-core-hot-path/L-ba051409/` | the previous candidate (`ba051409`, six commits, before axes I and M), same protocol; kept for the L → M deltas in `docs/TUNING.md` §7. Its thread-ring 1c cells (48 / 50.5) and the current candidate's (37 / 48) are the spread that subsection records for the g++ ring. |
| `qb-branch-perf-core-hot-path/burst-sweep/` | `savina/counting` at one core, spin, with the burst swept 2 k → 4 M messages for the candidate, and 30 k / 1 M for shipped 3.1.0, CAF, SObjectizer and the floor — the instrument behind `docs/TUNING.md` §9.11: the branch dispatches at 8.5 ns at 30 k and 35 at 1 M, the frameworks that allocate per message are flat. 7 repetitions + 2 warmup. Not rendered. |
| `qb-branch-perf-event-pipe-segmented/grid-final/` | **the candidate**: qb at `perf/event-pipe-segmented` `a017b8a5` (two commits over `f5c20eeb`: the segmented pipe, then the slab pool and the staggered segments) through the same adapters, all five benchmarks, **7 repetitions + 2 warmup**, measured 2026-09-05 01:18 UTC in one quiet session with `grid-f5c20eeb/` (01:19 UTC) and `grid-shipped-3.1.0/` (01:19–01:25 UTC, v3.1.0, the control), the Windows side idle throughout — README.md's `framework=qb` grid for this host. `docs/TUNING.md` §9.11 is the reading guide. |
| `qb-branch-perf-event-pipe-segmented/grid-*-pass2/` | the earlier pass of the same three grids (00:49–00:56 UTC), same protocol. Not rendered. |
| `qb-branch-perf-event-pipe-segmented/burst-sweep/` | `savina/counting` at one core, spin, burst swept 2 k → 4 M for the candidate AND for `f5c20eeb` in the same session (01:25 UTC), 30 k / 1 M for shipped 3.1.0, CAF, SObjectizer and the floor — the §9.11 table: 5.9–9.3 ns flat for the candidate against `f5c20eeb`'s 8.8 → 40.1. `burst-sweep-pass2/` is the earlier pass. 7 + 2. Not rendered. |
| `qb-branch-perf-event-pipe-segmented/burst-sweep-prehoist/`, `grid-slabs-prehoist/` | the A/B that shaped the branch, 2026-09-04 20:48–23:58 UTC: `pass1-malloc-segments/` is the segmented pipe with `malloc`-laid segments (copies nothing, still 15 640 minor faults at 1 M), `pass2-slabs/` and `pass2-slabs-rerun/` the slab pool before the width hoist and the stagger — and `grid-slabs-prehoist/` is the document with the defect in it: `big` at one core at 107 ms per repetition against 52, the 4K aliasing §9.11 explains. Not rendered. |
| `qb-branch-perf-park-in-ev-loop/` | **axis N** (`perf/park-in-ev-loop`, branched from `develop` `f0da4e32`, the control): `parked-io-wake.log` is the qb-only probe `tools/probes/parked-io-wake.cpp` over 12 latency × gap cells, two interleaved passes, 2000 rounds — a parked core with a readable socket under it answered in **923 / 9961 µs** at `setLatency` 1 / 10 ms and answers in **~31 µs** on the branch (the probe pinning core and client itself; the earlier unpinned grid, same p50s, is kept beside it for what its tails taught); `ab/` is the ping-pong regression gate, 4 cells × 3 interleaved reps, no cell moved. 2026-09-06 02:19–03:22 UTC, quiet sessions. `docs/TUNING.md` §10. Not rendered. |
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
