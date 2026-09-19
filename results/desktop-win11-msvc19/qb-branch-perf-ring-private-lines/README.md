# qb branch `perf/ring-private-lines` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-184** (the WSL2 half, with the raw-ring study, the
profile and the two censuses, is `../../wsl-debian-g++14/qb-branch-perf-ring-private-lines/`):
the qb branch that gives each side of the mailbox's SPSC ring a private line for its working
index and its snapshot of the peer's index, and leaves the published index alone on the line
the peer polls — so that a producer never reads the line it publishes on. Measured against the
`develop` it forks from (`670e9433`) on all eight shapes, 4 configurations each, plus the four
core `dev/bench` binaries and the two probes. Same host, CPUs and flags as the published
directories beside this one: `/O2 /Ob2 /DNDEBUG`, CPUs 0,2, **9 repetitions + 2 warmup**,
qb-only builds (`build/ab184-cand` against the working tree of the branch, `build/ab184-ctl`
against `D:\repo\qb-ctl-670e9433`, a clean clone at `670e9433`, 0 dirty), candidate / control /
candidate in ONE quiet session on 2026-09-07 with the WSL2 side idle (its own session had ended
18:48 UTC and its suites 18:56):

| directory | qb at | what |
|---|---|---|
| `grid-worktree/`, `grid-worktree-pass2/` | **the branch** — measured first and third | **32 cells** each, qb only, all verified: eight shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. 18:59:20–19:00:04 UTC. |
| `grid-670e9433/` | `develop` `670e9433` — the control, measured second | same 32 cells, same session. |
| `census/` | branch vs `670e9433`, **10 interleaved launches** each, 3 reps + 1 warmup, on the 2c cells of all eight shapes (ping-pong and thread-ring in both wait modes) and the ping-pong 1c anchor | 19:00:04–19:01:18 UTC. |
| `bench/` | the four core `dev/bench` binaries, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions, every iteration recorded) | 19:01:18–19:04:54 UTC. |
| `probe.txt` | `qvoprobe-pass-cost` k = 1, 2, 4 and `qvoprobe-xcore-hop` send / push, phase-averaged (jitter 150 ns) and locked, candidate and control alternated three times | 19:04:54–19:06:50 UTC. |
| `census-throughput/` | the cells whose grid or census readings had moved the other way — fib 2c (both modes), bank-transaction 2c-park, big 2c-park, counting 2c-park and 1c-spin — **15 interleaved launches** each, a second quiet window | 19:10–19:14 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when the final candidate is measured on all eight shapes.

## The grids, same session (p50 per unit, ns; candidate pass 1 / pass 2 against the control)

| cell | `670e9433` | **branch** p1 / p2 | Δ |
|---|---:|---:|---:|
| ping-pong 2c-park (round trip) | 257.5 | **209.4 / 206.1** | **−19 / −20 %** |
| ping-pong 2c-spin | 258.0 | **217.5 / 198.5** | **−16 / −23 %** |
| thread-ring 2c-park (hop) | 130.2 | **101.6 / 99.8** | **−22 / −23 %** |
| thread-ring 2c-spin | 143.9 | **104.6 / 95.9** | **−27 / −33 %** |
| chameneos 2c-park (meeting) | 61.7 | 63.4 / 54.0 | +3 / −12 % |
| chameneos 2c-spin | 55.4 | **53.2 / 51.2** | −4 / −8 % |
| big 2c-park (round trip) | 22.49 | **20.44 / 20.28** | **−9 / −10 %** |
| big 2c-spin | 20.92 | 21.56 / 21.22 | +3 / +1 % |
| fork-join 2c-spin (message) | 13.36 | 10.43 / 10.18 | −22 / −24 % (the control's 13.4 is its upper mode; 2c-park 10.3 → 10.1 / 10.3) |
| bank-transaction 2c-park / 2c-spin (transfer) | 147.2 / 150.3 | 158.6 / 154.4 — 154.7 / 148.4 | +8 / +5 — +3 / −1 % (census below: level) |
| ping-pong 1c-spin / 1c-park | 32.21 / 32.59 | 32.26 / 32.23 — 32.40 / 32.26 | level |
| thread-ring 1c-spin / 1c-park | 19.17 / 19.12 | 19.40 / 19.34 — 19.34 / 19.29 | +1 % |
| big 1c, chameneos 1c, fib 1c, counting 1c, fork-join 1c, bank 1c | — | within spread | level |

`census/`, ten interleaved launches, medians (min … max): ping-pong 2c-park **204.9** (183.0 …
226.1) vs 263.2 (234.9 … 286.8), **−22 %**; ping-pong 2c-spin **206.1** vs 257.0, **−20 %**;
thread-ring 2c-park **98.9** (93.6 … 110.3) vs 138.6 (133.4 … 146.2), **−29 %**; thread-ring
2c-spin **98.4** vs 137.0, **−28 %**; chameneos 2c 52.0 vs 54.7 (−5 %); fork-join 2c 10.4 vs
12.2 (the control bimodal, 10.1 … 15.4); counting 2c 11.3 vs 11.4; bank 2c 156.4 vs 155.5; big 2c
21.7 vs 21.1; fib 2c 124.7 vs 118.8 (+5 %, overlapping — re-measured below); ping-pong 1c 32.2 vs
32.6.

`census-throughput/`, fifteen interleaved launches: fib 2c-spin **117.8** vs 118.3, 2c-park
119.9 vs 118.6 (+1 %, 116.1–127.4 vs 111.2–127.8); bank 2c-park 158.2 vs 154.7 (143–179 vs
147–165); big 2c 20.5 vs 21.1 (−3 %); counting 2c 11.2 vs 12.0 (the control's tail), counting 1c
9.2 vs 9.0. Level, all of them — the same verdict WSL2's fifteen-launch census gave the same
cells: the batched cross-core shapes publish runs of hundreds of events per ring write and do
not see one load per publish, and the same-core shapes never touch this ring.

## `dev/bench` — the four core binaries (median of three run medians, ns; `bench/`)

| cell | `670e9433` | **branch** | Δ |
|---|---:|---:|---:|
| `BM_Multi_PingPong_Latency` (cross-core round trip) | 317.8 | **245.6** | **−23 %** |
| `BM_Reference_Multi_PingPong_Latency` (raw spsc ring, tight poll, no actor) | 285.6 | **222.9** | **−22 %** — MSVC's tight poll sees the fix too |
| `BM_Pipeline_Chain_Latency` 8 actors / 8 cores (per delivery) | 252.5 | **220.2** | **−13 %** |
| `BM_Ask_RoundTrip_CrossCore` | 51.7 | **47.0** | −9 % |
| `BM_PINGPONG<TinyEvent>` 64 actors, 8 cores (per round trip) | 24.9 | 23.9 | −4 % |
| `BM_Ask_RoundTrip_SameCore` | 45.4 | 36.9 | the control bimodal (36 … 54); the branch's three runs 36–37 |
| `BM_Mono_PingPong_Latency` (same core) | 71.7 | 70.9 | level |
| pipeline 10 actors / 1 core | 36.7 | 37.4 | +2 % (37 … 37 vs 37 … 38) |
| `BM_PINGPONG` 64 actors, 1 core | 26.5 | 26.2 | level |

## The probes (`probe.txt`, ns; cand / ctl, three alternations)

`xcore-hop` phase-averaged (jitter 150): send **257 / 259 / 271** vs 295 / 296 / 303 (**−12 %**),
push 254 / 260 / 273 vs 258 / 280 / 283 (−7 %); locked: send 232 / 235 / 242 vs 278 / 280 /
284 (−16 %), push 212 / 273 / 287 vs 256 / 258 / 265 (the branch's locked push is bimodal on
this host — the phase lock §16.1 describes, which is why the phase-averaged row is the one to
read). `pass-cost` k = 1 / 2 / 4: 16.1–16.5 / 23.5–23.9 / 37.7–38.6 vs 16.2–16.3 / 23.6–23.7 /
37.8–38.7 — identical; the one-core pass does not touch this ring.

Suite at the branch: `dev/agent/verify-windows.ps1` — the numbers are in the Huly comment and the
qb `CHANGELOG.md` `[Unreleased]` entry.
