# TUNING.md — the configuration sweeps, including the one that embarrassed the author

Every framework here has knobs. Choosing them badly for a competitor and well for your own
framework is the most effective way to rig a comparison, and it does not require any bad faith —
it only requires knowing your own framework better than theirs, which is the normal condition.

So the knobs are **swept, and the sweeps are published**, including the ones that made this
repository's first numbers wrong.

Measured on: i9-12900K, Windows 11, MSVC 19.51.36256, `/O2 /Ob2 /DNDEBUG`, pinned to CPUs 0 and 2,
`savina/ping-pong` at 1 000 000 round trips, 3 repetitions + 1 warmup. Figures are ns per round
trip, median. §9 is the exception: it is the four benchmarks added on 2026-09-04, on both
platforms, and says so.

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
"CAF barely moves" (README point 3) is partly this: one of its two columns is a no-op.

### 1.1 The sweep that settles it (2026-09-04, Windows/MSVC, quiet host, 2c, 5 reps + 2 warmup)

The open item was "raise `aggressive-poll-attempts` well above 100 without the steal-interval
collapse of the first guess". Done, on the two knobs independently. Documents and the script are
in `results/desktop-b67osn6-win-msvc/caf-spin-sweep/`; ns per round trip, p50 [min, max]:

| `aggressive-poll-attempts` | `aggressive-steal-interval` | ns/round trip |
|---:|---:|---:|
| **100 (default)** | **10 (default)** | **493.4** [487, 529] |
| 1 000 | 10 | 533.8 [503, 544] |
| 10 000 | 10 | 718.8 [640, 781] |
| 100 000 | 10 | 730.0 [718, 853] |
| 1 000 000 | 10 | 752.1 [732, 894] |
| 10 000 | 1 | 754.4 [704, 1072] |
| 10 000 | 100 | 534.2 [516, 540] |
| 10 000 | 1 000 | 490.4 [488, 496] |
| 1 000 000 | 1 000 000 | 495.3 [492, 529] |
| 100 (default), re-run last | 10 (default) | 495.0 [490, 498] |

Read the two axes separately. **Polling harder never helps**: at any steal interval, raising the
poll budget is neutral at best (steal ≥ 1 000: 490–495 ns whether the budget is 100 or a
million). **Stealing more often is what costs**: at a fixed budget of 10 000 polls, steal-interval
1 → 754 ns, 100 → 534, 1 000 → 490. The mechanism is in §1's opening paragraph and in
`frameworks/caf/caf_support.h`: on a two-actor ping-pong the receiver is `delay()`ed onto the
SENDER's worker, so the only thing an aggressively polling idle worker can find is a steal — and
a steal moves the actor to the other core, which is the one transfer CAF's placement was avoiding.
CAF's fastest ping-pong is the one where nothing ever crosses a core, and its defaults already
produce it.

**Consequence for the tables.** There is no honest CAF spin profile faster than the defaults, so
the adapter runs `wait=1` and `wait=0` as the SAME configuration and says so in its caveats; the
two CAF columns are one number, deliberately, and "CAF barely moves" is retired as a claim. The
question the reader actually has — what does CAF pay when its two actors DO sit on two cores —
is answered by the `caf-detached` row (§8), not by any pool knob.

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
- **SObjectizer's spin budget.** The two-agent benchmarks run `active_obj` (one work thread per
  agent) and the many-agent ones a `thread_pool` of `cores` threads with `fifo_t::individual`
  (`so_support.h`); both spell "spin" as `combined_lock_factory(10 s)`. That budget was chosen,
  not swept, and §9 records two cells where the spin lock is slower than the plain one.
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
| **I. router double lookup** | `flat_hash` by EventId → virtual resolve → `flat_hash` by ActorId → fn ptr | both ids are dense (`_type_id_counter`, `ActorId::sid()`), so `router::dense_index<Key>` + `internal::key_table` (branch `router.h:87`, `:119`) index a vector directly and keep the full-key compare; interleaved A/B against L on Windows (9 reps): 1c counting 28.0 → 25.8, ring 51.4 → 45.6, big 29.8 → 24.0, fork-join 2c 42.2 → 37.0 — and **counting at two cores 30.6 → 43.5 (+42 %)**, which is finding M | **taken** — commit `cc7044eb`, with M in the same branch; never ship I without M |
| **M. per-event publish on the cross-core pipe flush** | `__flush_all__` sent one event per `try_send`: one `enqueue`, one release store of the ring index, one `notify()` per event; the consumer's `consume_all` re-reads that index per batch, so a consumer FASTER than the producer turns every event into a coherence round trip on the index line | exposed by I: counting 2c with batch counters, sparse (3.1.0) consumer ~1.7k–3.9k batches of **250–600** events, dense (I) consumer ~108k–179k batches of **6–9**, both cores ~40 % slower (p50 30.6–32.1 → 38.8–41.9 ns). Fix: gather a run of whole events (≤ `kFlushRunBuckets` = 256 buckets, `VirtualCore.cpp:277`), one `write_room`, one `enqueue<true>`, one `notify` per run (`SharedCoreCommunication::send_run`, `Main.cpp:237`); the slow per-event path stays for zero-width, oversize, QoS-0 and back-off. A/B L vs I+M (4 passes): counting 2c 31.0 → **28.0** / 31.9 → **28.5**, ring 2c-spin 159 → **134**, big 2c 29.7 → **26.4**, ping-pong 2c level (±3 %), 1c cells −7…−27 % everywhere | **taken** — commit `f5c20eeb`; a batching regime that only a faster consumer can reveal, so it was invisible on 3.1.0 |
| **L. out-of-line accessors on the per-event path** | `ActorId`'s constructors, `operator uint32_t`, `sid()`/`index()`/`is_broadcast()`/`is_valid()`, `Actor::is_alive()`, `CoreSet::resolve()`, `VirtualCore::__getPipe__()` defined in the archive, called from `Actor::push<T>` / the dispatch trampoline instantiated in the USER's TU — a call into the archive for a two-field load, opaque without LTO | found by `perf` on the four §9 benchmarks (WSL2): `ActorId(uint32_t)` + `is_broadcast` + `operator uint32_t` ~10 % of the counting profile, `is_alive` + `resolve` ~5 % more; in-class, 3-rep min: counting 1c 43.5 → 39.2, fork-join 2c 59.3 → 42.1, big 1c 31.6 → 25.2 ns | **taken** — commit `ba051409`; no behaviour change, three readme citations move with it |

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

The fixes landed on a local qb branch, `perf/core-hot-path` (eight commits over v3.1.0: F, E, D,
then A + B in one commit together with a start-barrier fix found on the way — below — K, L,
which the four §9 benchmarks found after this subsection was measured, then I and M, measured
in their own subsection below). The
four qb cells were re-measured through the **unmodified adapter**, same protocol as the published
tables (7 repetitions of 1 000 000 round trips, CPUs 0 and 2, p50 with min–max), **with the
shipped v3.1.0 build measured in the same session** rather than quoted from the README:

| cell | Windows / MSVC 19.51 — shipped 3.1.0 → branch | WSL2 g++ 14.2 — shipped → branch |
|---|---:|---:|
| 1c-spin | 114 → **90** ns (88–90) | 98 → **75** ns (74–75) |
| 1c-park | 114 → **89** ns (88–91) | 98 → **74** ns (73.5–74) |
| 2c-spin | 315 → **262** ns (236–278) | 275 → **206** ns (202–208) |
| 2c-park | 7.6 µs → **259** ns (252–291) | 26.5 µs → **208** ns (201–217) |

Every checksum verified. (This table is the branch at `39992047`, before axis L; the 2026-09-04
re-measurement of all five benchmarks — shipped and branch in one session, 9 repetitions on
Windows, 5 on WSL2 — is the pair of grids in README.md and the one `check-report.py` verifies,
at `ba051409` first and at `f5c20eeb` since. The latter's ping-pong cells, 77 / 78 / 277 / 266 ns
on Windows and 69 / 67.5 / 233.5 / 214 ns on WSL2, are the figures to quote; the 2c-park cells there are 20–30 ns above this table's because
the shipped and branch runs alternated on a host running the other three frameworks' cells in
between, a spread this document's own §7 caution predicts.) The park row is now a framework
figure on both platforms — 29× on Windows, 127× on WSL2 — below CAF's 511 / 291 ns, which never
crosses a core — and the two
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

Still open, in order: a native Linux run (the park floor here is the hypervisor's, §6); arm64,
where the fence of axis K is a `dmb ish` whose cost and benefit are both unmeasured — qb's own
`dev/bench` gate on macOS is the instrument for that; and the two design questions of §9 (9.2,
the 64-byte bucket; 9.4, placement). The CAF spin profile, the cross-core CAF cell and axis I,
which used to head this list, are closed by §1.1, §8 and the subsection below.

### Axes I and M — the dense router, and the regression it exposed

Axis I replaces the router's two `flat_hash` lookups per dispatch with vectors indexed by the
id itself (`router::dense_index<Key>`, enabled for `EventId` and for `ActorId` by `sid()`;
`internal::key_table` keeps the full key in the slot and compares it, so a wrong index is a
miss and, in a debug build, an assert — never a wrong handler). Measured alone against L,
interleaved on a quiet Windows host, it did what §9.1 predicted at one core and **lost 42 %
on counting at two cores** (30.6 → 43.5 ns spin, 31.7 → 44.5 park), while every other
two-core cell improved. The instrumented build explained it with two counters: at 3.1.0 the
consumer of a 2c funnel drains **250–600 events per `consume_all` batch**; with the dense
router it is fast enough to keep up with the producer and drains **6–9**, and each of those
batches re-reads the ring's write index while the producer publishes it once per EVENT — one
coherence round trip per message where there used to be one per few hundred. The producer
was never the bottleneck (`full_hits` ≈ 0 in both regimes); it was pacing the consumer.

Axis M is the fix, and it is in the flush rather than in the ring: `__flush_all__` now
gathers a run of whole deliverable events from the head of the pipe (up to 256 buckets, a
quarter of the 1023-slot ring so a run never waits on a consumer that is already draining)
and hands it to `SharedCoreCommunication::send_run`, which asks the ring for room once,
writes the run in one `enqueue<true>` — one release store of the index — and notifies once.
What the ring cannot take is cut at an event boundary and re-sent on the next pass. The
per-event path stays for the cases it exists for: a zero-width or oversize event, QoS-0, and
the bounded spin/yield back-off on a full ring. `spsc::ringbuffer::write_room()` is the one
new primitive and has its own unit test; the slow-path fall-through is asserted by the same
test file (`spsc-cached-index.cpp`).

**Interleaved A/B, L against I + M, Windows, 4 passes (L1, IM1, L2, IM2) × 9 repetitions,
p50 ns/unit, best pass of each** (`ab-axis-IM/` beside the grids):

| benchmark | 1c-spin | 1c-park | 2c-spin | 2c-park |
|---|---|---|---|---|
| ping-pong | 83.4 → 76.0 (−9 %) | 86.7 → 77.1 (−11 %) | 259.9 → 261.0 (level) | 259.3 → 252.5 (−3 %) |
| counting | 27.6 → 25.7 (−7 %) | 27.6 → 25.4 (−8 %) | 31.0 → 28.5 (−8 %) | 31.0 → 28.0 (−10 %) |
| thread-ring | 48.3 → 45.2 (−7 %) | 48.8 → 43.4 (−11 %) | 159.4 → 133.7 (−16 %) | 144.5 → 143.3 (level) |
| fork-join | 37.1 → 34.4 (−7 %) | 37.5 → 34.2 (−9 %) | 42.2 → 38.8 (−8 %) | 40.5 → 36.0 (−11 %) |
| big | 29.7 → 22.6 (−24 %) | 31.7 → 23.0 (−27 %) | 29.8 → 26.6 (−11 %) | 29.7 → 26.4 (−11 %) |

The two cells that carry a message across a core per hop (ping-pong, ring, at 2c) are level
or better; the counting regression of I alone is gone (43.5 → 28.5); and `big` gains the most
because 120 actors × 60 handler-map entries was exactly the shape the hash tables missed on.

**Official grids, shipped 3.1.0 → branch at `f5c20eeb`, same session, same protocol as the
published tables** (Windows 9 reps, WSL2 5 reps; `M-f5c20eeb-shipped-3.1.0/` and `M-f5c20eeb/`
under each host's `qb-branch-perf-core-hot-path/`): on Windows ping-pong 112 → 77 / 113 → 78 /
338 → 277 / 6624 → 266; counting 30 → 25.5 / 30 → 26 / 34 → 28 / 34 → 28; thread-ring
63.5 → 46 / 63 → 43 / 206 → 146 / 2122 → 135; fork-join 42 → 35 / 42 → 35 / 45 → 40 / 42.5 → 36;
big 37 → 24 / 36 → 24 / 32 → 27 / 31 → 27. On WSL2 ping-pong 98 → 69 / 98 → 67.5 / 276 → 233.5 /
26 523 → 214; counting 44 → 35 / 44 → 34 / 48 → 43 / 49 → 44; thread-ring 64 → 37 / 62 → 48 /
191 → 123 / 13 273 → 130; fork-join 71 → 65 / 70 → 59 / 50 → 54 / 48.5 → 45; big 38 → 25 /
40 → 26.5 / 29 → 23 / 32 → 23 (1c-spin / 1c-park / 2c-spin / 2c-park, ns per unit). Against the
previous candidate `L-ba051409` every cell moves −5…−25 % except three that are level within
spread: Windows ping-pong 2c-spin (254 → 277) and ring 2c-spin (133 → 146), whose repetitions
run 234–335 and 128–176 ns for L against 252–311 and 130–160 for M with the same minima, and
which the interleaved A/B above puts at level or −16 %; and WSL2 fork-join 2c-spin (55 → 54).

Two observations recorded rather than explained. The g++ thread-ring at one core spinning
varies between runs — 36.9 ns in the official grid (repetitions 36.6–37.3) and 44.2 in a
re-run minutes later, against a park cell steady at 47.9 — and the previous candidate showed
the same 40–50 range; nothing in the code path differs between spin and park on an ACTIVE
pass (the park policy only runs once `getLatency() > 0` and an active pass merely resets
`_idle_since`), and WSL2 exposes no PMU to look further. And qb's suite is unchanged by both
axes on every platform it can be run from here: Windows/MSVC release **183 / 183 / 0** and
debug **183 / 183 / 0** (asserts armed, the `dense_index` invariants included), WSL2 g++-14
release, ASan+UBSan and TSan **187 / 187 / 0** each, 0 warnings on all five builds.

## 8. What crossing a core and sleeping actually costs — the `caf-detached` row and qb's idle floor

§1.1 left one question standing: CAF's `cores=2` ping-pong never crosses a core, so what does CAF
pay when its two actors DO sit on two cores? And §7's branch figures raised its mirror image: the
branch's 259 / 208 ns park cell is the cell of a core that keeps polling for 50 µs after its last
event (`CoreInitializer::setIdleSpin`, default `kDefaultIdleSpin`), and a ping-pong reply lands
within a microsecond, so that floor never expires and the "park" cell is a polling figure. Neither
table said what a qb actor pays when it genuinely sleeps. This section measures both, on both
platforms, in the same quiet session as the published tables (2026-09-04).

### 8.1 `caf-detached`: CAF's own cross-core placement

`frameworks/caf-detached/` spawns every actor `caf::detached` — one OS thread per actor, pinned
one per CPU through the same `thread_hook`, parked on a condition variable between messages
(`caf/detail/private_thread.cpp`, `await()` is an unconditional `cv_.wait()`). It is the only
placement primitive CAF's public API has, so it is the only honest way to force the hop; its two
spin cells are reported as **not applicable** (harness exit 3, reason in the JSON), because a
private thread has no spin mode and a number invented for the cell would be a pool measurement
under a detached label.

| cell | Windows / MSVC 19.51, per round trip | WSL2 g++ 14.2 |
|---|---:|---:|
| `caf-detached` 1c-park | 10.61 µs | 3.5 µs |
| `caf-detached` 2c-park | **10.57 µs — bimodal**: 2 of 9 repetitions at ~0.93 µs, 7 at ~10.58 µs | **4.07 µs — bimodal**: 3 of 5 at ~3.9 µs, 2 at ~25.7 µs (5 of 5 slow on the previous run) |
| `caf` (pool) 2c-park, for scale | 487 ns | 283 ns |
| `baseline` cv floor 2c-park | 435 ns (a single `notify` + `WaitOnAddress` wake at this cadence stays fast) | 25.1 µs |

Two readings. First, the 1c-park cell: on Windows a detached actor pays ~10.6 µs even on ONE
core, because the sender's thread must be descheduled before the receiver's can run, and that is
a full scheduler round trip — the same-core CAF pool cell is 481 ns. On WSL2 the same handoff is
3.5 µs. Second, the 2c-park cell is **bistable**, and on both platforms: a repetition of a million
round trips lands in one of two modes and stays there for its whole 10 s. The slow mode is the OS
cost of waking a thread parked on another core, and it is the same figure every framework that
truly blocks measures here — qb 3.1.0's 2c-park (5–7.6 µs Windows, ~27 µs WSL2), SObjectizer's
`simple_lock` (1.06 µs Windows, where its own spin-then-park hybrid keeps it fast; ~26 µs WSL2),
the raw condition-variable floor on WSL2. The fast mode costs what the same-core handoff costs,
which is consistent with the two threads locking into a phase where each message arrives before
its receiver reaches the futex / `WaitOnAddress` sleep, so the wait is signalled every time but
never slept on — an inference from timings, not a trace (ROADMAP.md names the trace that would
settle it). `tools/report.py` splits a sample at any gap wider than 2×, prints both modes under
the row, and refuses to claim an ordering against a bimodal cell; the README tables carry that
marker. The number to quote is "~1 µs or ~10.6 µs" — never the median, which is whichever mode won
the coin toss that run.

### 8.2 qb's idle-spin floor forced to zero — the cost of a qb actor that really sleeps

`QVO_QB_IDLE_SPIN_US` (`frameworks/qb/qb_support.h`) overrides the branch's idle-spin floor when
`wait=0`; on shipped 3.1.0, which has no such knob, the adapter reports the cell **not applicable**
rather than measuring the default under an experiment's label, and any document measured under the
override carries a caveat saying it is not qb's configuration. Branch `perf/core-hot-path` @
`39992047`, 7 repetitions + 2 warmup, CPUs 0 and 2, per round trip:

| cell | Windows / MSVC 19.51 | WSL2 g++ 14.2 |
|---|---:|---:|
| 2c-park, floor **default (50 µs)** | 259 ns (255–263) | 212 ns (208–229) |
| 2c-park, floor **0** — the core blocks on every idle pass | **386 ns (295–422)** | **25.8 µs (25.67–26.10)** |
| 1c-park, floor 0 | 85 ns (84–86) | 74 ns (72–74) |

The two platforms answer differently, and both answers matter for qb:

- **On WSL2 the floor is the whole story.** At 0, every reply finds the receiver already asleep
  and pays the hypervisor's futex wake: 25.8 µs, indistinguishable from the raw cv floor and from
  shipped 3.1.0. The branch's 208 ns park cell on Linux is therefore bought entirely by the 50 µs
  floor; the park handshake itself (§5's fixed `Mailbox::wait()`) does not catch the reply. That is
  the expected shape — a futex sleep is entered in well under a microsecond — and it means a qb
  actor on Linux whose traffic has real gaps > 50 µs pays the OS wake on every such gap, exactly
  like everyone else. What the branch changed is that it no longer pays it on gaps of 1 µs.
- **On Windows the handshake absorbs it.** At floor 0 the cell is 295–422 ns, not 10 µs: the
  `WaitOnAddress`-based park takes long enough to enter, relative to a ~150 ns reply, that the
  reply lands inside the handshake window and the wait returns without sleeping. The floor then
  buys 386 → 259 ns — a third, not a hundredfold. This is also why the Windows `caf-detached` and
  `baseline` cells sit at the fast end more often than WSL2's: the OS side of the phase lock is
  easier to hit. It means a Windows measurement of "qb's park cost" at ping-pong cadence is a
  measurement of the handshake, and the only way to see the OS wake from qb on Windows is a
  workload with real idle gaps — which is what the next benchmarks (`counting`, `thread-ring`,
  `fork-join`, `big`) introduce.
- **The 1c-park cell does not care** (85 / 74 ns at floor 0, equal to the default): a single
  core never idles between the two halves of a round trip, so the floor never arms.

**The qb finding to carry forward** (Huly QB-42): the branch's park cell is a real improvement
over 3.1.0 — on Linux it moves the threshold at which a qb actor starts paying the OS wake from
"the first idle pass" to "50 µs of idleness", and on Windows it additionally shortens the handshake
enough that a sub-microsecond reply is caught — but a cross-core park that actually sleeps costs
what the OS charges, ~10.6 µs on Windows and ~26 µs under WSL2's hypervisor, for qb as for CAF and
SObjectizer. No framework in this table beats the floor once it sleeps; the differences are in how
long each one refuses to.

## 9. What four more shapes said about qb — counting, thread-ring, fork-join, big

The ping-pong axes of §7 were found on a two-actor round trip, which exercises one pipe in each
direction and nothing else. On 2026-09-04 the four Savina benchmarks that add fan-in, a ring, a
fan-out and an all-to-all were measured for every framework on both platforms — 84 cells per
host, shipped qb 3.1.0 and the branch at `ba051409` through the same adapters in the same quiet
session, 9 repetitions on Windows and 5 on WSL2, checksums verified on every cell (the branch
figures in the table are that measurement; the §7 subsection on axes I and M carries the
re-measurement at `f5c20eeb`). The ranking is
README.md's business; this section is the list of **qb-side costs** those four shapes exposed,
each with where it is, what it measures and what was done. Figures are ns per unit of work
(message, hop, message, round trip), p50, Windows / WSL2, shipped → branch unless stated. The
`where` column cites **qb 3.1.0** coordinates: axis I rewrote `router.h` and axis M moved
`__flush_all__`, so on the branch at `f5c20eeb` the same symbols live at `router.h:87`
(`dense_index`), `router.h:119` (`key_table`), `VirtualCore.cpp:277` (`kFlushRunBuckets`) and
`Main.cpp:237` (`send_run`).

| # | finding | where | measured | state |
|---|---|---|---|---|
| **9.1** | **Same-core dispatch costs ~27–30 ns on MSVC and ~40 ns on g++ against a 3 ns floor.** `counting` at one core is the purest measurement of it: one producer, one consumer, no core crossing, no reply — the cell is `push<>` + pipe + `consume_all` + route, and nothing else. | `VirtualCore.cpp:199` (`_router.route`), `system/event/router.h:471` (`_registered_events.at(id)->resolve`), `router.h:169` (the second `unordered_map` by handler id), `VirtualCore.cpp:224` (`consume_all`) | 1c-spin: 29.4 → **27.4** / 44.6 → **40.8** at `ba051409`; **25.5 / 34.9** at `f5c20eeb`; floor 3.2 / 2.8 | axis L took 2–4 ns, axis I another 2 (MSVC) to 6 (g++) — the g++/MSVC gap closed from 13 to 9 ns; what remains (~22 / 32 ns over the floor) is the two 64-byte copies of 9.2 and the per-pass loop. **Taken** as far as the router goes; 9.2 is the open half |
| **9.2** | **Every event is at least one 64-byte bucket, copied twice.** `QB_LOCKFREE_EVENT_BUCKET_BYTES` is the cache-line size, so a 16-byte payload is relocated as 64 bytes into the pipe and 64 bytes out of the ring into the `consume_all` scratch before dispatch. At 1 M messages that is 128 MB of memcpy per cell for a benchmark whose data is 16 MB. | `utility/prefix.h:68`, `Event.h:473` (`bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES`), `VirtualCore.cpp:224` | not isolated — it is inside 9.1's 27 ns; the g++/MSVC gap (40 vs 27) is the one hint, g++'s `memcpy` of a 64-byte aligned block being the slower of the two here | **open**; a sub-cache-line bucket for small events changes the ABI fingerprint (`abi.h`) and is a major-version change |
| **9.3** | **The out-of-line accessors — axis L.** Found by `perf` on these four, not on ping-pong, because a ring or a fan-out spends a larger share of its time in `push<T>` and the trampoline than a two-actor exchange does. | the §7 row | counting 1c 29.4 → 27.4 / 44.6 → 40.8; thread-ring 1c 62.9 → **51.0** / 62.5 → **48.0**; big 1c 36.2 → **31.3** / 37.4 → **31.0**; fork-join 2c-park 42.2 → 40.0 / 57.1 → **44.6** | **taken**, `ba051409` |
| **9.4** | **A ring crosses a core on every hop, and qb has no way to not.** `thread-ring` places actor i on VirtualCore i % cores, so at cores=2 every hop is a cross-core hop: 172 / 173 ns shipped against 63 / 62 ns on one core. CAF runs the receiver on the sender's worker (`worker::delay` → `queue.prepend`) and measures the same 236 / 140 ns at one core and two. qb's placement is static — an actor lives where it was built — so the framework cannot pull a ring onto one core; only the application can, by building it there. | `frameworks/qb/savina/thread-ring.cpp` (placement), `Actor.h:1068` (`forward`) | 2c-spin: 172.4 → **132.6** / 172.6 → **131.0** (floor 109.9 / 114.4, CAF 238.6 / 140.4); 1c: 62.9 → 51.0 / 62.5 → 48.0 | the branch closes 2c to 1.2× the floor; the remaining gap to CAF on WSL2 (131 vs 140, level within spread) is placement, not dispatch. **Open as a design question**: a `push<>` whose destination shares no core with the sender could migrate an actor with no other traffic, and qb has no such policy |
| **9.5** | **The 2c-park collapse is on every cross-core-per-message shape, on both platforms.** §5 found it on ping-pong; the ring shows it bimodal on Windows — a repetition lands at ~565 ns or ~3.02 µs per hop and stays there (median 2446.5) — and on the hypervisor's floor on WSL2 (13 409 ns per hop, floor 13 008). counting, fork-join and big do NOT collapse (shipped 2c-park 33.0 / 42.2 / 33.2 on Windows) because a funnel or a fan-out never lets the consumer's pipe run dry. | §5 (`Mailbox::wait()`), §8.2 (the idle floor) | thread-ring 2c-park: 2446.5 → **145.0** / 13 409 → **130.0**; ping-pong 2c-park: 4225.5 → **278.3** / 26 780 → **241.5** | **taken** on the branch (A + B + K); the WSL2 figure is bought by the 50 µs idle floor, §8.2 |
| **9.6** | **`send<>` is not faster than `push<>` on the all-to-all.** The eager cross-core variant was expected to win on `big` (one request in flight per actor, so nothing to batch) and measured slower: WSL2 3-rep min, send 26.1 / 25.9 / 26.2 against push 24.9 / 25.8 / 25.1 ns per round trip (1c-spin / 2c-spin / 2c-park). With 120 actors on two cores, the per-(core, core) pipe flush still carries a batch, and `send<>` gives that up for nothing. | `frameworks/qb/savina/big.cpp:61` (the comment carries the figures), `qb/llm/qb.llm.md` (`push` = ordered) | see left | recorded in the adapter; the `send<>` documentation should say when it wins (a single event with real idle behind it), which it does not |
| **9.7** | **qb sits BELOW the raw-thread floor on the three shapes with parallelism, and the floor is why.** At two cores, counting 33.1 vs floor 42.3, fork-join 42.2 vs 48.5, big 33.2 vs 60.4 on Windows; the floor's SPSC ring pays one remote cache-line crossing per message, qb's staging pipe moves a batch per flush. This is the batching of `__getPipe__()` doing its job and is the strongest argument these four make for the engine — the report prints it as "below the floor" rather than as a ratio. | `VirtualCore.h` (`__getPipe__`), `Main.h:354` (`MaxRingEvents`) | 2c-spin counting 33.1 / 46.3, fork-join 46.0 / 58.4, big 30.8 / 32.8 (floors 42.3 / 23.7, 38.2 / 28.5, 43.3 / 25.4) | nothing to do; note the WSL2 floor is BELOW qb on counting and fork-join (23.7 / 28.5) — g++'s SPSC ring is cheaper and 9.2 is the suspect |
| **9.8** | **fork-join at one core costs 1.4–1.6× counting at one core, and the difference is not isolated.** Same producer, same `push<>`, same core; the only change is 60 destinations instead of one, so 60 handler-map entries and 60 actors' state instead of one hot line. | `router.h:169` (`_subscribed_handlers` by handler id), `Actor.h` (`is_alive` on delivery) | 1c-spin 42.5 → 37.9 / 67.0 → 67.9 at `ba051409`, **34.8 / 65.0** at `f5c20eeb`, against counting 25.5 / 34.9; 1c-park 64.4 → **58.9** on WSL2 | **taken in part** — the dense table (axis I) took 8 % on MSVC and 4–9 % on g++, so the 60-key lookup was a cost but not the whole 1.4–1.6×; what remains is 60 actors' state against one hot line, which is the shape and not the engine |
| **9.9** | **Windows 2c-park was the shipped engine's worst cell on every shape that crosses a core per message**, and on ping-pong it was worse than SObjectizer's `simple_lock` (4225.5 vs 1034.6) and 8.6× CAF; shipped 3.1.0 in park mode was, on this host, the slowest actor framework in the table for a cross-core round trip. | §5 | ping-pong 2c-park 4225.5 → 278.3; thread-ring 2446.5 → 145.0 | **taken**; the single most user-visible finding of the suite and the reason the branch exists |
| **9.10** | **A faster consumer made the cross-core pipe SLOWER — the per-event publish.** The first candidate with axis I lost 42 % on counting at two cores while winning every one-core cell: `__flush_all__` published the mailbox ring's write index once per event and the consumer re-read it once per `consume_all` batch, a cost that is invisible while the consumer lags (250–600 events per batch at 3.1.0) and one coherence round trip per message once it keeps up (6–9 per batch with the dense router). 3.1.0 never showed it because 3.1.0's consumer was never fast enough. | `VirtualCore.cpp:277` (`kFlushRunBuckets`, branch), `Main.cpp:237` (`send_run`, branch), `spsc.h` (`write_room`) | counting 2c-spin, L → I → I+M: 30.6 → 43.5 → **28.3**; 2c-park 31.7 → 44.5 → **28.2**; big 2c 29.5 → **26.9** / 30.9 → **23.0**; ping-pong 2c level | **taken** — axis M, `f5c20eeb`; the general lesson is that a dispatch optimisation must be measured at two cores, where it can flip the pipe into a regime the ring was not tuned for |

Two things the four benchmarks did NOT find, stated so the list is read as complete rather than
selective. There is no fan-in contention cost: `counting` at two cores is 33 / 46 ns shipped and
the consumer's `consume_all` never contends, because a qb mailbox is one MPSC ring with one
consumer and the producer's own core-local pipe — the "single hot mailbox" the Savina authors
built the benchmark to stress does not exist in this engine. And there is no per-actor cost at
120 actors: `big` at 30–33 ns per round trip, two messages, is the cheapest per-message cell in
the suite on both platforms, below the floor at two cores, and unchanged from 120 actors' worth
of `unordered_map` entries to 2's.

**SObjectizer's spin lock is slower than its plain lock on Windows at two cores on the ring and
the fan-out** (thread-ring 481.9 vs 473.9, fork-join 289.1 vs 268.4) while faster on counting
and big (289.4 vs 307.6, 378.0 vs 443.3). It is a note for `frameworks/sobjectizer/`, not a qb
finding: the many-agent adapters run a `thread_pool` of `cores` pinned work threads with
`fifo_t::individual` (`so_support.h` `make_pool_binder`, so a hundred agents are not a hundred
threads against two CPUs), and the spin half is `combined_lock_factory(10 s)` against
`simple_lock_factory()` — a sweep of that budget like §1.1's has not been run and is an open
question in §4.
