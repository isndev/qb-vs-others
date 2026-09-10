# qb branch `perf/core-hot-path` — the four qb cells, Windows / MSVC 19.51

Same host, CPUs and adapter as the published `savina-ping-pong/` directory beside this one; qb
built from the local branch `perf/core-hot-path` at `6a0897c0` (five commits over v3.1.0:
spsc per-side layout, `listener::has_work()` gate, `time()` on demand, race-free park handshake +
idle-spin floor + start-barrier yield, and the axis-K fence in `Mailbox::notify()`). 7 repetitions
+ 2 warmup, 1 000 000 round trips, 2026-09-04, on a **quiet host** — nothing else running, which
the first pass over this branch did not have (see `docs/TUNING.md` §7 "With the branch": host load
moved the two-core cells by 20–30 % while leaving the one-core cells untouched, so every figure
here was re-taken with the shipped build measured in the same session).

| file | what |
|---|---|
| `qb__*.json` | the branch: p50 1c-spin **89.9** ns, 1c-park **89.3**, 2c-spin **261.9**, 2c-park **259.0** |
| `shipped-3.1.0__*.json` | v3.1.0 (`830ea244`), same session: 113.9 / 114.4 / 314.7 / 7 564 |
| `ab-nofence__2c-spin-{1,2,3}.json` / `ab-fence__*` | the axis-K A/B, interleaved: 307 / 296 / 309 ns without the spin-mode fence, 263 / 259 / 262 with it |
| `idlespin0__2c-park.json`, `idlespin0__1c-park.json` | the branch with `QVO_QB_IDLE_SPIN_US=0` — the core blocks on its first idle pass, as v3.1.0 does, but through the branch's race-free handshake: 2c-park **385.5** ns (294.6–421.7), 1c-park 84.7. Not 10 µs: at ping-pong cadence the reply lands inside the `WaitOnAddress` handshake window and the wait returns without sleeping (`docs/TUNING.md` §8.2) |
| `idlespin-default__2c-park.json` | the same session's control at the default 50 µs floor: **258.8** ns (254.7–263.5), the published branch figure reproduced |
| `L-32b28130/` | the branch at `32b28130` (axis L), all five benchmarks × 4 configurations, 9 repetitions + 2 warmup; the previous README.md candidate grid |
| `ab-axis-I/{L,I}/` | axis I alone against L, interleaved, 9 repetitions: 1c cells −4…−24 %, counting 2c **+42 %** (30.6 → 43.5 spin, 31.7 → 44.5 park) — the regression that became axis M |
| `ab-axis-IM/{L1,IM1,L2,IM2}/` | L against I + M, four passes interleaved, 9 repetitions each; the best-of-pass table in `docs/TUNING.md` §7 "Axes I and M" |
| `M-230c5035-shipped-3.1.0/`, `M-230c5035/` | v3.1.0 then the branch at `230c5035`, same session, 9 repetitions + 2 warmup: the current README.md candidate grid (`M-230c5035/`) and its shipped control |
| `burst-sweep/` | counting 1c-spin against the burst size: `qb__counting-1c-spin-<N>.json` for N = 2 k … 4 M (20.6 / 29.9 / 25.0 / 28.3 / 32.8 / 25.3 / 25.7 ns), `shipped-3.1.0__`, `caf__`, `sobjectizer__`, `baseline__` at 30 k and 1 M. `docs/TUNING.md` §9.11 and §9.12 |

`docs/TUNING.md` §7 is the reading guide (§8.2 for the `idlespin*` files, which are an experiment on the branch, not a configuration). These are NOT merged into the published tables until
the branch ships.
