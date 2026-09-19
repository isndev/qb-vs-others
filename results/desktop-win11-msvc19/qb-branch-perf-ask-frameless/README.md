# qb `perf/ask-frameless` (Huly QB-214) — Windows 11, MSVC 19.51, 2026-09-18

Control = qb `develop` `7296ac8d` (clean worktree `D:\repo\qb-ctl-7296`, built as `build/ctl-7296`); candidate =
`91276be0` (`build/cnd-9127`, the branch working tree): `qb::ask` returns a frame-free awaitable that lives in the
awaiting coroutine's frame (`ask_operation<E>` / `ask_emplace_operation<E, Args...>`, convertible to `task<E>`).
One quiet session (WSL2 idle, Docker Desktop stopped), qb only, Release, native arch off, cpus 0,2; build
directories of the SAME name length; the probe run from ONE directory under same-length names (`x-ctl.exe`,
`x-cnd.exe`), pinned to CPU 2. Harness at `85e4277a` (idiom detected by callability — see the WSL README for the
first pass this corrected).

## The ask-cost probe (`probe-ask-cost.txt`, mode seconds=2 core_cpu=2 latency_us=0, ctl/cand alternated ×7)

| probe mode | control | candidate | Δ |
|---|---:|---:|---:|
| push (ns/trip, median of 7, min–max) | 31.15 (31.10–31.86) | 32.45 (32.14–32.99) | +4.2 % |
| ask (ns/trip, median of 7, min–max) | 63.97 (63.57–65.24) | 48.54 (48.29–48.73) | −24.1 % |
| stream (ns/trip, median of 7, min–max) | 69.12 (67.79–69.93) | 70.21 (68.78–71.41) | +1.6 % |

The ask mechanics above a bare push/reply round trip: **32.8 ns → 16.1 ns (−51 %)**. The probe's `push` and
`stream` modes do not touch `qb::ask`; their +4 % / +1.6 % is the candidate binary's code layout (the real-harness
ping-pong census below, the push path itself, is −1.9 %).

## The bank-transaction census (`census/`, 12 launches interleaved, 3 rep + 1 warmup each)

| cell | control (median of medians, min–max, n) | candidate | Δ |
|---|---:|---:|---:|
| bank-transaction 1c-spin | 228.8 (222.7–245.9, n=12) | 161.4 (155.1–167.3, n=12) | −29.5 % |
| bank-transaction 1c-park | 231.9 (224.6–258.1, n=12) | 162.5 (158.1–171.5, n=12) | −29.9 % |
| bank-transaction 2c-spin | 142.0 (133.3–149.2, n=12) | 106.1 (103.6–114.3, n=12) | −25.2 % |
| bank-transaction 2c-park | 142.7 (135.5–150.5, n=12) | 108.8 (104.5–114.4, n=12) | −23.7 % |

Every bank distribution is disjoint. Anchors and the ask-free cells that moved more than 5 % in one grid pass,
re-measured the same way (8 or 12 launches):

| cell | control | candidate | Δ |
|---|---:|---:|---:|
| ping-pong 1c-spin | 30.0 (29.2–30.4, n=8) | 29.5 (29.2–31.0, n=8) | −1.9 % |
| fib 1c-spin | 130.8 (126.3–133.5, n=8) | 127.8 (126.2–133.3, n=8) | −2.2 % |
| counting 1c-spin | 9.1 (8.2–12.2, n=8) | 9.4 (8.1–12.4, n=8) | bimodal, same modes |
| fork-join 2c-park | 10.4 (9.9–11.1, n=12) | 10.5 (9.5–13.1, n=12) | noise |
| counting 2c-spin | 11.8 (10.4–13.8, n=12) | 12.7 (10.1–13.9, n=12) | bimodal (10–14 both sides) |
| thread-ring 2c-spin | 91.9 (85.4–94.9, n=12) | 90.7 (85.3–96.6, n=12) | −1.3 % |
| big 2c-spin | 21.3 (19.4–24.5, n=12) | 22.5 (18.3–24.8, n=12) | noise (18–25 both sides) |

## The grids (`grid-ctl-7296ac8d`, `grid-cand-91276be0-pass{1,2}`, qb only, 9 + 2 repetitions, p50 ns/unit)

| cell | control | cand pass 1 | cand pass 2 | Δ (mean of passes) |
|---|---:|---:|---:|---:|
| bank-transaction 1c-spin | 227.1 | 173.6 | 159.5 | −26.7 % |
| bank-transaction 1c-park | 248.1 | 170.2 | 156.4 | −34.2 % |
| bank-transaction 2c-spin | 147.5 | 111.0 | 104.7 | −26.9 % |
| bank-transaction 2c-park | 142.0 | 102.3 | 108.7 | −25.7 % |
| ping-pong 1c-spin / 1c-park / 2c-spin / 2c-park | 30.0 / 30.0 / 182.4 / 192.9 | 29.9 / 29.9 / 181.6 / 198.1 | 29.9 / 29.5 / 178.7 / 198.2 | −0.3 / −1.1 / −1.2 / +2.7 % |
| thread-ring 1c-spin / 1c-park / 2c-spin / 2c-park | 18.6 / 18.1 / 87.8 / 107.2 | 18.3 / 18.3 / 94.4 / 98.6 | 18.2 / 18.2 / 94.6 / 103.3 | −1.9 / +0.7 / +7.6 / −5.8 % (2c: census above, noise) |
| counting 1c-spin / 1c-park / 2c-spin / 2c-park | 9.3 / 8.6 / 11.6 / 13.6 | 8.2 / 9.9 / 13.2 / 13.4 | 10.7 / 8.5 / 13.1 / 13.4 | +1.6 / +6.2 / +13.8 / −1.4 % (bimodal; census above) |
| chameneos 1c-spin / 1c-park / 2c-spin / 2c-park | 30.3 / 31.5 / 73.4 / 73.2 | 31.2 / 30.7 / 73.2 / 73.0 | 30.8 / 30.3 / 66.2 / 72.0 | +2.1 / −3.0 / −5.1 / −1.1 % |
| big 1c-spin / 1c-park / 2c-spin / 2c-park | 17.0 / 17.0 / 21.7 / 23.3 | 16.8 / 17.1 / 23.2 / 23.1 | 16.6 / 16.8 / 22.4 / 20.0 | −2.2 / −0.6 / +5.3 / −7.6 % (2c: census above, noise) |
| fib 1c-spin / 1c-park / 2c-spin / 2c-park | 124.1 / 128.2 / 84.1 / 80.9 | 124.5 / 124.6 / 83.3 / 83.8 | 125.5 / 126.9 / 83.0 / 83.8 | +0.7 / −1.9 / −1.1 / +3.6 % |
| fork-join 1c-spin / 1c-park / 2c-spin / 2c-park | 9.0 / 11.8 / 12.3 / 9.7 | 8.6 / 8.8 / 12.2 / 12.3 | 8.5 / 12.2 / 11.3 / 12.3 | −4.3 / −10.9 / −4.7 / +27.3 % (bimodal 9–12 both sides; census above) |

## Functional oracle, same tree

qb standalone `release` preset at `91276be0`, MSVC 19.51: 0 errors / 0 warnings, 199 registered / 199 executed /
0 skipped / 0 failed (198 + `request-operation`).
