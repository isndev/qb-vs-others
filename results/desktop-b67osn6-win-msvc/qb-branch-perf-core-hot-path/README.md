# qb branch `perf/core-hot-path` — the four qb cells, Windows / MSVC 19.51

Same host, CPUs and adapter as the published `savina-ping-pong/` directory beside this one; qb
built from the local branch `perf/core-hot-path` at `39992047` (five commits over v3.1.0:
spsc per-side layout, `listener::has_work()` gate, `time()` on demand, race-free park handshake +
idle-spin floor + start-barrier yield, and the axis-K fence in `Mailbox::notify()`). 7 repetitions
+ 2 warmup, 1 000 000 round trips, 2026-09-04, on a **quiet host** — nothing else running, which
the first pass over this branch did not have (see `docs/TUNING.md` §7 "With the branch": host load
moved the two-core cells by 20–30 % while leaving the one-core cells untouched, so every figure
here was re-taken with the shipped build measured in the same session).

| file | what |
|---|---|
| `qb__*.json` | the branch: p50 1c-spin **89.9** ns, 1c-park **89.3**, 2c-spin **261.9**, 2c-park **259.0** |
| `shipped-3.1.0__*.json` | v3.1.0 (`eac739ff`), same session: 113.9 / 114.4 / 314.7 / 7 564 |
| `ab-nofence__2c-spin-{1,2,3}.json` / `ab-fence__*` | the axis-K A/B, interleaved: 307 / 296 / 309 ns without the spin-mode fence, 263 / 259 / 262 with it |
| `idlespin0__2c-park.json`, `idlespin0__1c-park.json` | the branch with `QVO_QB_IDLE_SPIN_US=0` — the core blocks on its first idle pass, as v3.1.0 does, but through the branch's race-free handshake: 2c-park **385.5** ns (294.6–421.7), 1c-park 84.7. Not 10 µs: at ping-pong cadence the reply lands inside the `WaitOnAddress` handshake window and the wait returns without sleeping (`docs/TUNING.md` §8.2) |
| `idlespin-default__2c-park.json` | the same session's control at the default 50 µs floor: **258.8** ns (254.7–263.5), the published branch figure reproduced |

`docs/TUNING.md` §7 is the reading guide (§8.2 for the `idlespin*` files, which are an experiment on the branch, not a configuration). These are NOT merged into the published tables until
the branch ships.
