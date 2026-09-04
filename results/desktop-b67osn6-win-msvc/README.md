# desktop-b67osn6-win-msvc

Windows 11 (10.0.26100), MSVC 19.51 (VS 2026), `/O2 /DNDEBUG`, i9-12900K, pinned to CPUs 0 and 2
(two P-cores), **9 repetitions + 2 warmup**, 1 000 000 round trips per repetition. Every cell of
`savina-ping-pong/` was measured in ONE quiet session on 2026-09-04 (14:20–14:40 UTC) — no build,
no test suite and no WSL measurement running anywhere on the host — so all 20 cells share their
provenance; `run.json` records `merged_partial_runs` because the session was driven as five
`--only <framework>` runs that `tools/run.py` merged, and it refuses a merge whose host, CPU set or
repetition count differs.

`docs/TUNING.md` is the reading guide: §1.1 for CAF's two columns, §5/§7 for qb's park cell, §8
for `caf-detached` and the idle-floor experiment.

| directory | what |
|---|---|
| `savina-ping-pong/` | the published table — 4 configurations × {`baseline`, `qb` 3.1.0, `caf` 1.1.0, `caf-detached`, `sobjectizer` 5.8.5.1}: **18 verified + 2 `n/a`** (`caf-detached` has no spin mode; harness exit 3, reason in the document). `qb` is the shipped v3.1.0 (`eac739ff`, `git archive`d and built apart from the branch). The `caf-detached` 2c-park cell is **bimodal** (2 of 9 repetitions at ~0.93 µs, 7 at ~10.58 µs) and the report says so. |
| `qb-branch-perf-core-hot-path/` | side experiment: qb's local branch measured through the unmodified adapter, shipped build beside it, plus the axis-K A/B and the `idlespin*` floor experiment. Not rendered by `report.py` (its own README says why). |
| `caf-spin-sweep/` | side experiment: the ten-point sweep of CAF's two work-stealing knobs that established `wait=1` ≡ `wait=0` for CAF. Not rendered. |

Toolchain provenance is in every document's `env` object; `tools/report.py` prints it once per
report and refuses to render two documents for one cell.
