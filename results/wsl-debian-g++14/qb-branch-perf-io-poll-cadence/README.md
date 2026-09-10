# qb branch `perf/io-poll-cadence` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-191** (a core polls a quiet socket on a cadence, not on every pass; qev
`perf/nopoll-pass`, `EVRUN_NOPOLL`) against the `develop` it forks from (qb `dc91e6bd`, qev
`3c89e5a`: after QB-189), measured on 2026-09-08 **07:53–07:54 UTC** in one quiet window (load
1.4 at the start, no build, no other probe, the Windows side idle). The candidate is
`~/qvo/cand-191` built against `~/qb-191` (a clean LF clone at `dc91e6bd` plus the branch's
patch and the branch's qev), the control `~/qvo/cand-189`, the same clone at `dc91e6bd`; same
flags (`-O3 -DNDEBUG`), CPU 0, candidate and control alternated five times, 1.5 s per point,
then the candidate alone with the interval set to **0** (three times) as the knob's negative
control.

| file | what |
|---|---|
| `probe.txt` | the new `qvoprobe-io-pass` `pass` (ns per pass with one quiet loopback socket registered) and `wake` (the latency of an 8-byte write on that socket from a peer thread, every 100 µs and every 20 µs: p50 / p99 / max), `qvoprobe-ask-cost` `push` and `ask` with a 500 ms timeout, `qvoprobe-pass-cost` k = 1 — cand / ctl × 5; then `cand` with `poll_interval_us = 0` × 3. |

None of it is merged into the published tables.

## The probes (one core, medians of five)

| probe | control (`dc91e6bd`) | **branch** | Δ |
|---|---:|---:|---:|
| **pass with a quiet socket (ns)** | 124.2 | **48.5** | **−61 %** |
| wake latency, a byte every 100 µs — p50 (µs) | 3.36 | 3.93 | +0.57 (half the 1 µs interval, as designed) |
| wake latency, every 100 µs — p99 (µs) | 25.1 | 23.2 | level |
| wake latency, a byte every 20 µs — p50 / p99 (µs) | 3.30 / 22.3 | 3.78 / 23.5 | +0.48 / level |
| push (ns per trip) | 23.7 | 23.9 | level |
| ask with a 500 ms timeout (ns per trip) | 67.9 | 68.6 | inside the spread (cand 68.3–69.4, ctl 67.3–68.7; no socket, the cadence code is not entered) |
| `pass-cost` k = 1 (ns per pass, no watcher) | 12.4 | 12.3 | level |
| **the same candidate with the interval set to 0** — pass / wake p50 | — | 126.1 / 3.31 | reads the control: the knob is the whole effect |

What the pass paid for one registered socket before: **124 − 12 = 112 ns** — the `epoll_wait(0)`
~80, the rest of `ev_run` (the clock read, the timer heap, the pipe check) ~22, the listener's
gate and calls the remainder — and every one of those polls returned nothing. Now a cold pass
runs `ev_run(EVRUN_NOWAIT | EVRUN_NOPOLL)`: **48 − 12 = 36 ns** over a plain pass, of which the
`tsc_ticks` read is ~5 and the loop's own bookkeeping ~22 (the next cut, with QB-190: skip
`ev_run` altogether when no timer is due). A hot pass — the previous poll found something — still
polls on every pass, so a burst on the socket sees the old latency; the interval only bounds how
long a QUIET socket's first byte waits, and the p50 moved by exactly the interval's half.

The wake shape at 20 µs gaps is a witness that a "hot" socket does not stay hot on its own: 20 µs
between bytes is 400 passes, so every byte finds the loop cold and pays the same half-interval —
the loop is hot for exactly one pass after each delivery, which is the design (a burst is bytes
arriving back to back, not bytes 20 µs apart).

Suites at the branch: qb release on WSL2 195/195 (the two new binaries, six and three cases),
qev 57/57 × 6; the sanitizer presets, the superproject presets and the Windows gate on the
landed SHA: figures in the Huly comment.
