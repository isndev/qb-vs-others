# qb branch `perf/core-hot-path` — the four qb cells, WSL2 Debian 13 / g++ 14.2

Same host, CPUs and adapter as the published `savina-ping-pong/` directory beside this one; qb
built from the local branch `perf/core-hot-path` at `39992047` (five commits over v3.1.0:
spsc per-side layout, `listener::has_work()` gate, `time()` on demand, race-free park handshake +
idle-spin floor + start-barrier yield, and the axis-K fence in `Mailbox::notify()`). 7 repetitions
+ 2 warmup, 1 000 000 round trips, 2026-09-04, on a **quiet host** — nothing else running on the
Windows side either, which the first pass over this branch did not have (see `docs/TUNING.md` §7
"With the branch": host load moved the two-core cells by 20–30 % while leaving the one-core cells
untouched, so every figure here was re-taken with the shipped build measured in the same session).

| file | what |
|---|---|
| `qb__*.json` | the branch: p50 1c-spin **74.7** ns, 1c-park **73.7**, 2c-spin **205.9**, 2c-park **207.6** |
| `shipped-3.1.0__*.json` | v3.1.0 (`eac739ff`), same session: 98.1 / 98.4 / 275.1 / 26 459 |
| `ab-nofence__2c-spin-{1,2,3}.json` / `ab-fence__*` | the axis-K A/B, interleaved: 204 / 219 / 208 ns without the spin-mode fence (worst runs 236 / 295 / 211), 206 / 206 / 208 with it (worst 210 / 208 / 215) |

`docs/TUNING.md` §7 is the reading guide. These are NOT merged into the published tables until
the branch ships.
