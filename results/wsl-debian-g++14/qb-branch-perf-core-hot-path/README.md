# qb branch `perf/core-hot-path` — the four qb cells, WSL2 Debian 13 / g++ 14.2

Same host, CPUs and adapter as the published `savina-ping-pong/` directory beside this one; qb
built from the local branch `perf/core-hot-path` at `6a0897c0` (five commits over v3.1.0:
spsc per-side layout, `listener::has_work()` gate, `time()` on demand, race-free park handshake +
idle-spin floor + start-barrier yield, and the axis-K fence in `Mailbox::notify()`). 7 repetitions
+ 2 warmup, 1 000 000 round trips, 2026-09-04, on a **quiet host** — nothing else running on the
Windows side either, which the first pass over this branch did not have (see `docs/TUNING.md` §7
"With the branch": host load moved the two-core cells by 20–30 % while leaving the one-core cells
untouched, so every figure here was re-taken with the shipped build measured in the same session).

| file | what |
|---|---|
| `qb__*.json` | the branch: p50 1c-spin **74.7** ns, 1c-park **73.7**, 2c-spin **205.9**, 2c-park **207.6** |
| `shipped-3.1.0__*.json` | v3.1.0 (`830ea244`), same session: 98.1 / 98.4 / 275.1 / 26 459 |
| `ab-nofence__2c-spin-{1,2,3}.json` / `ab-fence__*` | the axis-K A/B, interleaved: 204 / 219 / 208 ns without the spin-mode fence (worst runs 236 / 295 / 211), 206 / 206 / 208 with it (worst 210 / 208 / 215) |
| `idlespin0__2c-park.json`, `idlespin0__1c-park.json` | the branch with `QVO_QB_IDLE_SPIN_US=0` — the core blocks on its first idle pass, as v3.1.0 does: 2c-park **25 796** ns (25 672–26 096), every repetition, no bimodality — the hypervisor's futex wake and nothing else, indistinguishable from shipped 3.1.0 and from the raw cv floor; 1c-park 73.5, unchanged. On Linux the 50 µs floor IS the branch's park gain (`docs/TUNING.md` §8.2) |
| `idlespin-default__2c-park.json` | the same session's control at the default floor: **211.7** ns (207.9–228.7) |
| `L-32b28130/` | the branch at `32b28130` (axis L), all five benchmarks × 4 configurations, 5 repetitions + 1 warmup; the previous README.md candidate grid |
| `M-230c5035-shipped-3.1.0/`, `M-230c5035/` | v3.1.0 then the branch at `230c5035`, same session, 5 repetitions + 1 warmup: the current README.md candidate grid (`M-230c5035/`) and its shipped control. The axis-I / I+M A/B was run on the Windows host only (`results/desktop-b67osn6-win-msvc/qb-branch-perf-core-hot-path/ab-axis-*`); here the branch went straight from L to I + M |
| `burst-sweep/` | counting 1c-spin against the burst size: `qb__counting-1c-spin-<N>.json` for N = 2 k … 4 M (**6.5** / 8.0 / 8.5 / 12.5 / 31.8 / 35.2 / 38.8 ns — the cliff between 100 k and 300 k is the pipe leaving the cache), `shipped-3.1.0__` (37.0 / 42.6), `caf__` (113 / 119), `sobjectizer__` (94 / 106), `baseline__` (2.9 / 3.0) at 30 k and 1 M. `docs/TUNING.md` §9.11 |

`docs/TUNING.md` §7 is the reading guide (§8.2 for the `idlespin*` files, which are an experiment on the branch, not a configuration). These are NOT merged into the published tables until
the branch ships.
