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
| `savina/ping-pong` × qb, CAF, CAF-detached, SObjectizer, floor | **done**, 20 cells: 18 verified + 2 declared `n/a` (a `caf::detached` actor has no spin mode) |
| The other 24 Savina benchmarks | **not yet written** — see [docs/ROADMAP.md](docs/ROADMAP.md) |
| Linux axis (WSL2 Debian 13 / g++ 14.2) | **run**, the same 20 cells, 18 verified + 2 `n/a` — with the WSL2 caveat below; native Linux not yet |
| Seastar | not yet — Linux-only, and its dependencies need root on this host |
| Cross-language references (Erlang, Pekko, Actix, Orleans) | not yet |

One benchmark is not a comparison of frameworks. It is a comparison of frameworks *on a
ping-pong*, which is the narrowest workload in the suite, and the results below should be read
that way.

## What one benchmark has shown so far

`savina/ping-pong`, 1 000 000 round trips per repetition, MSVC 19.51, i9-12900K, pinned to two
P-cores, 9 repetitions, every cell measured in one quiet session on 2026-09-04. Full tables in
[REPORT.md](REPORT.md), all regenerated from `results/`; the figures below are per round trip.

| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 114 ns** | SObjectizer 182 ns | 2 ns | CAF 483 ns · qb 1.60× |
| 1 core, park | **qb 115 ns** | SObjectizer 215 ns | 2 ns | CAF 481 ns · CAF-detached 10.61 µs · qb 1.86× |
| 2 cores, spin | **qb 304 ns** | CAF 490 ns | 193 ns | SObjectizer 931 ns · qb 1.61× |
| 2 cores, park | **CAF 487 ns** *(never crosses a core)* | SObjectizer 1.06 µs | 435 ns | **qb 5.02 µs** · CAF-detached **bimodal**, ~0.93 µs or ~10.6 µs |

The same 20 cells on Linux — WSL2 Debian 13, g++ 14.2, `-O3 -DNDEBUG`, the same two CPUs, 5
repetitions, one quiet session on 2026-09-04 (`results/wsl-debian-g++14/`):

| configuration | fastest | second | floor | the rest |
|---|---|---|---|---|
| 1 core, spin | **qb 100 ns** | SObjectizer 144 ns | 2 ns | CAF 275 ns · qb 1.44× |
| 1 core, park | **qb 98 ns** | SObjectizer 165 ns | 2 ns | CAF 275 ns · CAF-detached 3.5 µs · qb 1.69× |
| 2 cores, spin | **qb 262 ns** | CAF 283 ns | 187 ns | SObjectizer 639 ns · **no measurable difference** qb/CAF |
| 2 cores, park | **CAF 283 ns** *(never crosses a core)* | — | **25.1 µs** | SObjectizer 26.4 µs · qb 26.7 µs · CAF-detached **bimodal**, ~3.9 µs or ~25.7 µs |

**Read the Linux park row with its caveat.** WSL2 is a Hyper-V guest: a futex wake of a parked
thread on another vCPU is a virtualised IPI and costs ~12 µs here, so the raw `std::thread` +
condition-variable **floor itself** is 25.1 µs per round trip. qb, SObjectizer's `simple_lock`
and CAF's own detached threads (in their slow mode) all sit on that floor — on Linux qb's parked
path is a plain condition variable and nothing worse — while the pooled CAF row stays at 283 ns
because it never crosses a core (see point 3). Native Linux puts a futex wake at 2–5 µs; that
axis is not yet run, and until it is the Linux park row bounds the hypervisor, not the
frameworks.

Four things in that table are worth more than the ranking:

1. **Every framework that crosses a core is faster on ONE core than on two.** A ping-pong has
   no parallelism, so a second core buys nothing and costs a cache line crossing on every hop.
   qb goes 114 → 304 ns; SObjectizer 182 → 931. Anyone quoting a two-core actor benchmark as
   evidence of scalability is quoting the wrong number. (CAF's pooled row is flat at ~485 ns
   because it does not cross — point 3.)
2. **qb's parked mode is bad on Windows, and it is the most useful thing here.** 5.02 µs against
   CAF's 487 ns and SObjectizer's 1.06 µs, and the nine repetitions range 2.7–6.9 µs. It is
   **explained and reproduced** — [docs/TUNING.md §5](docs/TUNING.md): qb parks after two or
   three empty passes because its spin credit counts events rather than time, and its
   `Mailbox::wait()` has a lost-wakeup race that MSVC's millisecond `wait_for` turns into
   ~13 ms stalls. It is **fixed** on the local qb branch `perf/core-hot-path` — a race-free park
   handshake, a time-based idle-spin floor and a store-buffer-draining fence on every cross-core
   publish — and re-measured through this unmodified adapter on a quiet host with the shipped
   build beside it: **259 ns (Windows) / 212 ns (Linux)**, below pooled CAF and level with qb's
   own spin cell ([docs/TUNING.md §7](docs/TUNING.md); raw runs under
   `results/*/qb-branch-perf-core-hot-path/`). What the branch does NOT change is the price of
   actually sleeping: with its idle-spin floor set to 0 the same cell measures **386 ns** on
   Windows and **25.8 µs** on WSL2 — the OS wake cost, the same one every other parked framework
   pays ([§8.2](docs/TUNING.md)). The branch's gain on Linux is entirely the 50 µs of spinning
   before the park, which is a policy, not a mechanism. The tables above stay at the shipped
   3.1.0 until that branch ships.
3. **CAF's flat ~485 ns is one configuration, measured four times, that never crosses a core.**
   Its `wait=1` profile (`aggressive-poll-attempts=100, steal-interval=10`) is CAF's **own
   shipped default** (`libcaf_core/caf/defaults.hpp`), and a ten-point sweep found every more
   aggressive profile *slower* ([docs/TUNING.md §1.1](docs/TUNING.md)) — so the adapter now
   declares the two columns identical rather than measuring one thing twice. And on a ping-pong
   CAF runs the receiver on the **sender's worker** (`worker::delay` → `queue.prepend`,
   `scheduled_actor.cpp`), so its "2 cores" cell is a one-thread cell, immune to the park cost
   every other framework pays. The **`caf-detached`** row is CAF's honest cross-core cost — one
   pinned OS thread per actor, CAF's own placement primitive — and it is **bimodal**: a
   repetition lands at ~0.93 µs or at ~10.6 µs on Windows (~3.9 or ~25.7 µs on WSL2) and stays
   there, so the report prints both modes and refuses to rank against it
   ([frameworks/caf-detached/README.md](frameworks/caf-detached/README.md), [§8.1](docs/TUNING.md)).
   Read the two CAF rows together: the pool is the best case, detached is the cross-core case,
   and neither is "CAF's number" alone.
4. **The floor matters.** At 2 cores spinning, the fastest framework is 1.57× the floor; at 1
   core it is 67×. The same frameworks, the same code, and a completely different story about
   what "framework overhead" means. And a framework can sit *below* the floor — pooled CAF at
   2c-park on WSL2 is 0.01× a floor that crosses a core — which the report now says out loud
   instead of printing as a ratio.

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
  directories (`qb-branch-perf-core-hot-path/`, `caf-spin-sweep/`) whose documents declare a
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
docs/TUNING.md         the configuration sweeps, including the one that embarrassed the author
docs/ROADMAP.md        what is not done yet
tools/                 run.py, report.py, negative controls
```
