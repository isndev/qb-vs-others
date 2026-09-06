# qb branch `perf/dense-table-growth` — Windows 11 / MSVC 19.51

The first two Savina shapes that create actors INSIDE the window — `savina/fib` (57 312 actors
born and dead per repetition, `benchmarks/savina/fib.md`) and `savina/chameneos` (100 actors
through one broker, 200 000 meetings, `benchmarks/savina/chameneos.md`) — and the qb branch they
produced. Same host, CPUs and build flags as the published directories beside this one:
`/O2 /DNDEBUG`, CPUs 0 and 2 (two P-cores), **9 repetitions + 2 warmup**, one quiet session per
grid, no build, no test suite and no WSL measurement running anywhere on the host (the WSL2 grids
ran 17:02–17:03, 17:22–17:23 and 17:40–18:13 UTC, this host's at 17:01–17:02, 17:24 and
18:14–18:16 UTC on 2026-09-06 — the six sessions never overlap). CAF is 1.1.0, SObjectizer 5.8.5.1, the floor is
`frameworks/baseline/`. qb is `build/final`, built against the working tree of `qb/` — `develop`
(`22c9bf6e`, every axis through N merged) plus the branch's commits.

| directory | qb at | what |
|---|---|---|
| `grid-3ea9a2ae/` | `perf/dense-table-growth` **two** commits over `develop`: `b4baba33` (the dense router table grows by capacity, not by one slot; per-actor lifecycle logs demoted to VERBOSE) and `3ea9a2ae` (per-core actor registry as a slot-indexed vector, kill queue as a vector, `static_cast` in subscribe) | **32 cells**, all verified: fib × chameneos × {baseline, caf, qb, sobjectizer} × {2c-spin, 2c-park, 1c-spin, 1c-park}. |
| `grid-8362a4b8/` | the same plus `8362a4b8` (the actor's coroutine counter allocated on its first `spawn`, not in the constructor) — **the branch as proposed for merge** | **32 cells**, all verified, same protocol, 22 minutes later, after the WSL2 grid of the same commit had ended. |
| `grid-8362a4b8-session2/` | `8362a4b8` again | **8 cells**, qb only, 9 + 2, 18:16 UTC — the same quiet session as the shipped-3.1.0 control in `../savina-fib/` and `../savina-chameneos/` (18:14–18:16 UTC), which is the pair FAIRNESS.md asks for. Every cell lands inside the first grid's spread: fib 10.45 / 10.81 / 17.18 / 17.84 ms (2c-spin / 2c-park / 1c-spin / 1c-park) against 11.09 / 10.59 / 17.57 / 17.05; chameneos 12.79 / 12.67 / 7.57 / 7.26 against 11.75 / 12.86 / 7.40 / 7.93. |

None of the three is merged into the published tables: `../savina-fib/` and `../savina-chameneos/`
render shipped 3.1.0, like the five shapes before them, and the candidate joins the tables when
the 3.2.0 grid is measured for all seven shapes in one session. The branch's starting point,
`develop` `22c9bf6e`, measures fib at **43 s** per repetition on this host (below), which is not
a cell, it is the defect — introduced on `develop` with the dense tables (axis I,
`docs/TUNING.md` §7) and never shipped.

## Shipped 3.1.0 against the branch, same session (p50 ms, 18:14–18:16 UTC, 9 + 2)

| shape · config | shipped 3.1.0 | **`8362a4b8`** | ratio |
|---|---|---|---|
| fib · 2c-spin | 458.7 | **10.45** | 44× |
| fib · 2c-park | 477.1 | **10.81** | 44× |
| fib · 1c-spin | 558.1 | **17.18** | 32× |
| fib · 1c-park | 512.8 | **17.84** | 29× |
| chameneos · 2c-spin | 20.76 | **12.79** | 1.6× |
| chameneos · 2c-park | 20.68 | **12.67** | 1.6× |
| chameneos · 1c-spin | 13.21 | **7.57** | 1.7× |
| chameneos · 1c-park | 13.55 | **7.26** | 1.9× |

Shipped 3.1.0's fib figure is not the router (3.1.0 has no dense table): it is the LOGGER. At its
shipped default, `QB_WITH_LOGGING=ON`, a release build logs at INFO, and 3.1.0 logs **9 lines per
actor lifetime** — `registerEvent` × 7, the five default subscriptions and the benchmark's two, plus `New`
and `Delete` — 515 819 lines and 60.9 MB of `qb.1.log` per repetition
(measured on the WSL2 host, the same code); nanolog's producer side (format, timestamp, push to
the writer's queue) is inside the window and its writer thread shares the two pinned CPUs. That
Windows pays 459 ms where Linux pays 159 for the same 515 819 lines is the CRT heap and a
per-line `WriteFile` against glibc and ext4 — the two hosts' floors on fib lean the same way
(4.0 against 3.4). `b4baba33` demotes every one of those lines to VERBOSE. The caveat string the
shipped documents carry — "its logger writes at startup, outside the measured window" — was
written for the five static shapes and is false for fib on 3.1.0; the JSONs are kept as
measured, `frameworks/qb/qb_support.h` says the true thing now.

Chameneos is the same code on both sides of the logger (101 actors, nothing logged per meeting),
so its 1.6–1.9× is the branch's registry and the §7–§10 dispatch work landing on a shape that
pushes 400 000 events through one mailbox.

## What fib found, in the order the branch fixed it (2c-spin, p50, this host)

| qb | fib n=23 | what changed |
|---|---|---|
| `develop` `22c9bf6e` (axis I dense tables, unreleased) | **43.2 s** | the dense router's `key_table` grew by +1 slot per `registerEvent`, rehashing the whole table — five default events per actor × 57 312 actors, O(n²); latent because nothing had ever created more than a few hundred actors per core |
| `b4baba33` | 178 ms → 28 ms | growth by capacity (43 s → 178 ms); then `VirtualCore::addActor`/`removeActor`'s per-actor `LOG_INFO` (114 624 lines inside the window) demoted to `LOG_VERB` |
| `3ea9a2ae` | **12.13 ms** | `ActorMap` was a hash map keyed by an id that IS a per-core slot, the kill queue a hash set deduplicating what an idempotent `kill()` deduplicates itself, and subscribe paid a `dynamic_cast` per registration (5 per actor) |
| `8362a4b8` | **11.09 ms** | `active_coroutines_` was a `make_shared` in every constructor and a release in every destructor, for actors that never spawn — ~30 % of an actor's lifetime on the profile |

Chameneos moved on none of them (13.0 → 11.8 ms at 2c-spin, 8.3 → 7.4 at 1c-spin, p99s of 12–17 ms on every qb cell — inside its own spread): it creates 101
actors, and its cost is the broker's mailbox, which none of these commits touch.

## The field at `8362a4b8` (p50, ms)

| shape · config | floor | **qb** | CAF | SObjectizer |
|---|---|---|---|---|
| fib · 2c-spin | 4.03 | **11.09** | 52.5 | 188 |
| fib · 2c-park | 4.01 | **10.59** | 53.0 | 185 |
| fib · 1c-spin | 3.73 | **17.57** | 84.0 | 154 |
| fib · 1c-park | 3.98 | **17.05** | 83.8 | 198 |
| chameneos · 2c-spin | 68.6 | **11.75** | 215 | 248 |
| chameneos · 2c-park | 78.5 | **12.86** | 214 | 294 |
| chameneos · 1c-spin | 5.67 | **7.40** | 215 | 74.9 |
| chameneos · 1c-park | 9.24 | **7.93** | 211 | 81.1 |

Read with the caveats each JSON carries. On fib, qb's two-core cell is two independent sub-trees
(a child lives on its parent's core; fib(22) on core 0 does ~62 % of the work) while CAF and
SObjectizer steal, so 2c is qb's WORST placement and it still leads CAF by 4.7×; the floor's
1c-spin at 3.73 ms is one thread allocating 57 312 nodes with no registry at all (twice its Linux
figure — the CRT heap against glibc's), and the gap to qb (17.6 ms) is the registry —
`registerEvent` × 7 per actor ≈ 29 %, `unregisterEvents` walking every resolver ≈ 12 %, the actor
object's own `new`/`delete` the last heap traffic (`perf --call-graph dwarf` on the WSL2 host, fib
1c — the profile is Linux's, the shape is the same code). Routing the five default events through
the actor registry instead of five 2 MiB dense tables is the next lead (Huly QB-174), not
something this branch does. On chameneos the floor at 2c is a mutex-and-condvar broker crossing a
core per meeting, which is why it sits ABOVE qb at two cores (68.6 ms against 11.8) and below
every framework at one.

SObjectizer's fib is SLOWER on two cores than on one (188 vs 154 ms; 328 vs 148 on WSL2): each
node is its own cooperation, registered and deregistered through the environment, and the two work
threads contend on that path — consistent with the profile shape, not profiled here. Its `2c-park`
chameneos (294 ms, p99 299) is the §8 shape again — a broker on one thread and chameneos on the
other, a kernel wake per meeting.
