# qb branch `perf/ask-inline-resume` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-185** (an `ask` reply resumes the waiting coroutine inline from the handler
that routed it) and **QB-187** (qev: the clock read through libc's `clock_gettime` instead of the
raw syscall, and no backend poll over a loop with no fd), measured against the `develop` the
branch forks from (`c4f9d439`) on `savina/bank-transaction` — the one grid cell built on `ask` —
plus the two ask `dev/bench` binaries and two probes. Same host, CPUs and build flags as the
published directories beside this one: `-O3 -DNDEBUG`, `taskset -c 0,2`, **9 repetitions + 2
warmup**, qb-only builds, candidate / control / candidate in ONE quiet session on 2026-09-08, the
Windows side idle throughout. The candidate is `~/qvo/cand-pass`, built against the working tree
of the branch (qb + its qev copy); the control `~/qvo/v2`, a clean LF clone whose code is
`c4f9d439`'s.

| directory | qb at | what |
|---|---|---|
| `grid-worktree/`, `grid-worktree-pass2/` | **the branch** — measured first and third | **4 cells** each (bank-transaction × {1c-spin, 1c-park, 2c-spin, 2c-park}), qb only, all verified. 00:01:28–00:01:29 UTC. |
| `grid-c4f9d439/` | `develop` `c4f9d439` — the control, measured second | same 4 cells, same session. |
| `census/` | branch vs `c4f9d439`, **10 interleaved launches** each, 3 reps + 1 warmup, on the four bank cells and the ping-pong 1c anchor | 00:01:29–00:01:34 UTC. |
| `probe.txt` | `qvoprobe-ask-cost` push / ask / stream (64 chunks, 1 chunk) and `qvoprobe-pass-cost` k = 1, candidate and control alternated five times (00:01:34–00:02:50 UTC); then, in a second quiet window at 00:15 UTC, the same ask and one-chunk stream **with a 500 ms timeout**, five alternations | |
| `bench/` | `qb-core-bench-ask-roundtrip` (same-core and cross-core) and `BM_Mono_PingPong_Latency`, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions) | 00:02:50–00:04:05 UTC. |

None of the grids is merged into the published tables.

## The probes (ns per round trip, cand / ctl medians of five)

| probe | control `c4f9d439` | **branch** | Δ |
|---|---:|---:|---:|
| push (two passes, nothing else) | 24.4 | 24.3 | level |
| ask | 54.0 | **46.7** | **−14 %** (the machinery over push 29.7 → 22.4, −25 %) |
| stream, 64 chunks (per chunk) | 25.3 | 25.6 | level (one wake per burst) |
| stream, 1 chunk (per stream) | 106.5 | 106.2 | level |
| `pass-cost` k = 1 (one core, ns per pass) | 12.8 | 12.8 | level |
| **ask with a 500 ms timeout** | **798** | **172** | **−78 %** |
| stream, 1 chunk, with a 500 ms timeout | 860 | **238** | −72 % |

The first block is QB-185 alone (the qev fixes touch nothing without a watcher); the last two
rows are QB-187: a pending timer runs `ev_run(EVRUN_NOWAIT)` on every pass, and each of those
passes paid two raw `clock_gettime` syscalls (~95 ns each) and an `epoll_wait(0)` over no fd.
`qev/bench/bench-pass.c` gives the per-pass figures (297 → 50 ns for a timers-only loop) and
`docs/TUNING.md` §17 the reading.

## The grids and the census (bank-transaction, p50 per transfer, ns)

| cell | `c4f9d439` | **branch** p1 / p2 | census (10 launches) |
|---|---:|---:|---|
| 1c-spin | 142.05 | 135.37 / 134.13 | 142.3 → **137.7** (−3 %) |
| 1c-park | 138.35 | 152.37 / 132.68 | 140.8 → 138.5 (−2 %) |
| 2c-spin | 91.80 | **80.55 / 84.04** | 92.4 → **84.0** (**−9 %**) |
| 2c-park | 92.30 | 87.10 / 84.09 | 93.2 → 88.3 (−5 %) |
| ping-pong 1c-spin (anchor) | — | — | 23.0 → 23.1 (level) |

bank asks with `duration::zero()`, so it sees QB-185 only: one scheduler trip less per
transfer (two hash-set operations, a queue trip, the `run()` entry) — ~5–8 ns of its 85 ns of
per-transfer machinery.

## `dev/bench` — the ask cells (median of three run medians; `bench/`)

| cell | `c4f9d439` | **branch** | Δ |
|---|---:|---:|---:|
| `BM_Ask_RoundTrip_SameCore` (ms per 50 000 asks, each with a 500 ms timeout) | 42.3 | **11.1** | **−74 %** (846 → 222 ns per ask) |
| `BM_Ask_RoundTrip_CrossCore` | 46.5 | **18.9** | **−59 %** |
| `BM_Mono_PingPong_Latency` (same-core, no watcher) | 59.1 | 59.1 | level |

This is the cell that had been reading ~700–850 ns per ask since it was written and was taken
for its own instrumentation (a 500 ms timer plus three clock reads per ask); it was the loop.

Suites at the branch: WSL2 release / ASan+UBSan / TSan 192/192 ×3 (the ASan run is what found
the teardown leak QB-185's inline completion exposed — see the qb `CHANGELOG.md`), the
superproject sanitizer presets and the Windows gate: figures in the Huly comments.
