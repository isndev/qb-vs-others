# qb branch `perf/actor-arena` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-212, point 1** (the WSL2 half is
`../../wsl-debian-g++14/qb-branch-perf-actor-arena/`): the qb branch on which the actor object
comes from a per-thread size-class arena (`qb::Actor::operator new` / `operator delete` over
`qb::allocator::thread_arena`: 16-byte classes up to 1 KiB, LIFO reuse per class, chunks from
`slab_cache`, no lock and no TLS init guard on the path) instead of the process heap. The
measurement it answers is TUNING §13.5: after the 3.2 train the actor object's own `new` /
`delete` was the last heap traffic of an actor lifetime — one `malloc` per actor — and `fib` was
the one-core cell where qb lost most against the raw-thread floor.

Same host, CPUs and flags as the published directories beside this one: `/O2 /Ob2 /DNDEBUG`,
native arch off, CPUs 0,2, **9 repetitions + 2 warmup** per grid cell, qb-only candidate builds
(`build/ab-arena` against the working tree of the branch, 0 dirty), control = `build/final` (qb
`develop` `f2779605`, the 2026-09-13 build of the 3.2.0 candidate, not rebuilt). ONE quiet session
on 2026-09-17 (Docker Desktop stopped, WSL2 idle, nothing else on the host): candidate / control /
candidate grids, then the interleaved launch censuses; a second window on the branch's final commit
(comments and the orphan list of the exit path only — the hot path is byte-identical).

| directory | qb at | what |
|---|---|---|
| `grid-cand-bff6baf2-pass1/`, `grid-cand-bff6baf2-pass2/` | **the branch** (`bff6baf2`) — measured first and third | **32 cells** each, qb only, all verified: eight shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. 03:48:38–03:48:51 and 03:49:06–03:49:19 UTC. |
| `grid-ctl-f2779605/` | `develop` `f2779605` — the control, measured second | same 32 cells, same session. 03:48:51–03:49:06 UTC. |
| `census/` | branch vs `f2779605`, **12 interleaved launches** (fib, four configs) and **8** (the 1c-spin anchors bank-transaction, ping-pong, counting), 3 reps + 1 warmup each | 03:49:19–03:49:29 UTC. |
| `grid-cand-37d95467/` | the branch's final commit `37d95467` — one more 32-cell grid, second quiet window | 03:58:45 UTC. |
| `census-37d95467/` | `37d95467` vs `f2779605`, fib, four configs, 12 interleaved launches | 03:58:47–03:59:04 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when it is merged and the final candidate is measured on all eight shapes.

## fib — the cell the branch is for (census, median of the per-launch medians, ns per actor lifetime)

| cell | `f2779605` | **branch** (`bff6baf2`) | **branch** (`37d95467`) | Δ |
|---|---:|---:|---:|---:|
| fib 1c-spin | 185.3 (182–193) | **127.2** (123–130) | **125.5** (124–129) | **−31 / −32 %**, distributions separate |
| fib 1c-park | 189.5 (183–210) | **128.0** (126–132) | **124.5** (122–129) | **−32 / −34 %**, separate |
| fib 2c-spin | 117.2 (112–122) | **85.3** (82–93) | **83.4** (82–90) | **−27 / −29 %**, separate |
| fib 2c-park | 115.9 (109–124) | **86.0** (82–92) | **84.4** (82–88) | **−26 / −30 %**, separate |

The anchors, same session, 8 interleaved launches: bank-transaction 1c-spin 232.9 vs 233.9
(−0.4 %, overlap — the ask path allocates nothing per ask, so the arena cannot touch it: that is
QB-212 point 2), ping-pong 1c-spin 30.6 vs 29.3 (+4 %, separate — the two actors of the cell are
arena-allocated too, and their lines are now adjacent), counting 1c-spin 12.1 vs 10.4 (overlap:
the cell's two modes, 8 and 12 ns, are both present on both sides).

## The grids (p50 per unit, ns; candidate passes against the control)

| cell | `f2779605` | `bff6baf2` p1 / p2 | `37d95467` | Δ p1 / p2 / final |
|---|---:|---:|---:|---:|
| fib 1c-spin (actor lifetime) | 182.45 | **126.32 / 122.81** | **122.38** | **−31 / −33 / −33 %** |
| fib 1c-park | 180.23 | **123.65 / 124.31** | **122.64** | **−31 / −31 / −32 %** |
| fib 2c-spin | 122.13 | **81.01 / 82.61** | **83.24** | **−34 / −32 / −32 %** |
| fib 2c-park | 114.80 | **82.90 / 82.27** | **79.89** | **−28 / −28 / −30 %** |
| ping-pong 1c-spin (round trip) | 30.16 | 29.29 / 29.44 | 28.99 | −3 / −2 / −4 % |
| ping-pong 1c-park | 30.18 | 29.30 / 29.74 | 29.24 | −3 / −1 / −3 % |
| ping-pong 2c-spin | 185.46 | 188.08 / 177.48 | 180.47 | +1 / −4 / −3 % |
| ping-pong 2c-park | 201.87 | 191.90 / 203.26 | 196.75 | −5 / +1 / −3 % |
| thread-ring 1c-spin (hop) | 18.52 | 18.19 / 18.09 | 18.47 | −2 / −2 / 0 % |
| thread-ring 1c-park | 18.06 | 18.19 / 18.25 | 18.42 | +1 / +1 / +2 % |
| thread-ring 2c-spin | 97.99 | 90.86 / 87.44 | 92.16 | −7 / −11 / −6 % |
| thread-ring 2c-park | 100.92 | 96.86 / 104.94 | 98.86 | −4 / +4 / −2 % |
| counting 1c-spin (message) | 11.95 | 10.41 / 11.91 | 11.82 | −13 / 0 / −1 % (two modes, 8 and 12) |
| counting 1c-park | 11.93 | 8.24 / 11.36 | 8.56 | −31 / −5 / −28 % (idem) |
| counting 2c-spin | 13.47 | 13.49 / 13.40 | 13.46 | 0 / 0 / 0 % |
| counting 2c-park | 13.54 | 13.15 / 13.49 | 13.45 | −3 / 0 / −1 % |
| chameneos 1c-spin (meeting) | 30.55 | 31.71 / 30.87 | 31.16 | +4 / +1 / +2 % |
| chameneos 1c-park | 30.43 | 31.79 / 30.26 | 31.72 | +4 / −1 / +4 % |
| chameneos 2c-spin | 75.04 | 52.48 / 73.02 | 56.10 | −30 / −3 / −25 % (two modes, ~53 and ~74) |
| chameneos 2c-park | 74.05 | 62.25 / 68.02 | 53.23 | −16 / −8 / −28 % (idem) |
| big 1c-spin (round trip) | 16.95 | 17.55 / 16.93 | 16.78 | +4 / 0 / −1 % |
| big 1c-park | 17.22 | 17.02 / 16.74 | 16.88 | −1 / −3 / −2 % |
| big 2c-spin | 22.39 | 23.59 / 20.90 | 22.13 | +5 / −7 / −1 % |
| big 2c-park | 24.27 | 23.24 / 23.90 | 23.76 | −4 / −2 / −2 % |
| bank-transaction 1c-spin (transfer) | 233.46 | 232.44 / 236.29 | 232.25 | 0 / +1 / −1 % |
| bank-transaction 1c-park | 227.86 | 231.69 / 233.15 | 236.12 | +2 / +2 / +4 % |
| bank-transaction 2c-spin | 143.29 | 162.90 / 146.66 | 145.98 | +14 / +2 / +2 % (the cell's known upper mode on p1) |
| bank-transaction 2c-park | 142.57 | 142.82 / 142.63 | 139.25 | 0 / 0 / −2 % |
| fork-join 1c-spin (message) | 9.13 | 10.10 / 9.25 | 12.02 | +11 / +1 / +32 % (two modes, ~9 and ~12; the control landed in the low one) |
| fork-join 1c-park | 12.22 | 12.45 / 9.14 | 10.87 | +2 / −25 / −11 % (idem) |
| fork-join 2c-spin | 12.44 | 12.18 / 12.74 | 12.27 | −2 / +2 / −1 % |
| fork-join 2c-park | 12.59 | 11.81 / 9.63 | 12.20 | −6 / −23 / −3 % (idem) |

Reading: the four fib cells move by −28 to −34 % in every pass, the same band on both commits;
every other cell sits inside its own spread — the cells that read beyond ±5 % (chameneos 2c,
counting 1c-park, fork-join 1c/2c-park, big 2c-spin, bank-transaction 2c-spin) are the bimodal cells
the earlier grids beside this one already document, and they move the other way from one candidate
pass to the next, which a real effect does not do. The fork-join census of the same day
(`census-forkjoin/`, 12 interleaved launches, the tie-breaker for the one cell whose final pass
landed in its upper mode) reads 1c-spin 12.6 vs 11.2 and 1c-park 9.2 vs 11.9 with both modes (9 and
13 ns) present on both sides: **overlap, no measurable difference**, either way.
