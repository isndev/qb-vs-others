# desktop-b67osn6-win-msvc

Windows 11 (10.0.26100), MSVC 19.51 (VS 2026), `/O2 /DNDEBUG`, i9-12900K, pinned to CPUs 0 and 2
(two P-cores), **9 repetitions + 2 warmup**. Every one of the **84 cells** across the five
`savina-*/` directories was measured in ONE quiet session on 2026-09-04 (16:09–16:18 UTC) — no
build, no test suite and no WSL measurement running anywhere on the host (the WSL2 session ended
at 16:05 UTC) — and the candidate qb branch's 20 cells followed at 16:17–16:18 UTC in the same
session, so shipped and branch share their provenance. `run.json` records `merged_partial_runs`
because the session was driven as `--only <framework>` runs that `tools/run.py` merged; it
refuses a merge whose host, CPU set or repetition count differs.

`REPORT.md` beside this file is `tools/report.py`'s render of this directory and
`tools/check-report.py` fails if it drifts from the JSON. `docs/TUNING.md` is the reading guide:
§1.1 for CAF's two columns, §5/§7 for qb's park cell, §8 for `caf-detached` and the idle-floor
experiment, §9 for what the four newer shapes say about qb.

| directory | what |
|---|---|
| `savina-ping-pong/` | 4 configurations × {`baseline`, `qb` 3.1.0, `caf` 1.1.0, `caf-detached`, `sobjectizer` 5.8.5.1}: **18 verified + 2 `n/a`** (`caf-detached` has no spin mode; harness exit 3, reason in the document). `qb` is the shipped v3.1.0 (`eac739ff`, `git archive`d and built apart from the branch). The `caf-detached` 2c-park cell is **bimodal** (4 of 9 repetitions at ~1.0–1.5 µs, 5 at ~10.6 µs) and the report says so; so is shipped qb's 2c-park on `thread-ring` below. |
| `savina-counting/`, `savina-thread-ring/`, `savina-fork-join/`, `savina-big/` | the same 4 configurations × {`baseline`, `qb`, `caf`, `sobjectizer`}: **16 verified** each, no `n/a` (`caf-detached` declares itself omitted from these four in its `CMakeLists.txt`). Shipped qb's `thread-ring` 2c-park is **bimodal** — 2 of 9 at ~0.5–0.7 µs per hop, 7 at 2.0–3.6 µs — the §5 collapse on a ring. |
| `qb-branch-perf-core-hot-path/M-f5c20eeb/` | **the candidate**: qb at `perf/core-hot-path` `f5c20eeb` (eight commits over 3.1.0, axes I and M included) through the unmodified adapters, all five benchmarks, 9 repetitions — the `framework=qb` grid README.md carries and `check-report.py` verifies. `M-f5c20eeb-shipped-3.1.0/` beside it is v3.1.0 measured minutes earlier in the same session, the control. |
| `qb-branch-perf-core-hot-path/L-ba051409/` | the previous candidate (`ba051409`, six commits, before axes I and M), same protocol; kept so the L → M deltas in `docs/TUNING.md` §7 can be re-derived. |
| `qb-branch-perf-core-hot-path/ab-axis-I/`, `ab-axis-IM/` | the interleaved A/B documents behind §7 "Axes I and M": `ab-axis-I/{L,I}` is axis I alone against L (the counting-2c regression is in `I/savina-counting/`), `ab-axis-IM/{L1,IM1,L2,IM2}` the four-pass L against I + M. 9 repetitions each. Not rendered. |
| `qb-branch-perf-core-hot-path/burst-sweep/` | `savina/counting` at one core, spin, with the burst swept 2 k → 4 M messages for the candidate, and 30 k / 1 M for shipped 3.1.0, CAF, SObjectizer and the floor — the instrument behind `docs/TUNING.md` §9.11 and §9.12. 7 repetitions + 2 warmup. Not rendered. |
| `qb-branch-perf-core-hot-path/` (the loose files) | earlier side experiments on the branch at `39992047`: the four ping-pong cells with the shipped build beside them, the axis-K A/B and the `idlespin*` floor experiment. Not rendered by `report.py` (its own README says why). |
| `caf-spin-sweep/` | side experiment: the ten-point sweep of CAF's two work-stealing knobs that established `wait=1` ≡ `wait=0` for CAF. Not rendered. |

Toolchain provenance is in every document's `env` object; `tools/report.py` prints it once per
report and refuses to render two documents for one cell.
