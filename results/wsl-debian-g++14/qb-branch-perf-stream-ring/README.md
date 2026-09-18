# qb `perf/stream-ring` (Huly QB-215) — WSL2 Debian 13, g++ 14.2 (suites also clang 19.1.7), 2026-09-18

Control = qb `develop` `6712ef30`, candidate = `6632987e`: `qb::growable_ring` replaces the `std::deque`s of the
coroutine layer (the stream's chunk buffer, the channel's value buffer and its three waiter lists, the waiter lists
of `semaphore`, `async_mutex`, `async_rw_lock` and `async_event`). Both trees exported by `git archive` into
same-length paths (`~/qb-ctl-6712`, `~/qb-cnd-6632`), the harness built without native arch, qb's own benchmarks
with it; one quiet session (Windows side idle, Docker Desktop stopped), the benchmarks pinned to CPU 2.
The Windows half of the same change is `results/desktop-b67osn6-win-msvc/qb-branch-perf-stream-ring/README.md`.

**Why Linux is the control of the control.** libstdc++ packs 512 bytes per deque block, so the deque was already
amortised here: the probe's `stream` mode sat at 24 vs 24 before the change (Windows: 69 vs 31). This host answers
"does the ring cost anything where the deque was fine?" — and, on the channel's try-send / try-recv loop, whether a
pointer-cursor ring keeps up with a deque's block cursor (the first, index-based ring did not: +21 to +33 % there).

## The candidate's suites (Stage 0, standalone qb `Release`, `QB_BUILD_TESTS=ON`)

| compiler | build | ctest |
|---|---|---|
| clang 19.1.7 | 214 TUs, 0 warnings | 204 registered / 204 executed / 0 skipped / 0 failed |
| g++ 14.2 | 214 TUs, 0 warnings | 204 registered / 204 executed / 0 skipped / 0 failed |

## The ask-cost probe (`probe-ask-cost.txt`, mode seconds=2 core_cpu=0 latency_us=0, ctl/cand alternated ×7)

| probe mode | control | candidate | Δ |
|---|---:|---:|---:|
| push (ns/trip, median of 7, min–max) | 22.95 (22.91–23.73) | 22.60 (22.56–23.58) | −1.5 % |
| ask (ns/trip, median of 7, min–max) | 35.69 (35.57–36.22) | 35.62 (35.51–42.10) | −0.2 % |
| stream (ns/trip, median of 7, min–max) | 24.67 (24.36–24.78) | 24.65 (24.43–24.85) | −0.1 % |

Flat, as the premise predicted: with 512-byte blocks the deque allocated once per 8 chunks, and a ring that
allocates once per doubling has nothing left to remove.

## qb's own benchmarks (`bench/`, ctl/cand alternated ×3, 3 GB repetitions each, medians of the 3 passes)

| cell | control | candidate | Δ |
|---|---:|---:|---:|
| BM_Sync_Semaphore_Contended coros 1 / 8 / 64 / 512 | 0.41 / 0.56 / 1.71 / 12.40 µs | 0.41 / 0.55 / 1.68 / 12.34 µs | −1.8 / −1.2 / −1.4 / −0.4 % |
| BM_Sync_Semaphore_Uncontended coros 1 / 8 / 64 / 512 | 0.42 / 0.56 / 1.86 / 12.35 µs | 0.40 / 0.55 / 1.82 / 12.34 µs | −3.5 / −1.0 / −1.8 / −0.1 % |
| BM_Sync_AsyncMutex coros 1 / 8 / 64 / 512 | 0.42 / 0.65 / 2.36 / 17.62 µs | 0.41 / 0.65 / 2.43 / 17.77 µs | −1.6 / +0.5 / +2.8 / +0.8 % |
| BM_Sync_RwLock_Read coros 1 / 8 / 64 / 512 | 0.43 / 0.62 / 2.14 / 16.41 µs | 0.41 / 0.61 / 2.16 / 16.70 µs | −3.6 / −0.9 / +0.9 / +1.8 % |
| BM_Sync_RwLock_Write coros 1 / 8 / 64 / 512 | 0.43 / 0.66 / 2.44 / 18.46 µs | 0.41 / 0.67 / 2.52 / 18.84 µs | −3.9 / +1.2 / +3.2 / +2.0 % |
| BM_Sync_Latch arrivers 1 / 8 / 64 / 512 (a `std::vector`, untouched) | 0.45 / 0.56 / 1.48 / 10.10 µs | 0.45 / 0.58 / 1.52 / 10.20 µs | −0.5 / +3.1 / +2.6 / +1.0 % |
| BM_Channel_TrySendTryRecv messages 64 / 1024 / 8192 | 147.4 / 2567 / 21249 ns | 140.6 / 1871 / 14816 ns | −4.6 / **−27.1 / −30.3 %** |
| BM_Channel_SendRecv messages 64 / 512 / 2048 | 2.25 / 14.67 / 57.10 µs | 2.26 / 14.46 / 56.34 µs | +0.5 / −1.5 / −1.3 % |
| BM_Stream_FilterMapReduce items 64 / 512 / 4096 (untouched code) | 2.28 / 13.57 / 103.69 µs | 2.27 / 13.44 / 102.55 µs | −0.8 / −1.0 / −1.1 % |
| BM_Stream_MapCollect items 64 / 512 / 4096 (untouched code) | 2.36 / 14.12 / 107.90 µs | 2.47 / 15.07 / 115.34 µs | +4.7 / +6.7 / +6.9 % |
| BM_Generator_MapFilter items 64 / 512 / 4096 (untouched code) | 2.73 / 18.10 / 143.50 µs | 2.71 / 18.15 / 143.16 µs | −0.9 / +0.3 / −0.2 % |

Reading it: the sync primitives sit within the ±3 % band the untouched latch cell draws on the same binary — a
ring and a 512-byte-block deque cost the same per park here, as they should. The channel's try-send / try-recv
loop gains −27 / −30 % from 1024 messages up: past 64 elements the deque walks its block map on every push and
pop (a two-level indirection, a new block every 64 handles or 8 values of 64 bytes), the ring bumps a pointer.
`BM_Stream_MapCollect` (`range_stream().map().collect()` into a `std::vector`: no channel, no semaphore, no ring —
`stream.h` merely includes the changed headers) moved +5 to +7 %, systematic across the three passes, while its
twin `BM_Stream_FilterMapReduce` on the same `_next` chain sits at −1 % and the same cell on Windows at −0.4 to
−4.4 %: a code-placement effect of the rebuilt binary, the mirror of the MSVC-only generator drift (+5 to +8 %,
flat here) recorded in the Windows README. Noted, not explained away.
