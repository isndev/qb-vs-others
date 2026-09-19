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
| `savina/ping-pong`, `counting`, `thread-ring`, `fork-join`, `big` × qb, CAF, SObjectizer, floor (+ CAF-detached on ping-pong) | **done**, 84 cells per host: 82 verified + 2 declared `n/a` (a `caf::detached` actor has no spin mode) — first with shipped 3.1.0 (2026-09-04), **re-measured whole on 2026-09-13 with the 3.2.0 candidate `f2779605` in the candidate's own session** (every framework, 9 + 2, both hosts), which is what the tables below show |
| The document guards (`tools/check-roster.py`, `tools/check-report.py`) | **done**, and negative-controlled: 33 CAUGHT / 3 CONFIRMED / **0 MISSED** (`tools/guards-negative-control.py`) |
| Feature comparison, cited to the three sources | [docs/FEATURES.md](docs/FEATURES.md) |
| `savina/fib`, `savina/chameneos` × the same four (+ CAF-detached declared omitted) | **done on Windows and WSL2**: 16 cells each per host, shipped qb 3.1.0 like the five before them (`results/<host>/savina-fib/`, `savina-chameneos/`, rendered in each host's `REPORT.md`), **116 cells per host**; on macOS and on the arm64 Linux guest since 2026-09-19, with the candidate as the qb column. Written against the qb branch they produced (`results/<host>/qb-branch-perf-dense-table-growth/`): fib found a 43 s defect in unreleased `develop` and drove three qb commits, and its shipped-3.1.0 cell is a LOGGING figure — nine `LOG_INFO` lines per actor lifetime, 515 819 lines per repetition, 159 / 459 ms (WSL2 / Windows, 2c-spin) against the branch's 7.6 / 10.5 in the same session, CAF 39 / 53, floor 3.4 / 4.0 (`docs/TUNING.md` §11) |
| `savina/bank-transaction` × the same four (+ CAF-detached declared omitted) | **done on Windows and WSL2** (2026-09-07): 16 cells per host, shipped qb 3.1.0 (`results/<host>/savina-bank-transaction/`, rendered in each host's `REPORT.md`), **132 cells per host**; on macOS and on the arm64 Linux guest since 2026-09-19, with the candidate as the qb column. The first shape that WAITS for a reply — one `qb::ask` / CAF `request().then()` per transfer, 50 000 of them — and it found five defects on qb's ask path in one afternoon (`docs/TUNING.md` §12, qb `fa1c5ce3`): shipped 3.1.0 measures 14.7 / 9.2 ms (WSL2, 1c / 2c spin) and 25.5 / 29.2 (Windows), qb `develop` before the fixes 9.4 / 5.1 and 13.7 / 8.0, after them **8.1 / 4.6** and **12.9 / 7.6** in the same session (`results/<host>/qb-branch-perf-coro-scope-local-refcount/`), against CAF 41.5 / 36.6 and 57.8 / 57.5, SObjectizer 19.5 / 25.5 and 29.6 / 38.0, floor 1.1 / 6.3 and 3.5 / 32.2 |
| **The 3.2.0 candidate grid** — qb `develop` × all eight shapes | **done on Windows and WSL2, twice**: at the midpoint (`43f62afe`, 2026-09-07) and at the final commit (**`77b358d8`**, 2026-09-09) — 96 qb cells per host each time (candidate / shipped 3.1.0 / candidate, 9 + 2, one quiet session per host, `results/<host>/qb-branch-develop/`), the fastest framework in all 64 cells both times, every WSL2 cell faster at the end than at the midpoint (ping-pong 1c 66 → 23 ns, ring 1c 39 → 17), the Windows two-core cells level-or-better under the interleaved census; the two `framework=qb` grids below, `docs/TUNING.md` §13 and §13.4. — and **a third time on 2026-09-13**, at the release candidate **`f2779605`**, with the WHOLE field in the same session (`grid-f2779605/`, `grid-shipped-3.1.0-20260913/`, `census-f2779605-field/`; point 7 and §13.5): fastest in all 64 cells, no cell slower than 3.1.0, at or under the raw-thread floor on the two-core census cells — and **a fourth time on 2026-09-19, on the two arm64 hosts**, at **`174e515a`**: `f2779605` plus the four changes that landed after it (the actor arena QB-212, `pin_frame_copy` QB-213, the frame-free `qb::ask` QB-214, `qb::growable_ring` QB-215), against shipped 3.1.0 AND against `f2779605` in one session per host, the whole field beside them (`results/macbook-m4pro-macos-clang21/qb-branch-develop/`, `results/utm-debian13-arm64-g++14/qb-branch-develop/`; §13.9): fastest in all 64 cells again, no cell slower than 3.1.0, fib −22 % and bank-transaction −18 % against `f2779605` on macOS — and two small cells the arena costs, attributed there |
| The other 17 Savina benchmarks | **not yet written** — see [docs/ROADMAP.md](docs/ROADMAP.md) |
| Linux axis (WSL2 Debian 13 / g++ 14.2) | **run**, the same 132 cells, re-measured with the candidate on 2026-09-13 — with the WSL2 caveat below. **A native-arm64 Linux guest** (UTM / QEMU on the Apple M4 Pro, Debian 13 / g++ 14.2, vCPUs 2 and 4) joined on 2026-09-19: the same 132 cells with the candidate `174e515a`, and a guest's park floor of its own (20.8 µs); bare-metal Linux not yet |
| macOS axis (Apple M4 Pro / AppleClang 21, arm64) | **run**, all 132 cells since 2026-09-19 (84 on 2026-09-05) — **unpinned** (macOS has no verified affinity API; every document says `pinned:false`, and every two-core figure is read from a launch census); the candidate `174e515a` beside shipped 3.1.0 and `f2779605` in the same session, `docs/TUNING.md` §13.9 (§9.13 for the 2026-09-05 session) |
| Seastar | not yet — Linux-only, and its dependencies need root on this host |
| Cross-language references (Erlang, Pekko, Actix, Orleans) | not yet |

Five benchmarks are five shapes — a two-actor round trip, a many-to-one funnel, a ring, a
scatter-gather and an all-to-all — and the results below should be read per shape, not as one
ranking.

## What five benchmarks have shown so far

MSVC 19.51, i9-12900K, pinned to two P-cores, 9 repetitions + 2 warmup, every cell of every
framework measured in ONE quiet session on 2026-09-13 (03:23–03:30 UTC+2), with qb `develop`
**`f2779605`** — the 3.2.0 release candidate — as the `qb` column; shipped 3.1.0 is the
same-session control beside it (point 7, `results/<host>/qb-branch-develop/`). Until that day
these tables carried the shipped 3.1.0 measured on 2026-09-04; what 3.1.0 read is kept in the
control directories and in points 2 and 6 below. Full tables in [REPORT.md](REPORT.md), all
regenerated from `results/`;
the figures below are **per unit of work** — a round trip, a message, a hop, a message, a round trip —
and the unit is declared once, in each benchmark's spec header, never chosen by an adapter.
A **bold floor** is a floor the fastest framework sits *below*: it is not beating raw threads,
it is not doing what the floor does (there, crossing a core on every message).

`savina/ping-pong` — 1 000 000 round trips, two actors; per round trip:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 30 ns** | SObjectizer 182 ns | 2 ns | CAF 477 ns · qb 6.02× |
| 1 core, park | **qb 30 ns** | SObjectizer 210 ns | 2 ns | CAF 480 ns · CAF-detached 10.46 µs · qb 6.96× |
| 2 cores, spin | **qb 185 ns** | CAF 478 ns | 178 ns | SObjectizer 894 ns · qb 2.58× |
| 2 cores, park | **qb 208 ns** | CAF 476 ns | **354 ns** | SObjectizer 995 ns · CAF-detached **bimodal**, ~979 ns or ~10.42 µs · qb 2.29× |

`savina/counting` — 1 000 000 messages from a producer into one counter, then one retrieve; per message:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 10 ns** | SObjectizer 139 ns | 3 ns | CAF 180 ns · qb 14.26× |
| 1 core, park | **qb 11 ns** | SObjectizer 146 ns | 7 ns | CAF 179 ns · qb 12.70× |
| 2 cores, spin | **qb 13 ns** | CAF 129 ns | **40 ns** | SObjectizer 288 ns · qb 9.56× |
| 2 cores, park | **qb 14 ns** | CAF 173 ns | **100 ns** | SObjectizer 309 ns · qb 12.75× |

`savina/thread-ring` — 100 actors in a ring, a token making 1 000 000 hops; per hop:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 18 ns** | SObjectizer 91 ns | 9 ns | CAF 232 ns · qb 5.01× |
| 1 core, park | **qb 18 ns** | SObjectizer 105 ns | 10 ns | CAF 231 ns · qb 5.80× |
| 2 cores, spin | **qb 96 ns** | CAF 236 ns | **109 ns** | SObjectizer 501 ns · qb 2.47× |
| 2 cores, park | **qb 107 ns** | CAF 233 ns | **238 ns** | SObjectizer 427 ns · qb 2.18× |

`savina/fork-join` — 10 000 messages fanned out to each of 60 workers, 600 000 in all, each worker acknowledged once at the end; per message:

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 11 ns** | SObjectizer 139 ns | 5 ns | CAF 309 ns · qb 12.12× |
| 1 core, park | **qb 12 ns** | SObjectizer 144 ns | 9 ns | CAF 310 ns · qb 12.21× |
| 2 cores, spin | **qb 11 ns** | CAF 187 ns | **29 ns** | SObjectizer 283 ns · qb 17.35× |
| 2 cores, park | **qb 11 ns** | CAF 171 ns | **63 ns** | SObjectizer 292 ns · qb 15.01× |

`savina/big` — 120 actors each sending 20 000 pings to random peers, every ping answered; per round trip (2 400 000 of them):

<!-- check-report: results/desktop-b67osn6-win-msvc benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 17 ns** | SObjectizer 183 ns | 11 ns | CAF 479 ns · qb 10.68× |
| 1 core, park | **qb 17 ns** | SObjectizer 195 ns | **20 ns** | CAF 479 ns · qb 11.34× |
| 2 cores, spin | **qb 23 ns** | CAF 305 ns | **44 ns** | SObjectizer 357 ns · qb 13.20× |
| 2 cores, park | **qb 24 ns** | CAF 306 ns | **57 ns** | SObjectizer 423 ns · qb 12.94× |

The same cells on Linux — WSL2 Debian 13, g++ 14.2, `-O3 -DNDEBUG`, the same two vCPUs, 9 + 2,
one quiet session on 2026-09-13 (03:42–04:01 UTC+2, the Windows side idle), the same candidate
(`results/wsl-debian-g++14/`):

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 22 ns** | SObjectizer 139 ns | 2 ns | CAF 275 ns · qb 6.22× |
| 1 core, park | **qb 22 ns** | SObjectizer 163 ns | 2 ns | CAF 276 ns · CAF-detached 3.31 µs · qb 7.25× |
| 2 cores, spin | **qb 160 ns** | CAF 284 ns | **185 ns** | SObjectizer 666 ns · qb 1.77× |
| 2 cores, park | **qb 153 ns** | CAF 280 ns | **24.82 µs** | CAF-detached **bimodal**, ~3.34 µs or ~25.32 µs · SObjectizer 26.21 µs · qb 1.82× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 7 ns** | SObjectizer 105 ns | 3 ns | CAF 114 ns · qb 14.25× |
| 1 core, park | **qb 8 ns** | SObjectizer 113 ns | 8 ns | CAF 114 ns · qb 14.48× |
| 2 cores, spin | **qb 9 ns** | CAF 165 ns | **21 ns** | SObjectizer 171 ns · qb 18.14× |
| 2 cores, park | **qb 9 ns** | CAF 167 ns | **67 ns** | SObjectizer 185 ns · qb 18.39× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 17 ns** | SObjectizer 70 ns | 3 ns | CAF 138 ns · qb 4.13× |
| 1 core, park | **qb 17 ns** | SObjectizer 82 ns | 13 ns | CAF 139 ns · qb 4.87× |
| 2 cores, spin | **qb 73 ns** | CAF 141 ns | **102 ns** | SObjectizer 276 ns · qb 1.92× |
| 2 cores, park | **qb 75 ns** | CAF 140 ns | **12.63 µs** | SObjectizer 245 ns · qb 1.87× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 8 ns** | SObjectizer 103 ns | 3 ns | CAF 259 ns · qb 12.60× |
| 1 core, park | **qb 7 ns** | SObjectizer 107 ns | 7 ns | CAF 234 ns · qb 14.91× |
| 2 cores, spin | **qb 8 ns** | CAF 202 ns | **25 ns** | SObjectizer 287 ns · qb 24.87× |
| 2 cores, park | **qb 8 ns** | CAF 191 ns | **41 ns** | SObjectizer 317 ns · qb 23.96× |

<!-- check-report: results/wsl-debian-g++14 benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 18 ns** | SObjectizer 133 ns | 7 ns | CAF 291 ns · qb 7.58× |
| 1 core, park | **qb 18 ns** | SObjectizer 142 ns | 15 ns | CAF 288 ns · qb 8.06× |
| 2 cores, spin | **qb 18 ns** | SObjectizer 220 ns | **25 ns** | CAF 247 ns · qb 12.26× |
| 2 cores, park | **qb 18 ns** | CAF 247 ns | **52 ns** | SObjectizer 278 ns · qb 14.10× |

**Read the Linux park rows with their caveat.** WSL2 is a Hyper-V guest: a futex wake of a
parked thread on another vCPU is a virtualised IPI and costs ~12 µs here, so the raw
`std::thread` + condition-variable **floor itself** is 24.82 µs per ping-pong round trip and
12.63 µs per ring hop. SObjectizer's `simple_lock` and CAF's own detached threads (in their
slow mode) sit on that floor on the two benchmarks that cross a core on every message, and qb
3.1.0 sat there too (26.24 µs and 13.09 µs in the same-session control) — its parked path was a
plain condition variable and nothing worse. The candidate does not: 153 ns and 75 ns, because
it spins for 50 µs before it parks (point 2) and a ping-pong never leaves that window. The
pooled CAF row stays at ~280 ns because it never crosses a core (point 3). Native Linux puts a
futex wake at 2–5 µs; that axis is not yet run, and until it is the Linux park rows of the
frameworks that DO sleep bound the hypervisor, not them.

Seven things in those tables are worth more than the ranking:

1. **Every framework that crosses a core on every message is faster on ONE core than on two.**
   A ping-pong and a ring have no parallelism, so a second core buys nothing and costs a cache
   line crossing on every hop: qb goes 30 → 185 ns on ping-pong and 18 → 96 ns on the ring;
   SObjectizer 182 → 894 and 91 → 501. The three benchmarks that DO carry parallelism —
   counting, fork-join, big — are the ones where two cores cost qb a few nanoseconds or gain it some, and
   where qb sits **below the raw-thread floor** at two cores: the floor's SPSC ring pays one
   cache-line crossing per message, qb's staging pipe moves them in batches. Anyone quoting a
   two-core ping-pong as evidence of scalability is quoting the wrong number.
2. **qb 3.1.0's parked mode collapsed on a cross-core hop, on both platforms, and finding it
   was the most useful thing this repository did.** Ping-pong 2c-park read **4.23 µs** against
   CAF's 490 ns and SObjectizer's 1.03 µs on Windows (2.26 µs in the 2026-09-13 control, the
   cell being bimodal by launch); thread-ring 2c-park was **bimodal, ~565 ns or ~3.02 µs** per
   hop where CAF holds 236 ns; on WSL2 both cells sat on the hypervisor's floor. It is
   **explained and reproduced** — [docs/TUNING.md §5](docs/TUNING.md): qb parked after two or
   three empty passes because its spin credit counted events rather than time, and its
   `Mailbox::wait()` had a lost-wakeup race that MSVC's millisecond `wait_for` turned into
   ~13 ms stalls. It is **fixed** in the 3.2.0 candidate — a race-free park handshake, a
   time-based idle-spin floor, a store-buffer-draining fence on every cross-core publish, then
   the park moved inside the event loop (axis N) — and the same cells now read **208 ns** and
   **107 ns** on Windows, **153 ns** and **75 ns** on WSL2, through the unmodified adapters, in
   the same session as the 3.1.0 control. What the fix does NOT change is the price of actually
   sleeping: with the idle-spin floor set to 0 the ping-pong cell measures **386 ns** on Windows
   and **25.8 µs** on WSL2 — the OS wake cost, the same one every other parked framework pays
   ([§8.2](docs/TUNING.md)). The gain on Linux is entirely the 50 µs of spinning before the
   park, which is a policy, not a mechanism.
3. **CAF's flat figure per benchmark is one configuration, measured four times, that never
   crosses a core.** Its `wait=1` profile (`aggressive-poll-attempts=100, steal-interval=10`) is
   CAF's **own shipped default** (`libcaf_core/caf/defaults.hpp`), and a ten-point sweep found
   every more aggressive profile *slower* ([docs/TUNING.md §1.1](docs/TUNING.md)) — so the
   adapter declares the two columns identical rather than measuring one thing twice. And on a
   ping-pong or a ring CAF runs the receiver on the **sender's worker** (`worker::delay` →
   `queue.prepend`, `scheduled_actor.cpp`), so its "2 cores" cells are one-thread cells, immune
   to the park cost every other framework pays — which is why CAF won every 2c-park cell of
   the two benchmarks that cross a core per message against qb 3.1.0, and, on WSL2, thread-ring
   at 2 cores outright (140 ns against 3.1.0's 173). Against the candidate it wins none: 476 ns
   against 208 and 233 against 107 on Windows, 280 against 153 and 140 against 75 on WSL2 — a
   one-thread cell beaten by a two-thread one. The **`caf-detached`** row is CAF's honest
   cross-core cost — one pinned OS thread per actor, CAF's own placement primitive — and it is
   **bimodal**: a repetition lands at ~1.07 µs or at ~10.65 µs on Windows (~3.6 or ~25.8 µs on
   WSL2) and stays there, so the report prints both modes and refuses to rank against it
   ([frameworks/caf-detached/README.md](frameworks/caf-detached/README.md), [§8.1](docs/TUNING.md)).
   Read the two CAF rows together: the pool is the best case, detached is the cross-core case,
   and neither is "CAF's number" alone.
4. **The floor matters.** At 2 cores spinning on ping-pong, the fastest framework is 1.04×
   the floor on Windows (185 against 178 ns) and 0.86× on WSL2 (160 against 185); at 1 core it
   is 15× (30 against 2 ns) and 11×. The same framework, the same code, and a completely
   different story about what "framework overhead" means: a raw thread handing a cache line to
   another raw thread is what a cross-core hop costs, and an actor runtime that sits on it has
   nothing left to remove there; a function call is what the one-core floor costs, and the
   ~20–30 ns above it is the price of the model — a mailbox, a pipe, a dispatch — which is
   where 3.2.0 spent its effort (96–113 ns at 3.1.0). And a framework can sit *below* the
   floor — pooled CAF at 2c-park on WSL2 is 0.01× a floor that crosses a core; qb on counting,
   fork-join, big and chameneos at two cores is 0.13–0.73× a floor that crosses it per message,
   on the ring 0.72–0.88× where the floor spins (0.01× where it sleeps), bank-transaction
   0.64–1.20× — which the report says out loud instead of printing as a ratio.
5. **The widest margins are on the funnel and the all-to-all, and they are qb's dispatch, not
   its scheduler.** On `big`, every one of 120 actors sends to a random peer, so every message is
   a hash lookup and a type-erased dispatch in every framework; qb's 17–24 ns per round trip
   against CAF's 305–479 and SObjectizer's 183–423 is the cost of `EventBucket` relocation plus
   one dense-table lookup against a mailbox enqueue, a work-item allocation and a
   `std::function`-shaped handler call. The margin is the same on both compilers (WSL2: 18 ns
   vs 247–291 vs 133–278).
6. **The narrowest margin was the ring, and it named qb's own cost.** thread-ring is one hop
   per message with nothing to batch; qb 3.1.0's 63 ns per hop on one core was only 1.13×
   (WSL2) to 1.45× (Windows) faster than SObjectizer and 7–22× the floor. The candidate's
   **18 ns** on Windows and **17 ns** on WSL2 is 4–6× SObjectizer and 2× (Windows, a 9 ns
   floor) to 6× (WSL2, a 3 ns one) the floor: what removing the out-of-line accessors, the
   two hash lookups per dispatch, the per-event publish, the clock read per pass and the pass's
   own fixed cost was worth. Where the margin is narrowest NOW is the cross-core ring at two
   cores (1.9–2.5× CAF, at or under the raw-thread floor) — a hop that is a cache-line
   handoff, which nobody batches. Every qb-side finding — what it costs, where, and what was
   done about it — is in [docs/TUNING.md §9–§14](docs/TUNING.md).
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
   `qb` items above WERE the published shipped runs until 2026-09-13; the same-session controls
   agreed with them within the spread, except where a collapsed cell has no stable figure —
   which was the point. **Re-measured in full on 2026-09-13**, at **`f2779605`** — the release
   candidate as it will ship: `77b358d8` plus documentation commits and QB-211's CMake, the hot
   path byte-identical (`git diff 77b358d8..f2779605 -- src/` touches comments only) — and this
   time the WHOLE field in the candidate's session: candidate / shipped 3.1.0 / then every
   framework, 132 cells per host, 9 + 2, then a 12-launch interleaved census on the four
   two-core cells that decide a ranking (Windows 03:19–03:32, WSL2 03:33–04:02 UTC+2, the
   other side idle each time; `grid-f2779605/`, `grid-shipped-3.1.0-20260913/`,
   `census-f2779605-field/`; §13.5). The tables above are that session, and they say: qb is
   the fastest framework in **all 64 cells**, **no cell is slower than 3.1.0** (the smallest
   gain −26 %, on Windows `big` 2c-park; the largest −99.4 %), the candidate agrees with the
   2026-09-09 grid within the launch spread on every cell (the hot path did not move), and
   the census puts qb at the raw-thread floor on the Windows ping-pong (187 against 181 ns,
   overlapping) and **under** it everywhere else it was asked (thread-ring 105 against 112 on
   Windows; ping-pong 156 against 183 and thread-ring 75 against 103 on WSL2). Where qb still
   loses is only against the floor of the one-core cells whose floor is a bare function call —
   `fib` (an actor created and destroyed per unit: 124 / 179 ns against 28 / 58) and
   `bank-transaction` (an `ask` round trip per transfer: 144 / 230 ns against 22 / 74) — and,
   between the two compilers, MSVC against g++ on the same source: +36 % on the one-core
   ping-pong, +44 % on fib, +60 % on bank ([docs/TUNING.md §13.5](docs/TUNING.md)).

<!-- the two grids below are the 3.2.0 candidate at its FINAL commit; check-report verifies them against their own directory -->
The candidate on Windows (`results/desktop-b67osn6-win-msvc/qb-branch-develop/grid-f2779605/`; per unit — round trip, message, hop, message, round trip, actor, meeting, transfer):

<!-- check-report: results/desktop-b67osn6-win-msvc/qb-branch-develop/grid-f2779605 framework=qb -->
| benchmark | 1 core, spin | 1 core, park | 2 cores, spin | 2 cores, park |
|---|---|---|---|---|
| ping-pong | 31 ns | 31 ns | 186 ns | 200 ns |
| counting | 8 ns | 9 ns | 14 ns | 14 ns |
| thread-ring | 18 ns | 19 ns | 93 ns | 103 ns |
| fork-join | 9 ns | 13 ns | 13 ns | 10 ns |
| big | 17 ns | 17 ns | 19 ns | 25 ns |
| fib | 184 ns | 182 ns | 117 ns | 111 ns |
| chameneos | 30 ns | 30 ns | 74 ns | 75 ns |
| bank-transaction | 250 ns | 237 ns | 151 ns | 146 ns |

The candidate on WSL2 (`results/wsl-debian-g++14/qb-branch-develop/grid-f2779605/`):

<!-- check-report: results/wsl-debian-g++14/qb-branch-develop/grid-f2779605 framework=qb -->
| benchmark | 1 core, spin | 1 core, park | 2 cores, spin | 2 cores, park |
|---|---|---|---|---|
| ping-pong | 22 ns | 22 ns | 155 ns | 155 ns |
| counting | 8 ns | 8 ns | 9 ns | 10 ns |
| thread-ring | 17 ns | 17 ns | 75 ns | 75 ns |
| fork-join | 8 ns | 7 ns | 8 ns | 8 ns |
| big | 18 ns | 18 ns | 18 ns | 18 ns |
| fib | 124 ns | 123 ns | 84 ns | 86 ns |
| chameneos | 26 ns | 26 ns | 44 ns | 46 ns |
| bank-transaction | 140 ns | 142 ns | 76 ns | 84 ns |

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

The same five shapes on **macOS** — Apple M4 Pro (10 P + 4 E cores), macOS 26.6.2, AppleClang 21.0.0, `-O3 -DNDEBUG`, **unpinned**, 9 repetitions + 2 warmup, one quiet session on 2026-09-19 (`results/macbook-m4pro-macos-clang21/`, all eight shapes, 132 cells). macOS has no verified CPU affinity API, so the harness was run with `--no-pin` and every document carries `pinned:false`. **The qb rows are the 3.2.0 candidate `174e515a`** — `f2779605` plus the arena, the frame-free ask and the ring (QB-212 to QB-215); shipped 3.1.0 and `f2779605` were measured in the same session and sit in `qb-branch-develop/`, read in `docs/TUNING.md` §13.9. A two-core figure on this host is bimodal by launch: the census there is the instrument (ping-pong 2c-spin qb 196 [181 – 213] against CAF 387 and the floor's 238; thread-ring 92 against 192 and 120).

`savina/ping-pong` — 1 000 000 round trips, two actors; per round trip:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 25 ns** | SObjectizer 138 ns | 4 ns | CAF 264 ns · qb 5.50× |
| 1 core, park | **qb 25 ns** | SObjectizer 153 ns | 4 ns | CAF 261 ns · CAF-detached 6.33 µs · qb 6.06× |
| 2 cores, spin | **qb 207 ns** | CAF 386 ns | **229 ns** | SObjectizer 704 ns · qb 1.87× |
| 2 cores, park | **qb 196 ns** | CAF 386 ns | **4.42 µs** | SObjectizer 5.60 µs · CAF-detached 6.34 µs · qb 1.97× |

`savina/counting` — 1 000 000 messages from a producer into one counter, then one retrieve; per message:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 5 ns** | SObjectizer 65 ns | 4 ns | CAF 75 ns · qb 12.57× |
| 1 core, park | **qb 5 ns** | SObjectizer 69 ns | 4 ns | CAF 75 ns · qb 13.70× |
| 2 cores, spin | **qb 7 ns** | CAF 148 ns | **61 ns** | SObjectizer 280 ns · qb 21.57× |
| 2 cores, park | **qb 7 ns** | CAF 153 ns | **27 ns** | SObjectizer 172 ns · qb 22.30× |

`savina/thread-ring` — 100 actors in a ring, a token making 1 000 000 hops; per hop:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 18 ns** | SObjectizer 60 ns | 5 ns | CAF 123 ns · qb 3.25× |
| 1 core, park | **qb 19 ns** | SObjectizer 68 ns | 5 ns | CAF 125 ns · qb 3.61× |
| 2 cores, spin | **qb 89 ns** | CAF 183 ns | **115 ns** | SObjectizer 389 ns · qb 2.06× |
| 2 cores, park | **qb 84 ns** | CAF 184 ns | **2.45 µs** | SObjectizer 185 ns · qb 2.19× |

`savina/fork-join` — 10 000 messages fanned out to each of 60 workers, 600 000 in all, each worker acknowledged once at the end; per message:

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 6 ns** | SObjectizer 47 ns | **27 ns** | CAF 157 ns · qb 8.15× |
| 1 core, park | **qb 5 ns** | SObjectizer 49 ns | **25 ns** | CAF 147 ns · qb 9.22× |
| 2 cores, spin | **qb 6 ns** | CAF 108 ns | **30 ns** | SObjectizer 342 ns · qb 18.50× |
| 2 cores, park | **qb 6 ns** | CAF 107 ns | **23 ns** | SObjectizer 210 ns · qb 18.84× |

`savina/big` — 120 actors each sending 20 000 pings to random peers, every ping answered; per round trip (2 400 000 of them):

<!-- check-report: results/macbook-m4pro-macos-clang21 benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 13 ns** | SObjectizer 179 ns | 5 ns | CAF 256 ns · qb 13.97× |
| 1 core, park | **qb 13 ns** | SObjectizer 181 ns | 6 ns | CAF 255 ns · qb 14.24× |
| 2 cores, spin | **qb 16 ns** | CAF 375 ns | **48 ns** | SObjectizer 503 ns · qb 23.06× |
| 2 cores, park | **qb 15 ns** | SObjectizer 376 ns | **38 ns** | CAF 393 ns · qb 24.26× |

The **2 cores, park** column used to measure macOS's condition-variable wake rather than a framework — the raw floor is still **4.42 µs per ping-pong round trip** (`baseline__2c-park`), SObjectizer 5.60 µs, `caf-detached` 6.34 µs, and shipped qb 3.1.0 measured 6.91 µs in this same session — and the candidate's cell is the park handshake plus the 50 µs idle-spin floor: a ping-pong never sleeps. The pooled `caf` row is below the floor because it never crosses a core.

The candidate on macOS, all eight shapes (`results/macbook-m4pro-macos-clang21/qb-branch-develop/grid-174e515a/`; unpinned — the two-core columns are one launch each, the census in that directory's README is the figure to quote):

<!-- check-report: results/macbook-m4pro-macos-clang21/qb-branch-develop/grid-174e515a framework=qb -->
| benchmark | 1 core, spin | 1 core, park | 2 cores, spin | 2 cores, park |
|---|---|---|---|---|
| ping-pong | 25 ns | 25 ns | 195 ns | 208 ns |
| counting | 5 ns | 5 ns | 7 ns | 7 ns |
| thread-ring | 19 ns | 19 ns | 89 ns | 92 ns |
| fork-join | 6 ns | 6 ns | 6 ns | 6 ns |
| big | 13 ns | 13 ns | 17 ns | 16 ns |
| fib | 70 ns | 73 ns | 45 ns | 50 ns |
| chameneos | 25 ns | 24 ns | 40 ns | 46 ns |
| bank-transaction | 82 ns | 83 ns | 77 ns | 67 ns |

The same five shapes on a **native-arm64 Linux guest** — UTM / QEMU on the same Apple M4 Pro, Debian 13.7, g++ 14.2.0, `-O3 -DNDEBUG`, pinned to vCPUs 2 and 4, 9 repetitions + 2 warmup, one quiet session on 2026-09-19 an hour after the macOS one (`results/utm-debian13-arm64-g++14/`, 132 cells, the qb rows the same candidate `174e515a`). A guest pins a vCPU, not a core, and its cross-vCPU futex wake is the hypervisor's: the **2 cores, park** floor is 20.8 µs per round trip, WSL2's caveat on another hypervisor and another architecture.

`savina/ping-pong` — 1 000 000 round trips, two actors; per round trip:

<!-- check-report: results/utm-debian13-arm64-g++14 benchmark=savina/ping-pong -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 22 ns** | SObjectizer 100 ns | 4 ns | CAF 298 ns · qb 4.55× |
| 1 core, park | **qb 22 ns** | SObjectizer 132 ns | 4 ns | CAF 296 ns · CAF-detached 2.21 µs · qb 6.04× |
| 2 cores, spin | **qb 167 ns** | CAF 291 ns | **215 ns** | SObjectizer 742 ns · qb 1.75× |
| 2 cores, park | **qb 169 ns** | CAF 298 ns | **20.83 µs** | CAF-detached **bimodal**, ~2.23 µs or ~21.84 µs · SObjectizer 22.13 µs · qb 1.76× |

`savina/counting` — 1 000 000 messages from a producer into one counter, then one retrieve; per message:

<!-- check-report: results/utm-debian13-arm64-g++14 benchmark=savina/counting -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 6 ns** | SObjectizer 69 ns | 4 ns | CAF 79 ns · qb 11.66× |
| 1 core, park | **qb 6 ns** | SObjectizer 72 ns | 4 ns | CAF 78 ns · qb 12.36× |
| 2 cores, spin | **qb 8 ns** | SObjectizer 124 ns | **58 ns** | CAF 196 ns · qb 16.47× |
| 2 cores, park | **qb 8 ns** | SObjectizer 124 ns | **59 ns** | CAF 213 ns · qb 16.29× |

`savina/thread-ring` — 100 actors in a ring, a token making 1 000 000 hops; per hop:

<!-- check-report: results/utm-debian13-arm64-g++14 benchmark=savina/thread-ring -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 24 ns** | SObjectizer 48 ns | 5 ns | CAF 143 ns · qb 1.97× |
| 1 core, park | **qb 24 ns** | SObjectizer 57 ns | 5 ns | CAF 144 ns · qb 2.35× |
| 2 cores, spin | **qb 80 ns** | CAF 143 ns | **115 ns** | SObjectizer 333 ns · qb 1.78× |
| 2 cores, park | **qb 79 ns** | CAF 151 ns | **10.65 µs** | SObjectizer 181 ns · qb 1.90× |

`savina/fork-join` — 10 000 messages fanned out to each of 60 workers, 600 000 in all, each worker acknowledged once at the end; per message:

<!-- check-report: results/utm-debian13-arm64-g++14 benchmark=savina/fork-join -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 7 ns** | SObjectizer 48 ns | 4 ns | CAF 235 ns · qb 6.90× |
| 1 core, park | **qb 8 ns** | SObjectizer 66 ns | 4 ns | CAF 260 ns · qb 8.45× |
| 2 cores, spin | **qb 8 ns** | CAF 184 ns | **32 ns** | SObjectizer 334 ns · qb 22.59× |
| 2 cores, park | **qb 8 ns** | SObjectizer 273 ns | **19 ns** | CAF 298 ns · qb 34.13× |

`savina/big` — 120 actors each sending 20 000 pings to random peers, every ping answered; per round trip (2 400 000 of them):

<!-- check-report: results/utm-debian13-arm64-g++14 benchmark=savina/big -->
| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 16 ns** | SObjectizer 106 ns | 7 ns | CAF 291 ns · qb 6.74× |
| 1 core, park | **qb 16 ns** | SObjectizer 115 ns | 7 ns | CAF 292 ns · qb 7.34× |
| 2 cores, spin | **qb 16 ns** | CAF 254 ns | **44 ns** | SObjectizer 269 ns · qb 15.81× |
| 2 cores, park | **qb 17 ns** | CAF 246 ns | **88 ns** | SObjectizer 275 ns · qb 14.36× |

The candidate on the arm64 Linux guest (`results/utm-debian13-arm64-g++14/qb-branch-develop/grid-174e515a/`):

<!-- check-report: results/utm-debian13-arm64-g++14/qb-branch-develop/grid-174e515a framework=qb -->
| benchmark | 1 core, spin | 1 core, park | 2 cores, spin | 2 cores, park |
|---|---|---|---|---|
| ping-pong | 22 ns | 22 ns | 173 ns | 163 ns |
| counting | 6 ns | 6 ns | 8 ns | 7 ns |
| thread-ring | 24 ns | 25 ns | 80 ns | 80 ns |
| fork-join | 8 ns | 7 ns | 8 ns | 8 ns |
| big | 16 ns | 16 ns | 16 ns | 16 ns |
| fib | 80 ns | 80 ns | 52 ns | 53 ns |
| chameneos | 28 ns | 28 ns | 57 ns | 67 ns |
| bank-transaction | 79 ns | 79 ns | 66 ns | 64 ns |

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
