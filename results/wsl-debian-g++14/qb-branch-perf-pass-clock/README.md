# qb branch `perf/pass-clock` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-190** (the pass that does not run the loop: a non-blocking pass with nothing
pending, no wake, no poll to make and no timer within reach calls `ev_run` not at all; qev
`perf/embedder-clock`: `ev_now_set`, `ev_clock_now`, `ev_timer_count_addr`, `ev_timer_next`,
`ev_wake_pending_addr`) against the `develop` it forks from (qb `4903750e`, qev `2b27a6c`: after
QB-191), measured on 2026-09-08 **11:59–12:01 UTC** in one quiet window (no build during the points,
no other probe, the Windows side idle). The candidate is `~/qvo/cand-191` built against `~/qb-191`
(a clean LF clone at `4903750e` plus the branch's patch and the branch's qev), the control
`~/qvo/ctl-191` against `~/qb-ctl`, a clone at `4903750e`; same flags (`-O3 -DNDEBUG`), CPU 0,
candidate and control alternated five times, 1.5 s per point.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-io-pass` in its new `timer` mode (a busy core holding one far libev timer, no socket: ns per pass), `pass` (a quiet loopback socket, cold between two polls) and `wake` (the latency of a byte on that socket, 100 µs gaps), `qvoprobe-ask-cost` `push` and `ask` with a 500 ms timeout, `qvoprobe-pass-cost` k = 1 — cand / ctl × 5. |
| `bench-pass.txt` | qev's own `bench-pass` on the branch, × 3: `timer` (the loop pass over a far timer), `timer+set` (the same with `ev_clock_now` + `ev_now_set` before each pass), `timer+set0` (a free sample: the loop's bookkeeping floor given its time), `gate` (no `ev_run`: the embedder's decision alone), `now`, `arm+now`. |

None of it is merged into the published tables.

## The probes (one core, medians of five)

| probe | control (`4903750e`) | **branch** | Δ |
|---|---:|---:|---:|
| **busy pass with a far timer (ns)** | 36.9 | **26.2** | **−29 %** |
| **pass with a quiet socket, cold (ns)** | 48.6 | **28.5** | **−41 %** |
| wake latency on that socket, p50 / p99 (µs) | 3.88 / 23.8 | 3.89 / 17.2 | level |
| push (ns per trip) | 24.0 | 24.3 | inside the spread (ctl 23.9–24.1, cand 24.1–24.3) |
| ask with a 500 ms timeout (ns per trip) | 68.7 | 69.5 | inside the spread (ctl 68.5–69.2, cand 69.2–69.8; the control read 67.9 in the QB-191 session) |
| `pass-cost` k = 1 (ns per pass, no watcher) | 12.44 | 12.46 | level |

What a busy core paid for one far timer: **36.9 − 12.4 = 24.5 ns** a pass — `ev_run` with its clock
read, the timer heap and the pending walk, to find nothing. Now **13.8**: the loop is not entered;
the gate reads the counter (`rdtsc`, ~8 ns of it), the timer count and the earliest deadline
(`ev_timer_next`, a call) and compares against an estimate anchored on the last real reading. A
quiet socket between two polls: 36 → 16 ns over a plain pass — the same gate, its counter read
shared with the io cadence. The timer's precision is unchanged: the estimate only decides WHEN
the real readings start (2 ms of slack, a 0.1 % rate margin, a fresh anchor every second at most);
within that window each pass reads `ev_clock_now` (17 ns) and hands the reading to the loop
(`ev_now_set`) when the timer is due, so the pass that fires it reads the clock once.

## qev's pass, by shape (`bench-pass`, ns per pass, three runs)

| shape | ns |
|---|---:|
| `timer` — `ev_run(EVRUN_NOWAIT)` over a far timer | 21.4 – 21.8 |
| `timer+set` — the same, the embedder reading the clock and handing it over | 21.2 – 22.1 (the read moved, not saved) |
| `timer+set0` — a free sample: the loop's bookkeeping floor | **9.7 – 11.0** |
| `gate` — no `ev_run`: count, deadline, wake flag | **1.8** |
| `now` — `ev_now_update` alone | 17.1 – 18.0 |
| `arm+now` — `ev_now_update` + start + stop | 20.4 – 21.3 |

The roadmap's phase-3 target (`timer` ≤ 10 ns given the clock) is the `timer+set0` row; qb takes the
`gate` row instead on every pass where nothing is due, and pays `timer+set` on the one that fires.

Suites at the branch: qb release 195/195 with the new `listener-timer-gate` (7 cases; each mechanism
disabled in turn is rejected: the wake flag, the pending events, a timer never within reach, the
timer wrapper's refresh), qev 73/73 (16 new checks; floor 35 → 48); the sanitizer presets, the
superproject presets and the Windows gate on the landed SHA: figures in the Huly comment.
