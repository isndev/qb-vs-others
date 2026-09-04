# TUNING.md — the configuration sweeps, including the one that embarrassed the author

Every framework here has knobs. Choosing them badly for a competitor and well for your own
framework is the most effective way to rig a comparison, and it does not require any bad faith —
it only requires knowing your own framework better than theirs, which is the normal condition.

So the knobs are **swept, and the sweeps are published**, including the ones that made this
repository's first numbers wrong.

Measured on: i9-12900K, Windows 11, MSVC 19.51.36256, `/O2 /Ob2 /DNDEBUG`, pinned to CPUs 0 and 2,
`savina/ping-pong` at 1 000 000 round trips, 3 repetitions + 1 warmup. Figures are ns per round
trip, median.

---

## 1. CAF work-stealing: the profile that made CAF 2× slower

CAF's scheduler polls aggressively, then moderately, then sleeps
(`libcaf_core/caf/scheduler.cpp`, lines 58–74). To give CAF a busy-spin setting comparable to
qb's `setLatency(0)`, this repository first set the aggressive budget as high as it would go.

| profile | ns/round trip |
|---|---:|
| `aggressive-poll-attempts=1e9, steal-interval=1` — **the first guess** | **955.7** |
| `aggressive-poll-attempts=1, steal-interval=1` | 554.7 |
| `aggressive-poll-attempts=100, steal-interval=10` | 584.1 |
| `aggressive-poll-attempts=1000, steal-interval=10` | 568.7 |
| `aggressive-poll-attempts=10000, steal-interval=100` | 562.9 |
| `aggressive-poll-attempts=100000, steal-interval=1000` | 554.6 |
| **CAF's own shipped defaults, parked** | **531.0** |

**The first guess made CAF almost 2× slower than doing nothing at all.** With two workers on two
CPUs, a maximally aggressive steal interval has the idle worker hammering the busy worker's queue
on every poll; the contention costs more than the parking it was meant to avoid.

Had that profile shipped, this repository would have published *"qb is 2.6× faster than CAF"*.
The honest figure at each framework's best configuration is **1.53×**.

What this cost, and what it bought: one sweep, twenty minutes. The rule it produced is now in
FAIRNESS.md 1.1 and is not negotiable — **a knob is swept before a number using it is published**.

CAF's shipped defaults are the fastest thing measured here, so the adapter leaves them alone for
`wait=0` and uses `poll=100, steal=10` for `wait=1`. Both columns are published.

**Correction (2026-09-04): `poll=100, steal=10` IS CAF's shipped default** —
`libcaf_core/caf/defaults.hpp` sets `aggressive-poll-attempts = 100` and
`aggressive-steal-interval = 10`. The `wait=1` column therefore re-measures the `wait=0`
configuration, and the 584.1-vs-531.0 gap in the table above is run-to-run spread, not a knob.
"CAF barely moves" (README point 3) is partly this: one of its two columns is a no-op. A genuine
CAF spin profile would have to raise `aggressive-poll-attempts` well above 100 without the
steal-interval collapse of the first guess; that sweep is an open item in ROADMAP.md.

## 2. qb park interval — the same treatment, applied to the author's own framework

Tuning the competitor's knob while leaving your own at an arbitrary value rigs the axis just as
effectively in the other direction. qb's `setLatency(d)` parks a VirtualCore on a condition
variable for at most `d` (`qb/src/qb/core/Main.h`, `Mailbox::wait`/`notify`).

| park cap | ns/round trip |
|---|---:|
| 1 µs | 11 472.7 |
| 10 µs | 5 537.4 |
| 50 µs | 5 211.8 |
| 200 µs | 5 297.2 |
| 1000 µs | 5 177.8 |

qb's parked mode is **~5.2 µs per round trip at any sensible interval**, against CAF's 511 ns.
No value of this knob rescues it; the cost is in the wake-up path, not the cap. The adapter keeps
1000 µs and the result stands as measured.

This is the most useful finding in the repository so far, and it is a finding against qb.

## 3. Worker placement — a handicap this repository inflicted on qb

The harness pins the **process** to a CPU set. That is necessary on a hybrid 8P+8E part, but it is
not sufficient, and for a while it was actively wrong:

qb's entire design is one pinned worker thread per core owning its actors. With only the process
pinned, qb's own `CoreInitializer::setAffinity` was asking for CPUs outside the process mask and
**failing** — it says so in `qb.1.log`: `set thread affinity failed`. qb was being measured with
the mechanism it is built on switched off, while CAF and SObjectizer, which do not pin at all,
were measured exactly as they ship.

The fix is not to stop pinning qb; it is to give **every** framework the same placement through
its own public API:

| framework | API used | pinned |
|---|---|---|
| qb | `CoreInitializer::setAffinity(qb::CoreIdSet{cpu})` | VirtualCores |
| CAF | `caf::thread_hook::thread_started(thread_owner::scheduler)` | scheduler workers |
| SObjectizer | a custom `so_5::disp::abstract_work_thread_factory_t` | dispatcher work threads |
| floor | `qvo::pin_this_thread` | both threads |

Effect on `2c-spin`:

| framework | process-pinned only | workers pinned |
|---|---:|---:|
| qb | 384.0 | **347.2** |
| CAF | 955.7 → (also affected by §1) | 506.2 |
| SObjectizer | 899.8 | 1039.8 |

Note SObjectizer got *slower* with pinning on this workload. It is published as measured. Pinning
is not universally good, and a repository that only kept the pinning results that helped would be
doing the thing this whole document exists to prevent.

## 4. Open tuning questions — stated because they are unresolved, not because they are unimportant

- **qb's logger is left ON** (`QB_WITH_LOGGING`, qb's shipped default). It writes only at startup,
  outside the measured window, but its thread shares the pinned CPU set. Turning it off would
  improve qb's figure and no other framework has an equivalent subtraction, so it stays on and
  qb's number is conservative in this respect. Unmeasured.
- **CAF builds itself at C++17**; qb and the harness are C++20. Forcing CAF to C++20 would measure
  a build CAF does not ship. Whether it would be faster is unmeasured.
- **SObjectizer's `active_obj` vs `thread_pool` dispatchers** — only `active_obj` and `one_thread`
  are exercised. A thread-pool dispatcher may suit the fan-in benchmarks better and has not been
  tried.
- **Allocators** are the system allocator for all three. No framework here bundles its own, so
  nothing needed flagging, but this has not been re-checked per benchmark.

## How to re-run a sweep

```powershell
$env:QVO_CAF_AGGRESSIVE_POLL = "1000"; $env:QVO_CAF_STEAL_INTERVAL = "10"
.\build\final\bin\qvo-caf-savina-ping-pong.exe --repetitions 3 --warmup 1 --cpus 0,2 `
    --param messages=1000000 --param cores=2 --param wait=1

$env:QVO_QB_PARK_US = "50"
.\build\final\bin\qvo-qb-savina-ping-pong.exe --repetitions 3 --warmup 1 --cpus 0,2 `
    --param messages=1000000 --param cores=2 --param wait=0
```

## 5. qb's parked mode — the root cause, found by instrumenting qb rather than the benchmark

§2 stopped at "the cost is in the wake-up path". The wake-up path was then instrumented
(qb branch `perf/mailbox-lost-wakeup`, local, env-driven `QVO_QB_*` knobs — not merged) and
measured on both platforms, 300 000–1 000 000 round trips, `cores=2, wait=0`:

| | Windows / MSVC 19.51 | WSL2 Debian 13 / g++ 14.2 |
|---|---:|---:|
| shipped 3.1.0 | 10.7 µs | 27.6 µs (floor: 26.6 µs) |
| fix A: race-free `Mailbox::wait()`/`notify()` | 720 ns | 26.6 µs |
| fix A + 100 idle passes before parking | **323 ns** | **282 ns** |
| spin (`wait=1`), for reference | 332 ns | 283 ns |
| **qb branch `perf/core-hot-path`** (A + B + D + E + F + K, the real fix, 7 repetitions, quiet host; shipped re-measured beside it: 7.6 µs / 26.5 µs) | **259 ns** | **208 ns** |

Two defects, and their weight differs by platform:

1. **The park policy is the dominant cost everywhere.** `VirtualCore` refills its spin credit
   from the *number of events* seen on the previous pass, so on a ping-pong — one event per pass —
   the core parks after two or three empty passes. Every hop then pays a full OS park + wake:
   ~13 µs on WSL2 (virtualised IPI), ~2–5 µs on native Linux, ~300 ns on Windows when the wake is
   not lost. CAF polls ~600 times before it sleeps; SObjectizer's `combined_lock` spins for a
   budget first. qb has no time-based idle floor at all.
2. **`Mailbox::wait()` loses wakeups.** It is `cv.wait_for(lk, latency)` with **no predicate**, and
   `notify()` is `notify_all()` **without taking the mutex**, so an enqueue that lands between the
   consumer's empty `consume_all` and its registration on the condition variable is never seen and
   the core sleeps the full `latency`. On Linux that is a bounded 1 ms stall (measured 246–293
   stalls per 300k messages, ~5 % of wait time). On Windows MSVC's `wait_for` rounds up to whole
   milliseconds and lands on the 15.6 ms scheduler tick, so each lost wakeup costs ~13 ms —
   measured 48–2849 per 300k messages, **50–95 % of all wait time**. Sub-millisecond `setLatency`
   values are meaningless on Windows for the same reason.

Both fixes are engine changes, not adapter changes, and the published tables stay at the shipped
3.1.0 until they land — the last row is the branch that carries them (§7, "with the branch"),
measured through the unmodified adapter, and it is what the next qb release will ship. The methodology point stands on its own: **the benchmark found the
defect, but only instrumenting the framework found the cause** — no sweep of the adapter's knob
(§2) could have, because the knob was not where the cost was.

## 6. The Linux axis — what WSL2 can and cannot measure

`results/wsl-debian-g++14/` is the full 16-cell matrix under WSL2 (Debian 13, g++ 14.2,
`-O3 -DNDEBUG`, CPUs 0 and 2, 5 repetitions). Three things carry over from Windows unchanged:
qb leads on one core (100 ns vs SObjectizer 147–170 ns, CAF 285–287 ns); at two cores spinning
qb and CAF are within noise of each other (283 vs 290 ns) above a 209 ns floor; and CAF's four
cells are one figure (285–291 ns), because it never crosses a core (§1, README point 3).

What does NOT carry over is the park row: **the floor itself is 26.6 µs per round trip.** WSL2 is a
Hyper-V guest and a futex wake of a thread parked on another vCPU is a virtualised IPI. qb (27.6 µs)
and SObjectizer's `simple_lock` (27.7 µs) sit on that floor; nothing in either framework is
measured there, only the hypervisor. The row is published because the protocol says every cell
is, and it is flagged so that nobody quotes it. A native Linux run is the open item; until it
exists, the Linux park comparison is unmeasured.

## 7. The qb performance audit this benchmark triggered

§5 explained the collapse. The same instrumented branch (`perf/mailbox-lost-wakeup`, local, three
WIP commits, env-driven `QVO_QB_*` knobs) was then used to price every fixed cost on qb's hot
path, on both platforms, so that the fix list is measured rather than guessed. Every figure is a
p50 over 7 repetitions of 1 000 000 round trips, `savina/ping-pong`, CPUs 0 and 2. "Win" is MSVC
19.51, "Linux" is WSL2 g++ 14.2. The per-primitive costs come from a standalone micro-benchmark
compiled with the same flags (`system_clock::now` 19 / 30 ns Win / Linux, `ev_run(EVRUN_NOWAIT)`
on an idle wepoll / epoll loop 370 / 300 ns, `cv.notify_all()` with no waiter 2 / 1 ns,
`SpinLock` 5 ns, `std::mutex` 12 / 2 ns).

| axis | what the engine does today | measured effect | verdict |
|---|---|---|---|
| **A. park policy** | spin credit = *events* seen last pass; a 1-event/pass workload parks after 2–3 empty passes | 2c-park 10.7 µs (Win) / 27.6 µs (WSL2) → **323 / 282 ns** with a 100-pass idle floor, = the spin figure | **dominant, fix first** |
| **B. lost wakeup** | `Mailbox::wait()` = `cv.wait_for` with no predicate; `notify()` without the mutex | Win: each loss = ~13 ms (MSVC ms-ceiling + 15.6 ms tick), 50–95 % of wait time; Linux: bounded 1 ms, ~5 % | **correctness defect**, fix with A |
| **C. `notify()` per event** | producer notifies on every cross-core enqueue | 1–2 ns when nobody waits | negligible; keep, gate on a `_waiting` flag when B lands |
| **D. wall clock per pass** | `wall_now()` every loop pass (19 / 30 ns) | 1c: 124 → 93 ns (Win), 101 → 75 (Linux) reading it every 64 passes = **−25 %**; 2c: 0 alone, 232 → 209 ns on top of F (Linux) | worth taking; cadence or `steady_clock` |
| **E. io poll per pass** | `listener::run(EVRUN_NOWAIT)` runs every pass once *any* coroutine scheduler exists, even with zero watchers | 1c: 124 → **694** (Win), 101 → **739** (Linux), 5.6–7.3× slower; 2c: 408 → 645, 271 → 594. A gate on `size() \|\| has_deferred()` (still draining deferred + `run_ready`) restores the figures | **large, hits any app that ever `co_await`ed**; also the ~300–380 ns/pass floor of any app with one watcher |
| **F. spsc producer re-reads `read_index_`** | one remote cache-line read per hop | 2c-spin 404 → **290** ns (Win, −28 %), 281 → **232** (Linux, −17 %); 1c unchanged | take; mirror it on the consumer side |
| **G. copies per hop** | event → pipe → mpsc ring → `consume_all` scratch → dispatch | analysed, not isolated; an in-place `consume_all(func)` exists in `mpsc.h` | minor, after F |
| **H. SpinLock on the send path** | — | the indexed `enqueue(index, …)` used by `SharedCoreCommunication::send` takes **no** lock; the lock is only on the round-robin variants | **retired** — not a cost |
| **K. store-buffer drain on the publish** | `notify()` returned before its fence at latency 0, so a spin-mode enqueue reached the polling peer only when the producer's store buffer drained on its own | 2c-**park** beat 2c-**spin** on BOTH platforms; fencing in spin mode too, interleaved A/B on a quiet host (3 pairs × 7 reps): Win 2c-spin 296–309 → **259–263** ns (park 256–272); WSL2 p50 a wash, 204–219 → 205–208, but the worst run 295 → 215; 1c and a 1M-event bulk push unchanged (18.6 M msg/s) | **taken** — one fence per cross-core publish; commit `39992047` on the branch |
| **I. router double lookup** | `flat_hash` by EventId → virtual resolve → `flat_hash` by ActorId → fn ptr | analysed: ~15–20 of the ~37 ns a same-core hop costs; both ids are dense (`_type_id_counter`, `ServiceId`) so direct tables are feasible | measure next; medium |

Two shapes to keep in mind when reading the table. A remote cache-line read on this machine costs
60–110 ns, and it is the unit everything at two cores is priced in: the instrumentation's own
enqueue-counter snapshot (one such read per pass) is what puts the instrumented 2c-spin at 404 ns
against the shipped 332 — the same phenomenon as F, in the other direction. And an `epoll_wait` /
wepoll poll costs 300–380 ns whether or not anything is registered, which is why E is a per-pass
tax and not a per-event one: on a 1-core ping-pong it is paid twice per round trip.

**With A + D + F, qb's 2-core spin ping-pong on Linux measures 209 ns against a 209 ns raw-thread
floor** — within the spread, the framework costs nothing above the two cache-line crossings the
problem requires. That is the figure the fixes are aiming at; it is not yet the figure qb ships.

### With the branch

The fixes landed on a local qb branch, `perf/core-hot-path` (five commits over v3.1.0: F, E, D,
then A + B in one commit together with a start-barrier fix found on the way — below — and K). The
four qb cells were re-measured through the **unmodified adapter**, same protocol as the published
tables (7 repetitions of 1 000 000 round trips, CPUs 0 and 2, p50 with min–max), **with the
shipped v3.1.0 build measured in the same session** rather than quoted from the README:

| cell | Windows / MSVC 19.51 — shipped 3.1.0 → branch | WSL2 g++ 14.2 — shipped → branch |
|---|---:|---:|
| 1c-spin | 114 → **90** ns (88–90) | 98 → **75** ns (74–75) |
| 1c-park | 114 → **89** ns (88–91) | 98 → **74** ns (73.5–74) |
| 2c-spin | 315 → **262** ns (236–278) | 275 → **206** ns (202–208) |
| 2c-park | 7.6 µs → **259** ns (252–291) | 26.5 µs → **208** ns (201–217) |

Every checksum verified. The park row is now a framework figure on both platforms — 29× on
Windows, 127× on WSL2 — below CAF's 511 / 291 ns, which never crosses a core — and the two
two-core cells are now one figure: before axis K the park cell was *below* the spin cell on both
platforms, which is what exposed it. The 1-core figures are the axes D + E alone. On WSL2 the
branch's 2c-spin sits at the 209 ns raw-thread floor measured above.

**The first pass over this branch was taken on a loaded host, and the two-core cells moved by
20–30 % because of it** — Windows 2c-spin 319 → 262 and WSL2 270 → 206 between that pass and this
one, with the one-core cells within 7 % (96 → 90, 75.5 → 75). Same binaries, same protocol; the
difference was a build and a test suite running on the other cores (WSL2's pinned vCPUs 0 and 2
land on whichever host cores the hypervisor picks). The one-core cell cannot see it because it
never leaves its core; the two-core cell is priced in remote cache-line reads, and those are what
a busy sibling core perturbs. Every figure in this subsection and in the two
`qb-branch-perf-core-hot-path/` result directories is from the quiet re-run, shipped and branch
side by side; the published tables (§2, 5 repetitions) were taken under the same caution and are
consistent with the shipped column here (Windows 332 / 10.7 µs vs 315 / 7.6 µs; WSL2 283 / 27.6 µs
vs 275 / 26.5 µs). `FAIRNESS.md` §1.4 carries the rule now.

Two things the branch's own test suites say, because a benchmark that only measures the fast path
is the trap this document exists to avoid. On Windows the release suite passes 367/367 with 0
warnings; on WSL2 the release, ASan+UBSan and **TSan** suites pass 186/186 each. The TSan run is
the one that found something: `MainLifecycle.StopMultiCoreGracefulNoError` hung past its 600 s
timeout — **at v3.1.0 too**, so not a regression of the branch. `Main::__wait__all__cores__ready()`
and `Main::start(true)`'s wait were pure spins; with `hardware_concurrency()` cores (24 here —
WSL2 ignores `taskset` and cgroup quotas for that value) the last core to initialise is starved by
23 waiting peers, and under TSan the 23 acquire loads hold the sanitizer's atomics lock in read
mode so the 24th core's `fetch_add` never gets in: a hang at ANY `taskset` width, diagnosed with
gdb attached to the 24 threads. The barrier now spins 1024 times and then yields: 2 s under TSan,
the signal variant 61 s → 2 s, release unchanged. It is in the same commit as A + B because a
ping-pong benchmark never starts 24 cores and would never have seen it.

Still open, in order: a native Linux run (the park floor here is the hypervisor's, §6); a real CAF
spin profile and a forced cross-core CAF cell (README point 3); axis I; arm64, where the fence
of axis K is a `dmb ish` whose cost and benefit are both unmeasured — qb's own `dev/bench` gate on
macOS is the instrument for that.

