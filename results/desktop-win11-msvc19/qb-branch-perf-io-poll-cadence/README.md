# qb branch `perf/io-poll-cadence` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-191** (a core polls a quiet socket on a cadence, not
on every pass; qev `perf/nopoll-pass`, `EVRUN_NOPOLL`), measured on 2026-09-08 ending
**07:58 UTC** in one quiet session (no build during the points, no leftover probe, Docker
Desktop quit), the WSL2 side idle — its own run had ended at 07:54. Control = `build/ab189-cand`
against `D:\repo\qb-189-dev` (= `develop` `dc91e6bd` after QB-189, with its ev copy), candidate
= `build/ab191-cand` against `D:\repo\qb-191-dev` (the branch, final code, the branch's qev).
Same flags as the published directories (`/O2 /Ob2 /DNDEBUG`), CPU 0, candidate and control
alternated five times, 1.5 s per point, then the candidate alone with the interval set to **0**
(three times) as the knob's negative control.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-io-pass` `pass` (ns per pass with one quiet loopback socket registered) and `wake` (the latency of an 8-byte write on that socket from a peer thread, every 100 µs and every 20 µs: p50 / p99 / max), `qvoprobe-ask-cost` `push` and `ask` with a 500 ms timeout, `qvoprobe-pass-cost` k = 1 — cand / ctl × 5; then `cand` with `poll_interval_us = 0` × 3. |

None of it is merged into the published tables.

## The probes (one core, medians of five)

| probe | control (`dc91e6bd`) | **branch** | Δ |
|---|---:|---:|---:|
| **pass with a quiet socket (ns)** | 275.0 | **83.1** | **−70 %** |
| wake latency, a byte every 100 µs — p50 (µs) | 17.7 | 18.9 | inside the spread (both bimodal between ~10 and ~20; the same binary at interval 0 reads 20.1) |
| wake latency, a byte every 20 µs — p50 (µs) | 18.1 | 18.9 | inside the spread |
| wake latency — p99 (µs) | 60–680 | 76–300 | noisy on both, no conclusion (the writer thread's sleep, not the reader) |
| push (ns per trip) | 31.1 | 31.1 | level |
| ask with a 500 ms timeout (ns per trip) | 90.2 | 89.6 | level |
| `pass-cost` k = 1 (ns per pass, no watcher) | 15.8 | 15.7 | level |
| **the same candidate with the interval set to 0** — pass / wake p50 | — | 272.5 / 20.1 | reads the control: the knob is the whole effect |

What the pass paid for one registered socket before: **275 − 16 = 259 ns**, of which the IOCP
wait behind wepoll's `epoll_wait(0)` is ~245 — the largest single item any qb pass paid on any
host, on every pass of every core that owns a socket, for a call that on a quiet socket returned
nothing. A cold pass now: **83 − 16 = 67 ns** over a plain pass (the `ev_run` bookkeeping with
the precise clock read of QB-193, ~31, the `__rdtsc` ~5, the listener's calls). Two regimes in
the run are worth stating rather than smoothing: the control's first two points read 425 and 409
(the session's first minute; the next three 274–275), the candidate's last two 70.5 against
83–85 for the first three — both sides' medians sit in their steadier regime.

The wake latency on this host is ~18 µs, five times WSL2's, and it is not the reader's: the
peer thread's `sleep_for(100 µs)` and the loopback path through AFD dominate, and both control
and candidate straddle two modes (~10 and ~20 µs) launch to launch. A 1 µs interval cannot be
resolved under it, and the interval-0 run reading 20.1 says the candidate's 18.9 is the host, not
the cadence.

Suites at the branch on this host: the two new binaries 6/6 (×15) and 3/3 on the incremental
`release` build; the full gate on the landed SHA: figures in the Huly comment.
