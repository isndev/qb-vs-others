# qb branch `perf/ask-slot-table` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-178**: the qb branch that turns the pending-ask registry into a slot table
the correlation id indexes, measured against the `develop` it forks from on the one Savina shape
that sits on the request path — `savina/bank-transaction` (one teller, 1 000 accounts, 50 000
transfers, each a `qb::ask<Deposit>` nested in a request; `benchmarks/savina/bank-transaction.md`)
— plus the two `dev/bench` ask cells that gate it (`qb-core-bench-ask-roundtrip`,
`qb-core-bench-ask-all-fanout`). Same host, CPUs and build flags as the published directories
beside this one: `-O3 -DNDEBUG`, `taskset -c 0,2`, **9 repetitions + 2 warmup**, qb-only builds
(`-DQVO_WITH_CAF=OFF -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF`; the field is in
`../savina-bank-transaction/`), candidate / control / candidate in ONE quiet session, the Windows
side idle throughout. The candidate is `~/qvo/qb178`, built against the branch's tree; the
control is `~/qvo/ctl-1ec7e794`, a clean LF clone at `1ec7e794` (0 dirty). The branch is two
commits, and each was measured in its own session on 2026-09-07:

| directory | qb at | what |
|---|---|---|
| `grid-8f28e7bf/`, `grid-8f28e7bf-pass2/` | `develop` `1ec7e794` + **`8f28e7bf`** (the slot table: `[core:16][generation:26][slot:22]`, FIFO free list, no hashing) — the first candidate, measured first and third | **4 cells**, qb only, all verified: bank-transaction × {1c-spin, 1c-park, 2c-spin, 2c-park}. Session A, 06:01:55–06:03:35 UTC. |
| `grid-1ec7e794/` | `develop` `1ec7e794` — **the control** of session A, measured second | same 4 cells, same session. |
| `grid-a61bded8/`, `grid-a61bded8-pass2/` | `8f28e7bf` + **`a61bded8`** (take binds the slot in the same call, `finish()` runs once, the owner is built before the send) — the branch head, measured first and third | same 4 cells. Session B, 06:18:10–06:19:50 UTC. |
| `grid-1ec7e794-session-b/` | `1ec7e794` again — the control of session B, measured second | same 4 cells, same session. |
| `bench-8f28e7bf/`, `bench-a61bded8/` | the two `dev/bench` ask binaries, candidate and control alternated three times (`cand-N-*` / `ctl-N-*`), 5 repetitions each, aggregates only | run right after each session's grids; medians of the three medians below. |

None of the grids is merged into the published tables (`../savina-bank-transaction/` renders
shipped 3.1.0 and the two `develop` builds of §12); the branch joins them when the 3.2.0 grid is
measured for all seven shapes in one session.

## Control against the candidate, same session (p50 ms)

| config | session A: `1ec7e794` | **`8f28e7bf`** | pass 2 | Δ | session B: `1ec7e794` | **`a61bded8`** | pass 2 | Δ |
|---|---|---|---|---|---|---|---|---|
| 1c-spin | 7.519 | **7.460** | 7.515 | -0.8 % / -0.1 % | 7.700 | **7.212** | 7.250 | **-6.3 % / -5.8 %** |
| 1c-park | 7.333 | **7.209** | 7.427 | -1.7 % / +1.3 % | 7.581 | **7.303** | 7.244 | **-3.7 % / -4.4 %** |
| 2c-spin | 4.814 | **4.635** | 4.680 | -3.7 % / -2.8 % | 4.741 | **4.776** | 4.687 | +0.7 % / -1.1 % |
| 2c-park | 4.874 | **4.754** | 4.706 | -2.5 % / -3.4 % | 4.807 | **4.735** | 4.610 | -1.5 % / -4.1 % |

The slot table alone (`8f28e7bf`) is inside the spread on one core and −3 % on two; the head of
the branch is **−6 % on 1c-spin and −4 % on 1c-park, both passes**, with the 2c cells inside
their spread (the cross-core transfer is dominated by the hop, not the registry). Per transfer on
one core, spin: 154 → 144 ns — **≈ 9–10 ns per ask**, which is the size the profile predicted
(below), and why the `dev/bench` round-trip cells do not move: a round trip there is ≈ 880 ns
of loop pass, timer and latency sampling, so 9 ns is 1 %, inside the cell's 2 % spread.

| `dev/bench` cell (median of 3 medians) | session A ctl → `8f28e7bf` | session B ctl → `a61bded8` |
|---|---|---|
| `Ask_RoundTrip_SameCore` real_time | 44.4 → 44.0 ms (-0.8 %) | 44.1 → 44.4 ms (+0.7 %) |
| `Ask_RoundTrip_CrossCore` real_time | 50.1 → 49.6 ms (-1.1 %) | 49.3 → 49.0 ms (-0.7 %) |
| `Ask_Scatter<All>/responders:8` | 66.1 → 60.3 ms (-8.7 %) | 68.0 → 59.8 ms (-12.1 %) |
| `Ask_Scatter<Any>/responders:8` | 63.2 → 63.4 ms (+0.3 %) | 62.7 → 63.6 ms (+1.3 %) |

The two scatter cells are recorded-only in `dev/bench/baseline/linux-wsl2-x86_64.json` for a
16–20 % run-to-run spread, and their three ctl medians here span 61.2–68.4: the −9 / −12 % on
`All` is the same direction twice but not a claim.

## What the profile said, and what each commit bought

`perf record -F 20000 -g` on the candidate binary, 1c spin, 30 repetitions
(`--no-children --sort sym`, registry symbols only):

| symbol | `1ec7e794` | `8f28e7bf` | `a61bded8` |
|---|---|---|---|
| `ask_unregister` | 7.65 % | 4.69 % | 1.08 % |
| `ask_register` | 3.33 % | 0.93 % | — (gone) |
| `ask_next_id` / `ask_take` | — | 0.91 % | 1.45 % |
| `ask_deliver` | 1.80 % | 1.89 % | 1.83 % |
| **registry total** | **12.8 %** | **8.4 %** | **4.36 %** |
| `ask_awaiter::await_suspend` | 1.19 % | 3.47 % | 2.06 % |

`8f28e7bf` removed the hashing — and the run moved 1–4 %, less than the 4.4 points the registry
lost. `perf annotate` on its `ask_unregister` said why: 4.69 % spread over ~10 instructions —
15 % on one `jne`, 9.5 % on the epilogue, 7.5 % on the prologue `push` — and no miss signature
anywhere, the table being warm. That is the cost of the CALL (a `thread_local` with a
destructor is a guard check per access), not of the work, and the path paid it five times per
ask: `ask_next_id`, `ask_register`, `ask_deliver`, `ask_unregister` from `await_resume` and
`ask_unregister` again from the awaiter's destructor, the second an out-of-line trip that ends
on `next != busy`. `a61bded8` makes it three — `ask_take(owner, slot)` pops and binds in one
call, `finish()` keeps a byte so the destructor's pass is a compare, and the awaiter takes the
entry in its constructor, before the send, so the `ask_slot_guard` the throw window needed is
deleted — and the 1c gain arrives.

Suites at the head, standalone `cmake -S qb`, 0 warnings: WSL2 g++-14 Release / ASan+UBSan / TSan
192/192/0 each; Windows/MSVC 19.51 Release 188/188/0. The Windows A/B is in
`../../desktop-b67osn6-win-msvc/qb-branch-perf-ask-slot-table/`.
