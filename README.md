# qb-vs-others

A benchmark comparison of the [qb](https://github.com/isndev/qb) actor framework against other
actor frameworks — written and run by qb's own maintainer, which is exactly why
**[FAIRNESS.md](FAIRNESS.md) is the first thing to read.** It is not a preface; it is the design.

The short version of it:

> A benchmark that does not verify its own result measures the wrong thing. A framework that
> silently drops, coalesces or reorders messages *wins* a wall-clock benchmark, and wins it by a
> lot. So every benchmark here computes a checksum, the expected value is computed in a
> framework-free header, and a framework that loses one message in a million produces **no timing
> at all**.

## What is measured

| | |
|---|---|
| **Frameworks** | [qb](https://github.com/isndev/qb) 3.1.0 · [CAF](https://github.com/actor-framework/actor-framework) 1.1.0 · [SObjectizer](https://github.com/stiffstream/sobjectizer) 5.8.5.1 |
| **Floor** | raw `std::thread` + a hand-written SPSC ring — *not* a framework, and never ranked as one |
| **Problems** | the [Savina](https://github.com/shamsimam/savina) suite (Imam & Sarkar, AGERE 2014) — chosen by neither side, and predating qb |
| **Build** | every framework compiled **from source, in one project, under one `CMAKE_CXX_FLAGS_RELEASE`** |
| **Placement** | every framework's workers pinned one per CPU, through *its own* public API |

Nothing here consumes a prebuilt package: a CAF from a package manager is compiled with that
package manager's flags while qb would be compiled with ours, and "similar flags" is not
"measured flags".

## Current status

Honest, because a benchmark repository that overstates its own coverage has already lost the
argument it exists to win:

| | state |
|---|---|
| Harness, verification, pinning, reporting | **done**, and negative-controlled: 7 CAUGHT / 4 CONFIRMED / **0 MISSED** |
| `savina/ping-pong`, `counting`, `thread-ring`, `fork-join`, `big` × qb, CAF, SObjectizer, floor (+ CAF-detached on ping-pong) | **done**, 84 cells per host: 82 verified + 2 declared `n/a` (a `caf::detached` actor has no spin mode) |
| The document guards (`tools/check-roster.py`, `tools/check-report.py`) | **done**, and negative-controlled: 33 CAUGHT / 3 CONFIRMED / **0 MISSED** (`tools/guards-negative-control.py`) |
| Feature comparison, cited to the three sources | [docs/FEATURES.md](docs/FEATURES.md) |
| `savina/fib`, `savina/chameneos` × the same four (+ CAF-detached declared omitted) | **done on Windows and WSL2**: 16 cells each per host, shipped qb 3.1.0 like the five before them (`results/<host>/savina-fib/`, `savina-chameneos/`, rendered in each host's `REPORT.md`), **116 cells per host**; macOS not yet. Written against the qb branch they produced (`results/<host>/qb-branch-perf-dense-table-growth/`): fib found a 43 s defect in unreleased `develop` and drove three qb commits, and its shipped-3.1.0 cell is a LOGGING figure — nine `LOG_INFO` lines per actor lifetime, 515 819 lines per repetition, 159 / 459 ms (WSL2 / Windows, 2c-spin) against the branch's 7.6 / 10.5 in the same session, CAF 39 / 53, floor 3.4 / 4.0 (`docs/TUNING.md` §11) |
| `savina/bank-transaction` × the same four (+ CAF-detached declared omitted) | **done on Windows and WSL2** (2026-09-07): 16 cells per host, shipped qb 3.1.0 (`results/<host>/savina-bank-transaction/`, rendered in each host's `REPORT.md`), **132 cells per host**; macOS not yet. The first shape that WAITS for a reply — one `qb::ask` / CAF `request().then()` per transfer, 50 000 of them — and it found five defects on qb's ask path in one afternoon (`docs/TUNING.md` §12, qb `fa1c5ce3`): shipped 3.1.0 measures 14.7 / 9.2 ms (WSL2, 1c / 2c spin) and 25.5 / 29.2 (Windows), qb `develop` before the fixes 9.4 / 5.1 and 13.7 / 8.0, after them **8.1 / 4.6** and **12.9 / 7.6** in the same session (`results/<host>/qb-branch-perf-coro-scope-local-refcount/`), against CAF 41.5 / 36.6 and 57.8 / 57.5, SObjectizer 19.5 / 25.5 and 29.6 / 38.0, floor 1.1 / 6.3 and 3.5 / 32.2 |
| **The 3.2.0 candidate grid** — qb `develop` × all eight shapes | **done on Windows and WSL2, twice**: at the midpoint (`43f62afe`, 2026-09-07) and at the final commit (**`77b358d8`**, 2026-09-09) — 96 qb cells per host each time (candidate / shipped 3.1.0 / candidate, 9 + 2, one quiet session per host, `results/<host>/qb-branch-develop/`), the fastest framework in all 64 cells both times, every WSL2 cell faster at the end than at the midpoint (ping-pong 1c 66 → 23 ns, ring 1c 39 → 17), the Windows two-core cells level-or-better under the interleaved census; the two `framework=qb` grids below, `docs/TUNING.md` §13 and §13.4. macOS not yet: its machine measures the candidate when it is next on. |
| The other 17 Savina benchmarks | **not yet written** — see [docs/ROADMAP.md](docs/ROADMAP.md) |
| Linux axis (WSL2 Debian 13 / g++ 14.2) | **run**, the same 84 cells — with the WSL2 caveat below; native Linux not yet |
| macOS axis (Apple M4 Pro / AppleClang 21, arm64) | **run**, the same 84 cells — **unpinned** (macOS has no verified affinity API; every document says `pinned:false`); the candidate branch measured beside shipped 3.1.0 in the same session, `docs/TUNING.md` §9.13 |
| Seastar | not yet — Linux-only, and its dependencies need root on this host |
| Cross-language references (Erlang, Pekko, Actix, Orleans) | not yet |

Five benchmarks are five shapes — a two-actor round trip, a many-to-one funnel, a ring, a
scatter-gather and an all-to-all — and the results below should be read per shape, not as one
ranking.

## What five benchmarks have shown so far

MSVC 19.51, i9-12900K, pinned to two P-cores, 9 repetitions, every cell measured in one quiet
session on 2026-09-04. Full tables in [REPORT.md](REPORT.md), all regenerated from `results/`;
the figures below are **per unit of work** — a round trip, a message, a hop, a message, a round trip —
and the unit is declared once, in each benchmark's spec header, never chosen by an adapter.
A **bold floor** is a floor the fastest framework sits *below*: it is not beating raw threads,
it is not doing what the floor does (there, crossing a core on every message).

`savina/ping-pong` — 1 000 000 round trips, two actors; per round trip:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 112 ns** | SObjectizer 184 ns | 2 ns | CAF 482 ns · qb 1.64× |
| 1 core, park | **qb 113 ns** | SObjectizer 214 ns | 2 ns | CAF 489 ns · CAF-detached 10.63 µs · qb 1.90× |
| 2 cores, spin | **qb 308 ns** | CAF 485 ns | 183 ns | SObjectizer 922 ns · qb 1.57× |
| 2 cores, park | **CAF 490 ns** | SObjectizer 1.03 µs | 469 ns | qb 4.23 µs · CAF-detached **bimodal**, ~1.07 µs or ~10.65 µs · CAF 2.11× |

`savina/counting` — 1 000 000 messages from a producer into one counter, then one retrieve; per message:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 29 ns** | SObjectizer 139 ns | 3 ns | CAF 182 ns · qb 4.75× |
| 1 core, park | **qb 30 ns** | SObjectizer 146 ns | 8 ns | CAF 182 ns · qb 4.85× |
| 2 cores, spin | **qb 33 ns** | CAF 127 ns | **42 ns** | SObjectizer 289 ns · qb 3.83× |
| 2 cores, park | **qb 33 ns** | CAF 150 ns | **53 ns** | SObjectizer 308 ns · qb 4.54× |

`savina/thread-ring` — 100 actors in a ring, a token making 1 000 000 hops; per hop:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 63 ns** | SObjectizer 91 ns | 8 ns | CAF 236 ns · qb 1.45× |
| 1 core, park | **qb 63 ns** | SObjectizer 106 ns | 10 ns | CAF 235 ns · qb 1.68× |
| 2 cores, spin | **qb 172 ns** | CAF 239 ns | 110 ns | SObjectizer 482 ns · qb 1.38× |
| 2 cores, park | **CAF 236 ns** | SObjectizer 474 ns | **271 ns** | qb **bimodal**, ~565 ns or ~3.02 µs · CAF 2.01× |

`savina/fork-join` — 10 000 messages fanned out to each of 60 workers, 600 000 in all, each worker acknowledged once at the end; per message:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 43 ns** | SObjectizer 140 ns | 3 ns | CAF 309 ns · qb 3.28× |
| 1 core, park | **qb 40 ns** | SObjectizer 150 ns | 7 ns | CAF 310 ns · qb 3.74× |
| 2 cores, spin | **qb 46 ns** | CAF 171 ns | 38 ns | SObjectizer 289 ns · qb 3.72× |
| 2 cores, park | **qb 42 ns** | CAF 168 ns | **49 ns** | SObjectizer 268 ns · qb 3.98× |

`savina/big` — 120 actors each sending 20 000 pings to random peers, every ping answered; per round trip (2 400 000 of them):

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 36 ns** | SObjectizer 187 ns | 11 ns | CAF 497 ns · qb 5.15× |
| 1 core, park | **qb 36 ns** | SObjectizer 200 ns | 19 ns | CAF 502 ns · qb 5.47× |
| 2 cores, spin | **qb 31 ns** | CAF 308 ns | **43 ns** | SObjectizer 378 ns · qb 10.00× |
| 2 cores, park | **qb 33 ns** | CAF 308 ns | **60 ns** | SObjectizer 443 ns · qb 9.26× |

The same 84 cells on Linux — WSL2 Debian 13, g++ 14.2, `-O3 -DNDEBUG`, the same two CPUs, 5
repetitions, one quiet session on 2026-09-04 (`results/wsl-debian-g++14/`):

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 98 ns** | SObjectizer 145 ns | 2 ns | CAF 284 ns · qb 1.48× |
| 1 core, park | **qb 99 ns** | SObjectizer 166 ns | 2 ns | CAF 276 ns · CAF-detached 3.54 µs · qb 1.69× |
| 2 cores, spin | **qb 268 ns** | CAF 298 ns | 210 ns | SObjectizer 630 ns · **no measurable difference** qb/CAF |
| 2 cores, park | **CAF 290 ns** | CAF-detached **bimodal**, ~3.63 µs or ~25.81 µs | **25.47 µs** | SObjectizer 26.53 µs · qb 26.78 µs |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 45 ns** | SObjectizer 108 ns | 3 ns | CAF 116 ns · qb 2.42× |
| 1 core, park | **qb 44 ns** | SObjectizer 110 ns | 6 ns | CAF 117 ns · qb 2.49× |
| 2 cores, spin | **qb 46 ns** | SObjectizer 163 ns | 24 ns | CAF 171 ns · qb 3.52× |
| 2 cores, park | **qb 46 ns** | CAF 167 ns | **60 ns** | SObjectizer 173 ns · qb 3.66× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 63 ns** | SObjectizer 71 ns | 3 ns | CAF 140 ns · qb 1.13× |
| 1 core, park | **qb 62 ns** | SObjectizer 82 ns | 13 ns | CAF 140 ns · qb 1.33× |
| 2 cores, spin | **CAF 140 ns** | qb 173 ns | 114 ns | SObjectizer 304 ns · CAF 1.23× |
| 2 cores, park | **CAF 141 ns** | SObjectizer 265 ns | **13.01 µs** | qb 13.41 µs · CAF 1.87× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 67 ns** | SObjectizer 101 ns | 3 ns | CAF 171 ns · qb 1.50× |
| 1 core, park | **qb 70 ns** | SObjectizer 107 ns | 7 ns | CAF 170 ns · qb 1.52× |
| 2 cores, spin | **qb 58 ns** | CAF 160 ns | 28 ns | SObjectizer 282 ns · qb 2.75× |
| 2 cores, park | **qb 57 ns** | CAF 170 ns | 42 ns | SObjectizer 310 ns · qb 2.98× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 37 ns** | SObjectizer 134 ns | 7 ns | CAF 291 ns · qb 3.57× |
| 1 core, park | **qb 39 ns** | SObjectizer 144 ns | 14 ns | CAF 294 ns · qb 3.65× |
| 2 cores, spin | **qb 33 ns** | SObjectizer 227 ns | 25 ns | CAF 248 ns · qb 6.93× |
| 2 cores, park | **qb 33 ns** | CAF 250 ns | **57 ns** | SObjectizer 280 ns · qb 7.48× |

**Read the Linux park rows with their caveat.** WSL2 is a Hyper-V guest: a futex wake of a
parked thread on another vCPU is a virtualised IPI and costs ~12 µs here, so the raw
`std::thread` + condition-variable **floor itself** is 25.47 µs per ping-pong round trip and
13.01 µs per ring hop. qb 3.1.0, SObjectizer's `simple_lock` and CAF's own detached threads (in
their slow mode) all sit on that floor on the two benchmarks that cross a core on every message
— on Linux qb's parked path is a plain condition variable and nothing worse — while the pooled
CAF row stays at ~290 ns because it never crosses a core (see point 3). Native Linux puts a futex
wake at 2–5 µs; that axis is not yet run, and until it is the Linux park rows bound the
hypervisor, not the frameworks.

Seven things in those tables are worth more than the ranking:

1. **Every framework that crosses a core on every message is faster on ONE core than on two.**
   A ping-pong and a ring have no parallelism, so a second core buys nothing and costs a cache
   line crossing on every hop: qb goes 112 → 308 ns on ping-pong and 63 → 172 ns on the ring;
   SObjectizer 184 → 922 and 91 → 482. The three benchmarks that DO carry parallelism —
   counting, fork-join, big — are the ones where two cores cost qb a few nanoseconds or gain it some, and
   where qb sits **below the raw-thread floor** at two cores: the floor's SPSC ring pays one
   cache-line crossing per message, qb's staging pipe moves them in batches. Anyone quoting a
   two-core ping-pong as evidence of scalability is quoting the wrong number.
2. **qb 3.1.0's parked mode collapses on a cross-core hop, on both platforms, and it is the most
   useful thing here.** Ping-pong 2c-park: **4.23 µs** against CAF's 490 ns and SObjectizer's
   1.03 µs on Windows; thread-ring 2c-park: **bimodal, ~565 ns or ~3.02 µs** per hop, where CAF
   holds 236 ns. On WSL2 both cells sit on the hypervisor's floor. It is **explained and
   reproduced** — [docs/TUNING.md §5](docs/TUNING.md): qb parks after two or three empty passes
   because its spin credit counts events rather than time, and its `Mailbox::wait()` has a
   lost-wakeup race that MSVC's millisecond `wait_for` turns into ~13 ms stalls. It is **fixed**
   on the local qb branch `perf/core-hot-path` — a race-free park handshake, a time-based
   idle-spin floor and a store-buffer-draining fence on every cross-core publish — and
   re-measured through these unmodified adapters, on the same quiet host, minutes after the
   shipped build, for all five benchmarks (the grids below). What the branch does NOT change is
   the price of actually sleeping: with its idle-spin floor set to 0 the ping-pong cell measures
   **386 ns** on Windows and **25.8 µs** on WSL2 — the OS wake cost, the same one every other
   parked framework pays ([§8.2](docs/TUNING.md)). The branch's gain on Linux is entirely the
   50 µs of spinning before the park, which is a policy, not a mechanism. The tables above stay
   at the shipped 3.1.0 until that branch ships.
3. **CAF's flat figure per benchmark is one configuration, measured four times, that never
   crosses a core.** Its `wait=1` profile (`aggressive-poll-attempts=100, steal-interval=10`) is
   CAF's **own shipped default** (`libcaf_core/caf/defaults.hpp`), and a ten-point sweep found
   every more aggressive profile *slower* ([docs/TUNING.md §1.1](docs/TUNING.md)) — so the
   adapter declares the two columns identical rather than measuring one thing twice. And on a
   ping-pong or a ring CAF runs the receiver on the **sender's worker** (`worker::delay` →
   `queue.prepend`, `scheduled_actor.cpp`), so its "2 cores" cells are one-thread cells, immune
   to the park cost every other framework pays — which is why CAF wins every 2c-park cell of
   the two benchmarks that cross a core per message, and, on WSL2, thread-ring at 2 cores
   outright (140 ns against qb 3.1.0's 173). The **`caf-detached`** row is CAF's honest
   cross-core cost — one pinned OS thread per actor, CAF's own placement primitive — and it is
   **bimodal**: a repetition lands at ~1.07 µs or at ~10.65 µs on Windows (~3.6 or ~25.8 µs on
   WSL2) and stays there, so the report prints both modes and refuses to rank against it
   ([frameworks/caf-detached/README.md](frameworks/caf-detached/README.md), [§8.1](docs/TUNING.md)).
   Read the two CAF rows together: the pool is the best case, detached is the cross-core case,
   and neither is "CAF's number" alone.
4. **The floor matters.** At 2 cores spinning on ping-pong, the fastest framework is 1.68× the
   floor; at 1 core it is 66×. The same frameworks, the same code, and a completely different
   story about what "framework overhead" means. And a framework can sit *below* the floor —
   pooled CAF at 2c-park on WSL2 is 0.01× a floor that crosses a core; qb on counting, fork-join
   and big at two cores is 0.55–0.87× a floor that crosses it per message — which the report now says out loud
   instead of printing as a ratio.
5. **The widest margins are on the funnel and the all-to-all, and they are qb's dispatch, not
   its scheduler.** On `big`, every one of 120 actors sends to a random peer, so every message is
   a hash lookup and a type-erased dispatch in every framework; qb's 31–36 ns per round trip against
   CAF's 308–502 and SObjectizer's 187–443 is the cost of `EventBucket` relocation plus one
   `unordered_map` lookup against a mailbox enqueue, a work-item allocation and a
   `std::function`-shaped handler call. The margin is the same on both compilers (WSL2: 33–39 ns vs
   248–294 vs 134–280).
6. **The narrowest margin is the ring, and it names qb's own cost.** thread-ring is one hop per
   message with nothing to batch; qb 3.1.0's 63 ns per hop on one core is only 1.13× (WSL2) to
   1.45× (Windows) faster than SObjectizer and 7–22× the floor; the branch's 43–46 ns on Windows
   and 37–48 ns on WSL2 (a spread the ring shows between runs there, spin and park alike) is
   what removing the out-of-line accessors, the two hash lookups per dispatch and the
   per-event publish on that path is worth. Every qb-side finding from these five benchmarks — what
   it costs, where, and what was done about it — is in [docs/TUNING.md §9](docs/TUNING.md).
7. **The 3.2.0 candidate is measured for all EIGHT shapes, on both hosts, in one session each,
   and it is the fastest framework in every one of the 64 cells — twice.** The grid was taken
   at the programme's midpoint and again at its end. **Midpoint, 2026-09-07:** qb `develop` at
   `43f62afe`, 29 commits over 3.1.0 — axes A–N, the segmented pipe (QB-43), the dense router,
   the default-event registry (QB-174), the dense-table growth fib found, the five ask-path
   fixes bank-transaction found and the ask slot table (QB-178) — candidate / shipped 3.1.0 /
   candidate, 9 + 2 (`results/<host>/qb-branch-develop/grid-43f62afe/`; `docs/TUNING.md` §13).
   **Final, 2026-09-09:** qb `develop` at **`77b358d8`**, 66 commits over 3.1.0 and 37 over
   the midpoint — the loop clock (QB-180), the pass's fixed cost (QB-182), the ring's private
   lines (QB-184), the ask resumed inline (QB-185), the qev programme (QB-187 to QB-195: the
   non-blocking pass at its floor, the deadline list, the io cadence, the pass without the
   loop, the Windows clock on QPC), the one loop reference (QB-199), the sub-millisecond park
   (QB-196) — the same protocol, the same adapters, candidate / shipped 3.1.0 / candidate back
   to back (Windows 15:39:09–15:42:12 UTC, WSL2 14:19:38–14:27:51 UTC, the other side idle each
   time; `grid-77b358d8/`, `grid-shipped-3.1.0-final/`, `grid-77b358d8-pass2/`; §13.4). The
   `qb` items above are the published shipped runs; the same-session controls agree with them
   within the spread, except where a collapsed cell has no stable figure — which was the point.

<!-- the two grids below are the 3.2.0 candidate at its FINAL commit; check-report verifies them against their own directory -->
The candidate on Windows (`results/desktop-b67osn6-win-msvc/qb-branch-develop/grid-77b358d8/`; per unit — round trip, message, hop, message, round trip, actor, meeting, transfer):

<!-- check-report: results/desktop-b67osn6-win-msvc/qb-branch-develop/grid-77b358d8 framework=qb -->
| benchmark | 1 core, spin | 1 core, park | 2 cores, spin | 2 cores, park |
|---|---|---|---|---|
| ping-pong | 30 ns | 30 ns | 191 ns | 193 ns |
| counting | 8 ns | 11 ns | 13 ns | 13 ns |
| thread-ring | 18 ns | 18 ns | 100 ns | 104 ns |
| fork-join | 9 ns | 8 ns | 11 ns | 12 ns |
| big | 17 ns | 17 ns | 24 ns | 24 ns |
| fib | 176 ns | 180 ns | 108 ns | 110 ns |
| chameneos | 30 ns | 30 ns | 74 ns | 70 ns |
| bank-transaction | 231 ns | 234 ns | 143 ns | 139 ns |

The candidate on WSL2 (`results/wsl-debian-g++14/qb-branch-develop/grid-77b358d8/`):

<!-- check-report: results/wsl-debian-g++14/qb-branch-develop/grid-77b358d8 framework=qb -->
| benchmark | 1 core, spin | 1 core, park | 2 cores, spin | 2 cores, park |
|---|---|---|---|---|
| ping-pong | 23 ns | 23 ns | 159 ns | 152 ns |
| counting | 7 ns | 7 ns | 9 ns | 9 ns |
| thread-ring | 17 ns | 17 ns | 72 ns | 75 ns |
| fork-join | 8 ns | 7 ns | 8 ns | 8 ns |
| big | 18 ns | 18 ns | 18 ns | 18 ns |
| fib | 124 ns | 123 ns | 83 ns | 83 ns |
| chameneos | 26 ns | 25 ns | 44 ns | 44 ns |
| bank-transaction | 143 ns | 141 ns | 79 ns | 85 ns |

Against shipped 3.1.0 in the same session, **all 64 cells are faster and none is inside the
spread**; against the midpoint grid, **every one of the 32 WSL2 cells is faster in both passes**
(ping-pong 1c 66 → 23 ns, thread-ring 1c 39 → 17, ping-pong 2c 222 → 159, ring 2c 112 → 72,
counting 2c 11.0 → 9.0, fork-join 8–10 → 7–8, big 21–23 → 18, fib 133 → 124 and 90 → 83,
chameneos 35 → 26 and 54 → 44, bank 2c 93 → 79; bank 1c level at 143) and on Windows the
one-core cells follow (ping-pong 84 → 30, ring 45 → 18, big 20 → 17, chameneos 35 → 30, bank
263 → 231, fib 199 → 176) while its two-core cells are bimodal per launch, so a grid median lands
wherever the majority fell — counting 2c read 11.7 at the midpoint and 13.4 now, chameneos 2c
65 and 74 — and for those the interleaved census is the instrument: ten alternated launches of
the final build against the midpoint build in one session
(`qb-branch-develop/census-77b358d8-vs-43f62afe/`) read counting 13.6 vs 13.5, chameneos 66.9 vs
67.8 (its lower mode 46.8 vs 55.2), ping-pong 189 vs 243 and thread-ring 107 vs 120 — level or
better on every cell, the cross-session difference being the host's mode of the day. The two 2c-park
collapses are gone on both hosts (ping-pong 3.07 µs → 193 ns on Windows this session, 26.27 µs →
152 ns on WSL2; thread-ring 514 → 104 and 13.10 µs → 75 — the §5 defect, and on WSL2 gone under a
hypervisor whose futex wake alone is 12 µs). The one-core round trip the midpoint left as the next
thing to profile (84 / 66 ns, two dispatches each paying a full pass) is **30 / 23 ns** now, and
the ring's hop 18 / 17: §14–§17 are what that cost. Against the field the margin runs from 1.87× (ping-pong 2c-spin on WSL2, against CAF's 297 ns)
to 20.9× (fork-join 2c-park on WSL2, against CAF's 170) and 2.27× to 18.1× on Windows, and the
candidate sits **below the raw-thread floor in 13 of the 16 two-core cells on Windows and 14 of
16 on WSL2** (11 and 12 at the midpoint); the cells still above it cross a core per message with
nothing to batch.

What the final grid leaves is named in `docs/TUNING.md` §13.4: fib's 176 / 124 ns per actor
lifetime against a floor of 65 / 30, MSVC's wide two-core cells (the census's lower modes say what
the hardware can do; the host decides how often it does it), and the two-core round trip on
Windows at 191 ns against 159 on WSL2 — the cross-core hop's remaining cost is the platform's, not
the pass's.


The one-core cells measure the dispatch, not a cold burst: swept along the burst size
(`burst-sweep/` on both hosts, `docs/TUNING.md` §9.11, measured at `279e6cd4` and unchanged by
the commits since — counting 1c is 10.0 / 8.7 ns in the grids above), the same-core dispatch is
**5.9–9.3 ns from 2 000 to 4 M messages on g++** and 6.6–10.4 on MSVC — 9.2 / 9.5 ns at the
Savina 1 M against 35.0 / 25.8 for `230c5035`, 43 / 30 for shipped 3.1.0, 115–185 for CAF and
91–143 for SObjectizer, over a 2.8–3.0 ns floor. The 1 M protocol stays — it is the Savina
figure and every framework runs it — and it is no longer a caveat. The previous candidate's
five-shape grids (`perf/event-pipe-segmented` `279e6cd4`, `qb-branch-perf-event-pipe-segmented/grid-final/`)
stay beside the new ones as the A/B that produced QB-43.

The same 84 cells on macOS — Apple M4 Pro (10 P + 4 E cores), macOS 26.6, AppleClang 21.0.0, `-O3 -DNDEBUG`, **unpinned**, 7 repetitions + 2 warmup, one quiet session on 2026-09-05 (`results/macbook-m4pro-macos-clang21/`). macOS has no verified CPU affinity API, so the harness was run with `--no-pin` and every document carries `pinned:false` — the qb rows here are the shipped v3.1.0 (`830ea244`); the candidate's grids sit beside them in `qb-branch-perf-event-pipe-segmented/`, read in `docs/TUNING.md` §9.13:

`savina/ping-pong` — 1 000 000 round trips, two actors; per round trip:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 87 ns** | SObjectizer 135 ns | 4 ns | CAF 263 ns · qb 1.55× |
| 1 core, park | **qb 84 ns** | SObjectizer 151 ns | 4 ns | CAF 261 ns · CAF-detached 6.15 µs · qb 1.80× |
| 2 cores, spin | **qb 247 ns** | CAF 416 ns | 181 ns | SObjectizer 729 ns · qb 1.69× |
| 2 cores, park | **CAF 381 ns** | SObjectizer 5.61 µs | **4.62 µs** | CAF-detached 6.13 µs · qb 6.85 µs · CAF 14.75× |

`savina/counting` — 1 000 000 messages from a producer into one counter, then one retrieve; per message:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 11 ns** | SObjectizer 64 ns | 4 ns | CAF 73 ns · qb 5.77× |
| 1 core, park | **qb 11 ns** | SObjectizer 67 ns | 4 ns | CAF 72 ns · qb 6.08× |
| 2 cores, spin | **qb 21 ns** | CAF 140 ns | **65 ns** | SObjectizer 215 ns · qb 6.70× |
| 2 cores, park | **qb 52 ns** | CAF 124 ns | 39 ns | SObjectizer 155 ns · qb 2.39× |

`savina/thread-ring` — 100 actors in a ring, a token making 1 000 000 hops; per hop:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 43 ns** | SObjectizer 57 ns | 5 ns | CAF 122 ns · qb 1.32× |
| 1 core, park | **qb 43 ns** | SObjectizer 65 ns | 5 ns | CAF 123 ns · qb 1.52× |
| 2 cores, spin | **qb 126 ns** | CAF 178 ns | 90 ns | SObjectizer 337 ns · qb 1.41× |
| 2 cores, park | **SObjectizer 170 ns** | CAF 173 ns | **2.49 µs** | qb 3.39 µs · **no measurable difference** SObjectizer/CAF |

`savina/fork-join` — 10 000 messages fanned out to each of 60 workers, 600 000 in all, each worker acknowledged once at the end; per message:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 13 ns** | SObjectizer 45 ns | **33 ns** | CAF 146 ns · qb 3.52× |
| 1 core, park | **qb 13 ns** | SObjectizer 49 ns | **34 ns** | CAF 148 ns · qb 3.80× |
| 2 cores, spin | **qb 15 ns** | CAF 117 ns | **27 ns** | SObjectizer 262 ns · qb 7.95× |
| 2 cores, park | **qb 22 ns** | CAF 110 ns | 21 ns | SObjectizer 203 ns · qb 5.10× |

`savina/big` — 120 actors each sending 20 000 pings to random peers, every ping answered; per round trip (2 400 000 of them):

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 24 ns** | SObjectizer 174 ns | 5 ns | CAF 258 ns · qb 7.35× |
| 1 core, park | **qb 24 ns** | SObjectizer 181 ns | 6 ns | CAF 257 ns · qb 7.65× |
| 2 cores, spin | **qb 25 ns** | CAF 381 ns | **36 ns** | SObjectizer 392 ns · qb 15.26× |
| 2 cores, park | **qb 24 ns** | SObjectizer 373 ns | **38 ns** | CAF 386 ns · qb 15.23× |

The **2 cores, park** column measures macOS's condition-variable wake, not a framework: the raw floor is **4.62 µs per ping-pong round trip** (`baseline__2c-park`), shipped qb 3.1.0 6.85 µs, SObjectizer 5.61 µs, `caf-detached` 6.13 µs — the pooled `caf` row (381 ns) is below that floor because it never crosses a core. The candidate branch's park handshake (axes A/B/C) brings qb's cell to ~210 ns on this host; see §9.13.

## Running it

```powershell
# Windows / MSVC
. tools/msvc-env.ps1
cmake -S . -B build/final -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build/final

python tools/run.py --build build/final --out results/<host-id> --repetitions 9 --cpus 0,2
python tools/report.py --results results/<host-id> > REPORT.md
```

`--cpus` is not optional in spirit. On a hybrid CPU an unpinned run is not a measurement, and the
harness **aborts** rather than report a number when a requested pin cannot be read back.

Three things about a run worth knowing before reading its output:

- **A cell has three verdicts, not two.** `ok` (every repetition verified), `FAILED` (a
  checksum, a pin or the window was wrong — harness exit 1 or 2), and **`n/a`** (harness exit 3):
  the adapter declared, with a reason, that this framework cannot express the configuration.
  The reason is written into the JSON and rendered in the cell. `caf-detached` reports its two
  spin cells this way, because a `caf::detached` actor has no spin mode; a number invented for
  that cell would be a pool measurement under a detached label. `run.py` counts the three
  separately and never folds `n/a` into `not verified`.
- **A filtered run merges, and refuses to merge across conditions.** `--only`, `--benchmark`
  and `--config` re-run a subset; the manifest keeps every cell that was not re-run and counts
  `merged_partial_runs`. It refuses if the host, platform, CPU set, repetition or warmup count
  differ from the manifest already there — a table stitched from two hosts is not a table.
- **The report reads `results/<host>/<benchmark>/` only.** Side experiments live in sibling
  directories (`qb-branch-perf-core-hot-path/`, `caf-spin-sweep/`, `sobjectizer-spin-sweep/`) whose documents declare a
  benchmark their directory is not named for; `report.py` names them on stderr and leaves them
  out, and two documents for one cell are a hard stop rather than a last-writer-wins. A cell
  whose repetitions split into two modes more than 2× apart is marked **bimodal** with both
  modes printed and no ordering claimed against it.

## Disagreeing with it

[docs/CHALLENGE.md](docs/CHALLENGE.md). If you know one of these frameworks better than this
repository's author does — and for CAF and SObjectizer that is likely — send a better
implementation. The rule is written down in advance:

> A submitted implementation that is correct, idiomatic and faster replaces the one here, and the
> tables are regenerated. Including when that makes qb lose.

That has already happened once without anyone having to ask: the spin profile this repository
first handed CAF made CAF almost 2× slower than leaving it alone. It is recorded in
[docs/TUNING.md](docs/TUNING.md).

## Layout

```
FAIRNESS.md            the protocol -- read first
REPORT.md              generated; every figure comes from results/
harness/               the only timing, verification and reporting code
benchmarks/specs/      framework-free expected values, included by every implementation
frameworks/<fw>/       one adapter per framework + its shared setup header
results/<host-id>/     one JSON per (framework x benchmark x configuration)
docs/TUNING.md         the configuration sweeps, including the one that embarrassed the author, and every qb-side finding
docs/FEATURES.md       what each framework offers, cited to its source
docs/ROADMAP.md        what is not done yet
tools/                 run.py, report.py, check-report.py, check-roster.py, the negative-control batteries
tools/probes/          qb-only instruments (`qvoprobe-*`): undiscoverable by run.py, never a table cell; docs/TUNING.md sections 10, 15–19
```
