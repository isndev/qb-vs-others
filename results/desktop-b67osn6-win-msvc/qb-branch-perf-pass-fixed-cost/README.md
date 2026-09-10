# qb branch `perf/pass-fixed-cost` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-182** (the WSL2 half, with the profile, the probe
decomposition and the per-step table, is `../../wsl-debian-g++14/qb-branch-perf-pass-fixed-cost/`):
the qb branch that walks the self pipe in place up to a fence instead of swapping a second pipe
in, resolves a `CoreId`'s outbound pipe with one indexed load, scans the peer pipes inline before
entering the flush drain, reads the io loop's counters inline (qev `ev_active_count_addr()` /
`ev_pending_count_addr()`) and keeps the broadcast walk out of the unicast route — measured
against the `develop` it forks from (`c42abddf`) on `savina/ping-pong`, `counting`, `thread-ring`,
`fork-join` and `big`, 4 configurations each, plus the four core `dev/bench` binaries and the
`pass-cost` probe. Same host, CPUs and flags as the published directories beside this one:
`/O2 /Ob2 /DNDEBUG`, CPUs 0,2, **9 repetitions + 2 warmup**, qb-only builds (`build/ab182-cand`
against the working tree that became `670e9433`, `build/ab182-ctl` against
`D:\repo\qb-ctl-c42abddf`, a clean clone at `c42abddf`, 0 dirty), candidate / control /
candidate in ONE quiet session on 2026-09-07 with the WSL2 side idle (its own session had ended
14:59 UTC):

| directory | qb at | what |
|---|---|---|
| `grid-670e9433/`, `grid-670e9433-pass2/` | **the branch head `670e9433`** — measured first and third | **20 cells** each, qb only, all verified: five shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. 15:16:10–15:16:50 UTC. |
| `grid-c42abddf/` | `develop` `c42abddf` — the control, measured second | same 20 cells, same session. |
| `census/` | `670e9433` vs `c42abddf`, **10 interleaved launches** each, 3 reps + 1 warmup, on the four 2c cells of ping-pong and thread-ring and the three 1c anchors | 15:16:50–15:17:59 UTC. |
| `bench/` | the four core `dev/bench` binaries, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions, every iteration recorded) | 15:17:59–15:21:40 UTC. |
| `probe.txt` | `qvoprobe-pass-cost` k = 1, 2, 4, candidate and control alternated three times, CPU 2, 2 s windows | 15:21:54 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when the final candidate is measured on all eight shapes.

## The probe on MSVC

k = 1 **20.4 → 15.6 ns** per pass (−24 %), k = 2 **28.4 → 22.9** (−19 %), k = 4 **44.8 → 37.0**
(−17 %): a fixed pass of ~12 ns → ~8 ns and a marginal event of ~8.1 → ~7.1 ns. MSVC's per-event
cost moves less than g++'s (8.9 → 4.2 there) and its pass more — the same source, the same
five changes, a different compiler's view of the same dependency chains; the g++ profile behind
the changes is in the other README, and the MSVC one is the next thing to take once a Windows
profiler joins the protocol (§13.2).

## The grids, same session (p50 per unit, ns; candidate pass 1 / pass 2 against the control)

| cell | `c42abddf` | **`670e9433`** p1 / p2 | Δ |
|---|---:|---:|---:|
| ping-pong 1c-spin (round trip) | 41.35 | **30.56 / 30.83** | **−26 %** |
| ping-pong 1c-park | 41.63 | **30.72 / 31.20** | **−26 / −25 %** |
| ping-pong 2c-spin | 247.9 | 246.1 / 238.3 | −1 / −4 % |
| ping-pong 2c-park | 259.9 | 246.2 / 229.6 | **−5 / −12 %** |
| thread-ring 1c-spin (hop) | 22.74 | **18.46 / 18.64** | **−19 / −18 %** |
| thread-ring 1c-park | 22.61 | **18.31 / 18.72** | **−19 / −17 %** |
| thread-ring 2c-spin | 122.2 | 126.5 / 124.5 | **+4 / +2 %** — the WSL2 README's one cell, here too |
| thread-ring 2c-park | 123.0 | 125.9 / 126.0 | **+2 %** |
| counting 1c-spin (message) | 12.52 | 8.71 / 12.17 | bimodal on both builds (§9.11; census below) |
| counting 1c-park | 9.04 | 11.85 / 12.17 | idem |
| counting 2c-spin | 13.86 | 13.38 / 13.58 | −3 / −2 % |
| counting 2c-park | 13.78 | 13.35 / 13.54 | −3 / −2 % |
| fork-join 1c-spin (message) | 8.88 | 8.64 / 8.41 | −3 / −5 % |
| fork-join 1c-park | 12.77 | 8.99 / 8.37 | −30 / −35 % (the control's 12.8 is its upper mode) |
| fork-join 2c-spin | 12.82 | 11.51 / 9.56 | −10 / −25 % |
| fork-join 2c-park | 10.74 | 11.47 / 11.63 | +7 / +8 % (10.7–12.8 is this cell's spread across both builds) |
| big 1c-spin (round trip) | 18.81 | 17.28 / 17.08 | −8 / −9 % |
| big 1c-park | 19.35 | 17.01 / 17.37 | **−12 / −10 %** |
| big 2c-spin | 23.79 | 24.18 / 20.06 | +2 / −16 % |
| big 2c-park | 23.39 | 24.02 / 24.44 | +3 / +5 % |

`census/`, ten interleaved launches, medians (min … max): ping-pong 1c-spin **31.0** vs 41.2
(**−25 %**); thread-ring 1c-spin **18.5** vs 22.9 (−19 %); counting 1c-spin 12.1 vs 9.1 with
both builds bimodal within a launch — control 8.5 … 12.5, head 8.3 … 12.4, the two modes §9.11
recorded for this host, the head's launches landing in the upper one seven times out of ten
here and the control's three; ping-pong 2c-spin **238.2** (225.3 … 248.4) vs 254.2 (245.2 …
284.6), **−6 %**; ping-pong 2c-park **230.3** vs 252.5, **−9 %**; thread-ring 2c-spin **127.2**
(114.0 … 131.2) vs 121.2 (116.9 … 126.8), **+5 %**; thread-ring 2c-park 123.9 vs 123.8, level.

The ring at two cores is the cell the WSL2 README bisects to the inline flush scan (a waiting
core's idle pass ~2 ns shorter), and it reads the same way on MSVC: +2 to +5 %, with the
two-actor ping-pong at two cores −6 to −9 %. Recorded, kept, and handed to QB-181 (what an idle
spin pass should do while it waits) as its most sensitive instrument. The counting 1c cells and
big / fork-join at two cores carry their known bimodality; every same-core cell of the five
shapes is ahead, ping-pong and thread-ring by a quarter and a fifth.

## `dev/bench` — the four core binaries (median of three run medians, ns; `bench/`)

| cell | `c42abddf` | **`670e9433`** | Δ |
|---|---:|---:|---:|
| `BM_Mono_PingPong_Latency` (same-core round trip) | 76.6 | **66.4** | **−13 %** |
| `BM_Multi_PingPong_Latency` (cross-core) | 293.8 | 300.1 | +2 % (both bimodal) |
| `BM_Reference_Multi_PingPong_Latency` (raw spsc, no qb code) | 241.4 | 260.1 | +8 % — the host, not the branch |
| pipeline chain, 10 actors / 1 core (per delivery) | 42.7 | **34.6** | **−19 %** |
| pipeline chain, 8 actors / 8 cores | 263.5 | **228.1** | **−13 %** |
| `BM_PINGPONG<TinyEvent>` 64 actors, 1 core (per round trip) | 29.4 | 26.5 | −10 % |
| `BM_PINGPONG<TinyEvent>` 64 actors, 8 cores | 26.0 | 24.0 | −8 % |
| `BM_Ask_RoundTrip_SameCore` | 35.0 | 33.5 | −4 % |
| `BM_Ask_RoundTrip_CrossCore` | 44.3 | 43.0 | −3 % |

Suite at the head: `dev/agent/verify-windows.ps1` — the numbers are in the Huly comment and the
qb `CHANGELOG.md` `[Unreleased]` entry.
