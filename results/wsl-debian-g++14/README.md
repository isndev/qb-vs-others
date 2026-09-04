# wsl-debian-g++14

WSL2 (Hyper-V guest) — Debian 13, g++ 14.2.0, `-O3 -DNDEBUG`, i9-12900K, pinned to vCPUs 0 and 2,
5 repetitions + 1 warmup. `docs/TUNING.md` §6 is the reading guide, §8 for the `caf-detached` row
and the qb idle-floor experiment.

| directory | what it is |
|---|---|
| `savina-ping-pong/` | the table: **20 cells** — 18 verified + 2 declared `n/a` (`caf-detached` has no spin mode). Every cell re-measured on 2026-09-04 in one quiet session (14:21–14:52 UTC, `run.json` carries `merged_partial_runs`: the field was brought to single-session provenance framework by framework, the Windows side idle throughout). qb is the shipped v3.1.0 (`eac739ff`), built clean under `~/qvo/shipped`. |
| `qb-branch-perf-core-hot-path/` | side experiment, NOT rendered by the report: the same adapter against the local qb branch `perf/core-hot-path`, plus the `idlespin*` files (§8.2). |

**Caveat that applies to every `2c-park` cell here:** a futex wake of a thread parked on another
vCPU costs ~12 µs under WSL2, so the raw condition-variable floor is **25.1 µs per round trip**
(`baseline__2c-park`) and any framework that truly parks across cores measures the hypervisor,
not itself: qb 26.7 µs, SObjectizer 26.4 µs, `caf-detached` in its slow mode 25.7 µs. The
pooled `caf` row (283 ns) is below that floor because it never crosses a core. The `1c-*` and
`2c-spin` cells are unaffected.

**`caf-detached__2c-park` is bimodal** — 3 of 5 repetitions at ~3.9 µs, 2 at ~25.7 µs in this
document, 5 of 5 slow on an earlier run of the same binary — and the report marks it so instead
of quoting its median. See `frameworks/caf-detached/README.md`.
