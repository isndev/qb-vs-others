# qb `perf/stream-ring` (Huly QB-215) — Windows 11, MSVC 19.51, 2026-09-18

Control = qb `develop` `6712ef30` (clean worktree `D:\repo\qb-ctl-7296` checked out there, built as `build/ctl-6712`);
candidate = `6632987e` (`build/cnd-6632`): `qb::growable_ring` replaces the `std::deque`s of the coroutine layer —
the stream's chunk buffer, the channel's value buffer and its three waiter lists, the waiter lists of `semaphore`,
`async_mutex`, `async_rw_lock` and `async_event`. One quiet session (WSL2 idle, Docker Desktop stopped), Release,
native arch off for the harness, on for qb's own benchmarks; same-length build directory names; the probe run from
ONE directory under same-length names (`x-ctl.exe`, `x-cnd.exe`), pinned to CPU 2.

**Why.** MSVC's STL packs `sizeof(T) <= 1 ? 16 : <= 2 ? 8 : <= 4 ? 4 : <= 8 ? 2 : 1` elements per deque block
(`<deque>`, `_Deque_val::_Block_size`): one 64-byte event per block, two coroutine handles per block — a heap
allocation and a free per chunk or per park. libstdc++ packs 512 bytes per block. The probe's `stream` mode showed it
first: 69 ns per chunk here against 31 for a bare push, while WSL2 sat at 24 vs 24.

## The ask-cost probe (`probe-ask-cost.txt`, mode seconds=2 core_cpu=2 latency_us=0, ctl/cand alternated ×7)

| probe mode | control | candidate | Δ |
|---|---:|---:|---:|
| push (ns/trip, median of 7, min–max) | 32.37 (31.95–32.47) | 32.91 (32.68–33.32) | +1.7 % |
| ask (ns/trip, median of 7, min–max) | 49.01 (48.89–49.19) | 50.19 (49.95–50.53) | +2.4 % |
| stream (ns/trip, median of 7, min–max) | 72.25 (70.98–74.61) | 36.26 (35.92–36.39) | **−49.8 %** |

`push` and `ask` touch no ring; their +1.7 % / +2.4 % is the candidate binary's code placement (the same drift the
QB-214 run showed on this probe), and the real-harness censuses below, the same two paths, are flat.

## qb's own benchmarks (`bench/`, ctl/cand alternated ×3, 3 GB repetitions each, medians of the 3 passes)

| cell | control | candidate | Δ |
|---|---:|---:|---:|
| BM_Sync_Semaphore_Contended coros 1 / 8 / 64 / 512 | 0.31 / 0.51 / 2.13 / 16.32 µs | 0.30 / 0.50 / 2.13 / 16.59 µs | −4.6 / −2.2 / −0.1 / +1.7 % |
| BM_Sync_AsyncMutex coros 1 / 8 / 64 / 512 | 0.33 / 0.87 / 4.63 / 34.21 µs | 0.31 / 0.72 / 3.49 / 26.38 µs | −4.2 / **−17.1 / −24.6 / −22.9 %** |
| BM_Sync_RwLock_Write coros 1 / 8 / 64 / 512 | 0.33 / 0.87 / 4.57 / 33.80 µs | 0.32 / 0.73 / 3.56 / 26.53 µs | −5.0 / **−16.3 / −22.1 / −21.5 %** |
| BM_Sync_Latch arrivers 512 (a `std::vector`, untouched) | 14.60 µs | 14.79 µs | +1.3 % |
| BM_Channel_TrySendTryRecv messages 64 / 1024 / 8192 | 286.5 / 4527 / 36606 ns | 215.9 / 3401 / 30760 ns | **−24.7 / −24.9 / −16.0 %** |
| BM_Channel_SendRecv messages 64 / 512 / 2048 | 2.94 / 14.45 / 53.61 µs | 2.05 / 13.06 / 50.62 µs | **−30.1 / −9.6 / −5.6 %** |
| BM_Generator_MapFilter items 64 / 512 / 4096 (untouched code) | 3.11 / 17.94 / 138.8 µs | 3.26 / 19.17 / 149.8 µs | +4.6 / +6.8 / +7.9 % |

The generator cells use no ring and no changed code (`generator.h`, `stream.h`); their +5 to +8 % is systematic
across the three passes on this host only (WSL2: −0.2 %), a code-placement effect of the binary under MSVC — noted,
not explained away. The JSON files of the six runs are in `bench/`; the table is their medians.

## The real harness, censused (`census/`, 12 or 8 launches interleaved, 3 rep + 1 warmup each)

| cell | control (median of medians, min–max, n) | candidate | Δ |
|---|---:|---:|---:|
| bank-transaction 1c-spin (the ask path) | 162.7 (157.2–177.4, n=12) | 163.4 (159.5–181.3, n=12) | +0.4 %, overlapping |
| bank-transaction 2c-spin | 110.8 (106.1–120.8, n=12) | 109.0 (105.2–128.0, n=12) | −1.6 %, overlapping |
| ping-pong 1c-spin (the push path) | 30.2 (29.8–30.3, n=8) | 29.9 (29.5–30.8, n=8) | −1.0 % |
| fib 1c-spin | 126.6 (120.8–131.7, n=8) | 124.8 (121.5–130.0, n=8) | −1.4 % |

## Functional oracle, same tree

qb standalone `release` preset at `6632987e`, MSVC 19.51: 0 errors / 0 warnings, 200 registered / 200 executed /
0 skipped / 0 failed (199 + `growable-ring`).

## The two earlier passes this supersedes

`223cadeb` (the stream only, a local ring) and `c6244309` (the shared ring in channel and sync, index cursors): the
index form measured the same on Windows but cost g++ +21 to +33 % on `BM_Channel_TrySendTryRecv` (five member loads
and a mask per push against a deque's two-pointer cursor); the pointer-cursor ring of `6632987e` is what both hosts
were re-measured with.
