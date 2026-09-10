# qev branch `perf/nowait-pass-floor` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-188** (qev: the non-blocking pass at its floor — one clock read, no
wake-up handshake, the evpipe not counted as a pollable fd) on the probes it moves, measured
against the batch it forks from (qb `5de4b363` with qev `ea565d1`, i.e. QB-185 + QB-187) on
2026-09-08 in one quiet window (load 1.9, both gates finished, the Windows side idle). The
candidate is `~/qvo/cand-188` built against `~/qb-188` — a clean LF clone of qb `5de4b363`
whose `src/qb/ev/` copy carries the branch — and the control `~/qvo/ctl-185`, the same clone
without it; same flags (`-O3 -DNDEBUG`), same CPU 0, candidate and control alternated five
times. **QB-193** (the Windows clock) is in the same qev commit and changes nothing on Linux.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-ask-cost`: `ask` and one- and 64-chunk `ask_stream` **with a 500 ms timeout**, the untimed `ask`, `push`; `qvoprobe-pass-cost` k = 1 — cand / ctl × 5, 1.5 s each. 01:02:02–01:03:32 UTC. |
| `bench-pass.txt` | qev's own `bench/bench-pass.c` (2 000 000 `ev_run(EVRUN_NOWAIT)` per shape), `taskset -c 2`, the QB-187 build (`/tmp/bench-pass`, qev `ea565d1`) against the branch build (`~/qev-build`), × 5 alternations. |

None of it is merged into the published tables.

## What one non-blocking pass costs (ns, medians of five)

| shape | qev `ea565d1` (QB-187) | **branch** | Δ |
|---|---:|---:|---:|
| empty loop | 50.1 | **21.6** | −57 % |
| one far timer (a pending request timeout) | 51.5 | **22.1** | −57 % |
| one quiet socket | 132.1 | **101.4** | −23 % |
| socket + timer | 132.5 | **102.8** | −22 % |
| `ev_now_update` | 17.8 | 17.8 | level |
| `ev_timer_start` + `stop` + `ev_now_update` | 21.2 | 21.3 | level |

The 30 ns that left the pass: one `clock_gettime` (~17 ns through the vDSO) and the
`pipe_write_wanted` store + full fence (~10 ns), plus the bookkeeping around them. What is
left of a timers-only pass (22 ns) is the one clock read (17.8) and the pass's own
bookkeeping — the target of the roadmap's next step, the embedder's reading handed to the loop
(QB-190). The socket shape keeps its `epoll_wait(0)` (~80 ns): QB-191.

## The probes (ns per round trip, one core)

| probe | control (QB-185 + 187) | **branch** | Δ |
|---|---:|---:|---:|
| **ask with a 500 ms timeout** | 173.6 | **114.9** | **−34 %** |
| stream, 1 chunk, with a 500 ms timeout | 239.5 | **179.0** | −25 % |
| stream, 64 chunks, with a 500 ms timeout (per chunk) | 28.0 | 27.0 | −4 % |
| ask (no timeout) | 46.9 | 47.0 | level |
| push | 24.3 | 24.3 | level |
| `pass-cost` k = 1 (ns per pass, no watcher) | 12.7 | 12.8 | level |

A timed ask runs three NOWAIT passes per round trip; three × 30 ns is what came off. The
remaining ~68 ns over the untimed ask are the three passes' clock reads and bookkeeping
(3 × 22) plus the timer arm and disarm — the cost the roadmap's deadline list (QB-189) is
designed to remove entirely.

Suites at the branch: qev's own (`test-loops` 49/49 with the new NOWAIT and clock cases, ASan
20 runs × 46/46), the qb WSL2 suites and the superproject presets — figures in the Huly
comments.
