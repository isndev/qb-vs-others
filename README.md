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
| `savina/ping-pong` × qb, CAF, SObjectizer, floor | **done**, 16/16 cells verified |
| The other 24 Savina benchmarks | **not yet written** — see [docs/ROADMAP.md](docs/ROADMAP.md) |
| Linux axis (WSL2 Debian 13 / g++ 14.2) | **run**, 16/16 cells verified — with the WSL2 caveat below; native Linux not yet |
| Seastar | not yet — Linux-only, and its dependencies need root on this host |
| Cross-language references (Erlang, Pekko, Actix, Orleans) | not yet |

One benchmark is not a comparison of frameworks. It is a comparison of frameworks *on a
ping-pong*, which is the narrowest workload in the suite, and the results below should be read
that way.

## What one benchmark has shown so far

`savina/ping-pong`, 1 000 000 round trips, MSVC 19.51, i9-12900K, pinned to two P-cores, 9
repetitions. Full tables in [REPORT.md](REPORT.md), all regenerated from `results/`.

| configuration | fastest | second | floor | notes |
|---|---|---|---|---|
| 1 core, spin | **qb 117 ns** | SObjectizer 202 ns | 2 ns | qb 1.72× |
| 1 core, park | **qb 120 ns** | SObjectizer 228 ns | 2 ns | qb 1.89× |
| 2 cores, spin | **qb 332 ns** | CAF 508 ns | 203 ns | qb 1.53× |
| 2 cores, park | **CAF 511 ns** | SObjectizer 1.54 µs | 335 ns | **qb 10.71 µs — 21× worse than CAF** |

The same 16 cells on Linux — WSL2 Debian 13, g++ 14.2, `-O3 -DNDEBUG`, the same two CPUs, 5
repetitions (`results/wsl-debian-g++14/`):

| configuration | fastest | second | floor | notes |
|---|---|---|---|---|
| 1 core, spin | **qb 100 ns** | SObjectizer 147 ns | 2 ns | qb 1.47× |
| 1 core, park | **qb 100 ns** | SObjectizer 170 ns | 2 ns | qb 1.70× |
| 2 cores, spin | **qb 283 ns** | CAF 290 ns | 209 ns | no measurable difference |
| 2 cores, park | **CAF 291 ns** | SObjectizer 27.7 µs | **26.6 µs** | qb 27.6 µs — **equal to the floor** |

**Read the Linux park row with its caveat.** WSL2 is a Hyper-V guest: a futex wake of a parked
thread on another vCPU is a virtualised IPI and costs ~13 µs here, so the raw `std::thread` +
condition-variable **floor itself** is 26.6 µs per round trip. qb and SObjectizer's `simple_lock`
sit exactly on that floor — on Linux qb's parked path is a plain condition variable and nothing
worse — while CAF stays at 291 ns because it never crosses a core (see point 3). Native Linux
puts a futex wake at 2–5 µs; that axis is not yet run, and until it is the Linux park row bounds
the hypervisor, not the frameworks.

Four things in that table are worth more than the ranking:

1. **Every framework is faster on ONE core than on two.** A ping-pong has no parallelism, so a
   second core buys nothing and costs a cache line crossing on every hop. qb goes 117 → 332 ns.
   Anyone quoting a two-core actor benchmark as evidence of scalability is quoting the wrong
   number.
2. **qb's parked mode is bad, and CAF's is not.** 10.71 µs against 511 ns. That is qb losing by
   21× on the configuration most production services actually run, and it is the most useful
   thing this repository has produced so far. It is now **explained and reproduced** —
   [docs/TUNING.md §5](docs/TUNING.md): qb parks after two or three empty passes because its
   spin credit counts events rather than time, and its `Mailbox::wait()` has a lost-wakeup race
   that MSVC's millisecond `wait_for` turns into ~13 ms stalls. Two prototype changes bring the
   cell to ~320 ns (Windows) / ~280 ns (Linux), the spin figure, on the local qb branch
   `perf/mailbox-lost-wakeup`; the tables above stay at the shipped 3.1.0 until that lands.
3. **CAF barely moves — and two reasons why are defects in THIS repository, not properties of
   CAF.** 508–511 ns across all four configurations (285–291 on Linux). First, CAF's `wait=1`
   profile (`aggressive-poll-attempts=100, steal-interval=10`) turned out to be CAF's **own
   shipped defaults** (`libcaf_core/caf/defaults.hpp`), so CAF's two columns are one
   configuration measured twice. Second, on a ping-pong CAF runs the receiver on the **sender's
   worker** (`worker::delay` → `queue.prepend`, `scheduled_actor.cpp`): its "2 cores" cell never
   crosses a core, which is why its 2-core figure equals its 1-core figure and why it is immune to
   the park cost every other framework pays. Both are recorded in [docs/TUNING.md](docs/TUNING.md)
   §1 and §5; a real spin profile for CAF and a `cores=2` cell that forces a cross-core hop are
   open items in [docs/ROADMAP.md](docs/ROADMAP.md).
4. **The floor matters.** At 2 cores the fastest framework is 1.63× the floor; at 1 core it is
   64×. The same frameworks, the same code, and a completely different story about what
   "framework overhead" means.

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
