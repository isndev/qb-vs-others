# qb branch `perf/dense-table-growth` — WSL2 Debian 13 / g++ 14.2

The first two Savina shapes that create actors INSIDE the window — `savina/fib` (57 312 actors
born and dead per repetition, `benchmarks/savina/fib.md`) and `savina/chameneos` (100 actors
through one broker, 200 000 meetings, `benchmarks/savina/chameneos.md`) — and the qb branch they
produced. Same host, CPUs and build flags as the published directories beside this one:
`-O3 -DNDEBUG`, `taskset -c 0,2`, **9 repetitions + 2 warmup**, one quiet session per grid, the
Windows side idle throughout (its own grids ran 17:01–17:02, 17:24 and 18:14–18:16 UTC, this
host's at 17:02–17:03, 17:22–17:23 and 17:40–18:13 UTC on 2026-09-06 — the six sessions never
overlap). CAF is 1.1.0,
SObjectizer 5.8.5.1, the floor is `frameworks/baseline/`. qb is built under `~/qvo/linux`
against the working tree of `/mnt/d/repo/qb-dev/qb`, `develop` (`22c9bf6e`, every axis through N
merged) plus the branch's commits.

| directory | qb at | what |
|---|---|---|
| `grid-3ea9a2ae/` | `perf/dense-table-growth` **two** commits over `develop`: `b4baba33` (the dense router table grows by capacity, not by one slot; per-actor lifecycle logs demoted to VERBOSE) and `3ea9a2ae` (per-core actor registry as a slot-indexed vector, kill queue as a vector, `static_cast` in subscribe) | **32 cells**, all verified: fib × chameneos × {baseline, caf, qb, sobjectizer} × {2c-spin, 2c-park, 1c-spin, 1c-park}. |
| `grid-8362a4b8/` | the same plus `8362a4b8` (the actor's coroutine counter allocated on its first `spawn`, not in the constructor) — **the branch as proposed for merge** | **32 cells**, all verified, same protocol, 20 minutes later in the same quiet session. |
| `grid-8362a4b8-session2/` | `8362a4b8` again | **8 cells**, qb only, at this host's PUBLISHED protocol (**5 + 1**), 18:13 UTC — the same quiet session as the shipped-3.1.0 control in `../savina-fib/` and `../savina-chameneos/` (field 17:40, shipped qb 18:13), which is the pair FAIRNESS.md asks for. Every cell lands inside the 9 + 2 grid's spread: fib 7.59 / 7.14 / 11.89 / 11.79 ms (2c-spin / 2c-park / 1c-spin / 1c-park) against 7.08 / 7.20 / 11.44 / 11.29; chameneos 11.09 / 11.23 / 6.52 / 6.89 against 11.17 / 10.97 / 6.79 / 6.89. |

None of the three is merged into the published tables: `../savina-fib/` and `../savina-chameneos/`
render shipped 3.1.0, like the five shapes before them, and the candidate joins the tables when
the 3.2.0 grid is measured for all seven shapes in one session. The branch's starting point,
`develop` `22c9bf6e`, measures fib at **43 s** per repetition on Windows (below), which is not a
cell, it is the defect — introduced on `develop` with the dense tables (axis I, `docs/TUNING.md`
§7) and never shipped.

## Shipped 3.1.0 against the branch, same session (p50 ms, 18:13 UTC, 5 + 1)

| shape · config | shipped 3.1.0 | **`8362a4b8`** | ratio |
|---|---|---|---|
| fib · 2c-spin | 159.4 | **7.59** | 21× |
| fib · 2c-park | 147.1 | **7.14** | 21× |
| fib · 1c-spin | 205.2 | **11.89** | 17× |
| fib · 1c-park | 208.7 | **11.79** | 18× |
| chameneos · 2c-spin | 15.30 | **11.09** | 1.4× |
| chameneos · 2c-park | 16.10 | **11.23** | 1.4× |
| chameneos · 1c-spin | 11.96 | **6.52** | 1.8× |
| chameneos · 1c-park | 13.54 | **6.89** | 2.0× |

Shipped 3.1.0's fib figure is not the router (3.1.0 has no dense table): it is the LOGGER. At its
shipped default, `QB_WITH_LOGGING=ON`, a release build logs at INFO, and 3.1.0 logs **9 lines per
actor lifetime** — `registerEvent` × 7, the five default subscriptions and the benchmark's two, plus `New`
and `Delete` — measured at **515 819 lines and 60.9 MB of `qb.1.log` per
repetition**; nanolog's producer side (format, timestamp, push to the writer's queue) is inside
the window and its writer thread shares the two pinned CPUs. `b4baba33` demotes every one of
those to VERBOSE. Two consequences worth recording. The shipped runs were driven from an ext4
cwd (`/tmp/qvo-cwd`): from the 9p-mounted checkout the process EXIT takes minutes per repetition
draining that log line by line through `p9_client_rpc` (gdb: main in `exit()` → `~NanoLogger` →
`thread::join`, the writer in `flush()` → `write()`), which first read as a hang. And the caveat
string the shipped documents carry — "its logger writes at startup, outside the measured window"
— was written for the five static shapes and is false for fib on 3.1.0; the JSONs are kept as
measured, `frameworks/qb/qb_support.h` says the true thing now.

Chameneos is the same code on both sides of the logger (101 actors, nothing logged per meeting),
so its 1.4–2.0× is the branch's registry and the §7–§10 dispatch work landing on a shape that
pushes 400 000 events through one mailbox.

## What fib found, in the order the branch fixed it (2c-spin, p50; this host unless stated)

| qb | fib n=23 | what changed |
|---|---|---|
| `develop` `22c9bf6e` (axis I dense tables, unreleased) | **43.2 s** (Windows/MSVC; Linux not waited for) | the dense router's `key_table` grew by +1 slot per `registerEvent`, rehashing the whole table — five default events per actor × 57 312 actors, O(n²); latent because nothing had ever created more than a few hundred actors per core |
| `b4baba33` | 178 ms → 28 ms (Windows) | growth by capacity (43 s → 178 ms); then `VirtualCore::addActor`/`removeActor`'s per-actor `LOG_INFO` (114 624 lines inside the window) demoted to `LOG_VERB` |
| `3ea9a2ae` | **8.49 ms** | `ActorMap` was a hash map keyed by an id that IS a per-core slot, the kill queue a hash set deduplicating what an idempotent `kill()` deduplicates itself, and subscribe paid a `dynamic_cast` per registration (5 per actor) |
| `8362a4b8` | **7.08 ms** | `active_coroutines_` was a `make_shared` in every constructor and a release in every destructor, for actors that never spawn — ~30 % of an actor's lifetime on the profile |

Chameneos moved on none of them (10.8 → 11.0 → 11.2 ms, inside its own spread): it creates 101
actors, and its cost is the broker's mailbox, which none of these commits touch.

## The field at `8362a4b8` (p50, ms)

| shape · config | floor | **qb** | CAF | SObjectizer |
|---|---|---|---|---|
| fib · 2c-spin | 3.33 | **7.08** | 38.6 | 328 |
| fib · 2c-park | 3.58 | **7.20** | 38.4 | 332 |
| fib · 1c-spin | 1.69 | **11.44** | 68.5 | 148 |
| fib · 1c-park | 3.10 | **11.29** | 67.4 | 153 |
| chameneos · 2c-spin | 29.8 | **11.17** | 129 | 154 |
| chameneos · 2c-park | 75.4 | **10.97** | 124 | 202 |
| chameneos · 1c-spin | 2.89 | **6.79** | 123 | 54.1 |
| chameneos · 1c-park | 5.57 | **6.89** | 123 | 58.4 |

Read with the caveats each JSON carries. On fib, qb's two-core cell is two independent sub-trees
(a child lives on its parent's core; fib(22) on core 0 does ~62 % of the work) while CAF and
SObjectizer steal, so 2c is qb's WORST placement and it still leads CAF by 5.5×; the floor's
1c-spin at 1.69 ms is one thread allocating 57 312 nodes with no registry at all, and the gap to
qb (11.4 ms) is the registry — `registerEvent` × 7 per actor ≈ 29 %, `unregisterEvents` walking
every resolver ≈ 12 %, the actor object's own `new`/`delete` the last heap traffic (`perf --call-
graph dwarf`, fib 1c, `~/qvo/fib3.perf.data`). Routing the five default events through the actor
registry instead of five 2 MiB dense tables is the next lead (Huly QB-174), not something this
branch does. On chameneos the floor at 2c is a mutex-and-condvar broker crossing a core per
meeting, which is why it sits ABOVE qb at two cores (29.8 ms against 11.2) and below every
framework at one.

SObjectizer's fib is 2.2× SLOWER on two cores than on one (328 vs 148 ms; the same inversion on
Windows, 188 vs 154): each node is its own cooperation, registered and deregistered through the
environment, and the two work threads contend on that path — consistent with the profile shape,
not profiled here. Its `2c-park` chameneos (202 ms, p99 225) is the §8 shape again — a broker on
one thread and chameneos on the other, a futex wake per meeting.
