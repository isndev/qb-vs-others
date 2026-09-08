# qb branch `perf/ask-inline-resume` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-185** and **QB-187** (the WSL2 half, with the reading,
is `../../wsl-debian-g++14/qb-branch-perf-ask-inline-resume/`): the qb branch on which an `ask`
reply resumes the waiting coroutine inline from the handler that routed it, and whose qev copy
reads its clocks through libc and skips the backend poll over a loop with no fd. Measured against
the `develop` it forks from (`4a0b62be`, `D:\repo\qb-ctl-4a0b62be`, a clean clone, 0 dirty) on
`savina/bank-transaction`, the two ask `dev/bench` binaries and two probes. Same host, CPUs and
flags as the published directories beside this one: `/O2 /Ob2 /DNDEBUG`, CPUs 0,2, **9
repetitions + 2 warmup**, qb-only builds (`build/ab185-cand` / `build/ab185-ctl`), the WSL2 side
idle throughout each window:

| directory | qb at | what |
|---|---|---|
| `grid-worktree/`, `grid-worktree-pass2/` | **the branch** (QB-185 alone at that hour) — measured first and third | **4 cells** each (bank × {1c-spin, 1c-park, 2c-spin, 2c-park}), qb only, all verified. 2026-09-07 23:28:45–23:28:47 UTC. |
| `grid-4a0b62be/` | `develop` `4a0b62be` — the control, measured second | same 4 cells, same session. |
| `census/` | branch vs `4a0b62be`, **10 interleaved launches** each, 3 reps + 1 warmup, on the four bank cells and the ping-pong 1c anchor | 23:28:47–23:28:58 UTC. |
| `probe.txt` | first block (23:28:58 UTC): `ask-cost` push / ask / stream and `pass-cost`, cand/ctl × 5, QB-185 alone; second block (2026-09-08 00:12 UTC, a second quiet window after the qev fixes): ask, ask **with a 500 ms timeout**, one- and 64-chunk streams with a timeout, `pass-cost`, cand/ctl × 5 | |
| `bench/` | `qb-core-bench-ask-roundtrip` (same-core, cross-core) and `BM_Mono_PingPong_Latency`, candidate (with the qev fixes) and control alternated three times, 2026-09-08 00:20 UTC | |

None of the grids is merged into the published tables.

## The probes (ns per round trip, cand / ctl medians of five)

| probe | control `4a0b62be` | **branch** | Δ |
|---|---:|---:|---:|
| push | 34.2 | 33.6 | level |
| ask | 81.7 | **72.0** | **−12 %** (the machinery over push 47.5 → 38.4, −19 %) |
| stream, 64 chunks (per chunk) | 76.1 | 75.7 | level |
| stream, 1 chunk | 276 | 276 | level (one 456 outlier on the branch) |
| `pass-cost` k = 1 | 16.5 | 16.5 | level |
| **ask with a 500 ms timeout** | **1108** | **124** | **−89 %** |
| stream, 1 chunk, with a 500 ms timeout | 973 (bimodal 874 … 1110) | **330** | −66 % |
| stream, 64 chunks, with a 500 ms timeout (per chunk) | 88.4 | 76.5 | −13 % |

On MSVC the raw-syscall clock path does not exist; the whole of the timed ask's excess was
wepoll's `epoll_wait(0)` — an IOCP call — on every pass while the timer was pending, three
passes per round trip. `qev/bench/bench-pass.c` is the per-pass instrument (its Windows figures
are the next thing to record).

## The grids and the census (bank-transaction, p50 per transfer, ns; QB-185 alone)

| cell | `4a0b62be` | **branch** p1 / p2 | census (10 launches) |
|---|---:|---:|---|
| 1c-spin | 258.7 | 251.0 / 253.6 | 261.7 → **250.6** (−4 %) |
| 1c-park | 260.3 | 266.4 / 249.1 | 260.8 → **250.1** (−4 %) |
| 2c-spin | 151.4 | 147.0 / 149.3 | 153.7 → 152.7 (−1 %) |
| 2c-park | 150.4 | 154.8 / 147.9 | 154.7 → **147.4** (−5 %) |
| ping-pong 1c-spin (anchor) | — | — | 31.9 → 31.8 (level) |

## `dev/bench` — the ask cells (median of three run medians; `bench/`)

| cell | `4a0b62be` | **branch** | Δ |
|---|---:|---:|---:|
| `BM_Ask_RoundTrip_SameCore` (ms per 50 000 asks, each with a 500 ms timeout) | 36.7 (bimodal 36 … 50) | **8.6** | **−77 %** |
| `BM_Ask_RoundTrip_CrossCore` | 48.7 | **30.1** | **−38 %** |
| `BM_Mono_PingPong_Latency` (same-core, no watcher) | 69.4 | 70.5 | level |

Suite at the branch: `dev/agent/verify-windows.ps1` — the numbers are in the Huly comment and
the qb `CHANGELOG.md` `[Unreleased]` entry.
