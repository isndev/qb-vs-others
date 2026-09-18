# qb `perf/ask-frameless` (Huly QB-214) — WSL2 Debian 13, g++ 14.2, 2026-09-18

Control = qb `develop` `7296ac8d`; candidate = `91276be0` (`qb::ask` returns a frame-free awaitable that lives in
the awaiting coroutine's frame, `ask_operation<E>` / `ask_emplace_operation<E, Args...>`, convertible to `task<E>`).
One quiet session (Windows idle, Docker Desktop stopped), qb only, `-O3 -DNDEBUG`, native arch off, cpus 0,2.
Trees and harness directories of the SAME name length (`~/qb-ctl-7296` / `~/qb-cnd-9127`, `~/qvo/ctl-7296` /
`~/qvo/cnd-9127`): identical executable path lengths (the dev/bench lesson of 2026-09-17).

**Harness note.** A first pass measured the candidate +17 % on every bank-transaction config: the harness detected
the emplace-ask idiom with a concept keyed on the exact `task<Deposit>` return type, which the candidate no longer
has, and silently fell back to the by-value form — then wrapped it in the `task<Deposit>` that `deposit()` promised.
`frameworks/qb/savina/bank-transaction.cpp` now detects by callability and returns what `qb::ask` returns
(`85e4277a`); everything below is the second pass, both sides on the emplace idiom. The first pass's census and
grid directories were overwritten by the second; its probe run (same numbers as below) is in the Huly thread.

## The ask-cost probe (`probe-ask-cost.txt`, mode seconds=2 core_cpu=0 latency_us=0, ctl/cand alternated ×7)

| probe mode | control | candidate | Δ |
|---|---:|---:|---:|
| push (ns/trip, median of 7, min–max) | 23.62 (23.43–24.13) | 23.17 (22.93–24.30) | −1.9 % |
| ask (ns/trip, median of 7, min–max) | 45.16 (45.06–45.57) | 35.55 (35.47–35.70) | −21.3 % |
| stream (ns/trip, median of 7, min–max) | 24.05 (23.83–24.26) | 24.36 (24.24–25.07) | +1.3 % |

The ask mechanics above a bare push/reply round trip: **21.5 ns → 12.4 ns (−43 %)**. `stream` (`ask_stream`,
untouched) +1.3 % — code-layout noise of the probe binary; the real-harness anchors below are flat.

## The bank-transaction census (`census/`, 12 launches interleaved, 3 rep + 1 warmup each)

| cell | control (median of medians, min–max, n) | candidate | Δ |
|---|---:|---:|---:|
| bank-transaction 1c-spin | 136.2 (123.3–145.1, n=12) | 97.2 (92.2–101.3, n=12) | −28.6 % |
| bank-transaction 1c-park | 141.0 (137.0–144.5, n=12) | 98.5 (95.6–102.8, n=12) | −30.1 % |
| bank-transaction 2c-spin | 82.3 (78.5–87.5, n=12) | 72.7 (68.0–74.0, n=12) | −11.7 % |
| bank-transaction 2c-park | 83.5 (78.2–87.6, n=12) | 73.2 (66.8–79.9, n=12) | −12.3 % |

Every bank distribution is disjoint (control min > candidate max). Anchors and the ask-free cells that moved more
than 5 % in one grid pass, re-measured the same way (8 or 12 launches):

| cell | control | candidate | Δ |
|---|---:|---:|---:|
| ping-pong 1c-spin | 23.0 (22.9–23.4, n=8) | 23.0 (22.6–23.6, n=8) | −0.1 % |
| fib 1c-spin | 99.6 (97.4–102.1, n=8) | 99.4 (98.5–100.2, n=8) | −0.1 % |
| counting 1c-spin | 7.5 (7.3–8.1, n=8) | 7.6 (7.4–7.8, n=8) | +0.8 % |
| fork-join 1c-spin | 7.6 (7.2–8.6, n=12) | 7.5 (7.4–8.8, n=12) | noise |
| fork-join 1c-park | 7.4 (7.3–8.5, n=12) | 7.8 (7.3–9.9, n=12) | noise (bimodal) |
| counting 2c-spin | 9.4 (9.1–9.9, n=12) | 9.4 (9.2–9.8, n=12) | 0 |
| thread-ring 2c-spin | 76.3 (74.8–78.4, n=12) | 76.0 (74.5–79.4, n=12) | −0.4 % |

## The grids (`grid-ctl-7296ac8d`, `grid-cand-91276be0-pass{1,2}`, qb only, 9 + 2 repetitions, p50 ns/unit)

| cell | control | cand pass 1 | cand pass 2 | Δ (mean of passes) |
|---|---:|---:|---:|---:|
| bank-transaction 1c-spin | 138.8 | 97.5 | 98.7 | −29.3 % |
| bank-transaction 1c-park | 140.8 | 96.9 | 96.4 | −31.4 % |
| bank-transaction 2c-spin | 79.2 | 72.6 | 72.9 | −8.1 % |
| bank-transaction 2c-park | 81.4 | 68.5 | 72.2 | −13.6 % |
| ping-pong 1c-spin / 1c-park / 2c-spin / 2c-park | 22.8 / 22.6 / 161.6 / 157.4 | 22.7 / 22.9 / 161.1 / 153.2 | 22.3 / 22.3 / 152.1 / 156.1 | −1.4 / 0.0 / −3.1 / −1.8 % |
| thread-ring 1c-spin / 1c-park / 2c-spin / 2c-park | 16.9 / 17.1 / 78.3 / 73.3 | 17.1 / 17.1 / 74.5 / 73.9 | 17.0 / 16.9 / 74.3 / 73.4 | +0.5 / −0.8 / −5.0 / +0.5 % |
| counting 1c-spin / 1c-park / 2c-spin / 2c-park | 7.4 / 7.5 / 9.1 / 9.3 | 7.5 / 7.6 / 9.3 / 9.5 | 7.4 / 7.3 / 9.8 / 9.4 | +0.4 / −0.8 / +4.2 / +1.6 % |
| chameneos 1c-spin / 1c-park / 2c-spin / 2c-park | 25.8 / 25.7 / 44.3 / 44.9 | 25.8 / 25.7 / 43.6 / 46.6 | 25.6 / 26.1 / 43.4 / 44.2 | −0.5 / +0.8 / −1.8 / +1.0 % |
| big 1c-spin / 1c-park / 2c-spin / 2c-park | 18.3 / 17.7 / 17.9 / 18.2 | 18.3 / 17.8 / 18.5 / 17.6 | 17.7 / 17.7 / 17.8 / 17.6 | −1.5 / +0.2 / +1.2 / −3.2 % |
| fib 1c-spin / 1c-park / 2c-spin / 2c-park | 100.0 / 99.1 / 56.1 / 56.6 | 97.7 / 99.5 / 57.4 / 57.4 | 100.9 / 98.4 / 57.3 / 57.4 | −0.7 / −0.1 / +2.1 / +1.5 % |
| fork-join 1c-spin / 1c-park / 2c-spin / 2c-park | 7.3 / 7.2 / 8.3 / 8.3 | 7.4 / 8.6 / 8.8 / 8.2 | 8.7 / 7.2 / 8.7 / 8.1 | +10.6 / +9.4 / +6.0 / −1.4 % (bimodal; census above: noise) |

## dev/bench's `qb-core-bench-ask-roundtrip` (`bench-ask-roundtrip.txt`, ctl/cand alternated ×3, 5 reps, GB median, pinned 2,4)

| benchmark | control (3 passes) | candidate (3 passes) |
|---|---:|---:|
| BM_Ask_RoundTrip_SameCore | 5.6 / 5.5 / 5.5 ms | 5.1 / 5.1 / 5.1 ms (−8 %) |
| BM_Ask_RoundTrip_CrossCore | 14.7 / 14.5 / 14.5 ms | 14.4 / 14.1 / 14.3 ms (−2 %) |

## Functional oracle, same tree (stage 0 of the session)

qb standalone Release suite at `91276be0`: clang 19.1.7 213 TUs / 0 warnings / 203 registered-203 executed-0 skipped-0
failed; g++ 14.2 213 TUs / 0 warnings / 203-203-0-0 (202 + `request-operation`, the suite that pins every shape of the
new type).
