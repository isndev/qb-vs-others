# qb branch `perf/ring-private-lines` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-184**: the qb branch that gives each side of the mailbox's SPSC ring a
PRIVATE line for its working index and its snapshot of the peer's index, and leaves the published
index ALONE on the line the peer polls — so that a producer never reads the line it publishes on.
Measured against the `develop` it forks from (`670e9433`, the QB-182 head) on all eight shapes,
4 configurations each, plus the four core `dev/bench` binaries and two probes. Same host, CPUs
and build flags as the published directories beside this one: `-O3 -DNDEBUG`, `taskset -c 0,2`,
**9 repetitions + 2 warmup**, qb-only builds, candidate / control / candidate in ONE quiet session
on 2026-09-07, the Windows side idle throughout. The candidate is `~/qvo/cand-pass`, built against
the working tree of the branch; the control `~/qvo/ctl-670e9433`, a clean LF clone at `670e9433`
(0 dirty).

| directory | qb at | what |
|---|---|---|
| `grid-worktree/`, `grid-worktree-pass2/` | **the branch** — measured first and third | **32 cells** each, qb only, all verified: eight shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. 18:41:21–18:41:55 UTC. |
| `grid-670e9433/` | `develop` `670e9433` — the control, measured second | same 32 cells, same session. |
| `census/` | branch vs `670e9433`, **10 interleaved launches** each, 3 reps + 1 warmup, on the 2c cells of all eight shapes (ping-pong and thread-ring in both wait modes) and the ping-pong 1c anchor | 18:41:55–18:42:53 UTC. |
| `bench/` | the four core `dev/bench` binaries, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions, every iteration recorded) | 18:42:53–18:46:23 UTC. |
| `probe.txt` | `qvoprobe-pass-cost` k = 1, 2, 4 and `qvoprobe-xcore-hop` send / push, phase-averaged (jitter 150 ns) and locked, candidate and control alternated three times | 18:46:23–18:47:48 UTC. |
| `census-throughput/` | the cells whose GRID readings had moved +2 to +10 % — counting 1c (both modes) and 2c-spin, fib 2c (both modes), fork-join 1c and 2c-spin, bank-transaction 1c-park — **15 interleaved launches** each, same protocol, a second quiet window | 18:48:41–18:48:51 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when the final candidate is measured on all eight shapes.

## The defect, and the instrument that found it

`lockfree::spsc::internal::ringbuffer` — the ring behind every mailbox — kept `write_index_`
(published: the consumer polls it) and `cached_read_index_` (the producer's private snapshot) on
ONE line, so every `enqueue` began by loading both from the very line the consumer had just been
polling. On this host a snoop of that kind leaves the owner's copy behind: the load is a
cross-core miss on every hop, and a cpu-clock profile of the two-actor `send` round trip put as
many samples on it (8 544, the load of the producer line at `SharedCoreCommunication::send+0x4f`)
as on the release fence that follows the publish (9 633). `tools/probes/raw-ring.cpp` reproduces
it with no qb in the loop: a two-thread ring of the same shape, and ONE load of the published
index line before the store costs **+45 to +60 ns per round trip** at a random phase
(121 → 162–180 ns; +25 ns locked).

`tools/probes/xcore-hop.cpp` is the qb-side instrument: two actors, one per pinned core, a
round trip by `push<>` or by `send<>`, and a `jitter_ns` option that breaks the **phase lock** of
a two-actor ping-pong (a random 0..j busy-wait before each hop on the A end, rdtsc-paced,
reported net). The lock is why this defect was invisible to the grids and to every locked probe:
a 5 ns delay in front of the flush moved the locked round trip by +80 ns, and the same
variant read ±10 % from one binary's memory layout to the next (`../qb-branch-perf-flush-at-pass-end/`).

## The probes, same session (ns per round trip, cand / ctl, three alternations)

| probe | control `670e9433` | **branch** | Δ |
|---|---:|---:|---:|
| `xcore-hop` send, phase-averaged (jitter 150) | 233.5 / 240.1 / 242.9 | **188.9 / 197.7 / 210.1** | **−17 %** |
| `xcore-hop` push, phase-averaged | 228.4 / 240.3 / 241.2 | **158.8 / 159.1 / 164.2** | **−34 %** |
| `xcore-hop` send, locked | 216.9 / 229.7 / 237.4 | **162.9 / 162.9 / 180.3** | −29 % |
| `xcore-hop` push, locked | 211.2 / 213.5 / 224.7 | **151.7 / 153.6 / 154.8** | −29 % |
| `pass-cost` k = 1 / 2 / 4 (one core, ns per pass) | 12.9 / 17.1 / 25.0 | 12.7 / 17.1 / 25.0 | level |

## The grids, same session (p50 per unit, ns; candidate pass 1 / pass 2 against the control)

| cell | `670e9433` | **branch** p1 / p2 | Δ |
|---|---:|---:|---:|
| ping-pong 2c-park (round trip) | 208.99 | **164.92 / 159.35** | **−21 / −24 %** |
| ping-pong 2c-spin | 221.36 | **162.87 / 172.66** | **−26 / −22 %** |
| thread-ring 2c-park (hop) | 114.17 | **81.79 / 80.49** | **−28 / −30 %** |
| thread-ring 2c-spin | 113.95 | **80.78 / 82.89** | **−29 / −27 %** |
| chameneos 2c-park (meeting) | 48.40 | **44.62 / 46.39** | −8 / −4 % |
| chameneos 2c-spin | 49.16 | **45.15 / 45.02** | **−8 %** |
| big 2c-park (round trip) | 19.32 | 18.65 / 17.91 | −3 / −7 % |
| big 2c-spin | 18.61 | 18.10 / 18.01 | −3 % |
| bank-transaction 2c-park (transfer) | 93.44 | 88.13 / 90.37 | −6 / −3 % |
| bank-transaction 2c-spin | 92.58 | 89.00 / 89.29 | −4 % |
| ping-pong 1c-spin / 1c-park | 22.81 / 23.15 | 22.94 / 23.11 — 22.95 / 22.89 | level |
| thread-ring 1c-spin / 1c-park | 17.38 / 17.42 | 17.38 / 17.35 — 17.31 / 17.34 | level |
| big 1c, chameneos 1c, fib 1c | 18.35–18.66, 25.95–27.03, 126.5–129.1 | within spread | level |
| counting 1c-spin / 2c-spin, fork-join, fib 2c, bank 1c-park | 7.84 / 9.82, 8.3–8.7, 85.4–85.7, 136.1 | +2 to +11 % in the grid | **noise — see the throughput census** |

`census/`, ten interleaved launches, medians: ping-pong 2c-park **164.7** vs 211.7 (**−22 %**),
2c-spin **163.2** vs 207.5 (−21 %); thread-ring 2c-park **79.9** vs 115.0 (**−31 %**), 2c-spin
80.4 vs 116.1 (−31 %); chameneos 2c 45.6 vs 49.1 (−7 %); big 2c 18.3 vs 19.1 (−4 %); bank 2c
90.3 vs 90.4, fork-join 2c 9.0 vs 9.1, counting 2c 9.9 vs 10.0, fib 2c 92.1 vs 88.1 (+4.5 %,
overlapping distributions — re-measured below), ping-pong 1c 23.2 vs 23.4.

`census-throughput/`, fifteen interleaved launches on every cell the grids had moved the other
way: counting 1c-spin **8.4** vs 8.3, 1c-park 8.4 vs 8.3, 2c-spin 10.1 vs 10.0; fib 2c-spin
**89.3** vs 89.6, 2c-park 90.9 vs 89.6 (+1.5 %, distributions 88.3–99.3 vs 85.7–96.6); fork-join
1c 8.6 vs 8.5, 2c 9.2 vs 9.2; bank-transaction 1c-park 140.0 vs 142.5. Level, all of them: the
grid's +2 to +11 % on those cells was launch-to-launch noise of the batched shapes, not the
change. The ring change touches nothing a same-core cell executes — the self pipe is a
`segmented_pipe`, not this ring — and the batched cross-core shapes publish runs of hundreds of
events per ring write, where one load per publish is invisible.

## `dev/bench` — the four core binaries (median of three run medians, ns; `bench/`)

| cell | `670e9433` | **branch** | Δ |
|---|---:|---:|---:|
| `BM_Multi_PingPong_Latency` (cross-core round trip) | 253.3 | **202.8** | **−20 %** |
| `BM_Pipeline_Chain_Latency` 8 actors / 8 cores (per delivery) | 321.9 | **264.5** | **−18 %** |
| `BM_PINGPONG<TinyEvent>` 64 actors, 8 cores (per round trip) | 24.7 | **21.7** | **−12 %** |
| `BM_Ask_RoundTrip_CrossCore` | 48.5 | 46.3 | −4.5 % |
| `BM_Mono_PingPong_Latency` (same core) | 59.3 | 59.6 | level |
| pipeline 10 actors / 1 core | 30.4 | 30.4 | level |
| `BM_PINGPONG` 64 actors, 1 core | 21.3 | 21.3 | level |
| `BM_Ask_RoundTrip_SameCore` | 42.8 | 42.5 | level |
| `BM_Reference_Multi_PingPong_Latency` (raw spsc, tight poll, no qb code) | 196.8 | 203.4 | +3 % — the host; a tight poll is the regime the fix does not reach |

Suites at the branch: release / ASan+UBSan / TSan on this host — the figures are in the Huly
comment and the qb `CHANGELOG.md` `[Unreleased]` entry.
