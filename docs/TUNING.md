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
in `results/desktop-win11-msvc19/caf-spin-sweep/`; ns per round trip, p50 [min, max]:

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

### 1.2 SObjectizer's spin budget, swept the same way (2026-09-09, both hosts, 2c, 7 reps + 2 warmup — QB-47)

A comparison that swept one competitor's knob and not the other's is not symmetric, and
SObjectizer has exactly one on this axis: the `combined_lock` of its dispatcher queues spins for a
WAITING TIME before it falls back to a mutex and condition variable — a yield loop that re-reads
the clock each turn (`dev/so_5/disp/mpsc_queue_traits/pub.cpp`, `combined_lock_t::wait_for_notify`
in the pinned 5.8.5.1), whose default is 1 ms. The adapter's `wait=1` profile sets it to 10 s
(`frameworks/sobjectizer/so_support.h`, `tune_queue` / `tune_pool_queue`), `wait=0` to
`simple_lock_factory` (no spin at all). `QVO_SO_SPIN_WAIT_US` overrides the budget for a sweep
and for nothing else — a document measured under it carries `SWEEP DOCUMENT, NOT A TABLE CELL`
as its first caveat, like CAF's. `tools/so-sweep.sh` drives it: `savina/ping-pong` and
`savina/counting`, cores=2 wait=1, CPUs 0 and 2, 7 repetitions + 2 warmup, 1 000 000 messages,
the profile measured first and last as the drift control, each host in its own quiet session
(`results/<host>/sobjectizer-spin-sweep/`). ns per message, p50 [min, max]:

| budget | Windows / MSVC ping-pong | WSL2 / g++ ping-pong | Windows counting | WSL2 counting |
|---|---:|---:|---:|---:|
| **profile, 10 s** (first) | **827** [770, 895] | **631** [574, 693] | **245** [178, 294] | **169** [166, 178] |
| `simple_lock` (no spin) | 950 [916, 1298] | **26 344** [26 246, 26 751] | 278 [259, 436] | 168 [160, 171] |
| 1 µs | 959 [909, 1076] | **9 912** [9 138, 10 865] | 300 [257, 329] | 175 [160, 183] |
| 10 µs | 968 [870, 1016] | 657 [639, 669] | 300 [281, 345] | 172 [169, 187] |
| 100 µs | 879 [793, 911] | 695 [669, 722] | 295 [267, 327] | 159 [150, 171] |
| 1 ms (SObjectizer's default) | 836 [739, 866] | 667 [664, 699] | 224 [172, 289] | 171 [164, 178] |
| 10 ms | 888 [857, 1290] | 650 [639, 676] | 301 [283, 314] | 164 [153, 177] |
| 100 ms | 854 [780, 909] | 662 [658, 695] | 303 [279, 321] | 162 [150, 165] |
| 10 s, through the override | 901 [818, 1145] | 667 [663, 724] | 289 [271, 301] | 160 [152, 167] |
| **profile, 10 s** (last) | **821** [721, 947] | **608** [561, 655] | **288** [278, 304] | **166** [161, 180] |

Three readings. **No budget beats the profile.** Every budget from 100 µs up (10 µs up on Linux)
reads inside the launch-to-launch spread of the profile itself — the two profile runs are 827 / 821
on Windows and 631 / 608 on WSL2 for ping-pong, 245 / 288 for Windows counting, the bimodal
two-core launch every Windows document beside this one records — and SObjectizer's own 1 ms default
sits in that band with the rest: the adapter's 10 s and the framework's default are the same
setting for a hop that never lets the budget expire. **A budget shorter than the hop is the park
cell in disguise**: at 1 µs and under, the yield loop's first turn (~1 µs with the clock read)
already exhausts it and every hop pays the mutex-and-condition-variable path — on Windows +15 %
(950–968 against 821–827: the tickless kernel wake of §19.2 is cheap), on WSL2 ×16 at 1 µs and ×42
with no spin at all (26 344 ns: the hypervisor's futex wake, §6's floor) — which is what the
published `wait=0` column already says. **`counting` does not exercise the knob at all**: a
one-way flood keeps the consumer's queue non-empty, so its lock never waits and every row from
`simple_lock` to 10 s reads 159–175 ns on WSL2; the Windows column's 224–303 is the launch
bimodality, not the budget. The shape that measures a spin budget is the one whose queue drains
between hops.

**Consequence for the tables.** SObjectizer's spin cell IS its best profile, as CAF's is: the
tables stand, `FAIRNESS.md` §1.1 says both competitors were swept, and the symmetry the issue
asked for is measured rather than asserted.

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
| **D. wall clock per pass** | `wall_now()` every loop pass (19 / 30 ns) | 1c: 124 → 93 ns (Win), 101 → 75 (Linux) reading it every 64 passes = **−25 %**; 2c: 0 alone, 232 → 209 ns on top of F (Linux) | worth taking; cadence or `steady_clock`. **Landed as `time()` on demand (`985cbb3a`) — which MOVED the read into the tick phase's `LoopEvent` rather than removing it, so the −25 % was never delivered by that commit; §14 is where it arrived, at −57 %.** |
| **E. io poll per pass** | `listener::run(EVRUN_NOWAIT)` runs every pass once *any* coroutine scheduler exists, even with zero watchers | 1c: 124 → **694** (Win), 101 → **739** (Linux), 5.6–7.3× slower; 2c: 408 → 645, 271 → 594. A gate on `size() \|\| has_deferred()` (still draining deferred + `run_ready`) restores the figures | **large, hits any app that ever `co_await`ed**; also the ~300–380 ns/pass floor of any app with one watcher |
| **F. spsc producer re-reads `read_index_`** | one remote cache-line read per hop | 2c-spin 404 → **290** ns (Win, −28 %), 281 → **232** (Linux, −17 %); 1c unchanged | take; mirror it on the consumer side. **Its other half landed with §16 (QB-184): the producer also re-read its OWN published line, `write_index_`, on every enqueue — 2c ping-pong −22 %, the cross-core ring −31 % once the working index moved to a private line.** |
| **G. copies per hop** | event → pipe → mpsc ring → `consume_all` scratch → dispatch | analysed, not isolated; an in-place `consume_all(func)` exists in `mpsc.h` | minor, after F |
| **H. SpinLock on the send path** | — | the indexed `enqueue(index, …)` used by `SharedCoreCommunication::send` takes **no** lock; the lock is only on the round-robin variants | **retired** — not a cost |
| **K. store-buffer drain on the publish** | `notify()` returned before its fence at latency 0, so a spin-mode enqueue reached the polling peer only when the producer's store buffer drained on its own | 2c-**park** beat 2c-**spin** on BOTH platforms; fencing in spin mode too, interleaved A/B on a quiet host (3 pairs × 7 reps): Win 2c-spin 296–309 → **259–263** ns (park 256–272); WSL2 p50 a wash, 204–219 → 205–208, but the worst run 295 → 215; 1c and a 1M-event bulk push unchanged (18.6 M msg/s) | **taken** — one fence per cross-core publish; commit `6a0897c0` on the branch |
| **I. router double lookup** | `flat_hash` by EventId → virtual resolve → `flat_hash` by ActorId → fn ptr | both ids are dense (`_type_id_counter`, `ActorId::sid()`), so `router::dense_index<Key>` + `internal::key_table` (branch `router.h:87`, `:119`) index a vector directly and keep the full-key compare; interleaved A/B against L on Windows (9 reps): 1c counting 28.0 → 25.8, ring 51.4 → 45.6, big 29.8 → 24.0, fork-join 2c 42.2 → 37.0 — and **counting at two cores 30.6 → 43.5 (+42 %)**, which is finding M | **taken** — commit `44482a1d`, with M in the same branch; never ship I without M |
| **M. per-event publish on the cross-core pipe flush** | `__flush_all__` sent one event per `try_send`: one `enqueue`, one release store of the ring index, one `notify()` per event; the consumer's `consume_all` re-reads that index per batch, so a consumer FASTER than the producer turns every event into a coherence round trip on the index line | exposed by I: counting 2c with batch counters, sparse (3.1.0) consumer ~1.7k–3.9k batches of **250–600** events, dense (I) consumer ~108k–179k batches of **6–9**, both cores ~40 % slower (p50 30.6–32.1 → 38.8–41.9 ns). Fix: gather a run of whole events (≤ `kFlushRunBuckets` = 256 buckets, `VirtualCore.cpp:277`), one `write_room`, one `enqueue<true>`, one `notify` per run (`SharedCoreCommunication::send_run`, `Main.cpp:237`); the slow per-event path stays for zero-width, oversize, QoS-0 and back-off. A/B L vs I+M (4 passes): counting 2c 31.0 → **28.0** / 31.9 → **28.5**, ring 2c-spin 159 → **134**, big 2c 29.7 → **26.4**, ping-pong 2c level (±3 %), 1c cells −7…−27 % everywhere | **taken** — commit `230c5035`; a batching regime that only a faster consumer can reveal, so it was invisible on 3.1.0 |
| **L. out-of-line accessors on the per-event path** | `ActorId`'s constructors, `operator uint32_t`, `sid()`/`index()`/`is_broadcast()`/`is_valid()`, `Actor::is_alive()`, `CoreSet::resolve()`, `VirtualCore::__getPipe__()` defined in the archive, called from `Actor::push<T>` / the dispatch trampoline instantiated in the USER's TU — a call into the archive for a two-field load, opaque without LTO | found by `perf` on the four §9 benchmarks (WSL2): `ActorId(uint32_t)` + `is_broadcast` + `operator uint32_t` ~10 % of the counting profile, `is_alive` + `resolve` ~5 % more; in-class, 3-rep min: counting 1c 43.5 → 39.2, fork-join 2c 59.3 → 42.1, big 1c 31.6 → 25.2 ns | **taken** — commit `32b28130`; no behaviour change, three readme citations move with it |

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

Every checksum verified. (This table is the branch at `6a0897c0`, before axis L; the 2026-09-04
re-measurement of all five benchmarks — shipped and branch in one session, 9 repetitions on
Windows, 5 on WSL2 — is the pair of grids in README.md and the one `check-report.py` verifies,
at `32b28130` first and at `230c5035` since. The latter's ping-pong cells, 77 / 78 / 277 / 266 ns
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

Still open, in order: a BARE-METAL Linux run (the park floor here is the hypervisor's, §6) — the
self-hosted `qb-vm-linux-arm64` runner, where this list used to say it would happen, measured on
2026-09-19 and is a guest too: native arm64, and a park floor of 20.8 µs that is its hypervisor's
(§13.9); and the two design questions of §9 (9.2, the 64-byte
bucket, a 4.0 experiment on top of the segmented pipe; 9.4, placement, to be closed by design with
a placement paragraph in `qb.llm.md`). 9.11, the pipe's growth, which used to head this list, is
taken by `perf/event-pipe-segmented` (§9.11 carries the two-host A/B), and 9.12 closed with it.
arm64 and the axis-K fence, which used to be on this list, are answered by §9.13: the macOS host
measured the `dmb ish` without effect and qb's `dev/bench` gate passed with the engine metric
+94.9 %. The CAF spin profile, the cross-core CAF cell and axis I, which used to head this list,
are closed by §1.1, §8 and the subsection below; SObjectizer's spin budget, the symmetric
question, by §1.2. `docs/ROADMAP.md` carries the same list as the
3.2.0 pipeline, with what must happen before the branch ships and what comes after.

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

**Official grids, shipped 3.1.0 → branch at `230c5035`, same session, same protocol as the
published tables** (Windows 9 reps, WSL2 5 reps; `M-230c5035-shipped-3.1.0/` and `M-230c5035/`
under each host's `qb-branch-perf-core-hot-path/`): on Windows ping-pong 112 → 77 / 113 → 78 /
338 → 277 / 6624 → 266; counting 30 → 25.5 / 30 → 26 / 34 → 28 / 34 → 28; thread-ring
63.5 → 46 / 63 → 43 / 206 → 146 / 2122 → 135; fork-join 42 → 35 / 42 → 35 / 45 → 40 / 42.5 → 36;
big 37 → 24 / 36 → 24 / 32 → 27 / 31 → 27. On WSL2 ping-pong 98 → 69 / 98 → 67.5 / 276 → 233.5 /
26 523 → 214; counting 44 → 35 / 44 → 34 / 48 → 43 / 49 → 44; thread-ring 64 → 37 / 62 → 48 /
191 → 123 / 13 273 → 130; fork-join 71 → 65 / 70 → 59 / 50 → 54 / 48.5 → 45; big 38 → 25 /
40 → 26.5 / 29 → 23 / 32 → 23 (1c-spin / 1c-park / 2c-spin / 2c-park, ns per unit). Against the
previous candidate `L-32b28130` every cell moves −5…−25 % except three that are level within
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
`6a0897c0`, 7 repetitions + 2 warmup, CPUs 0 and 2, per round trip:

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
host, shipped qb 3.1.0 and the branch at `32b28130` through the same adapters in the same quiet
session, 9 repetitions on Windows and 5 on WSL2, checksums verified on every cell (the branch
figures in the table are that measurement; the §7 subsection on axes I and M carries the
re-measurement at `230c5035`). The ranking is
README.md's business; this section is the list of **qb-side costs** those four shapes exposed,
each with where it is, what it measures and what was done. Figures are ns per unit of work
(message, hop, message, round trip), p50, Windows / WSL2, shipped → branch unless stated. The
`where` column cites **qb 3.1.0** coordinates: axis I rewrote `router.h` and axis M moved
`__flush_all__`, so on the branch at `230c5035` the same symbols live at `router.h:87`
(`dense_index`), `router.h:119` (`key_table`), `VirtualCore.cpp:277` (`kFlushRunBuckets`) and
`Main.cpp:237` (`send_run`).

| # | finding | where | measured | state |
|---|---|---|---|---|
| **9.1** | **Same-core dispatch costs ~27–30 ns on MSVC and ~40 ns on g++ against a 3 ns floor.** `counting` at one core is the purest measurement of it: one producer, one consumer, no core crossing, no reply — the cell is `push<>` + pipe + `consume_all` + route, and nothing else. | `VirtualCore.cpp:199` (`_router.route`), `system/event/router.h:471` (`_registered_events.at(id)->resolve`), `router.h:169` (the second `unordered_map` by handler id), `VirtualCore.cpp:224` (`consume_all`) | 1c-spin: 29.4 → **27.4** / 44.6 → **40.8** at `32b28130`; **25.5 / 34.9** at `230c5035`; floor 3.2 / 2.8 | axis L took 2–4 ns, axis I another 2 (MSVC) to 6 (g++) — the g++/MSVC gap closed from 13 to 9 ns; what remains at 1 M messages (~22 / 32 ns over the floor) is NOT dispatch: the burst sweep of 9.11 measures the branch's same-core dispatch at **8.5 ns on g++ once the burst fits the cache** (30 000 messages; shipped 3.1.0: 37 — a 4.4× the 1 M cell reports as 20 %), and the rest of the 1 M cell is the memory system of a cold 64-MB pipe. **Taken** as far as the router goes; the remainder was 9.11, now taken — the 1 M cell is 9.2 ns on g++ and 9.5 on MSVC against the 2.8–3.0 floor — then 9.2 |
| **9.2** | **Every event is at least one 64-byte bucket, copied twice.** `QB_LOCKFREE_EVENT_BUCKET_BYTES` is the cache-line size, so a 16-byte payload is relocated as 64 bytes into the pipe and 64 bytes out of the ring into the `consume_all` scratch before dispatch. At 1 M messages that is 128 MB of memcpy per cell for a benchmark whose data is 16 MB. | `utility/prefix.h:68`, `Event.h:473` (`bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES`), `VirtualCore.cpp:224` | not isolated — it is inside 9.1's 27 ns; the g++/MSVC gap (40 vs 27) is the one hint, g++'s `memcpy` of a 64-byte aligned block being the slower of the two here | **open**; a sub-cache-line bucket for small events changes the ABI fingerprint (`abi.h`) and is a major-version change. 9.11 re-weighs it: the traffic a 32-byte bucket halves is the traffic the growth path pays twice, so it is an experiment to run ON TOP of the segmented pipe, where its remaining half is the honest one |
| **9.3** | **The out-of-line accessors — axis L.** Found by `perf` on these four, not on ping-pong, because a ring or a fan-out spends a larger share of its time in `push<T>` and the trampoline than a two-actor exchange does. | the §7 row | counting 1c 29.4 → 27.4 / 44.6 → 40.8; thread-ring 1c 62.9 → **51.0** / 62.5 → **48.0**; big 1c 36.2 → **31.3** / 37.4 → **31.0**; fork-join 2c-park 42.2 → 40.0 / 57.1 → **44.6** | **taken**, `32b28130` |
| **9.4** | **A ring crosses a core on every hop, and qb has no way to not.** `thread-ring` places actor i on VirtualCore i % cores, so at cores=2 every hop is a cross-core hop: 172 / 173 ns shipped against 63 / 62 ns on one core. CAF runs the receiver on the sender's worker (`worker::delay` → `queue.prepend`) and measures the same 236 / 140 ns at one core and two. qb's placement is static — an actor lives where it was built — so the framework cannot pull a ring onto one core; only the application can, by building it there. | `frameworks/qb/savina/thread-ring.cpp` (placement), `Actor.h:1112` (`forward`) | 2c-spin: 172.4 → **132.6** / 172.6 → **131.0** (floor 109.9 / 114.4, CAF 238.6 / 140.4); 1c: 62.9 → 51.0 / 62.5 → 48.0 | the branch closes 2c to 1.2× the floor; the remaining gap to CAF on WSL2 (131 vs 140, level within spread) is placement, not dispatch. **Open as a design question**: a `push<>` whose destination shares no core with the sender could migrate an actor with no other traffic, and qb has no such policy |
| **9.5** | **The 2c-park collapse is on every cross-core-per-message shape, on both platforms.** §5 found it on ping-pong; the ring shows it bimodal on Windows — a repetition lands at ~565 ns or ~3.02 µs per hop and stays there (median 2446.5) — and on the hypervisor's floor on WSL2 (13 409 ns per hop, floor 13 008). counting, fork-join and big do NOT collapse (shipped 2c-park 33.0 / 42.2 / 33.2 on Windows) because a funnel or a fan-out never lets the consumer's pipe run dry. | §5 (`Mailbox::wait()`), §8.2 (the idle floor) | thread-ring 2c-park: 2446.5 → **145.0** / 13 409 → **130.0**; ping-pong 2c-park: 4225.5 → **278.3** / 26 780 → **241.5** | **taken** on the branch (A + B + K); the WSL2 figure is bought by the 50 µs idle floor, §8.2 |
| **9.6** | **`send<>` is not faster than `push<>` on the all-to-all.** The eager cross-core variant was expected to win on `big` (one request in flight per actor, so nothing to batch) and measured slower: WSL2 3-rep min, send 26.1 / 25.9 / 26.2 against push 24.9 / 25.8 / 25.1 ns per round trip (1c-spin / 2c-spin / 2c-park). With 120 actors on two cores, the per-(core, core) pipe flush still carries a batch, and `send<>` gives that up for nothing. | `frameworks/qb/savina/big.cpp:61` (the comment carries the figures), `qb/llm/qb.llm.md` (`push` = ordered) | see left | recorded in the adapter; the `send<>` documentation should say when it wins (a single event with real idle behind it), which it does not |
| **9.7** | **qb sits BELOW the raw-thread floor on the three shapes with parallelism, and the floor is why.** At two cores, counting 33.1 vs floor 42.3, fork-join 42.2 vs 48.5, big 33.2 vs 60.4 on Windows; the floor's SPSC ring pays one remote cache-line crossing per message, qb's staging pipe moves a batch per flush. This is the batching of `__getPipe__()` doing its job and is the strongest argument these four make for the engine — the report prints it as "below the floor" rather than as a ratio. | `VirtualCore.h` (`__getPipe__`), `Main.h:354` (`MaxRingEvents`) | 2c-spin counting 33.1 / 46.3, fork-join 46.0 / 58.4, big 30.8 / 32.8 (floors 42.3 / 23.7, 38.2 / 28.5, 43.3 / 25.4) | nothing to do; note the WSL2 floor is BELOW qb on counting and fork-join (23.7 / 28.5) — g++'s SPSC ring is cheaper and 9.2 is the suspect |
| **9.8** | **fork-join at one core costs 1.4–1.6× counting at one core, and the difference is not isolated.** Same producer, same `push<>`, same core; the only change is 60 destinations instead of one, so 60 handler-map entries and 60 actors' state instead of one hot line. | `router.h:169` (`_subscribed_handlers` by handler id), `Actor.h` (`is_alive` on delivery) | 1c-spin 42.5 → 37.9 / 67.0 → 67.9 at `32b28130`, **34.8 / 65.0** at `230c5035`, against counting 25.5 / 34.9; 1c-park 64.4 → **58.9** on WSL2 | **taken in part** — the dense table (axis I) took 8 % on MSVC and 4–9 % on g++, so the 60-key lookup was a cost but not the whole 1.4–1.6×; what remains is 60 actors' state against one hot line, which is the shape and not the engine |
| **9.9** | **Windows 2c-park was the shipped engine's worst cell on every shape that crosses a core per message**, and on ping-pong it was worse than SObjectizer's `simple_lock` (4225.5 vs 1034.6) and 8.6× CAF; shipped 3.1.0 in park mode was, on this host, the slowest actor framework in the table for a cross-core round trip. | §5 | ping-pong 2c-park 4225.5 → 278.3; thread-ring 2446.5 → 145.0 | **taken**; the single most user-visible finding of the suite and the reason the branch exists |
| **9.10** | **A faster consumer made the cross-core pipe SLOWER — the per-event publish.** The first candidate with axis I lost 42 % on counting at two cores while winning every one-core cell: `__flush_all__` published the mailbox ring's write index once per event and the consumer re-read it once per `consume_all` batch, a cost that is invisible while the consumer lags (250–600 events per batch at 3.1.0) and one coherence round trip per message once it keeps up (6–9 per batch with the dense router). 3.1.0 never showed it because 3.1.0's consumer was never fast enough. | `VirtualCore.cpp:277` (`kFlushRunBuckets`, branch), `Main.cpp:237` (`send_run`, branch), `spsc.h` (`write_room`) | counting 2c-spin, L → I → I+M: 30.6 → 43.5 → **28.3**; 2c-park 31.7 → 44.5 → **28.2**; big 2c 29.5 → **26.9** / 30.9 → **23.0**; ping-pong 2c level | **taken** — axis M, `230c5035`; the general lesson is that a dispatch optimisation must be measured at two cores, where it can flip the pipe into a regime the ring was not tuned for |
| **9.11** | **The one-core cell at 1 M messages measures the memory system of a cold 64-MB pipe, not dispatch — and the pipe's growth is why.** A burst of N `push<>` in one handler is staged whole (nothing drains a core's own pipe while its handler runs), so the pipe's high-water mark IS the burst: 64 MB at 1 M events of one 64-byte bucket. `allocate_back` grows it by doubling — `_factor <<= 1`, a fresh `std::allocator::allocate`, a `memcpy` of the entire content, the old block freed — so a burst above the high-water mark copies every event it already holds (~64 MB of non-temporal `memmove` per 1 M-event burst), and because every block from 512 KB up is above the allocator's reuse threshold the doubling path re-faults them from the kernel on every fresh engine and every new high-water mark: 26 600 minor faults per repetition, 104 MB of first-touch for 64 MB of data. The sweep below is the measurement: on g++ the branch's same-core dispatch is **8.5 ns** at 30 000 messages and 35 at 1 M; shipped 3.1.0 is 37 → 43 on the same axis, so the branch's real dispatch gain is **4.4×** and the 1 M cell reports it as 20 %. CAF (118 → 118) and SObjectizer (94 → 106) do not have the cliff: a per-message heap allocation is reused by the allocator, a contiguous growable pipe is not. | `system/allocator/pipe.h:356` (`allocate_back`), `pipe.h:373` (`_factor <<= 1u`), `pipe.h:382` (the `memcpy`), `pipe.h:59` (`_SIZE = 4096` buckets = 256 KB), `Event.h:689` (`VirtualPipe = allocator::pipe<EventBucket>` — the mono pipe of `VirtualCore.cpp:220` and every cross-core `_pipes` entry alike) | WSL2 1c-spin, branch: 2 k **6.5**, 10 k 8.0, 30 k 8.5, 100 k 12.5, 300 k 31.8, 1 M 35.2, 4 M 38.8 ns; `time` at 1 M: user 90 ms, **sys 140–160 ms** over six runs; `perf` user share: 32 % libc `memmove` (non-temporal path, called from `allocate_back` under the producer's trampoline), 25 % the push loop, 17 % `__receive_events__`, 9 % `resolve`, 9 % the consumer trampoline. Windows: kernel 47 ms of 172 at 1 M, 312 of 656 at 4 M (six runs, 15.6 ms timer) — the shape there is in the subsection | **taken — `perf/event-pipe-segmented`**: a segmented pipe over a process-wide slab pool (growth appends, copies nothing; slabs stay warm across engines; `push<>` references stable, the `Pipe.h:118` contract retired). g++ 1 M: 35.0 → **9.2** ns, 4 M 40.1 → 9.3; MSVC 1 M 25.8 → **9.5**; page faults per 1 M process 230 942 → **291**. The subsection carries the two-host sweep, the five-benchmark grid, the launch census and the 4K-aliasing defect the A/B caught |
| **9.12** | **On MSVC the dispatch itself is 20–25 ns where g++'s is 6.5–8.5.** The same sweep on Windows: 2 k 20.6, 30 k 25.0, 1 M 25.3, 4 M 25.7 — flat, with no cliff and a non-monotonic 10 k–300 k stretch (29.9 / 25.0 / 28.3 / 32.8) that is recorded, not explained. Windows pays the growth too (kernel 8–13 ns per message at 1 M–4 M) but its faults and copies are cheap enough to hide under a dispatch that is 3× g++'s at cache-resident bursts. Whether that 3× is the compiler (inlining of the trampoline / `allocate_back` chain, the `std::span` walk) or the OS is the open question; the discriminating experiment is a clang-cl build of the same tree on the same host, which no cell here has. | the §9.1 path | Windows 1c-spin branch 20.6 (2 k) → 25.3 (1 M); shipped 32.7 → 31.4; CAF 174 → 186; SObjectizer 137 → 142; floor 3.0 / 3.4 | **closed by 9.11** — with the pipe's memory warm the same MSVC binary dispatches at 6.6–10.4 ns from 2 k to 4 M against g++'s 5.9–9.3; the "flat 20–25" was fresh pipe pages faulted per repetition, not the compiler. The clang-cl A/B has no premise left |

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

### 9.11 The burst sweep — what the one-core cell measures

`savina/counting` at one core, spin, with the producer's burst swept from 2 000 to 4 000 000
messages (`results/*/qb-branch-perf-core-hot-path/burst-sweep/`, 7 repetitions + 2 warmup, CPUs 0
and 2, quiet host, 2026-09-04, checksums verified on every document). The published cell is the
1 M column. ns per message, p50:

| burst | qb branch `230c5035` | qb 3.1.0 | CAF | SObjectizer | floor |
|---:|---:|---:|---:|---:|---:|
| **WSL2 g++ 14.2** | | | | | |
| 2 000 | **6.5** | | | | |
| 10 000 | 8.0 | | | | |
| 30 000 | 8.5 | 37.0 | 113.4 | 93.8 | 2.9 |
| 100 000 | 12.5 | | | | |
| 300 000 | 31.8 | | | | |
| 1 000 000 | 35.2 | 42.6 | 118.6 | 106.3 | 3.0 |
| 4 000 000 | 38.8 | | | | |
| **Windows MSVC 19.51** | | | | | |
| 2 000 | 20.6 | | | | |
| 10 000 | 29.9 | | | | |
| 30 000 | 25.0 | 32.7 | 174.1 | 137.4 | 3.0 |
| 100 000 | 28.3 | | | | |
| 300 000 | 32.8 | | | | |
| 1 000 000 | 25.3 | 31.4 | 185.9 | 142.3 | 3.4 |
| 4 000 000 | 25.7 | | | | |

Three things this says, each verified in the source or by a second instrument:

- **The cliff is qb's, and it is the pipe.** A handler that pushes N events stages all N — a
  core never drains its own pipe mid-handler, so the pipe's high-water mark is the burst, 64 MB
  at 1 M. `allocate_back` (`system/allocator/pipe.h:356`) grows by doubling with a `memcpy` of
  everything it holds and a fresh allocation each time; the 256 KB → 64 MB ladder copies ~64 MB
  per 1 M-event burst and touches 104 MB of never-mapped pages (26 600 minor faults per
  repetition, `perf stat`), because every rung from 512 KB up is above the allocator's reuse
  threshold and comes back from the kernel each time. On WSL2 `time` puts **60 % of the process
  in the kernel** at 1 M (sys 140–160 ms against user 90 over six runs), and the user profile
  puts 32 % of what is left in libc's non-temporal `memmove`, called from `allocate_back` under
  the producer's trampoline. CAF and SObjectizer allocate per message and get their memory back
  from `malloc` warm; they are flat across the sweep.
- **The branch's dispatch gain is 4.4×, and the published protocol shows a fifth of it.** At
  30 000 messages — 1.9 MB, cache-resident — shipped 3.1.0 dispatches at 37 ns and the branch at
  8.5, against a 2.9 ns floor. At 1 M both pay ~27 ns of memory system on top, and 43 → 35 reads
  as 20 %. The 1 M protocol stays (it is the Savina figure, and every framework runs it), but the
  one-core rows in README.md are to be read as "a cold 64-MB burst through the engine", not as
  the dispatch cost; §9.1's figures are corrected to say so.
- **Windows hides it under a slower dispatch.** MSVC's cell is 20–25 ns at every burst size — the
  growth costs the kernel 8–13 ns per message at 1 M–4 M there too (47 of 172 ms, 312 of 656),
  but Windows' demand-zero faults and MSVC's `memcpy` are cheap enough that the cliff never
  shows above a dispatch that is 3× g++'s at 2 000 messages (20.6 vs 6.5). That gap is 9.12.

**What was done about it — `perf/event-pipe-segmented`, measured against `230c5035` on both
hosts with this sweep as its instrument** (`results/*/qb-branch-perf-event-pipe-segmented/`,
2026-09-05, one quiet session per host and never the two at once: Windows 01:07–01:16 UTC for
the pass quoted, WSL2 01:18–01:26 UTC, the Windows grid-order census 01:27–01:29 UTC; the earlier
passes are kept as `*-pass<N>/` and every document carries its `env.utc`). The
branch is two local commits over `perf/core-hot-path`: `b34fbc23`, the segmented pipe itself —
growth links a 256 KB segment and copies nothing, segments are retained at the high-water mark,
`__receive_events__` / `__flush_all__` walk segments, and the reference `push<>` returns is
stable for the event's life, which retires the `Pipe.h:118` contract rather than documenting it
(`PushReferenceStability.*` asserts the opposite) — and `279e6cd4`, where the segments come from:
a process-wide `slab_cache` (`qb/system/allocator/slab.h`) of 2 MB slabs mapped by the platform
(`mmap` on POSIX, `madvise(MADV_HUGEPAGE)`d and prefaulted with `MADV_POPULATE_WRITE`;
`VirtualAlloc` on Windows), carved eight segments to a slab by each core's `segment_pool` and
kept warm across pools and engines. The A/B is what put the slabs there: with segments from
`malloc` the segmented pipe already copied nothing, but a 1 M burst in a fresh engine still took
15 640 minor faults — every 4 KB of every segment, ~15 ms of a 20 ms run on WSL2
(`burst-sweep-prehoist/pass1-malloc-segments/`). ns per message, p50, 7 repetitions + 2 warmup,
both qb binaries in the same session:

| burst | qb `perf/event-pipe-segmented` | qb `230c5035` | qb 3.1.0 | CAF | SObjectizer | floor |
|---:|---:|---:|---:|---:|---:|---:|
| **WSL2 g++ 14.2** | | | | | | |
| 2 000 | **5.9** | 9.7 | | | | |
| 10 000 | 8.8 | 11.4 | | | | |
| 30 000 | **6.4** | 8.8 | 34.2 | 114.7 | 90.6 | 2.8 |
| 100 000 | 6.6 | 10.5 | | | | |
| 300 000 | 6.7 | 34.7 | | | | |
| 1 000 000 | **9.2** | 35.0 | 43.4 | 116.5 | 106.8 | 2.8–3.8 |
| 4 000 000 | 9.3 | 40.1 | | | | |
| **Windows MSVC 19.51** | | | | | | |
| 2 000 | **6.6** | 19.3 | | | | |
| 10 000 | 9.4 | 29.1 | | | | |
| 30 000 | 10.4 | 25.5 | 31.9 | 180.7 | 139.5 | 3.0 |
| 100 000 | 7.5 | 28.5 | | | | |
| 300 000 | 9.2 | 34.0 | | | | |
| 1 000 000 | **9.5** | 25.8 | 30.2 | 185.3 | 143.3 | 2.9 |
| 4 000 000 | 9.6 | 26.8 | | | | |

The cliff is gone on both compilers: the 1 M cell is 9.2 ns on g++ against 35.0, and 9.5 on MSVC
against 25.8, both within a few ns of the cache-resident 30 000 figure and 3× the raw floor
rather than 12×. `perf stat` on the whole process (2 warmup + 7 repetitions, 1c-spin counting,
WSL2) puts the mechanism in one number: **291 page faults** at 1 M for the branch against
230 942 for `230c5035` and 287 732 for shipped 3.1.0 — and 290 at 30 000, i.e. the count no
longer depends on the burst at all, because a slab is faulted once per 2 MB by the kernel and
never again. CAF and SObjectizer do not move.

**The five-benchmark grid is the regression check**, same protocol as `M-230c5035/` (7 + 2 here
against 9 + 2 / 5 + 2 there; `grid-final/` beside `grid-230c5035/` and `grid-shipped-3.1.0/`,
the three binaries in one session). ns per unit, p50, branch vs `230c5035`:

| | 1c-spin | 1c-park | 2c-spin | 2c-park |
|---|---:|---:|---:|---:|
| **WSL2** ping-pong | 64.7 vs 67.6 | 66.5 vs 68.3 | 208.8 vs 222.8 | 232.4 vs 230.0 |
| counting | 9.4 vs 34.7 | 11.1 vs 36.2 | 10.3 vs 43.3 | 10.7 vs 44.0 |
| thread-ring | 41.5 vs 47.5 | 42.6 vs 45.4 | 126.2 vs 132.0 | 129.0 vs 130.8 |
| fork-join | 9.2 vs 64.4 | 10.1 vs 63.4 | 10.7 vs 45.0 | 12.2 vs 49.3 |
| big | 25.9 vs 30.0 | 28.7 vs 23.2 | 21.8 vs 22.3 | 24.4 vs 27.1 |
| **Windows** ping-pong | 82.7 vs 78.6 | 79.0 vs 78.3 | 318.7 vs 259.1 | 265.2 vs 270.2 |
| counting | 9.2 vs 27.4 | 9.6 vs 26.2 | 11.2 vs 28.8 | 11.6 vs 29.4 |
| thread-ring | 43.2 vs 45.9 | 46.1 vs 45.0 | 164.5 vs 136.9 | 144.4 vs 149.2 |
| fork-join | 9.9 vs 36.1 | 9.6 vs 35.6 | 10.8 vs 38.8 | 11.0 vs 36.5 |
| big | 21.8 vs 23.6 | 20.5 vs 22.9 | 22.9 vs 27.4 | 25.1 vs 27.3 |

The two shapes that stage a burst — counting and fork-join — are 3–7× cheaper at every cell on
both hosts (fork-join's 1c cell on g++ was 64 ns because its fan-out pushes 64 MB in one handler
and then walks it; it is 9–10 now, level with counting, which is what a fan-out without a growth
ladder costs). big, which is a memory-bound all-to-all, gains 2–17 % from the slabs' locality.
The two that cross a core per message move nothing at the mechanism level — the mailbox ring is
untouched, and the segments a core drains and fills are its own — and read as level or better
on WSL2 (ping-pong −2 to −6 %, ring −1 to −13 %).

**Three cells needed a second instrument, and one of them a third.** The grid is one launch per
cell, and on Windows the two `2c-spin` cells of ping-pong and thread-ring are bimodal within a
launch: the per-repetition sequence of either binary alternates between ~245–255 and ~300–345 ns
per round trip (ring ~120–145 vs ~165–180), and a 7-repetition median lands wherever the majority
fell. Over four grid passes the branch's ping-pong 2c-spin medians were 274–319 ns against
`230c5035`'s 258–263, the ring's 141–173 against 134–152, and a difference that survives four
passes is not dismissed by calling it noise. The **launch census** is the instrument for that —
the same binaries, the same arguments, launched standalone N times interleaved, 3 + 1 repetitions
each (`census-win-*/` under the scratch results, not kept as documents): ping-pong 2c-spin
**250.2 → 250.0** ns (10 launches), 251.7 → 239.6 (12), 2c-park 251.1 → 256.4 and 252.0 → 260.8,
then 262.3 → 259.7 over 16; thread-ring 2c-spin 130.3 → 124.7 and 126.6 → 124.6, 2c-park
137.7 → 134.0 and 138.1 → 129.8; and the one-core ping-pong cells, which are the sensitive
same-core path, 77.3 → 77.8 (spin) and 78.1 → 78.6 (park) — within 1 %, distributions
overlapping. The census was then re-run as a KEPT document with `tools/launch-census.py` — the macOS host's protocol, 12 launches interleaved, 3 + 1, pinned `0,2`, on all eight cells of ping-pong and thread-ring (`launch-census/`, 2026-09-06, `census.log` beside it): ping-pong 1c-spin 82.2 vs 82.5, 1c-park 84.5 vs 83.6, 2c-spin 269.8 vs 265.9, 2c-park 271.9 vs 271.3; thread-ring 1c-spin 49.0 vs 47.8, 1c-park 48.0 vs 47.4, 2c-spin 133.3 vs 138.8, 2c-park 142.9 vs 146.1 — every pair's distributions overlap, the ring's two-core cells 2–4 % in the branch's favour and nothing outside a spread. That session's absolute level sits 5–8 % above the scratch censuses on BOTH sides (an evening host with a browser open), which is the case interleaving exists for: it moves the pair, not the difference. A **mode census** under the grid's own 7 + 2 protocol, six launches per binary,
puts the branch's per-launch ping-pong medians at 216–267 against `230c5035`'s 249–283 and the
ring's at 120–135 against 125–130. The third instrument is the grid's own launcher with the sequence changed and nothing else: `run.py`, 7 + 2, the same cell order, over a `bin/` holding ONLY ping-pong and thread-ring, four interleaved rounds per binary (`grid-order-census/`, kept, with its log). With big, counting and fork-join no longer run first, the branch's ping-pong 2c-spin round medians read 236–256 against `230c5035`'s 223–255 (medians of medians 251.8 vs 238.3, one bimodal step apart), 2c-park 251.5 vs 254.5, and the ring's 2c-spin 122.8 vs 129.5, 2c-park 132.9 vs 134.6 — the 274–319 of the full grid appears in no instrument that does not run the three burst benchmarks first. Each cell is its own process, so what carries from one cell to the next is the host's state, not the pipe's; the mechanism is not identified, and the finding is recorded as what it is: a property of the sequence, not of the build. The grid cell is kept as measured and the census is the figure
to quote for those two cells; on WSL2 the same census (10 launches) reads ping-pong 2c-spin
217.2 → 210.1, 2c-park 214.8 → 214.0, 1c 66.8 → 65.1 and 67.5 → 65.6; thread-ring 1c-spin
37.7 → 35.9, 2c-spin 116.4 → 105.9, 2c-park 120.1 → 117.9; big 21.3–22.1 → 20.9–21.4 at all
four — nothing slower.

**The A/B caught one defect in the branch before it landed, and it is the reason the pool
staggers its segments.** With slabs in place, `savina/big` at one core on g++ measured 107 ms per
repetition against 52 for the malloc-laid segments it replaced — the burst cells were 4× faster
and the all-to-all was 2× slower. Segments carved on a fixed stride out of 2 MB-aligned slabs all
start at the same offset within a 4 KB page, so item `k` of the pipe being drained and item `k` of
the pipe being filled share their low twelve address bits; a reply is a byte copy of the received
event into the outbound pipe at the same index, and the receive loop then re-read `bucket_size`
from the received event to advance — a load trailing a store to an address the core cannot tell
apart from it until the store commits (Intel's 4K aliasing; `perf` put that reload alone at 12 %
of the samples). Two changes: the loop reads the width once, before the handler runs, and
`segment_pool` gives the `i`-th segment it carves a stagger of `(i × 27) mod 64` cache lines, so
the two pipes of a core never share a page offset — a one-line stagger was measured too, and cost
8 % on the same benchmark by moving the store onto the NEXT event's load. big is now faster than
before the segmented pipe on both hosts (the grid and the census above), and
`grid-slabs-prehoist/` is the document with the defect in it.

**9.12 closes with this.** The "MSVC dispatch gap" was fresh pipe pages faulted per repetition,
not the compiler: with the pipe's memory warm the same MSVC binary dispatches at 6.6–9.5 ns from
2 000 to 4 M against g++'s 5.9–9.3, and the flat 20–25 ns that made a clang-cl A/B look
informative is gone. What MSVC keeps is a one-core ping-pong 1–2 % slower than `230c5035`
in three instruments that agree on the sign (census 77.3 → 77.8 ns, 24 launches 77.2 → 78.0,
grid-order 76.3 → 77.7) where g++ gains 3 % — inside each instrument's spread, recorded, not
explained, and the one open item this branch leaves on the same-core path.

**And the A/B was run anyway, on 2026-09-08, against the 3.2.0 tree — the answer is "both,
in halves"** (`results/desktop-win11-msvc19/qb-46-clang-cl/`, Huly QB-46). qb `381e4995` built
twice from the same directory by `cl` 19.51 and by `clang-cl` 22.1.7 — LLVM's Clang behind MSVC's
command line, ABI, CRT and STL — and measured in one quiet session: on every dispatch-bound shape
the clang-cl binary is **10–18 % faster** (ping-pong 1c 31.6 → 27.4 ns, big 17.1 → 14.8, fib
179 → 161, `pass-cost` k = 1 15.6 → 14.1, k = 2 22.5 → 19.5, `push` 31.6 → 28.7, the io pass
48.9 → 43.7, `dispatch-population` at 16 actors 6.1 → 5.2), and on the `counting` burst sweep —
one actor, a cache-resident and perfectly predictable dispatch — the two are level (5.7–6.0 ns a
message from 2 k to 100 k, against g++'s ~5: what §9.11 left of the "3×" is a tenth). Against
g++-14 on WSL2, the same tree's ping-pong 1c reads 22.8 ns: MSVC is +39 % and clang-cl +20 %, so
half the gap is MSVC's codegen of the dispatch chain and the other half is the platform, which no
compiler switch reaches. What the experiment found on the way is worth more than its number:
under clang-cl **every `task<qb::Event>` crashed on its first `co_await`** — the MSVC STL's
`from_promise()` / `promise()` computed with an alignment of zero, wrong for an over-aligned
promise (QB-200: fixed in qb, `promise_access.h`, reproduced in thirty lines) — and qb's CMake
compiled clang-cl through its GCC/Clang branch, `-Wall` read as `/Wall` = `-Weverything`
(QB-201: clang-cl is an MSVC-frontend toolchain now, 0 warnings, the `clang-cl` preset, the suite
194/194). One anomaly stays recorded, not explained: the clang-cl `counting` binary reads +49 %
at a burst of exactly 1 M (12.3 against 8.2, its minimum as high as its median) and +5 % at 4 M —
a layout effect to re-measure on the fixed toolchain. §9.12's recommendation: on Windows, build
with clang-cl.

Suites before any of these numbers were quoted: WSL2 g++ 14.2 release 534 TUs, 0 warnings, ctest
368/368 executed, 0 skipped; ASan+UBSan 189/189; TSan 189/189; the example corpus 98/99 (586
`@expect` lines, 17 second-instance assertions; the one failure is `modules-http-http3` on a host
whose libnghttp3 1.8.0 turns HTTP/3 off, pre-existing and not this branch). Windows/MSVC 19.51,
`verify-windows.ps1`: release, debug, relwithdebinfo and dev-cxx23 each 553 TUs, 0 warnings,
370/370 executed, 0 skipped; feature-gates 324 registered / 311 executed / 13 skipped (the
structural pgsql self-skips); the package installed and consumed; `dev/agent/verify.sh` ALL GREEN
on both hosts' tree (175 docs, 4615 content digests, 0 non-conformant of 443 formatted files).
`dev/bench` (macOS arm64 baseline) ran on 2026-09-05 — §9.13 below.

### 9.13 The macOS arm64 host — the third host, and the first one nobody could pin

`results/macbook-m4pro-macos-clang21/`, 2026-09-05, one quiet session: Apple M4 Pro (10 P + 4 E
cores, 48 GB), macOS 26.6, AppleClang 21.0.0, `-O3 -DNDEBUG`, 7 repetitions + 2 warmup, the
field at 18:59–19:07 UTC and the candidate's grids, sweep and censuses at 19:06–19:21 UTC, with
the other agents on the machine paused first. Its README carries the residual load (a VM, two
idle dev servers, macOS's storage scan after 20 GB of build output). Three things distinguish this
host from the other two, and each shaped the protocol:

- **Nothing here is pinned.** macOS offers no affinity a program can read back; the harness
  refuses `--cpus` on such a platform rather than record a pin that did not happen (FAIRNESS.md
  1.4), and `tools/run.py --no-pin` — added for this host — forwards the harness's own escape
  hatch and writes `unpinned` where the CPU list goes, so a partial re-run cannot merge pinned
  and unpinned cells. Every document says `pinned:false`. The consequence is measured, not
  assumed: the two-core cells of ping-pong, thread-ring and big are bimodal within a launch
  (min–max over 7 repetitions 1.3–1.5×; one-core cells 1.01–1.04×), so **every two-core figure
  quoted here comes from a launch census**, never from the grid alone.
- **The compiler is the third one.** g++ 14 and MSVC 19.51 had disagreed on the one-core
  ping-pong cell (g++ −3 %, MSVC +1–2 % for the candidate); clang/arm64 is the tiebreak.
- **The fence of axis K is a `dmb ish`**, not an `mfence`, and had never been measured.

**The field** (`savina-*/`, shipped 3.1.0 against CAF 1.1.0, SObjectizer 5.8.5.1 and the floor;
README.md carries the tables under `check-report`). The shape of the other two hosts holds:
qb fastest in every one-core cell (ping-pong 87 ns against SObjectizer 135 and CAF 263; counting
11 / 64 / 73; thread-ring 43 / 57 / 122; fork-join 13 / 45 / 146; big 24 / 174 / 258), fastest in
every two-core cell of counting, fork-join and big, and **below the floor** in seven cells — a
framework that stays on one core beating two raw threads that cross one, as on Windows. What is
new is the **2c-park column**: the raw condition-variable floor is **4.62 µs per ping-pong round
trip** and 2.49 µs per ring hop — between Windows (~300 ns) and WSL2 (25 µs) — and every
framework that parks across cores pays it (qb 3.1.0 6.85 µs, SObjectizer 5.61 µs, `caf-detached`
6.13 µs); the pooled `caf` row (381 ns) never crosses a core.

**The burst sweep** (`qb-branch-perf-event-pipe-segmented/burst-sweep/`, `tools/burst-sweep.py`:
the three qb binaries launched interleaved at each burst, 7 + 2, page reclaims from
`/usr/bin/time -l` beside each document). ns per message, p50, counting 1c-spin:

| burst | `perf/event-pipe-segmented` `279e6cd4` | `230c5035` | 3.1.0 | CAF | SObjectizer | floor |
|---:|---:|---:|---:|---:|---:|---:|
| 2 000 | **5.9** | 6.9 | 9.3 | | | |
| 10 000 | 6.2 | 8.0 | 10.4 | | | |
| 30 000 | **5.7** | 7.7 | 10.4 | 73.5 | 64.4 | 4.3 |
| 100 000 | 5.6 | 8.2 | 10.5 | | | |
| 300 000 | 6.1 | 8.8 | 11.8 | | | |
| 1 000 000 | **6.2** | 8.2 | 11.3 | 74.8 | 64.2 | 4.4 |
| 4 000 000 | 6.1 | 8.6 | 11.3 | | | |

**There was no cliff to remove on macOS.** `230c5035`, which climbed 9.7 → 40 on g++ and
19 → 27 on MSVC, reads 6.9 → 8.6 here, and 3.1.0 9.3 → 11.3 where it read 43. The mechanism
§9.11 describes — the doubling ladder re-faulting fresh pages at every rung because the block is
above the allocator's reuse threshold — costs XNU almost nothing: its zero-fill fault is cheap and
`malloc`'s large-block path hands the ladder back warm. What the candidate buys here is the
dispatch, not the faults: **5.6–6.2 flat**, fastest at every burst, −25 % against `230c5035` and
−45 % against 3.1.0, 1.4× the raw floor. The page-reclaim count says where the residual is: at
1 M the candidate takes **4 344** against 8 364 / 8 365 (16 256 against 32 374 at 4 M) — halved,
where Linux divided by 800 — because `MADV_POPULATE_WRITE` does not exist on Darwin and
`slab_cache` maps its 2 MB slabs without prefaulting them, so each 16 KB page is still faulted on
first touch. Touching each page once at carve time would close that gap on this platform; it is
recorded as an option, not taken, because the slabs are retained at the high-water mark and the
faults are paid in warmup.

**The three grids** (`grid-final/`, `grid-230c5035/`, `grid-shipped-3.1.0/`, 7 + 2, 19:06–19:08 UTC).
One-core cells, ns per unit, p50 (min–max spread 1–4 %, quotable as they are):

| | candidate | `230c5035` | 3.1.0 | candidate vs `230c5035` |
|---|---:|---:|---:|---:|
| ping-pong 1c-spin | **49.7** | 53.8 | 84.9 | −7.6 % |
| ping-pong 1c-park | 50.4 | 53.3 | 84.5 | −5.4 % |
| counting 1c-spin | 6.6 | 8.3 | 11.6 | −20 % |
| thread-ring 1c-spin | 25.9 | 26.8 | 42.0 | −3.4 % |
| fork-join 1c-spin | 6.2 | 8.6 | 12.9 | −28 % |
| big 1c-spin | 15.0 | 19.6 | 23.9 | −23 % |

The one-core ping-pong cell — the sensitive same-core path where MSVC kept +1–2 % — reads
**−7.6 % for the candidate** on clang/arm64, in both spin and park. Two-core cells, from the
**launch census** (`launch-census/`: 12 launches interleaved candidate/`230c5035`, 3 + 1 each,
median of per-launch medians, and whether the two distributions overlap):

| | candidate | `230c5035` | | distributions |
|---|---:|---:|---:|---|
| ping-pong 2c-spin | **156.3** | 187.4 | −16.6 % | overlap (142–209 vs 156–218) |
| ping-pong 2c-park | 210.6 | **186.9** | +12.7 % | overlap (196–235 vs 172–241) |
| thread-ring 2c-spin | 75.9 | 74.7 | +1.6 % | overlap |
| thread-ring 2c-park | 95.0 | 90.6 | +4.9 % | overlap |
| big 2c-spin | **16.3** | 17.6 | −7.1 % | overlap |
| big 2c-park | **15.3** | 17.3 | −11.2 % | overlap |

Every pair overlaps, so by this repository's own rule none of these is a measured difference;
the medians favour the candidate on ping-pong 2c-spin and both big cells (the slabs' locality,
as on the other hosts) and are level on the ring. **The one residual is ping-pong 2c-park**: the
candidate's median is 12 % above `230c5035`'s in the grid (209.6 vs 185.4), in the 12-launch
census (210.6 vs 186.9) and in a third instrument built for it — 24 launches × 5 repetitions
(`launch-census-pingpong-2cpark-24x5/`: 209.7 vs 187.5, +11.8 %, candidate 185–228, control
166–241). Three instruments agree on the sign and all three distributions overlap. Nothing in the
two `event-pipe-segmented` commits touches the park handshake; what they change on this path is
where the staged event lives before its flush (a staggered slab segment rather than a `malloc`
block), and a cross-core wake that reads it is the one cell where that placement could show.
Recorded, not explained — the same standing as MSVC's one-core +1–2 %. Against shipped 3.1.0 the
same cell is 6.85 µs → 0.21 µs, the branch's largest single move on this host (axes A/B/C).

**Axis K on `dmb ish`** (`axis-k/`: qb at `6a0897c0`, the commit that made `Mailbox::notify()`
fence in spin mode too, against its parent `61b0b4cf`; three interleaved grid rounds, then the
12-launch census that is the figure to quote):

| | with fence | without | | distributions |
|---|---:|---:|---:|---|
| ping-pong 2c-spin | 190.4 | 181.5 | +4.9 % | overlap (169–218 vs 161–225) |
| ping-pong 2c-park | 199.1 | 190.5 | +4.5 % | overlap |
| thread-ring 2c-spin | 79.8 | 89.1 | −10.5 % | overlap |
| thread-ring 2c-park | 99.6 | 88.7 | +12.3 % | overlap |

Mixed signs, every pair overlapping: **the fence has no measurable cost on arm64** at the
precision this unpinned host allows (the grid rounds read +12.6 / 0.0 / +18.1 % on ping-pong
2c-spin, which is what sent the question to the census; the census does not confirm it). The
inversion QB-44 asked about — two-core spin slower than two-core park — does not occur: in the
candidate's own grid spin beats park in every cell (ping-pong 152.7 vs 209.6, thread-ring 76.9
vs 93.2, counting 7.3 vs 7.9).

**qb's own gate, `dev/bench`** (superproject `benchmarks` preset, `bench-run.sh --runs 3`,
`bench-compare.py --baseline dev/bench/baseline/macos-arm64.json`): **VERDICT PASS** — 51 gated
metrics, 46 within threshold, 0 regressed, 5 improved, 0 missing. The five are one benchmark, the
only engine-path metric that gates: ping-pong throughput, 64 pairs on one core, **82.0 → 159.7 M
messages/s (+94.9 %)**. Of the 69 recorded-only metrics the engine ones all moved the right way:
same-core pipeline hop 42.5 → 31.9 ns (+33.5 % deliveries/s), same-core ping-pong latency
+13.3 %, cross-core ping-pong latency +61 %, 8-core ping-pong throughput +20 %, cross-core
pipeline hop +5.1 % inside a 58 % recorded spread.

**Suites before any of these numbers were quoted** — the whole solution, as this superproject
validates it: qb standalone (`cmake --preset` from `qb/`) release, sanitize, sanitize-thread,
coverage each 195 TUs, 0 warnings, **189/189 executed, 0 skipped**; superproject release,
sanitize, sanitize-thread, coverage, dev-cxx23 each 554 TUs, 0 warnings, **372/372, 0 skipped**
(the ≥ 365 floor plus the branch's seven new tests; PostgreSQL 18 and Redis answered, so all 14
pgsql and 27 redis integration tests executed); feature-gates 485 TUs, **326 / 313 / 13** (the
thirteen structural pgsql self-skips); the example corpus **99 / 99**, 586 `@expect`, 18
second-instance assertions (`modules-http-http3` passes here — the WSL2 failure was libnghttp3,
not the branch). Two things the macOS pass found and fixed on the way, neither in the branch:
the superproject's `sanitize` preset linked a Homebrew GoogleTest compiled without ASan, and
libc++'s container annotations then disagreed at the library boundary — three qbm-http unit
tests aborted with a `container-overflow` that is not one (intermittent, gone with
`detect_container_overflow=0`; `GTestIsInitialized()` copying the library's argv vector) — so
under `QB_SANITIZE` qb now builds GoogleTest from the pinned tag, instrumented like everything
else (372/372 again with it); and the 19 `benchmark::internal::Benchmark` deprecation warnings
of google-benchmark 1.9.5 are gone with the public spelling, the floor and pin at 1.9.5, and the
deprecation exemption that hid them retired.

## 10. Axis N — a parked core that owns sockets, and what it took to wake it

§8.2 closed on the sentence every framework in this repository shares: once an actor thread
really sleeps, it pays what the OS charges to wake it, and the differences are in how long each
one refuses to. That sentence hid an assumption. It was measured on ping-pong, where the thing
that wakes a parked core is another core's push — and a qb core parked in `Mailbox::wait()` was
woken by exactly that and by nothing else. A qb core that also owns io watchers — a listening
socket, a session, a `qb::io::async::callback` timer — parked in the same condition variable, and
while it slept nothing polled its loop. A socket that became readable a microsecond into the park
waited for the park's timeout: the core's `latency`, which is the one knob every parked qb server
sets, and which §2 tuned for ping-pong cadence without ever asking what it did to io.

### 10.1 The instrument

`tools/probes/parked-io-wake.cpp` (`qvoprobe-parked-io-wake`) is a qb-only probe, built under
`QVO_BUILD_PROBES` and named so that `tools/run.py` cannot discover it: the question has no
counterpart in CAF or SObjectizer, whose schedulers own no sockets, so its figure must never sit
in a table beside theirs. One `VirtualCore` hosts an echo actor (`qb::Actor` +
`use<>::tcp::server`) with the cell's `latency`; a raw blocking-socket client on another thread
sleeps `gap` — longer than the 50 µs idle-spin floor, so the core has parked when the request
lands — sends one line, and times the round trip. Twelve cells, `latency` ∈ {0, 100, 1000,
10000} µs × `gap` ∈ {10, 200, 2000} µs, 2000 rounds after 50 warm-ups, control and candidate
interleaved twice per cell. The `latency=0` column is the busy-poll floor (the core never parks)
and the `gap=10us` row keeps the core inside its idle-spin floor (it polls); every other cell
parks. Control is qb `develop` at `d1897d3c`, the tree axis N branches from; candidate is
`perf/park-in-ev-loop`.

**The probe pins its own two threads, and the difference between that and a process mask is the
whole control.** With only an affinity mask of {0, 2}, Windows placed the client on the core's
CPU for a per-launch random fraction of rounds; a client spinning its gap on the core's CPU keeps
the core from ever seeing its idle floor elapse, so it never parks and the control's `latency=1000
gap=200` cell read p50 19–20 µs with a p90 anywhere from 23 µs to 1.8 ms between launches — a
defect measured as absent. Under WSL2 the load balancer spread the two busy threads and the
unpinned p50s agreed with the pinned ones everywhere (914 / 9966 → 30.5 / 31.8 µs on the headline
row), but its `latency=0` cells carried a p99 of 1.2–3.1 ms on both sides that the first reading
attributed to the hypervisor. It was the same trap in the tails: pinned, the same cells' p99 is
42–150 µs. The unpinned log is kept beside the pinned one
(`results/wsl-debian-g++14/qb-branch-perf-park-in-ev-loop/parked-io-wake.unpinned.log`) for
exactly that lesson. The pinning is `SetThreadAffinityMask` / `pthread_setaffinity_np` for the
client and `core.setAffinity()` for the core; macOS has neither, and a run there is unpinned by
construction — its figures, when they exist, carry that caveat.

### 10.2 The finding

p50 of the round trip in µs, control → candidate, both passes; the `latency=0` column and the
`gap=10us` row are the floors and move nowhere.

**WSL2 Debian 13 / g++ 14.2** (`taskset -c 0,2`, core on vCPU 0, client on vCPU 2, 2026-09-06
03:09–03:22 UTC):

| `latency` \ `gap` | 10 µs (polling) | 200 µs (parked) | 2000 µs (parked, really asleep) |
|---|---:|---:|---:|
| 0 (never parks) | 18.5 / 18.3 → 18.1 / 18.1 | 19.1 / 19.0 → 19.3 / 17.8 | 26.6 / 27.4 → 24.9 / 25.0 |
| 100 µs | 17.9 / 17.8 → 18.4 / 18.0 | 23.2 / 29.2 → 31.2 / 31.2 | **100.2 / 99.1 → 46.1 / 45.3** |
| 1 ms | 17.4 / 17.3 → 17.3 / 17.3 | **923.3 / 910.8 → 30.7 / 31.2** | **169.7 / 171.4 → 46.7 / 43.5** |
| 10 ms | 16.9 / 17.5 → 17.4 / 18.1 | **9960.8 / 9953.7 → 31.0 / 33.7** | **8154.2 / 8154.2 → 51.0 / 47.6** |

**Windows 11 / MSVC 19.51** (process mask {0, 2}, core on CPU 0, client on CPU 2, 03:02–03:27
UTC):

| `latency` \ `gap` | 10 µs (polling) | 200 µs (parked) | 2000 µs (parked, really asleep) |
|---|---:|---:|---:|
| 0 (never parks) | 19.7 / 19.7 → 19.6 / 19.7 | 19.5 / 20.6 → 19.3 / 19.3 | 28.4 / 28.1 → 28.7 / 29.1 |
| 100 µs | 18.8 / 18.9 → 18.7 / 19.0 | **1220.6 / 898.3 → 26.3 / 24.9** | **2112.4 / 2050.0 → 62.4 / 62.5** |
| 1 ms | 19.9 / 20.1 → 19.6 / 19.1 | **2215.4 / 2787.5 → 26.7 / 24.0** | **11 620.8 / 10 544.9 → 62.3 / 68.7** |
| 10 ms | 18.3 / 18.8 → 18.8 / 18.4 | **15 549.9 / 15 566.5 → 24.4 / 26.9** | **13 774.2 / 13 779.5 → 134.9 / 133.9** |

Three things to read off them:

- **The control answers io at the timeout, and on Windows the timeout is the scheduler tick.**
  Under WSL2 the `gap=200us` column is `latency` minus a little (923 µs at 1 ms, 9.96 ms at
  10 ms): the park began before the request, and the request waited for the rest of it. On
  Windows `std::condition_variable::wait_for` lands on the 15.6 ms tick that §5 measured, so
  even `latency=100us` costs 0.9–1.2 ms and `latency=10ms` costs 15.6 — a qb server tuned with
  a small `latency` for responsiveness was, on Windows, no more responsive to its sockets than
  one tuned with a large one. The `gap=2000us` column shows the same clock from the other side:
  the request lands inside a 2 ms gap, and what it pays is however much of the park's timeout
  was left. Neither is a defect in the OS; both are a core that was asked to sleep on the wrong
  primitive.
- **The candidate answers at poll latency plus one wake.** 31 µs on WSL2 and 24–27 µs on Windows
  at `gap=200us`, on all three latencies — the 18–20 µs busy-poll floor plus the cost of leaving
  `ev_run`'s backend poll. `latency` no longer appears in the figure; it only bounds the park.
- **A thread that really slept still pays the OS.** At `gap=2000us` the candidate reads
  46–51 µs on WSL2 and 62 µs on Windows at latency 100 µs / 1 ms, 134 µs at 10 ms: the deeper the
  cap let the thread sleep, the longer the OS takes to bring it back, and on Windows a 10 ms
  cap visibly parks the thread in a deeper state than a 1 ms one. That is §8.2's floor, unchanged
  and untouched by axis N, whose whole claim is that io now ends the sleep at all.

The `gap=10us` row is not quite as flat as its p50s: in the control, the p99 of the polling cells
IS the latency (WSL2: 713 / 1078 µs at 1 ms, 10 119 / 10 009 µs at 10 ms; Windows: 260 / 787 µs
at 1 ms, 11.6 / 12.0 ms max at 10 ms) — once in a hundred rounds the client's 10 µs gap plus a
scheduling hiccup let the core reach its 50 µs floor, and the next request then paid the full
park. The candidate's p99 on the same cells is 48–78 µs. A busy server was not immune to the
defect; it merely met it less often.

### 10.3 The mechanism, and what it costs where it does not apply

The candidate changes where a core parks, not whether. In `VirtualCore::__workflow__`, once the
idle-spin floor has elapsed, `listener::has_work()` selects the path: a core whose loop has
nothing to deliver parks in the condition variable exactly as shipped; a core with io watchers
parks in `Mailbox::wait(listener &)` → `listener::run_once_for(latency)`, which is
`ev_run(EVRUN_ONCE)` blocking in the backend poll under a one-shot cap timer armed at `latency`
(the loop clock is refreshed first — a timer scheduled against libev's stale `mn_now` expires
early or at once — and the cap is stopped by a guard on every exit, a throw out of a handler
included). A turn that already has something to run — a deferred callback, a ready coroutine, an
event fed to the loop since its last pass — does not block, because `ev_run` computes its wait
from watcher deadlines alone and would sleep on top of ready work. Io delivered by the park counts
as activity: the idle stamp is cleared, so the reply and the request after it are met at polling
latency rather than by another park.

A producer on another core ends the loop park through libev's one thread-safe entry point,
`ev_async_send`, on an `ev::async` the listener arms lazily on its first loop park and `unref()`s
so it never counts as work; the mailbox's `_parked` flag became a tri-state (`None` / `Cv` /
`Loop`), and `notify()` — still one fence and one load on the common path — takes the mutex only
on an announced park and then re-reads WHICH park under it, waking the loop or the cv. The
listener pointer that `wake()` dereferences is published by `attach_loop()` before the start
barrier and withdrawn by `detach_loop()` under the same mutex on every exit path, so a producer
holding the mutex sees a live listener or none. ThreadSanitizer found the one ordering the first
draft lacked, on the first park of `core-park-wake`: the `Park::Loop` announce must be a release
store and the producer's re-read an acquire, or the `eventfd()` that `arm_wake()` creates on the
core's thread is unordered against the producer's first write to it. The embedded qev profile
gains the async family for this (`QB_EV_ASYNC_ENABLE 1` in qb's copy of the loop; the standalone
already ships it): three exported symbols, 24 bytes.

**Where the path does not apply it must cost nothing, and the ping-pong A/B is that proof**:
ping-pong carries no io watcher, so on this benchmark the candidate's parked core takes exactly
the control's cv branch through the new tri-state flag and the acquire re-read. Per-rep p50 in
ns per round trip, control → candidate, three interleaved reps per cell
(`results/<host>/qb-branch-perf-park-in-ev-loop/ab/`):

| cell | WSL2 g++ 14.2 | Windows / MSVC 19.51 |
|---|---:|---:|
| 1c-w1 (one core, spin) | 66.6 / 67.2 / 67.1 → 67.3 / 67.9 / 67.5 | 84.2 / 79.7 / 81.2 → 80.8 / 80.3 / 80.5 |
| 2c-w0 (park, default floor) | 230.0 / 228.9 / 216.1 → 236.8 / 219.3 / 217.9 | 292.0 / 286.2 / 304.8 → 274.2 / 294.7 / 270.0 |
| 2c-w1 (spin) | 238.0 / 223.2 / 211.1 → 219.4 / 238.8 / 221.8 | 351.1 / 309.8 / 275.4 → 270.3 / 330.4 / 249.3 |
| 2c-w0, idle-spin floor 0 (§8.2) | 27 332 / 27 624 / 26 910 → 27 357 / 27 350 / 27 331 | 390.3 / 463.8 / 355.8 → 393.1 / 421.1 / 422.0 |

No cell moved outside its own rep-to-rep spread on either host. The WSL2 one-core cell's
+0.4 ns (0.6 %) is below the 1 ns this benchmark resolves and is the `has_work()` gate evaluated
once per idle pass on a core that never parks; the §8.2 floor-0 cells read the same OS wake on
both sides, which is the statement that axis N did not touch it.

### 10.4 What was validated before the figures were quoted

WSL2, qb standalone from the branch: release, ASan+UBSan and TSan each **191 / 191 executed, 0
skipped** — the shipped suite plus `core-park-policy` (past the spin floor the core parks inside
its loop, an io timer on that core fires at its own delay, a loop park is capped by `latency` and
sees `Main::stop()`) and `core-park-wake` (a readable socket ends a loop park, a cross-core push
ends both a loop park and a cv park, each in far less than `latency`, with the process's CPU time
proving the core was parked rather than polling). The TSan run is the one that found the
release/acquire pair above; it is clean with it. The Windows/MSVC pass is
`dev/agent/verify-windows.ps1` over the five presets at their recorded floors, and the macOS
pass is the same agent protocol as QB-44's — both are recorded in the Huly issue (QB-42) as they
land, not here in advance.

**What axis N does not change, stated so nobody reads it as more than it is.** A qb core with no
io watchers parks exactly as before, and pays exactly what §8.2 measured once it sleeps. A core
WITH io watchers that really sleeps still pays the OS wake — 46–51 µs under WSL2, 62–135 µs on
Windows — because the socket wakes the thread through the same kernel path a futex or a
`WaitOnAddress` would; what it no longer pays is the park's own timeout on top. And a spinning
core (`latency = 0`) was never affected, which is why the audits of §7 and §9, all measured
with spin and park at ping-pong cadence, never saw it: the defect lived only in the one
configuration that both parks and serves.

To re-run: build with `-DQVO_BUILD_PROBES=ON` (the default when qb is enabled) against a control
tree and a candidate tree, then for each of the 12 cells launch `qvoprobe-parked-io-wake
<latency_us> <gap_us> 2000` from each build, interleaved, on a quiet host with nothing else
running; the probe pins itself, so no `taskset` or `start /affinity` is needed beyond keeping the
rest of the machine off CPUs 0 and 2.

The Windows `gap = 2000 µs` figures of this section are one session's reading of a wake that
turned out to be bimodal — §19 re-measures the cell as an A/B against this very tree and explains
what moves it.

## 11. What two shapes that CREATE actors said — fib and chameneos

Every shape through §10 builds its actors before the window opens. §7's audit therefore never
had an actor's LIFETIME on a profile: the registry, the five default subscriptions each
`Actor::Actor()` makes, the kill path. `savina/fib` puts 57 312 lifetimes inside one window and
nothing else (`benchmarks/savina/fib.md`); `savina/chameneos` puts 200 000 meetings through one
broker actor (`benchmarks/savina/chameneos.md`). Both were written on 2026-09-06, both hosts,
9 repetitions + 2 warmup for the branch grids and the host protocol for the shipped-3.1.0 cells
that joined the published tables the same evening (`results/<host>/savina-fib/`,
`savina-chameneos/`), the six sessions never overlapping
(`results/<host>/qb-branch-perf-dense-table-growth/`).

### 11.1 fib's first run: 43 seconds

qb `develop` (`00b60afe`, every axis through N) measured fib at **43.2 s** per repetition on
Windows/MSVC against CAF's 86 ms — three orders of magnitude, super-linear in the live count.
The dense router of axis I grew its `key_table` with `reserve(max(idx + 1, size() * 2))` on
each insert: one element past the previous doubling, so every subscription of a NEW id copied
the whole table — O(n) per `registerEvent`, O(n²) per core, one table per event type, five
default subscriptions per actor. Invisible to every static-topology test and benchmark, whose
tables reach their size once and stay there. Unreleased: 3.1.0 has no dense table.

That is the whole argument for writing the other eighteen shapes. §7 and §9 measured the
dispatch path forty ways and could not have seen this, because nothing they run creates an
actor after start-up.

### 11.2 The chain, 2c-spin p50 (WSL2 g++-14 / Windows MSVC)

| qb commit | fib | what the profile showed next |
|---|---|---|
| shipped **3.1.0** | **159 / 459 ms** | no dense table — the LOGGER: 9 `LOG_INFO` lines per actor lifetime at the shipped `QB_WITH_LOGGING=ON` default (`registerEvent` × 7, `New`, `Delete`), 515 819 lines and 60.9 MB per repetition, formatted on the actor's core inside the window; see 11.5 |
| `develop` `00b60afe` | 43.2 s (Windows) | the O(n²) table growth of 11.1 |
| `5665b6f8` | 178 ms → 28 ms (Windows) | growth by capacity; then every per-lifecycle `LOG_INFO` line (the nine above) demoted to VERBOSE |
| `d44e7b44` | **8.49 / 12.13 ms** | `ActorMap` a hash map keyed by what is already a per-core slot; the kill queue a hash set; a `dynamic_cast` per subscription |
| `001be013` | **7.08 / 11.09 ms** | `active_coroutines_` `make_shared`'d in every constructor — ~30 % of a non-spawning actor's lifetime |

Field at `001be013`, fib 2c-spin: floor 3.3 / 4.0, **qb 7.1 / 11.1**, CAF 38.6 / 52.5,
SObjectizer 328 / 188 ms. One core: floor 1.7 / 3.7, **qb 11.4 / 17.6**, CAF 68.5 / 84.0,
SObjectizer 148 / 154. Per actor lifetime on one core that is **200 ns** for qb against 30 for a
malloc-and-free node, 1.2 µs for CAF and 2.6 µs for SObjectizer.

Against shipped 3.1.0 in the SAME session (the pair FAIRNESS.md asks for —
`grid-001be013-session2/` beside `savina-fib/`, 18:13 UTC on WSL2 and 18:14–18:16 on Windows):
fib 2c-spin **159 → 7.6 ms** (21×) and 1c-spin **205 → 11.9** (17×) on WSL2; **459 → 10.5**
(44×) and **558 → 17.2** (32×) on Windows. Chameneos, which creates 101 actors and logs nothing
per meeting, moves 15.3 → 11.1 / 12.0 → 6.5 (WSL2) and 20.8 → 12.8 / 13.2 → 7.6 (Windows) —
the §7–§10 dispatch work and the registry landing on a broker's mailbox.

Two placements in that table are not what a reader expects. qb's two-core cell is its WORST
placement — a child lives on its parent's core, so fib(22) and fib(21) run as two unbalanced
sub-trees (62 / 38) while CAF and SObjectizer steal — and it still leads. SObjectizer is slower
on two cores than on one on both hosts (328 vs 148, 188 vs 154): each node is its own
cooperation, registered and deregistered through the environment, and two work threads contend
on that path.

### 11.3 What is left in an actor's lifetime, and what is not this branch's

`perf --call-graph dwarf` on fib 1c at `001be013`: the actor object's own `new`/`delete` is the
last heap traffic on the path. The rest is the router — `registerEvent` × 7 per actor
(5 default + 2 of the benchmark's) ≈ 29 %, `unregisterEvents` ≈ 12 % because it walks EVERY
resolver on the core asking each to forget an id it mostly never held, `removeActor` 23 %
inclusive. The five default events are five `dense::key_table`s of 65 536 × 32 B — 10 MiB per
core whose key set is exactly "the live actors", which `ActorMap` already is. Routing them
through the registry, and making `unregisterEvents` visit only the resolvers an actor is in, is
Huly QB-174: ~28 % of what remains, against a floor that is 3.5× away. The actor object's slab,
the 16-bit slot ceiling that keeps fib at n=23 (n=24 needs 92 735 live on one core) and the
absence of a cross-core spawn are QB-175, a 4.0 conversation.

### 11.4 Chameneos: nothing moved, and that is the finding

qb 11.2 / 11.8 ms at 2c-spin, 6.8 / 7.4 at 1c-spin, on every commit of the branch — inside its
own spread. It creates 101 actors and its cost is the broker's mailbox: 200 000 meetings, each a
push to the broker and a push back, exactly the counting shape of §9 with state. CAF 129 / 215,
SObjectizer 154 / 248. The floor is the shape where a mutex-and-condvar broker sits ABOVE qb at
two cores (29.8 / 68.6 ms — a core crossing per meeting) and below everyone at one (2.9 / 5.7).

### 11.5 What 3.1.0's fib cell is, and what the benchmark said about its own caveat

The shipped cell was measured last, after the branch chain, because the first attempt to run
it looked like a hang: under `tools/run.py` the 2c-park cell had not returned after eight
minutes. gdb said otherwise — main thread in `exit()` → `~unique_ptr<NanoLogger>` →
`thread::join`, the writer in `NanoLogger::pop()` → `flush()` → `write()` inside
`p9_client_rpc`. 3.1.0 at its shipped default (`QB_WITH_LOGGING=ON`, INFO in a release build,
`qb/cmake/qbConfig.cmake`) logs nine lines per actor lifetime: `[registerEvent] Actor(c.i)
subscribed to <E>` for the five default events and the benchmark's two, `[appendActor] New
Actor[...]`, `[removeActor] Delete Actor[...]` — **515 819 lines, 60.9 MB of `qb.1.log` per
repetition**, counted. On an ext4 cwd the run is 522 ms wall for one repetition; on the
9p-mounted checkout nanolog's per-line flush makes every `write()` a round trip to the Windows
host and the EXIT of a one-repetition run takes minutes. `5665b6f8` demotes all nine to VERBOSE.
That is a user-visible 3.1.0 characteristic and it is recorded as such: the published cell is
the qb a 3.1.0 user builds, and the 21× / 44× above is what 3.2.0 changes for a program that
creates actors — most of it the logger, the rest the registry.

The benchmark also caught its own caveat lying. Every qb document carried "its logger writes at
startup, outside the measured window", written when the five shapes all built their actors
before the window and true of every one of them. It is false for fib on 3.1.0, and the
measurement is what said so. The sixteen shipped documents keep the sentence as the provenance
of their run; `frameworks/qb/qb_support.h` now says what is true of every shape — whatever qb
logs at INFO inside the window is part of the cost, the writer thread shares the pinned set,
and the option stays ON because no other framework gets an equivalent subtraction.

## 12. What the first shape that WAITS said — bank-transaction

Every shape through §11 pushes and forgets. `savina/bank-transaction`
(`benchmarks/savina/bank-transaction.md`) is one teller, a thousand accounts and 50 000
transfers, each a request nested inside a request: the source account `co_await`s a
`qb::ask<Deposit>` to the destination and acknowledges the teller when the reply lands. It is the
first benchmark to sit on the coroutine request path — `ScopedCoroContext`, `ask_awaiter`, the
cancellation token, `reply()` — and its one-core `perf` profile on qb `develop` (`9d4aa94c`,
every axis of §7–§11 in) read as a list of things that should not be on a request path. Written
2026-09-06, the five fixes landed on `develop` as one commit (`fa1c5ce3`, Huly QB-42), and the
field, shipped 3.1.0 and the two `develop` builds were measured in ONE quiet session per host on
2026-09-07 (WSL2 02:21 UTC, 5 + 1; Windows 02:24 UTC, 9 + 2, each host idle while the other ran):
`results/<host>/savina-bank-transaction/` and
`results/<host>/qb-branch-perf-coro-scope-local-refcount/`.

### 12.1 The five, and the sixth they exposed

| # | what the profile showed (1c spin, g++-14, `9d4aa94c`) | what changed |
|---|---|---|
| 1 | `ScopedCoroContext` copied a `shared_ptr` on every ask and every spawn — **29 % of the core, 24 % on the single `lock xadd` of `_M_release`**, on a state its own contract calls single-threaded | the cancellation token's state carries an intrusive non-atomic `refs`; the actor's coroutine census is `detail::coro_census {active, refs}` behind `coro_census_ref` — zero atomics on a counter only the owning core's thread touches |
| 2 | `ask_awaiter` registered through `on_cancel`: a `std::function` + vector push per ask, a linear `remove_on_cancel` per completion | `cancellation_token::cancel_hook` + `token.link(hook)`: an intrusive, allocation-free node the awaiter owns, unlinked on completion |
| 3 | the by-value `qb::ask(ctx, dst, E req, timeout)` built a 64-byte temporary, moved it three times, and read header fields back with 16-byte loads over the narrow stores that had just written them — `Account::start`'s frame was **21.7 % of the core** | the emplace form `qb::ask<E>(ctx, dst, timeout, args...)` (and `ask_by<E>`) constructs the request IN the pipe slot; `CoroContext::push` / `push_to` return the built `E&` so the correlation id is set in place. The frame left the top of the profile |
| 4 | `reply()` / `forward()` wrote `dest` / `source` / `alive` with narrow stores and the copy that followed loaded the header wide — libc `memmove` **4.3 % of the core**, all of it that store-forwarding stall | `detail::event_wire` composes the 16-byte header in one register (SSE2 / NEON / two words) and stores it once; `copy` reads back exactly those bytes and clears `alive` in the register |
| 5 | `__receive_events__` and the activation pump stored `alive = 0` into every arriving event before routing it — **70 % of `deliver_thunk`'s first-header-load time** | the bit is already 0 in the bytes every transport carries; the store is gone |
| 6 | removing 5 exposed what it had been masking: a cross-core `forward()` of an event already `reply()`ed relocated ORIGINAL bytes carrying `alive = 1`, and the receiver skipped its destructor — **a leak per relay**, shipped | `VirtualCore::send(Event const&)` refuses to relocate a raised flag (the local pipe's `event_wire::copy` clears it), and `reply()` / `forward()` raise the original only AFTER the copy is taken; `RelayChain.*` pins both polarities |

Tests added with them: `ActorCoroutineAsk.EmplaceAsk*` (6), `CancellationToken.Hook*` /
`LinkOn*` (9), `EventWire.*` (12), `RelayChain.*` (5); standalone `cmake -S qb` suites on
Linux/g++-14 Release / ASan+UBSan / TSan 192/192/0 each, Windows/MSVC Release 188/188/0, 0
warnings.

### 12.2 The chain, p50 ms (1c spin / 1c park / 2c spin / 2c park)

| qb | WSL2 g++-14 | Windows MSVC |
|---|---|---|
| shipped **3.1.0** | 14.71 / 14.58 / 9.21 / 9.31 | 25.48 / 25.45 / 29.20 / 33.45 |
| shipped 3.1.0, `QB_WITH_LOGGING=OFF` | 15.80 / 16.28 / 8.99 / 9.75 | — |
| `develop` `9d4aa94c` — the base | 9.40 / 9.44 / 5.09 / 5.02 | 13.65 / 13.86 / 7.97 / 7.78 |
| `develop` `fa1c5ce3` — the five | **8.06 / 8.80 / 4.56 / 4.77** | **12.91 / 12.82 / 7.57 / 7.56** |

Field in the same sessions: floor 1.10 / 2.59 / 6.30 / 4.63 and 3.47 / 3.92 / 32.2 / 7.25; CAF
41.5 / 43.0 / 36.6 / 36.6 and 57.8 / 58.0 / 57.5 / 58.1; SObjectizer 19.5 / 20.5 / 25.5 / 27.7
and 29.6 / 31.5 / 38.0 / 44.4. Per transfer on one core, spin: **qb 161 ns** at `fa1c5ce3`
(294 at 3.1.0), SObjectizer 391, CAF 830, the floor 22.

Three readings. The five fixes alone are **−14 % / −10 %** (1c / 2c spin) on g++ and **−5 % /
−5 %** on MSVC — the profile that found them was taken on Linux and no Windows profile was, so
why MSVC gains less is recorded, not explained. 3.1.0 → base is the larger step on both hosts
(1.6× / 1.8× on WSL2, 1.9× / 3.7× on Windows): §7–§11 landing on a request path for the first
time, and the one place it was measured before the fixes. The 1c-park candidate cell on WSL2 is
the noisy one of the twelve (7.96–9.39, IQR 0.63); other launches of the same binary in that
session read 7.54–8.08.

### 12.3 Two things the controls said

**The control in the commit message was mislabelled, and the same-session pair is what caught
it.** `fa1c5ce3`'s message and qb's `[Unreleased]` quoted "shipped 3.1.0" at 9.74 / 9.60 ms (1c
spin / park). Shipped 3.1.0 measures 14.7 in the same session, three times over; 9.4 is
`9d4aa94c`, the commit the work branched from — which is what that "shipped" build had been all
along. It changes nothing about the five (base → candidate is the delta they own) and it is why
the bank page presents three controls rather than one, and why the CHANGELOG entry names the base
by SHA. The rule this repository already had — shipped and candidate in the same quiet session,
never a remembered number — is the rule that found it.

**3.1.0's figure is not a logging figure here, unlike fib's.** A second 3.1.0 built from the same
tree with `-DQB_WITH_LOGGING=OFF` measures 15.80 / 16.28 / 8.99 / 9.75 in the same session —
no faster — and its `qb.1.log` is 1.5 KB against ~10 000 lines per repetition with logging on,
every one of them the thousand accounts' `registerEvent` / `New` / `Delete`, written before the
window opens and after it closes. What 3.1.0 pays on this shape is the request path §12.1 lists,
plus what §7–§11 took: the shipped profile has `_Sp_counted_base::_M_release` at 4.8 %, the
by-value ask frame at 3.75 %, `malloc` at 2.8 % and `ask_awaiter` at 2.4 % of the core.

### 12.4 What is left, and the Windows 2c cells

`perf` on `fa1c5ce3`, 1c spin: the ask registry — a hash map keyed by correlation id,
inserted on every ask and erased on every reply — was **≈10–12 % of the core** and the largest
single item. It was recorded as the follow-up (a slot table keyed by what is already a per-core
counter, the shape §11.3 gave `ActorMap`), not done in the commit. **Done since, as QB-178**
(`perf/ask-slot-table`, two commits over `develop` `1ec7e794`; the A/B documents are
`results/<host>/qb-branch-perf-ask-slot-table/`, each README with its chain), and the branch is
a lesson in what a profile percentage buys. `8f28e7bf` made the registry a slot table the
correlation id itself indexes — `[core:16][generation:26][slot:22]`, a FIFO free list, nothing
hashed — and the registry fell 12.8 → 8.4 % of the core (`perf record -F 20000 -g`, 1c spin,
30 repetitions, registry symbols only) while bank-transaction moved 1–4 %, less than the 4.4
points lost. `perf annotate` on the remaining `ask_unregister` said why: 4.69 % spread over ~10
instructions, 15 % of it on one `jne`, 9.5 % on the epilogue, 7.5 % on the prologue `push`, no
miss signature anywhere. That is the cost of the CALL — a `thread_local` with a destructor is
a `__tls_init` guard check per access on g++ — and the path paid it five times per ask:
`ask_next_id`, `ask_register`, `ask_deliver`, `ask_unregister` from `await_resume` and
`ask_unregister` again from the awaiter's destructor. `a61bded8` makes it three (`ask_take(owner,
slot)` pops and binds in one call, `finish()` keeps a byte so the destructor's pass is a compare,
the awaiter takes its entry in its constructor before the send so the throw-window guard is
deleted): registry **4.36 %**, and the 1c gain arrives — WSL2 same-session p50, control →
candidate / candidate pass 2: 1c-spin 7.70 → 7.21 / 7.25 ms (**−6.3 / −5.8 %**), 1c-park
7.58 → 7.30 / 7.24 (**−3.7 / −4.4 %**), 2c inside its spread (the hop, not the registry,
owns the cross-core cell). Per transfer on one core, spin: 154 → 144 ns, ≈ 9–10 ns per ask —
which is 1 % of a ≈ 880 ns `dev/bench` round trip, so those cells stay flat (±1 %) and the
savina shape is the one that sees it. **On MSVC the branch is inside the spread on every cell**
(1c-spin 12.22 → 11.99 / 12.07 ms with the control's minimum the lowest of the three; round
trips +1.5 %): MSVC runs dynamic TLS initialisation once at thread start and has no per-access
guard, so three calls fewer buy correspondingly less — a reading from the g++ profile, not a
Windows measurement, since no Windows profile is in this protocol.

On Windows, three of the `2c` cells that cross a core per transfer are WIDE: the floor's 2c-spin
spans 20.0–62.2 ms over nine repetitions (IQR 29.5), shipped qb's 2c-spin 24.3–49.6 (IQR 13.0)
and its 2c-park 19.2–42.1 (IQR 18.5), while every 1c cell and both `develop` builds' 2c cells sit
inside a tenth of that (`fa1c5ce3`: 7.57 / 7.56, IQR 0.2). The report's bimodality rule does not
fire on them — no gap of 2× between clusters — so they render as medians, and the page quotes
them with their spread. What makes a busy-polling ring floor and a 3.1.0 that crosses a core per
transfer spread 3× on MSVC where the `develop` builds do not is not measured; it is the same
host, the same session and the same CPU set.

---

## 13. The 3.2.0 candidate grid — eight shapes, two hosts, one session each

Everything §7–§12 produced is one branch now: qb `develop` at **`43f62afe`**, 29 commits over
the shipped v3.1.0 (`830ea244`) — axes A–N (§7, §10), the segmented pipe (§9.11, QB-43), the
dense router (axes I / M), the default-event registry (QB-174), the dense-table growth chain
fib produced (§11), the five ask-path fixes bank-transaction produced and the ask slot table
(§12, QB-178). On 2026-09-07 it was measured through the unmodified adapters on **all eight
Savina shapes**, candidate / shipped 3.1.0 / candidate, **9 repetitions + 2 warmup**, in one
quiet session per host with the other host idle: Windows / MSVC 19.51 08:21:40–08:26:18 UTC
(`build/final` rebuilt at `43f62afe` against `build/shipped-win`), WSL2 / g++-14 08:45:23–
08:54:36 UTC (`~/qvo/linux` against `~/qvo/shipped`, both on ext4 — shipped 3.1.0's fib
writes 61 MB of log per repetition, §11.5, which is 8 min 40 s of that session). The three
grids per host are `results/<host>/qb-branch-develop/`, each 32 / 32 verified; README.md's
two `framework=qb` grids render `grid-43f62afe/` and `check-report.py` holds them to the
JSON. The field columns below are the published `savina-*/` cells of each host (their own
sessions, 2026-09-04 / -06 / -07); "× floor" is the candidate over the raw-thread floor, "×
best other" is the better of CAF and SObjectizer over the candidate.

| shape (per unit) | config | Windows: 3.1.0 → `43f62afe` | WSL2: 3.1.0 → `43f62afe` | × floor (W / L) | × best other (W / L) |
|---|---|---|---|---|---|
| `ping-pong` (round trip) | 1c-spin | 117.4 → **84.2** (-28 %) | 100.3 → **66.0** (-34 %) | 49 / 40 | 2.2 / 2.2 |
|  | 1c-park | 118.7 → **83.4** (-30 %) | 99.8 → **66.4** (-33 %) | 49 / 40 | 2.6 / 2.5 |
|  | 2c-spin | 373.9 → **297.2** (-21 %) | 290.9 → **222.0** (-24 %) | 1.63 / 1.06 | 1.6 / 1.3 |
|  | 2c-park | 7,793.0 → **274.5** (-96 %) | 29,026.5 → **227.0** (-99 %) | 0.59 / 0.01 | 1.8 / 1.3 |
| `counting` (message) | 1c-spin | 31.7 → **10.0** (-69 %) | 43.6 → **8.7** (-80 %) | 3.14 / 3.11 | 14.0 / 12.5 |
|  | 1c-park | 31.7 → **9.5** (-70 %) | 43.9 → **8.5** (-81 %) | 1.22 / 1.31 | 15.4 / 12.9 |
|  | 2c-spin | 34.3 → **11.7** (-66 %) | 47.9 → **11.0** (-77 %) | 0.28 / 0.46 | 10.8 / 14.9 |
|  | 2c-park | 34.2 → **11.7** (-66 %) | 48.4 → **11.0** (-77 %) | 0.22 / 0.18 | 12.8 / 15.2 |
| `thread-ring` (hop) | 1c-spin | 65.0 → **45.4** (-30 %) | 55.1 → **39.3** (-29 %) | 5.35 / 14 | 2.0 / 1.8 |
|  | 1c-park | 63.3 → **45.0** (-29 %) | 55.8 → **38.7** (-31 %) | 4.44 / 3.01 | 2.4 / 2.1 |
|  | 2c-spin | 209.2 → **154.2** (-26 %) | 169.1 → **112.3** (-34 %) | 1.40 / 0.98 | 1.5 / 1.3 |
|  | 2c-park | 2,621.0 → **146.4** (-94 %) | 14,541.5 → **120.7** (-99 %) | 0.54 / 0.01 | 1.6 / 1.2 |
| `fork-join` (message) | 1c-spin | 43.6 → **10.5** (-76 %) | 62.3 → **9.5** (-85 %) | 3.15 / 2.92 | 13.3 / 10.6 |
|  | 1c-park | 42.9 → **10.5** (-75 %) | 61.8 → **9.6** (-85 %) | 1.44 / 1.36 | 14.3 / 11.2 |
|  | 2c-spin | 46.1 → **12.2** (-74 %) | 51.7 → **9.6** (-81 %) | 0.32 / 0.34 | 14.0 / 16.6 |
|  | 2c-park | 44.5 → **11.2** (-75 %) | 50.9 → **9.9** (-81 %) | 0.23 / 0.23 | 15.0 / 17.2 |
| `big` (round trip) | 1c-spin | 37.1 → **20.3** (-45 %) | 33.9 → **21.4** (-37 %) | 1.80 / 2.97 | 9.2 / 6.2 |
|  | 1c-park | 36.0 → **20.0** (-44 %) | 33.2 → **22.7** (-32 %) | 1.03 / 1.64 | 10.0 / 6.3 |
|  | 2c-spin | 32.6 → **24.8** (-24 %) | 28.8 → **21.7** (-25 %) | 0.57 / 0.85 | 12.4 / 10.5 |
|  | 2c-park | 31.8 → **23.9** (-25 %) | 29.9 → **21.9** (-27 %) | 0.39 / 0.38 | 12.9 / 11.5 |
| `fib` (actor) | 1c-spin | 8,826.2 → **198.8** (-98 %) | 3,388.3 → **133.3** (-96 %) | 3.07 / 4.45 | 7.5 / 9.0 |
|  | 1c-park | 8,868.4 → **197.6** (-98 %) | 3,420.4 → **135.3** (-96 %) | 2.73 / 2.54 | 7.5 / 9.0 |
|  | 2c-spin | 8,175.1 → **117.2** (-99 %) | 2,598.8 → **90.4** (-97 %) | 1.67 / 1.52 | 7.8 / 7.5 |
|  | 2c-park | 8,251.8 → **116.5** (-99 %) | 2,413.5 → **89.8** (-96 %) | 1.65 / 1.54 | 7.8 / 7.5 |
| `chameneos` (meeting) | 1c-spin | 66.9 → **35.3** (-47 %) | 59.4 → **35.3** (-41 %) | 1.06 / 2.40 | 10.7 / 7.6 |
|  | 1c-park | 66.1 → **34.9** (-47 %) | 65.4 → **33.3** (-49 %) | 0.76 / 1.20 | 11.6 / 8.9 |
|  | 2c-spin | 100.3 → **65.3** (-35 %) | 72.2 → **54.3** (-25 %) | 0.21 / 0.38 | 16.6 / 11.6 |
|  | 2c-park | 105.2 → **63.5** (-40 %) | 78.5 → **53.8** (-31 %) | 0.17 / 0.15 | 16.8 / 11.9 |
| `bank-transaction` (transfer) | 1c-spin | 529.8 → **263.2** (-50 %) | 283.6 → **143.5** (-49 %) | 3.80 / 6.50 | 2.2 / 2.7 |
|  | 1c-park | 539.6 → **272.5** (-50 %) | 278.7 → **145.7** (-48 %) | 3.48 / 2.81 | 2.3 / 2.8 |
|  | 2c-spin | 749.6 → **156.6** (-79 %) | 179.3 → **92.8** (-48 %) | 0.24 / 0.74 | 4.9 / 5.5 |
|  | 2c-park | 701.4 → **149.6** (-79 %) | 177.5 → **96.4** (-46 %) | 1.03 / 1.04 | 5.9 / 5.7 |

### 13.1 What the two hosts agree on

**Every one of the 64 cells is faster than shipped 3.1.0, and none by less than the spread.**
The smallest deltas are the cross-core spin cells (−21 / −24 % ping-pong, −26 / −34 % ring);
everything else is −28 % or more. Both `2c-park` collapses (§5) are gone on both hosts, and on
WSL2 they are gone under a hypervisor whose futex wake alone is 12 µs: the parked candidate
answers a cross-core round trip in 274 / 227 ns where 3.1.0 took 7.79 / 29.03 µs, because a
parked core no longer sleeps outside its loop (§10, axis N — the park is `ev_run` capped by
a timer, and a producer's `ev_async_send` ends it). The two shapes that stage a burst are
3–8× cheaper at every cell — that is the segmented pipe, and its burst sweep (§9.11) is
unchanged by the 18 commits since `279e6cd4`: counting 1c is 10.0 / 8.7 ns here against
9.5 / 9.2 there. fib is the largest delta the repository has recorded — **44× / 70×** on
Windows, **25× / 29×** on WSL2 — and §11.5 says what 3.1.0's cell was (nine `LOG_INFO` lines
per actor lifetime, then an O(n²) table growth): a defect a benchmark had to exist to see.
bank-transaction is −48 to −50 % at one core on both hosts, and −79 % at two cores on Windows
where 3.1.0's 2c cells were the wide ones §12.4 records (342–1133 ns per transfer across nine
repetitions there; 145–208 in the candidate).

**The candidate is the fastest framework in all 64 cells** — by 1.3× at the least (ring and
ping-pong 2c cells on WSL2, against pooled CAF, which never crosses a core: §8.1) and 17× at
the most (fork-join 2c-park on WSL2) — and it is **below the raw-thread floor in 11 of the 16
two-core cells on Windows and 12 on WSL2**. §9.7 explains the mechanism for the batched shapes
(the floor pays one remote cache-line crossing per message on its SPSC ring; qb moves a batch
per flush), and axis N explains the park cells (the floor's condition variable pays the wake;
qb's parked core answers before it sleeps). One cell is new to that list, and it is a one-core cell:
chameneos 1c-park on Windows sits at 0.76× the floor (34.9 vs 45.8 ns). The floor's parked
mode is a `seq_cst` fence plus a `sleeping` load on every `send` — an `mfence` per message on
x86, `frameworks/baseline/baseline_support.h` — which is why the seven Mesh floors' 1c-park cells are
1.1–4.4× their own 1c-spin cells (ping-pong's floor at one core is one SPSC ring in both
columns, 1.0×); the candidate's park costs it nothing on the same core (34.9 vs
35.3), because a core that owns the whole ring never announces that it sleeps. What stays above the floor is exactly the set of
two-core cells that cross a core per message with nothing to batch: ping-pong 2c-spin (1.63× /
1.06×), thread-ring 2c-spin on Windows (1.40×; 0.98× on WSL2), the two fib cells (1.5–1.7× —
actor creation, not messaging) and bank-transaction 2c-park (1.03× / 1.04×, level).

### 13.2 What the two hosts disagree on

- **MSVC is 1.3–1.5× g++ on the same-core dispatch cells** (ping-pong 84 vs 66, ring 45 vs 39,
  fib 199 vs 133, bank 263 vs 144) and level or ahead on the all-to-all (big 20 vs 21; 25 vs 22
  at two cores). The fib gap is the widest — the same source, one generation of the same dense
  tables (§11.3) — and is the cell to profile if a Windows profile ever joins this protocol.
- **The WSL2 pass 2 sits above pass 1 on the cross-core cells by a level shift, not a spread**
  (ring 2c-spin 107.2 … 122.6 against 125.0 … 129.4, ping-pong 2c-spin 207.2 … 245.6 against
  224.4 … 257.9; the one-core and the batched cells do not move), after the 9-minute shipped
  leg between the passes. On Windows the same two cells are bimodal WITHIN a launch, as §9.11
  measured. Either way a grid median is where the majority fell, and the interleaved launch
  census (§9.11) is the instrument for those two cells; both passes rank the two builds the
  same way on both hosts.
- **The Windows 1c bank-transaction cells are 8–10 % above the same host's A/B session of two
  hours earlier** (13.16 / 13.62 ms against 11.99–12.22 at 06:28 UTC, §12.4's QB-178 session)
  while WSL2's agree to 1–2 % (143.5 / 145.7 against 145.0 / 144.9). The 2c cells agree on both
  hosts. It is the host's level, and the reason a session is compared within itself.

### 13.3 What the grid leaves, and where the next axis is

Named per host in each `qb-branch-develop/README.md`; the two readings agree on the order:

1. **The per-pass cost of a core with one event in flight.** A one-core round trip is 84 / 66
   ns and a round trip is two dispatches; counting measures the same dispatch at 10 / 9 ns when
   a million events are staged and drained in batches. ping-pong's two actors alternate one
   event at a time, so every event pays a whole `__flush_all__` → `consume_all` → route pass —
   42 / 33 ns per hop against 10 / 9 batched, 49× / 40× the floor's atomic hand-off, 2.2× the
   best of the field. This is the `perf` target on g++, and the cell every other shape's
   one-core figure is made of.
2. **An actor's lifetime.** fib is 199 / 133 ns per actor against a floor of 65 / 30 (one
   heap `Node` in a per-worker slot table and two ring messages): `addActor`, a registry slot,
   two subscriptions, `kill`, and the pipe traffic of two events. §11.3 lists what is in it.
3. **The cross-core hop with nothing to batch** — ping-pong and ring at 2c-spin, 1.0–1.6× a
   floor that IS the hop. Not a defect; the shape of the measurement. The census, not a grid,
   is what will say whether an axis moved it.

Nothing in the grid argues for another core axis before the train: the collapses are closed,
every shape is ahead of the field on both compilers, and the residuals are each a named
figure with a named instrument. This is the grid 3.2.0 ships with (`docs/ROADMAP.md`).

### 13.4 The final candidate — the same grid at `77b358d8` (2026-09-09), and what the second half bought

The grid of §13 was the programme's midpoint. The release measurement is the same protocol at
the last commit before the train — qb `develop` `77b358d8`, 66 commits over 3.1.0 and 37 over
`43f62afe`: the loop clock (§14), the pass's fixed cost and the ring's private lines (§15, §16),
the request/reply machinery and the loop under a timer (§17), the qev programme (QB-187 to
QB-195), the one loop reference (QB-199), the sub-millisecond park (§19) — candidate / shipped
3.1.0 / candidate, 9 + 2, one quiet session per host (Windows 15:39:09–15:42:12 UTC, WSL2
14:19:38–14:27:51 UTC, the other side idle each time), `results/<host>/qb-branch-develop/
grid-77b358d8/`, `grid-shipped-3.1.0-final/`, `grid-77b358d8-pass2/`, 32 / 32 verified in each.
ns per unit, p50; shipped 3.1.0 is this session's control, `43f62afe` the midpoint's own session:

| shape, config | Win 3.1.0 | Win `43f62afe` | **Win final** | pass 2 | WSL2 3.1.0 | WSL2 `43f62afe` | **WSL2 final** | pass 2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ping-pong 1c-spin | 112.0 | 84.2 | **29.7** | 30.4 | 96.0 | 66.0 | **22.6** | 22.9 |
| ping-pong 1c-park | 113.5 | 83.4 | **29.8** | 30.3 | 96.8 | 66.4 | **22.7** | 22.7 |
| ping-pong 2c-spin | 300.8 | 297.2 | **190.9** | 193.0 | 253.1 | 222.0 | **159.1** | 153.5 |
| ping-pong 2c-park | 3 069.3 | 274.5 | **193.5** | 208.9 | 26 266.8 | 227.0 | **152.3** | 159.1 |
| counting 1c-spin | 29.1 | 10.0 | **8.3** | 10.2 | 41.4 | 8.7 | **7.4** | 7.4 |
| counting 1c-park | 28.9 | 9.5 | **10.6** | 8.0 | 41.4 | 8.5 | **7.3** | 7.9 |
| counting 2c-spin | 32.6 | 11.7 | **13.4** | 13.5 | 44.4 | 11.0 | **9.0** | 9.0 |
| counting 2c-park | 32.3 | 11.7 | **13.4** | 13.6 | 44.6 | 11.0 | **9.0** | 9.0 |
| thread-ring 1c-spin | 63.6 | 45.4 | **17.9** | 18.3 | 52.4 | 39.3 | **17.1** | 17.2 |
| thread-ring 1c-park | 62.3 | 45.0 | **18.0** | 18.2 | 53.4 | 38.7 | **17.2** | 17.1 |
| thread-ring 2c-spin | 162.2 | 154.2 | **99.7** | 107.5 | 138.6 | 112.3 | **72.2** | 76.4 |
| thread-ring 2c-park | 513.5 | 146.4 | **104.1** | 103.2 | 13 104.7 | 120.7 | **74.9** | 74.6 |
| fork-join 1c-spin | 40.9 | 10.5 | **8.8** | 8.3 | 57.3 | 9.5 | **7.9** | 7.2 |
| fork-join 1c-park | 42.0 | 10.5 | **8.3** | 8.7 | 57.0 | 9.6 | **7.2** | 7.2 |
| fork-join 2c-spin | 43.7 | 12.2 | **11.1** | 9.7 | 46.3 | 9.6 | **8.3** | 8.3 |
| fork-join 2c-park | 41.1 | 11.2 | **12.4** | 11.6 | 46.6 | 9.9 | **8.1** | 8.3 |
| big 1c-spin | 36.2 | 20.3 | **16.7** | 17.3 | 32.0 | 21.4 | **17.6** | 18.0 |
| big 1c-park | 36.5 | 20.0 | **16.9** | 17.1 | 32.8 | 22.7 | **17.9** | 17.7 |
| big 2c-spin | 30.6 | 24.8 | **24.1** | 22.9 | 26.9 | 21.7 | **17.6** | 18.3 |
| big 2c-park | 32.3 | 23.9 | **24.1** | 24.5 | 28.6 | 21.9 | **17.5** | 17.6 |
| fib 1c-spin | 7 934.8 | 198.8 | **175.9** | 179.7 | 3 001.9 | 133.3 | **124.0** | 126.2 |
| fib 1c-park | 8 941.5 | 197.6 | **179.7** | 178.2 | 3 023.2 | 135.3 | **122.7** | 126.1 |
| fib 2c-spin | 7 179.3 | 117.2 | **108.4** | 114.8 | 2 402.6 | 90.4 | **83.0** | 82.8 |
| fib 2c-park | 7 117.1 | 116.5 | **110.1** | 112.5 | 2 193.4 | 89.8 | **82.7** | 83.2 |
| chameneos 1c-spin | 69.1 | 35.3 | **30.1** | 30.0 | 57.9 | 35.3 | **25.5** | 26.2 |
| chameneos 1c-park | 78.5 | 34.9 | **29.5** | 30.1 | 58.4 | 33.3 | **24.8** | 25.3 |
| chameneos 2c-spin | 97.9 | 65.3 | **73.5** | 73.4 | 70.0 | 54.3 | **43.7** | 43.4 |
| chameneos 2c-park | 117.0 | 63.5 | **70.0** | 75.2 | 73.8 | 53.8 | **43.8** | 45.2 |
| bank-transaction 1c-spin | 494.4 | 263.2 | **231.0** | 250.1 | 269.5 | 143.5 | **143.0** | 143.2 |
| bank-transaction 1c-park | 484.2 | 272.5 | **233.8** | 253.9 | 266.6 | 145.7 | **141.2** | 143.3 |
| bank-transaction 2c-spin | 714.8 | 156.6 | **142.9** | 160.9 | 183.3 | 92.8 | **78.8** | 77.3 |
| bank-transaction 2c-park | 911.5 | 149.6 | **139.4** | 162.6 | 178.2 | 96.4 | **84.9** | 81.7 |

**What the second half bought, on g++ (WSL2), where every cell moved in the same direction in
both passes:** the one-core round trip 66 → 23 ns (−66 %: §14's clock, §15's pass, §17's inline
resume and the loop under a timer), the one-core hop 39 → 17 (−56 %), the two-core round trip
222 → 159 (−28 %: §16's ring), the two-core hop 112 → 72 (−36 %), counting 8.7 → 7.4 and
11.0 → 9.0, fork-join 9.5–9.9 → 7.2–8.3, big 21.4–22.7 → 17.5–18.0, fib 133 → 124 and 90 → 83,
chameneos 35 → 26 and 54 → 44, bank-transaction 2c 93 → 79 and 96 → 85; bank 1c is the one cell
the second half did not touch (143.5 → 143.0: its cost is the ask path §12 and §17 already
took, and the transfer coroutine's own structure). Against 3.1.0 in this session: ping-pong 1c
96 → 23 (4.2×), 2c-park 26 267 → 152, the ring 52 → 17 and 13 105 → 75, fib 3 002 → 124 (24×),
bank 270 → 143 and 183 → 79.

**On MSVC (Windows) the one-core cells follow** — ping-pong 84 → 30, ring 45 → 18, big 20 → 17,
chameneos 35 → 30, bank 263 → 231, fib 199 → 176 — **and the two-core cells are the host's mode
of the day.** The grid read counting 2c 11.7 at the midpoint and 13.4 now, chameneos 2c 65 and
74, fork-join 2c-park 11.2 and 12.4, and the shipped control itself moved between the two
sessions by up to 11 % (chameneos 2c-park 105 → 117): every Windows two-core cell is bimodal
within a launch (counting 2c's nine repetitions sort 10.6, 10.8, 12.3, 13.3 … 13.5 — a lower
mode at 10.6 and an upper at 13.4, and the median lands wherever the majority fell). So the
final build was censused against the midpoint build in ONE session — ten alternated launches of
3 + 1 on CPUs 0 and 2, `build/final` (`77b358d8`) against `build/ab180-ctl` (`43f62afe`, the same
adapters), `results/desktop-win11-msvc19/qb-branch-develop/census-77b358d8-vs-43f62afe/`
— median of the ten launch medians, [min … max]: counting 2c **13.6** [10.0 … 13.9] against
**13.5** [10.9 … 14.0]; chameneos 2c **66.9** [46.8 … 74.7] against **67.8** [55.2 … 81.0]; ping-pong
2c **189.3** [187.0 … 192.5] against **242.7** [215.9 … 249.7]; thread-ring 2c **107.2** [91.2 … 114.7]
against **119.7** [117.6 … 125.4]. Level on the two cells the grids disagreed on — with the final
build's lower mode BELOW the midpoint's on both (10.0 vs 10.9, 46.8 vs 55.2) — and −22 % / −10 %
on the two the grids agreed on. The census's lower modes are what the hardware can do; how often
the host does it is not the build's.

**What it leaves**, for the train and after it: fib's 176 / 124 ns per actor lifetime against a
floor of 65 / 30 (creation, not messaging — §11's remaining term is the registry and the five
default subscriptions); MSVC's two-core cells, wide by the host and not by the build; the
two-core round trip at 191 on Windows against 159 on WSL2, the cross-core hop's cost now the
platform's (§16's private lines took the build's share); and bank-transaction 1c at 143, the one
cell of the 64 the second half did not move. The published tables' `qb` column stayed shipped
3.1.0 until the field was re-measured with the candidate in its own session — §13.5.

### 13.5 The whole field re-measured with the release candidate (2026-09-13), and where qb still loses

The 2026-09-09 grid measured the candidate beside 3.1.0; the published field tables still carried
the shipped 3.1.0 of 2026-09-04, measured in another session with 5 + 1. On 2026-09-13 the whole
field was re-measured **in the candidate's own session**, once per host, at qb `develop`
**`f2779605`** — the 3.2.0 release candidate as it will ship: `77b358d8` plus the documentation
commits of the train and QB-211's CMake, `git diff 77b358d8..f2779605 -- src/` touching
comments only. Four legs, in this order, the other host idle, Docker Desktop stopped, the harness
rebuilt at the candidate then 60 s of quiet: (A) the candidate, `--only qb`, 9 + 2, CPUs 0 and 2
→ `qb-branch-develop/grid-f2779605/`; (B) shipped 3.1.0 through the same adapters →
`grid-shipped-3.1.0-20260913/`; (C) **every framework** — qb, CAF, CAF-detached, SObjectizer, the
raw-thread floor — 9 + 2, 132 cells → `results/<host>/` (a fresh `run.json`, the README tables
re-transcribed from it, `check-report` 367 figures verified); (D) a 12-launch interleaved census
of 3 + 1 on the four two-core cells that decide a ranking — ping-pong and thread-ring, spin and
park — qb against CAF and the floor → `census-f2779605-field/`. Windows 03:19–03:32, WSL2
03:33–04:02 (UTC+2). 132 / 132 cells verified on each host, 2 declared `n/a` (CAF-detached has
no spin mode), 0 census launch unverified.

**The candidate did not move.** Every cell of `grid-f2779605/` sits inside the launch spread
of `grid-77b358d8/` on both hosts: ping-pong 1c 22.5 against 22.6 ns (WSL2), 31.0 against 29.7
(Windows); 2c-spin 154.9 against 159.1 and 185.9 against 190.9; thread-ring 1c 16.9 against 17.1
and 18.3 against 17.9; fib 1c 123.7 against 124.0 and 184.4 against 175.9. The hot path is
byte-identical and the measurement says so.

**Against the other three, qb is the fastest framework in all 64 cells**, and the ratio to the
best other framework in the cell — CAF or SObjectizer, whichever won second place — has a
geometric mean of **0.13 on both hosts** (one core and two); the narrowest cells of the field
are thread-ring 2c-spin on Windows (96 against CAF's 236, 0.41) and ping-pong 2c-spin on WSL2
(160 against 284, 0.56) — hops CAF runs on one thread and qb across two. **Against 3.1.0, in
the same session, no cell is slower**: the two smallest gains are the Windows `big` and
`chameneos` 2c-park cells (−27 % and −26 %), the largest is −99.4 % (WSL2 ping-pong 2c-park,
26.24 µs → 153 ns), and the other 62 of the 64 are beyond −34 %.

**The census** (median of the twelve launch medians, [min … max], ns per unit):

| cell | qb | CAF | floor |
|---|---|---|---|
| ping-pong 2c-spin, Windows | **186.8** [179.6 … 199.7] | 475.8 [472.9 … 480.8] | 180.8 [175.8 … 187.2] |
| ping-pong 2c-park, Windows | **205.5** [191.1 … 213.9] | 476.2 [474.0 … 481.6] | — |
| thread-ring 2c-spin, Windows | **105.1** [87.4 … 112.9] | 235.4 [230.8 … 241.9] | 112.4 [108.3 … 116.9] |
| thread-ring 2c-park, Windows | **105.6** [94.4 … 110.2] | 235.6 [232.0 … 240.0] | — |
| ping-pong 2c-spin, WSL2 | **155.9** [148.1 … 161.5] | 279.6 [277.0 … 291.6] | 182.9 [178.2 … 186.4] |
| ping-pong 2c-park, WSL2 | **154.0** [146.6 … 161.2] | 284.4 [281.0 … 290.0] | — |
| thread-ring 2c-spin, WSL2 | **75.4** [72.9 … 80.9] | 140.3 [139.7 … 145.6] | 103.1 [100.2 … 107.9] |
| thread-ring 2c-park, WSL2 | **74.3** [71.3 … 84.3] | 141.9 [141.1 … 142.4] | — |

qb sits ON the raw-thread floor on the Windows ping-pong (187 against 181, the distributions
overlap: a two-thread cache-line handoff is what that cell costs and the runtime adds nothing
measurable to it) and **under** it on the three others — the floor's SPSC ring pays one line per
message where qb's producer publishes a batch. The park cells read the spin cells: the 50 µs
idle-spin floor keeps a ping-pong from ever sleeping, so "park" is a policy label there, not a
mechanism (§8.2 for what sleeping costs). CAF's 2c cells are one-thread cells (§13, point 3 of
the README) and still lose to a two-thread one by 2.5× (Windows) and 1.8× (WSL2).

**Where qb loses — the report the maintainer asked for.** Not against any framework, and not
against 3.1.0. It loses against the raw-thread floor in exactly two kinds of cell, and against
itself between the two compilers:

1. **The one-core cells whose floor is a bare function call.** The floor of a one-core
   ping-pong is 2 ns (a call and a return), of a one-core counting 3 ns; qb pays 22 / 30 ns and
   7 / 10 ns (WSL2 / Windows) — the mailbox, the pipe, the dispatch: the price of the model,
   11–18× a function call and 96–113 ns at 3.1.0. The geometric mean of qb over the floor across
   the sixteen one-core cells is 2.7 (WSL2) and 2.4 (Windows). The two cells that stand out are
   the two whose unit is not a message: **fib**, an actor created and destroyed per unit —
   124 / 179 ns against a floor of 28 / 58 (4.4× / 3.1×): `addRefActor`'s `new`, the registry
   entry, the five default subscriptions, the reap (§11; QB-175's per-core slab for the actor
   object is the named next step) — and **bank-transaction**, an `ask` round trip per transfer —
   144 / 230 ns against 22 / 74 (6.5× / 3.1×): the request event, the slot, the coroutine frame,
   the reply event, two dispatches (§12; five ask-path defects came out of this shape already;
   what is left is the frame and the second dispatch). The remaining one-core cells sit at
   1.0–2.7× floors of 5–28 ns, and at 5.9× the 3 ns floor of the WSL2 ring — every one of them
   the same ~10–20 ns of runtime above a floor that is itself a few nanoseconds.
2. **Almost nothing at two cores** — the cross-core hop is the floor's own cost, and qb is at
   it or under it in every two-core cell whose unit is a message. The exceptions are the same
   two shapes: fib 2c at 1.3–1.4× on WSL2 (84 / 83 against 63 / 60) and 1.6× on Windows park
   (112 against 69; the Windows raw-thread fib at 2c-spin reads 446 and is the floor's own
   oddity), and bank-transaction 2c on Windows at 143 against a floor of 120 (1.19×;
   0.64–0.80× on WSL2) — the actor object and the `ask` again.
3. **MSVC against g++ on the same source**, one core: ping-pong +35 %, counting +32 %,
   fork-join +41 %, fib +45 %, bank-transaction +59 %, chameneos +17 %, thread-ring +7 %, big
   −2 % — a geometric mean of **+28 %** at one core and +40 % at two (where the Windows
   cross-core hop itself is 16–79 % dearer, chameneos and bank the widest). QB-46 answered half
   of it: clang-cl runs qb's dispatch 10–18 % faster than cl 19.51 on this host (§9.12), and the
   clang-cl preset ships in 3.2; the other half is the platform (the wake, the timer, the
   scheduler). The field is measured with cl because that is what a Windows user builds with.

What that leaves as the next axes, in order of what a nanosecond buys: the actor object's
allocation (fib, both hosts), the ask frame and its second dispatch (bank, both hosts), and
the MSVC codegen gap on the dispatch (every one-core cell, Windows only). None of the three
is a regression, none is a loss to a competitor, and none belongs to the 3.2.0 train.

### 13.6 The actor object's allocation (QB-212, point 1): the arena, measured

The first of the three axes §13.5 left. On the branch `perf/actor-arena` (qb `385bdb37`; not
merged when this was written) the actor object stops coming from the process heap: `qb::Actor`
declares class-level `operator new` / `operator delete` over `qb::allocator::thread_arena`, a
thread-private allocator of 16-byte size classes up to 1 KiB with LIFO reuse per class — the
block a dying actor gives back is the one the next spawn takes, still in cache — bump-filled from
a 64 KiB first chunk and then from the same 2 MB `slab_cache` slabs the pipes use, with
constant-initialised thread-local state (no TLS init guard on the path, the QB-178 lesson) and a
teardown at thread exit that returns the slabs only when nothing is live. An actor is created
and destroyed on one thread by construction, so the arena has no lock, no atomic and no
cross-thread free path; a type above 1 KiB or over-aligned falls through to the global
allocator, and a class that declares its own operators keeps them. The measurement behind it is
§11's: with the registries and the pipes silent, the actor object's own `malloc` / `free` was the
last heap traffic of a `fib` lifetime, one `malloc` per actor, 27.8 % of the core on WSL2.

Measured against `develop` `f2779605` in one quiet session per host, the control never rebuilt,
candidate / control / candidate grids (8 shapes × 4 configs, 9 + 2) then 12 interleaved launches
of 3 + 1 on the four fib cells (`results/<host>/qb-branch-perf-actor-arena/`):

| fib cell | Windows/MSVC: `f2779605` → branch | WSL2 g++-14: `f2779605` → branch |
|---|---:|---:|
| 1c-spin (ns per actor lifetime) | **185.3 → 127.2 (−31 %)** | **129.2 → 99.1 (−23 %)** |
| 1c-park | **189.5 → 128.0 (−32 %)** | **128.8 → 100.7 (−22 %)** |
| 2c-spin | **117.2 → 85.3 (−27 %)** | **86.3 → 57.9 (−33 %)** |
| 2c-park | **115.9 → 86.0 (−26 %)** | **89.2 → 58.3 (−35 %)** |

Every fib distribution is separate; the 28 other cells of each grid sit inside ±5 % and change
sign between the two candidate passes wherever they reach it, and the anchors' censuses overlap
(bank-transaction 1c-spin 232.9 vs 233.9 and 145.6 vs 141.5, ping-pong 1c-spin 30.6 vs 29.3 and
22.9 vs 23.0, counting 12.1 vs 10.4 and 7.6 vs 7.7). The one-core gain is larger on Windows
because the pair it removes was dearer there (the Windows heap against glibc's fast path: 179
against 124 ns of lifetime in §13.5); the two-core cells, where the lifetime is split across two
heaps, gain the same third on both hosts. Against the raw-thread floor of §13.5 the fib ratio
moves from 4.4× / 3.1× to 3.5× / 2.2× (WSL2 / Windows) at one core.

The `dev/bench` gate of the same WSL2 session is worth its own paragraph, because it said FAIL
and was wrong about the branch. Against the 2026-09-07 baseline: 107 metrics within threshold,
24 beyond their improvement threshold (the train's work since the baseline), 5 regressed — all
five on two binaries that construct no actor, and a control comparison built at `f2779605` in
the same window still read the parser's fragmented case +5.8 %, a raw two-thread reference
ping-pong +4.0 % and a coroutine channel +3.5 %, tight across three alternated passes. `cmp`
then showed the parser and the channel binaries to be **identical bytes** between control and
candidate: the same program, 5.8 % apart, because the two trees' executables have paths of
different lengths and the executable path is part of the initial stack and environment layout
every hot loop's alignment inherits (Mytkowicz et al., ASPLOS 2009). Copied into one directory
under same-length names, pinned and alternated: parser 405.1 vs 404.9 ns, channel 55.67 vs
55.74 µs, the reference ping-pong 200.9 vs 203.4 ms (+1.2 %, overlap over seven processes), the
qb mono ping-pong 59.0 vs 56.7 ms (−4.0 %, separate). The lesson is the protocol's: two trees
compared through `bench-run.sh` must run their binaries from one path, and a baseline older than
the tree's last hot-path commit is a drift meter, not a control. The next two axes are
unchanged: the ask frame and its second dispatch (bank), then the MSVC codegen gap.

## 14. The loop clock — what one line cost, and what an idle spin pass needs

§13.3 named the per-pass cost of a core with ONE event in flight as the first residual and `perf`
on g++ as its instrument. The first profile answered in one line. `perf record -e cpu-clock
-F 25000` (WSL2 has no PMU) on `43f62afe`'s `savina/ping-pong` at one core, spin:

```
39.56%  [vdso] __vdso_clock_gettime
 9.51%  qb::VirtualCore::__workflow__()
 9.24%  qb::VirtualCore::__receive__()
 8.08%  qb::VirtualCore::__receive_events__(span<EventBucket>)
 6.93%  router::memh<Event,true>::EventResolver<Ball>::resolve
 3.64%  ev_pending_count      2.15% ev_active_count      (listener::has_work(), every pass)
 3.62%  VirtualCore::send(Event const&)   3.39% send<Ball>   2.97% __flush_all__   1.42% time()
```

Axis D (§7) had been "landed" as `985cbb3a`, `VirtualCore::time()` sampled on demand and keyed on
the pass counter, with a field comment promising that a pass nobody asks costs nothing. It had
moved the read, not removed it: `__workflow__` still built `const qb::LoopEvent loop_ev{time(),
_loop_count}` on every pass — the event handed to `ICallback::on()` — whether or not a single
callback was registered to receive it, so every pass of a callback-free core (every benchmark
here; every server that drives itself from io and events) paid one `clock_gettime`, ~13 ns of a
~33 ns pass, for an event nobody received. The fix is the obvious one: the tick phase, snapshot
and `LoopEvent` included, sits behind `if (!_callback_list.empty())`.

### 14.1 The guard alone: −56 % at one core, +25 % on the cross-core spin cells

Measured through the unmodified adapters, qb-only builds, candidate / control (`43f62afe`) /
shipped 3.1.0 / candidate in one quiet WSL2 session (9 + 2, CPUs 0,2;
`results/wsl-debian-g++14/qb-branch-perf-loop-clock-on-demand/grid-guard-only*/`): ping-pong
1c-spin **64.9 → 28.4 ns** per round trip and thread-ring 1c-spin **37.9 → 17.5 ns** per hop —
and ping-pong 2c-spin **206.9 → 253 / 259**, thread-ring 2c-spin **105.9 → 133 / 134**, with the
2c-park cells and the batched shapes level. The ten-launch interleaved census (`census-guard-only/`)
made it a finding rather than a level shift: 250.6 … 262.1 against 198.2 … 212.9, 127.9 … 141.6
against 101.4 … 111.4, fully separated distributions.

The mechanism is the idle pass. A SPINNING core whose pass just lost its only clock read polls its
peer's ring index in ~20 ns of unserialized code and re-runs the whole pass between two reads —
signal load, `has_work()`, flush, receive, the empty-tick check — and the cross-core exchange gets
slower for it; the parked configurations did not move because a parkable core's idle pass still
reads `mono_now()` for the idle-spin floor (axis A), and that read is `lfence; rdtsc` under the
vDSO. What an idle spin pass needs was then measured rather than reasoned, one insertion at a time
at the end of an idle spin-mode pass, interleaved launches, 3 reps + 1 warmup (per-primitive costs
from a pinned micro-benchmark on the same CPUs: `_mm_pause` **33.4 ns** on the i9-12900K P-core,
`_mm_lfence` 2.8 ns, `clock_gettime` 13.1 ns, bare `rdtsc` 6.0 ns):

| idle spin pass | ping-pong 2c-spin | thread-ring 2c-spin | ping-pong 1c-spin |
|---|---:|---:|---:|
| `43f62afe` (the control: a wall clock read per pass, everywhere) | 205.8 | 105.6 | 65.0 |
| guard only (~20 ns, unserialized) | 259.9 | 134.4 | 28.3 |
| + bounded tight `has_data()` poll, ×256 | **271.6** | **144.3** | 28.9 |
| + `spin_loop_pause()` (33 ns) | 228.2 | 123.5 | 28.6 |
| + `lfence` (3 ns) | 221.3 | 120.3 | 28.6 |
| + `lfence` + `pause` | 224.0 | 126.5 | 28.4 |
| + `lfence; rdtsc` (11 ns) | 204.6 | 114.8 | 28.6 |
| **+ `mono_now()` on the idle pass (13 ns)** | **212.4** | **107.4** | **28.6** |

(Three censuses, 7 / 5 / 7 launches; the per-directory README carries all three, the third's
JSON is `census-variants/`.) Read together: the tightest poll is the worst — more reads of the
peer's line per unit time, not fewer, is what hurts; an `lfence` alone recovers most of the loss
for 3 ns; a `pause` recovers less and costs 33 ns of detection latency on this generation; and
~10–15 ns of serialized work between two reads puts both cells back on the control's figure. The
monotonic clock read the park policy already takes on an idle pass is exactly that, so the fix
takes it in every latency mode — `_idle_since` is stamped on idle passes whether or not the core
can park, and only the park stays gated on `latency > 0`; a busy pass reads no clock in any mode.
The pacing is documented in `VirtualCore.cpp` as pacing, with these figures, not left as an
accident a later cleanup would remove again. A hardware wait on the peer line (`umonitor` /
`umwait` and `tpause` on WAITPKG parts, `wfe` on arm64) is the axis this leaves open: the
right primitive for "sleep until that line is written" exists on both ISAs this repository
measures, and nothing here has tried it.

### 14.2 The branch head, both hosts, one session each

| cell (per unit) | Windows/MSVC: `43f62afe` → head | WSL2/g++: `43f62afe` → head | census (10 / 15 launches, ctl vs head) |
|---|---|---|---|
| ping-pong 1c-spin (round trip) | 80.7 → **41.6** (−48 %) | 65.8 → **28.3** (−57 %) | 80.7 vs 41.9 / 64.8 vs 28.2 |
| ping-pong 1c-park | 81.5 → **42.8** (−47 %) | 66.5 → **28.4** (−57 %) | — |
| thread-ring 1c-spin (hop) | 44.7 → **22.9** (−49 %) | 38.1 → **17.8** (−53 %) | — |
| thread-ring 1c-park | 45.5 → **23.0** (−49 %) | 38.3 → **23.1 / 18.0** | — |
| ping-pong 2c-spin | 251.7 → 267.1 / 245.5 | 197.1 → 211.1 / 212.9 | 247.3 vs 255.0 / 205.6 vs 209.7, overlapping |
| thread-ring 2c-spin | 123.3 → 123.3 / 124.7 | 105.8 → 107.4 / 110.9 | 122.4 vs 122.8 / 104.4 vs 109.3, overlapping |
| ping-pong 2c-park | 257.3 → 257.1 | 219.4 → 209.3 | — |
| thread-ring 2c-park | 128.3 → 123.3 | 114.9 → 108.0 | — |
| counting (all four) | bimodal on both builds (§9.11) | 8.0–9.9, level | 9.8 vs 9.8 / 9.9 vs 9.8 (2c) |

Shipped 3.1.0 in the same sessions: ping-pong 1c 113.6 / 97.9, so the head is **−63 / −71 %**
against the release. `dev/bench`'s same-core cells say the same thing in the engine's own units:
`BM_Mono_PingPong_Latency` **117.8 → 76.7** (MSVC) and **100.2 → 62.9** (g++), the 10-actor
one-core pipeline **79.1 → 43.3** and **65.1 → 31.8 ns** per delivery, the 8 × 8 pipeline and the
cross-core `Multi_PingPong` inside their spread (257.3 vs 258.4 over eight interleaved g++ runs),
the ask round trips −3 to −7 %. The cross-core spin medians on g++ sit +2 / +5 % with
overlapping distributions across three censuses — recorded with their sign, not rounded away:
the head's idle pass is a few nanoseconds shorter than the control's (it no longer copies an
empty tick snapshot), and the table above is not sharp enough to say whether that is the reason.

What is left of residual 1 after this: the pass itself — `has_work()` is two `ev_*` calls per
pass, `__receive__` swaps and walks an empty pipe before it reads the rings, and the resolver's
dispatch is 7 % of the profile — and the profile is the instrument again.

## 15. The pass itself — what a core pays per pass and per event, and the probe that separates them

§14 removed the clock read from the pass and left a one-core ping-pong at 28.3 ns per round trip
on g++: two passes of ~14 ns, each carrying one event. What is a pass, and what is the event?
`counting` measures the event at ~8 ns when a million of them are drained in a burst, so the
fixed part of a pass had to be ~6 ns; but neither number could be read off a benchmark directly.
`tools/probes/pass-cost.cpp` (Huly QB-182) is the instrument: one actor on one pinned core with
k independent self-event chains, so every pass carries exactly k events through the self pipe,
the router and the handler — no tick, no clock inside the window — and k = 1, 2, 4 separate the
per-pass and per-event costs. On `c42abddf` (the §14 head), g++-14: **14.6 / 23.1 / 41.0 ns**
for k = 1 / 2 / 4 — a fixed pass of ~6 ns and **8.4–8.9 ns per event**; MSVC 19.51: 20.4 /
28.4 / 44.8.

The per-instruction profile of the k = 1 pass (cpu-clock samples joined to the disassembly by
symbol offset, since `perf annotate` has no PMU to lean on under WSL2) put a third of it in
the handler's `push`: a **four-deep dependent-load chain** to find the outbound pipe (the
engine's core set → its dense-index table → the pipe vector's data pointer → the pipe's
cursors), a six-register prologue inherited from the inlined slow half of `allocate_back`, and
the cursors themselves just rewritten by `__receive__`'s pipe swap; a fifth in `__receive__`
(the swap — six loads and six stores per pass — then `front()` reading them back); and the
rest spread over `__flush_all__`'s prologue on a pass with nothing to flush, the two `ev_*`
calls behind `listener::has_work()` (one a loop over five priorities), the resolver's prologue
(the broadcast walk's thread-local snapshot vector, inlined into the unicast path) and a
divide-by-24 in the router's bounds check. Five changes, each kept only after the probe moved
(ns per pass, k = 1 / 2 / 4, g++-14):

| step | k = 1 | k = 2 | k = 4 |
|---|---:|---:|---:|
| `c42abddf` | 14.55 | 23.15 | 40.99 |
| io counters read inline (qev `ev_active_count_addr` / `ev_pending_count_addr`), swap gated on a non-empty self pipe | 14.46 | 23.35 | — |
| + `_pipe_of_core[CoreId]` (one indexed load), `allocate_back_slow` out of line | 14.08 | 20.88 | — |
| + the self pipe walked in place up to a fence — no second pipe, no swap | 13.80 | 20.24 | 33.25 |
| + inline peer scan before the flush drain, broadcast walk out of line | 12.68 | 17.12 | 25.37 |
| + cached slot count in `key_table::find` (**`670e9433`**) | **12.9** | **16.6** | **24.6** |

The pass with one event is **−12 %** and the marginal event **8.9 → 4.2 ns**; on MSVC 20.4 →
15.6 / 28.4 → 22.9 / 44.8 → 37.0. The fence is the design change: `__receive__` used to swap a
second pipe in so that a handler's same-core pushes could not grow the range being walked; a
`segmented_pipe::fence` — the tail segment and write cursor at the top of the pass — gives the
same guarantee on ONE pipe (pushes land behind it and are the next pass's), and a pipe drained
to its fence rewinds its resident segment, so a one-event pass reads and writes the same 64
bytes every time.

### 15.1 Both hosts, one session each, against `c42abddf` (p50 per unit; the per-directory READMEs carry every cell and the censuses)

| cell | Windows/MSVC | WSL2/g++ |
|---|---|---|
| ping-pong 1c-spin (round trip) | 41.4 → **30.6** (−26 %) | 28.7 → **22.6** (−21 %) |
| thread-ring 1c-spin (hop) | 22.7 → **18.5** (−19 %) | 21.1 → **17.2** (census 18.0 → 17.3) |
| fork-join 1c / 2c-park (message) | 8.9 → 8.4 / 10.7 → 11.5 | 9.2 → **7.4** / 11.1 → **8.4** (−25 %) |
| big 1c-park (round trip) | 19.4 → **17.0** (−12 %) | 22.5 → **18.5** (−18 %) |
| counting 1c-spin (message) | bimodal, both builds | 8.9 → 8.0 |
| ping-pong 2c-park / 2c-spin | 260 → 230–246 / 248 → 238–246 | 213 → 200 / 209 → 200–211 |
| thread-ring 2c-spin / 2c-park | 122 → 125–127 / 123 → 126 (**+2–5 %**) | 108.5 → 110–114 / 108 → 112 (**+1–5 %**) |
| `dev/bench` `Mono_PingPong` / one-core pipeline | 76.6 → **66.4** / 42.7 → **34.6** | 63.7 → 58.5 / 32.2 → 29.9 |
| `dev/bench` `BM_PINGPONG` 64 actors, 1 core | 29.4 → 26.5 | 26.5 → **21.3** (−20 %) |

### 15.2 The cell that moves the other way, and why it is kept

thread-ring at two cores — a hundred actors round-robin over two cores, every hop crossing, one
token in flight — reads +1 to +5 % against the control in every interleaved census on both
hosts, while the two-actor ping-pong at two cores reads −4 to −9 %. The bisect (the head with
one change undone at a time, eight interleaved launches each, g++): the inline flush scan alone
brings the ring back to 105 ns (control 107); the pipe table and the router split do not. A
waiting core's idle pass is ~2 ns shorter without the drain's prologue and walk, and the
100-actor ring's cross-core hop is slower for it — the sensitivity §14 measured when the idle
spin pass lost its clock read, in the shape that had already shown it most. Undoing the scan
would cost 0.5 ns on every one-event pass, 3 ns on a four-event pass and 6 % on ping-pong 1c;
the ring's ~4 ns per hop is the recorded price, and QB-181 (what an idle spin pass should do
while it waits — `umonitor`/`umwait`, `tpause`, `wfe`) now has the ring as its instrument. The
probe for that question measured here, under WSL2 on the i9-12900K: `tpause` has a 25 ns floor
whatever its deadline, `umwait` wakes ~86 ns after the remote write against 31 ns for a tight
spin, `pause` 33 ns.

### 15.3 What is left of residual 1

The one-event pass is ~13 ns on g++ for ~180 instructions; the marginal event ~4 ns. The next
instrument is the same probe over the receive side — `__receive_events__`'s prologue and the two
table lookups per event, the trampoline's `is_alive` byte, `dispose` — and the open design
question is the ORDER of the pass: the flush runs before the receive, so a handler's cross-core
`push` waits a whole pass before it leaves the core; `send`/`forward`/`reply` already deliver
straight into the peer's ring, and moving the flush after the receive would give `push` the same
latency at the cost of a documented ordering change (engine.md, steps 5 and 6).

## 16. The ring's other line — a producer that re-reads what it publishes, and what a cross-core hop is made of

§15 closed with a design question: does a cross-core `push` pay a whole pass because the flush
runs before the receive? The answer took a day and turned out to be about something else. This
section records the three instruments the question needed, the two dead ends they measured,
and the defect they found — one line of the SPSC ring that both sides had been reading.

### 16.1 The flush position is not the variable (QB-183, parked)

Moving `__flush_all__` to the end of the pass, measured on the eight shapes in one session
(`results/wsl-debian-g++14/qb-branch-perf-flush-at-pass-end/`), moved no cell beyond its
spread: fib 2c −0.7 %, bank, big and fork-join 2c level, chameneos 2c +4 % at the census. The
expected gain had been mis-stated — a handler's push waits the ~4 ns of instructions between
the end of the receive and the next pass's flush, not "a whole pass" — and nothing the grids
can resolve. To resolve it anyway, `tools/probes/xcore-hop.cpp`: two actors, one per pinned
core, a round trip by `push<>` or by `send<>`, so ns(push) − ns(send) is what the pipe and the
flush's place in the pass cost a hop.

What that probe found first was that its own locked figure cannot be trusted. A two-actor
ping-pong **phase-locks**: the producer's store lands at a fixed point of the consumer's idle
cadence, so five flush positions read 166–237 ns per round trip with no monotonic relation to
where the flush sits, a **5 ns** busy-wait in front of the flush moved the round trip by
**+80 ns**, and the SAME variant read ±10 % from one binary to the next — the control 241 →
219 and the "end of pass" variant 215 → 231 when the ring's index lines were spaced 128 bytes
apart, i.e. from the memory layout, not the code (a 1–2-byte shift of the loop's code moved
nothing: 239–246 vs 217–218 at three alignments). The probe's `jitter_ns` option breaks the
lock — a uniformly random 0..j busy-wait before every hop on the A end, rdtsc-paced, reported
NET — and at a random phase the picture is flat: `send` **235 ± 2 ns** on every one of eight
builds and two layouts, `push` inside a ±6 % band that follows the layout. Phase-averaged
figures are the only ones comparable across builds; every number below is one.

### 16.2 What a cross-core hop is made of — a raw ring, no qb in the loop

`tools/probes/raw-ring.cpp` is two threads and two rings of 64-byte slots, the shape of qb's
mailbox ring, with one thing varied at a time. Its first lesson is about itself: **a launch
sits in one of two regimes ~90 ns apart**, decided at launch and not by the code — the same
binary, the same row, six launches in a row read ~120 and ~210 — and the A-side jitter that
breaks the phase lock of §16.1 does not move a launch between them (what does is presumably
where the rings land physically, which no user-space layout controls). So the table gives all
six launches of each row, sorted, and the reading is which regime a shape can REACH, not a
median (i9-12900K / WSL2 g++-14, CPUs 0 and 2, 1.2 s windows, jitter 150 ns, ns per round trip
net of the jitter; `results/wsl-debian-g++14/qb-branch-perf-ring-private-lines/raw-ring.txt`):

| what varies | six launches, sorted |
|---|---|
| the two-line handshake (slot, then index line, then a full fence — qb's ring), tight poll | 148 · 154 · 155 · 222 · 226 · 228 |
| … the consumer spins with `pause` between polls | **121 · 122 · 126 · 130** · 210 · 213 |
| … with `lfence; rdtsc` between polls | **114 · 119 · 137** · 184 · 193 · 201 |
| … with one `clock_gettime(CLOCK_MONOTONIC)` (`qb::mono_now()`) | **120 · 125** · 189 · 191 · 195 · 216 |
| … the clock, then six independent L1-hit loads with compares (a pass's checks) | 190 · 193 · 233 · 237 · 241 · 243 |
| … the six checks, then the clock | 165 · 174 · 183 · 213 · 217 · 238 |
| … ~30 ns of dependent ALU work, no fence | 174 · 177 · 178 · 184 · 230 · 239 |
| … `lfence; rdtsc` then ~15 ns of ALU work | 168 · 176 · 176 · 182 · 238 · 243 |
| **… the producer loads its published index line before its store**, `mono_now()` gap | **166 · 167 · 167 · 172 · 178 · 197** |
| one line per hop (a lap-tagged sequence in the slot, two stores), `pause` | **119 · 120 · 122 · 123** · 183 · 185 |
| one line per hop as two 32-byte AVX2 stores, `pause` | 182 · 183 · 185 · 185 · 186 · 186 |
| one line per hop written by ONE `movdir64b` + `sfence`, `pause` | 210 · 213 · 213 · 269 · 270 · 270 |

Four things read off that table. The one-line hop — the "sequence in the slot" design that was
axis O — reaches the same ~120 the two-line handshake reaches, in the same share of launches:
the two misses of a hop overlap, or the spatial prefetcher pairs them; it is not worth a ring.
`movdir64b` never gets below 210: a direct store goes past the caches and the consumer fetches
it from far away. The poll's SHAPE decides whether the low regime exists at all: a bare
serialized read between two polls (`pause`, `lfence; rdtsc`, one `clock_gettime`) reaches
~120 in a third to two thirds of the launches, while any ordinary work around that read — six
L1-hit loads, 15–30 ns of ALU work — never goes below 165, because the next poll's load issues
speculatively at the top of the gap and the gap's length is added on top. The pre-3.2 shape of
a qb idle pass — ~30 ns of checks around one clock read — is that second family.

The bold row is the defect. ONE load of the published index line by the producer, before its
store, and the row that read 120 · 125 in its low launches never gets below **166**: the
consumer's poll has snooped that line, and on this microarchitecture the owner's copy does not
survive the snoop, so the load is a cross-core miss on every hop. qb's `spsc::ringbuffer` did
exactly that: `write_index_` (published) and `cached_read_index_` (the producer's snapshot)
shared a line, and every `enqueue` began by loading both. A cpu-clock profile of the two-actor
`send` round trip had already said so in numbers nobody could read until the raw ring gave them
a meaning: 8 544 samples on the load of the producer line at
`SharedCoreCommunication::send+0x4f`, 9 633 on the release fence after the publish — the miss
cost what the fence cost.

### 16.3 The fix: private working lines (QB-184)

Each side of the ring owns two lines now, each in its own two-line block so no spatial-prefetch
partner belongs to the peer: a PRIVATE line with the working index and the snapshot of the
peer's index, and a PUBLISHED line with nothing but the index the peer polls. The rule the
layout encodes is that a side only ever *writes* the line it publishes on. 512 bytes of header
per ring instead of 128, against 64 KiB of slots.

Same host, one quiet session per host, candidate / control / candidate against `670e9433` (p50
per unit; the per-directory READMEs carry every cell, the censuses and the bench cells):

| cell | WSL2 / g++-14 | Windows / MSVC 19.51 |
|---|---|---|
| ping-pong 2c-park (round trip) | 209 → **165 / 159** (census **211.7 → 164.7**, −22 %) | 257 → **209 / 206** (census **263.2 → 204.9**, −22 %) |
| ping-pong 2c-spin | 221 → **163 / 173** (census 207.5 → 163.2, −21 %) | 258 → **217 / 199** (census 257.0 → 206.1, −20 %) |
| thread-ring 2c-park (hop) | 114 → **82 / 80** (census **115.0 → 79.9**, −31 %) | 130 → **102 / 100** (census **138.6 → 98.9**, −29 %) |
| thread-ring 2c-spin | 114 → **81 / 83** (census 116.1 → 80.4, −31 %) | 144 → **105 / 96** (census 137.0 → 98.4, −28 %) |
| chameneos 2c (meeting) | 49 → **45** (census −7 %) | 55 → **53 / 51** (census −5 %) |
| big 2c (round trip) | 19.3 → 18.7 / 17.9 (census −4 %) | 22.5 → 20.4 / 20.3 (−9 %; census 21.1 → 20.5) |
| bank-transaction 2c (transfer) | 93 → 88–90 (census level) | 147 → 159 / 154 in the grid, 154.7 → 158.2 on a 15-launch census — level |
| every 1c cell, counting, fork-join, fib | level (15-launch census on each cell the grid had moved) | level (15-launch census on fib 2c, counting, bank, big) |
| `dev/bench` `Multi_PingPong` (cross-core) | 253 → **203** (−20 %) | 318 → **246** (−23 %); the raw-ring reference 286 → 223 (−22 %) |
| `dev/bench` pipeline 8 × 8 cores / `BM_PINGPONG` 64 actors, 8 cores | 322 → **264** (−18 %) / 24.7 → **21.7** | 252 → **220** (−13 %) / 24.9 → 23.9 |
| `dev/bench` same-core cells (`Mono`, pipeline 1c, `BM_PINGPONG` 1c, ask) | level | level (`Mono` 71.7 → 70.9, pipeline 1c 36.7 → 37.4, `BM_PINGPONG` 1c 26.5 → 26.2) |
| `xcore-hop` send / push, phase-averaged (jitter 150) | 240 → **198** / 240 → **159** | 295–303 → **257–271** / 258–283 → 254–273 (the locked push bimodal on MSVC) |
| `pass-cost` k = 1 / 2 / 4 | 12.9 / 17.1 / 25.0 → 12.7 / 17.1 / 25.0 | 16.2 / 23.7 / 38.1 → 16.1 / 23.6 / 37.9 |

The same-core figures do not move because the same-core path never touches this ring (the
self pipe is a `segmented_pipe`), and the batched cross-core shapes do not move because they
publish runs of hundreds of events per ring write. What moves is every cell whose hop is one
event on an idle peer — the cross-core round trip, the cross-core ring, the broker — by a
fifth to a third.

### 16.4 What §16 leaves

The raw floor for this handshake on this host is ~120 ns per round trip in the launches that
reach it; qb's two-actor `send` round trip is ~198 after the fix, so ~75 ns per round trip
remain between the raw ring and qb's hop — the receive and dispatch of the event, the fence
stall the producer pays after its publish, and the shape of the idle pass between two polls
(16.2: the shapes that never reach the low regime are the ones with work around the clock read). A tight idle loop — `has_data()`, the clock, the signal and stop checks, and
nothing else, entered when a pass had no activity, no tick and no io work — was prototyped
against the fix on the probe and split: `send` −5 %, `push` +6 %. It is not shipped; it is
QB-181's next measurement, against this section's figures as the base and with the eight
shapes as the judge, and the ring's cross-core hop is still its most sensitive cell.

## 17. The request/reply machinery, and the loop under a timer

`tools/probes/ask-cost.cpp` (Huly QB-185) puts two actors on one pinned core and measures three
round trips: `push` — the asker's handler pushes, the responder `reply()`s: two passes and
nothing else; `ask` — one coroutine looping `co_await qb::ask<E>()`, the responder `reply()`s,
the asker routes with `resolve_ask()`; `stream` — `ask_stream` drained with `next()`, per
chunk. A `timeout_ms` option gives every ask (or every `next()`) a deadline, the documented
idiom.

### 17.1 What an `ask` pays over a push, and where the queue was

On `develop` `c4f9d439`, g++-14: push **24.0 ns**, ask **53.6** — ~30 ns of machinery over the
two passes an ask cannot avoid. The cpu-clock profile: the `qb::ask<E>` coroutine's ramp 16 %,
`__workflow__` 21 % (the scheduler's `run_ready()`: a ready-queue pop and a hash-set erase),
`deliver_thunk` 5 %, the registry (`ask_take` / `ask_deliver` / `ask_unregister`) 9 %,
`__tls_init` 1.7 % (a `static thread_local bool` with dynamic initialisation, read through the
TLS init wrapper on every ask). The defect: `ask_awaiter::deliver_thunk`, reached from
`resolve_ask()` in the asker's own handler on its own core, did not resume the waiting frame —
it queued it (`schedule_via_current`: a hash-set insert and a queue push) for the next pass's io
phase to pop, erase and resume, while the `task<E>` the ask returns already completed by
symmetric transfer. Resuming inline from the thunk (the cancel and timeout paths keep the
queue: they run from a token callback or a timer) and making the flag `constinit`:

| probe, one core (ns per round trip) | WSL2 / g++-14 | Windows / MSVC 19.51 |
|---|---|---|
| push | 24.4 → 24.3 | 34.2 → 33.6 |
| ask | **54.0 → 46.7** (−14 %) | **81.7 → 72.0** (−12 %) |
| the ask machinery over push | 29.7 → 22.4 (−25 %) | 47.5 → 38.4 (−19 %) |
| `savina/bank-transaction` 2c-spin / 2c-park (census, per transfer) | 92.4 → **84.0** / 93.2 → 88.3 | 153.7 → 152.7 / 154.7 → **147.4** |
| bank 1c-spin / 1c-park | 142.3 → 137.7 / 140.8 → 138.5 | 261.7 → 250.6 / 260.8 → 250.1 |

Measured and dropped on the way: draining the scheduler's ready coroutines right after the
receive, in the same pass — for `ask_stream`, `ping`, `require`, `ask_all`, whose wakes keep
the queue — a null result (a one-chunk stream 107 → 105.7 ns) that cost ~1 ns on every pass:
a deferred wake already ran in the next pass's io phase *before* that pass's receive, so its
push was handled in that same pass and the queue, not a pass, was the whole price. What the
sanitizer added: a spawned coroutine completing outside `run_ready()` — now the normal end of
an ask-driven coroutine — leaked its frame at teardown through a `!done()` guard in the
scheduler's cascade; fixed with the test that pins it (qb `CHANGELOG.md`).

What is left of the ask machinery (~22 ns on g++): the `task<E>` frame's ramp and teardown,
the registry, the cancellation hook, three moves of E — spread thin; a lever would have to be
structural (an awaitable that is not a coroutine, a 4.0 shape).

### 17.2 The timer under the ask — where libev's 2010 defaults were

The same probe with `timeout_ms = 500`: **798 ns** per round trip on g++ against 46 without
the timeout, **1108** on MSVC against 82 — the documented idiom 17× the timeout-less ask, and a
one-chunk `ask_stream` with a timeout 860 / 973. No Savina cell had shown it: `bank-transaction`
asks with `duration::zero()`; the `dev/bench` ask cell (~700 ns per ask, bimodal on MSVC) had
been showing it all along and was read as "instrumentation".

Two libev defaults, both in qev now (qev `CHANGELOG.md` `[Unreleased]`, Huly QB-187), measured
with `qev/bench/bench-pass.c` — N `ev_run(EVRUN_NOWAIT)` over a loop whose watchers never fire,
what a core with one watcher pays on EVERY pass:

| shape (ns per non-blocking pass, WSL2 g++-14) | qev `47a4151` | qev after QB-187 |
|---|---:|---:|
| empty loop | 297 | **50** |
| one far timer (a pending request timeout) | 297 | **51** |
| one quiet socket | 296 | **131** |
| `ev_now_update` (what every timer arm pays first) | 97 | **17.5** |
| `ev_timer_start` + `ev_timer_stop` | 3.2 | 3.3 |

1. **The clock through the raw syscall.** `EV_USE_CLOCK_SYSCALL`: the config probe's
   `HAVE_CLOCK_SYSCALL` always compiles on Linux, and libev took it as "use the syscall" to
   spare glibc < 2.17 a librt dependency. Every clock read was a trap, ~95 ns, and `ev_run`
   reads it twice per pass: 52 % of the timed ask's profile. Now only where libc has no
   `clock_gettime`.
2. **A poll over nothing.** A NOWAIT pass called `backend_poll(0)` — `epoll_wait`, wepoll's
   `GetQueuedCompletionStatusEx`, `kevent` — with no fd registered. Now the loop counts its
   active `ev_io` watchers and skips a poll that would not block when there is none; a
   blocking wait is kept (with no fd it is the sleep).

The timed ask after both: **172 ns** on g++ (the poll skip 798 → 604, the clock 604 → 172),
**124 ns** on MSVC (1108 → 124: there the whole cost was the poll), the one-chunk timed stream
860 → 238 / 973 → 330; a 64-chunk timed stream 38 → 34.6 / 88 → 76.5 per chunk. What remains
of libev per timed ask on g++ (~115 ns of 172): one clock read per pass plus the pass's
bookkeeping (a full memory fence for a wake-up handshake a non-blocking pass never needs, a
second `time_update`), and the fact that a pending timer keeps the loop running on every pass
at all — the programme that follows.

### 17.3 The qev programme

qev is on the path of every core that owns a timer or a socket, and on none of the Savina
cells, which is why its cost went unmeasured for the whole 3.2 audit until 17.2.
`dev/plans/roadmaps/QEV_PERFORMANCE_ROADMAP.md` (Huly QB-186) is the audit — the three entry
points, twelve findings ranked by measured cost — and the plan: the non-blocking pass at its
floor (no handshake, one clock read, the evpipe not counted as a pollable fd — it is, after the
first park, and it re-enables the poll for the life of the loop; QB-188, target ≤ 25 ns per
pass), request timeouts without a libev timer (a deadline list on the pass clock; QB-189,
target a timed ask ≤ 60 ns), the embedder's clock handed to the loop (QB-190, ≤ 10 ns per
pass), the io pass — io_uring's user-space completion queue against `epoll_wait(0)`, wepoll on
Windows, a quiet-fd cadence (QB-81, QB-191) — and the wake/park path (QB-192). Every step
carries `bench-pass` and `ask-cost` on both hosts, and no step is judged on one.

### 17.4 Phase 1 delivered — the pass at its floor, and what Windows had been hiding

QB-188, measured with `bench-pass` and `ask-cost` on both hosts
(`results/<host>/qev-branch-perf-nowait-pass-floor/`): a NOWAIT pass reads the clock once,
raises no wake-up handshake (the sender's flag path delivers, and the pass tail reads
`sig_pending` / `async_pending` beside `pipe_write_skipped` so a byte written around a park
that lands after its poll is picked up by the next pass), and the loop's own evpipe is not
counted as a pollable fd — which mattered more than the two others in qb, because the first
`Park::Loop` of a core arms the wake, creates that pipe, and had been re-enabling the poll
QB-187 removed for the life of the core.

| ns | WSL2 / g++-14 | Windows / MSVC 19.51 |
|---|---|---|
| NOWAIT pass, timers-only loop | **51 → 22** (−57 %) | 19.7 → 15.8 |
| NOWAIT pass, one quiet socket | 132 → 101 (−23 %) | — |
| ask with a 500 ms timeout | **174 → 115** (−34 %) | 124 → 115 (−7 %) |
| stream, 1 chunk, with a timeout | 240 → 179 (−25 %) | 327 → 320 |
| untimed ask / push / one-core pass | level | level |

The MSVC column was small for a reason the suite found rather than the profiler: qev's loop
suite had never run on Windows, and its first run measured **27 checks, 2 failed** on the
shipped code — a 0-second timer started and run inside one pass did not fire. The loop's
clock there was `GetSystemTimeAsFileTime`, a 3 ns memory read of the system tick (steps every
2.2 ms on this host, 15.6 nominal; not monotonic), and MSVC having no `clock_gettime`, it was
the monotonic clock too: every libev timer in qb on Windows — ask timeouts, `callback` delays,
sleeps, retries, the park cap — was judged at that granularity. QB-193 puts the clocks on
`QueryPerformanceCounter` and `GetSystemTimePreciseAsFileTime`; a precise read is ~16 ns (the
TSC, the same floor the vDSO has on Linux), so the MSVC pass is 15.8 → 31 ns, a timer arm with
`ev_now_update` 5.8 → 24, and the timed ask **115 → 166** — four precise reads per round trip
(three passes and the arm) where there were four tick reads. (Corrected in §17.7, QB-195: the
`QueryPerformanceCounter` half was not compiled in until then — libev's misconfiguration block
had switched the monotonic clock off for lack of `CLOCK_MONOTONIC` on MSVC, and the precise
system time was standing in for it; the figures of this section are those of that clock.) That column is reported as the
regression it is on the probe and the fix it is for every timer; the next two phases remove
exactly those four reads: the deadline list (QB-189) takes the libev timer off the request
path, and the embedder's reading handed to the loop (QB-190) takes the pass's read off a core
that already made one. Windows is where the deadline list pays most.

Two more findings for the phases ahead, recorded on the way: the io_uring backend arms its
timerfd to "now" on a NOWAIT pass (`iouring_poll` with a zero timeout: a `timerfd_settime`
syscall and a spurious completion per pass, QB-191's first item), and the superproject's
TSan preset does not instrument `ev.c` — the C target never receives `-fsanitize=thread`, so
the evpipe protocol has never been under ThreadSanitizer; instrumented standalone, it reports
the volatile-flag exchange libev has always used (QB-192, with C11 atomics as the likely
answer).

### 17.5 Phase 2 delivered — the request path without a libev timer

QB-189 (`results/<host>/qb-branch-perf-request-deadlines/`). Every timed `ask`,
`ask_stream::next()`, `ping` and `require` armed an `ev_timer`, and the timer's real cost was not
the arm: it was a referenced active watcher for the whole wait, so `listener::has_work()` stayed
true and every pass of the core ran `ev_run` — 22 ns a pass on g++ after §17.4, 31 on MSVC with a
precise clock — for the one request in flight. The deadline is an intrusive node in the awaiter
now, in a per-core list sorted by deadline (a `VirtualCore` member), and the pass checks it
without the loop: a member load when nothing is armed; a COARSE clock read (the scheduler tick —
`CLOCK_MONOTONIC_COARSE`, `GetTickCount64`, `CLOCK_MONOTONIC_RAW_APPROX` — ~3–5 ns) when
something is, and the precise `mono_now()` only within one tick of the earliest deadline, so a
timeout keeps its sub-microsecond precision. An idle pass hands the list the reading it already
makes for the park policy, and a park is bounded by the earliest deadline (the loop holds no
timer for it any more — the regression the reading of the whole pass avoided).

| ns per round trip, one core | WSL2 / g++-14 | Windows / MSVC 19.51 |
|---|---|---|
| ask with a 500 ms timeout | **111.6 → 67.8** (−39 %) | **157.1 → 90.1** (−43 %) |
| the timed ask over the untimed one | 66 → 22 | 90 → 25 |
| stream, 1 chunk, with a timeout | 173.6 → 131.3 (−24 %) | 354.8 → 301.9 (−15 %) |
| untimed ask / push / one-core pass | level (ten launches) | level |
| bank 2c / 1c, ping-pong 2c (ten launches) | 83.0 → 80.9 / 142.9 → 144.8 / 159.5 → 160.3 | — |

The programme's line for the timed ask, both hosts: g++ **798 → 174 → 115 → 68**, MSVC
**1108 → 124 → 115 → 166 → 90** — the Windows figure back under its pre-QB-193 124, as the
roadmap's acceptance asked, with the timers precise. What remains over the untimed ask is one
precise clock read at the arm (~16–17 ns on both hosts, the TSC) and the list; the read is what
QB-190 shares with the pass on a core that already made one.

Two shapes were measured and replaced on the way, and they are the reason the list is a core
member rather than a thread_local. Read out-of-line, the per-pass gate cost ~0.5 ns on EVERY
pass of every core (`push` 23.3 → 24.4 on g++). Read inline as an `extern constinit
thread_local`, g++'s cost vanished (the linker relaxes the general-dynamic access to one `%fs`
load in an executable) but MSVC's did not: its TLS access is four dependent loads and read
`push` 31.6 → 33.6. A member load off the core object the pass already holds is free on both.
Also found on the way: an `io::async::callback` timer armed on the calling thread's core
outlives its `qb::Main` — `start(false)` runs core 0 on the caller's thread, whose listener is
that thread's, not the engine's — so a test's far timer leaked into the next test in the same
process (`active_count` read 1 with no watcher of its own); the test uses a scope-cancelled
`sleep` instead, and the hygiene note is in the milestone's memory.

### 17.6 Phase 3 delivered — the io pass on a cadence

QB-191 (`results/<host>/qb-branch-perf-io-poll-cadence/`; qev `EVRUN_NOPOLL`, `ev_io_count_addr`,
`ev_io_fed_addr`). A core that owns one socket ran the backend poll — `epoll_wait(0)`, wepoll's
IOCP wait, `kevent` — on EVERY pass, and on a quiet socket every one of those calls returned
nothing. `tools/probes/io-pass.cpp` (`qvoprobe-io-pass`) is the instrument: one actor on one
pinned core owning the accepted end of a loopback TCP pair as a raw `event::io` watcher (the
shape every qb-io session registers), driving itself with a self-event chain (`pass`) or spinning
idle while a peer thread writes an 8-byte timestamp every 100 or 20 µs (`wake`: p50 / p99 / max
of `now − sent`). With the pass at its floor, an io pass cost **124 ns against 12 for a plain
pass** on g++ (the poll ~80, the rest of `ev_run` ~22), **275 against 16 on MSVC** (the IOCP
wait ~245 — the largest single item any qb pass paid on any host).

The listener polls on a cadence now: on every pass while the loop is HOT (the previous pass's
poll reported a ready fd — read inline off qev's `ev_io_fed_addr()`, so a burst stays at poll
latency) and otherwise once per `CoreInitializer::setIoPollInterval` (1 µs by default; 0 = every
pass, the 3.1 contract), measured on the CPU's own counter (`qb::tsc_ticks`, ~5 ns, calibrated
once against `mono_now()`). The passes in between run `ev_run(EVRUN_NOWAIT | EVRUN_NOPOLL)`:
timers, periodics and pending events exactly as before, no backend call. A blocking pass — a
park, `run_once_for` — always polls (there the poll IS the wake), and the listener's own default
stays "every pass", so a program driving `run(EVRUN_NOWAIT)` itself keeps one call, one poll; a
`VirtualCore` opts its listener in at thread start.

| one core, medians of five | WSL2 / g++-14 | Windows / MSVC 19.51 |
|---|---|---|
| pass with a quiet socket (ns) | **124.2 → 48.5** (−61 %) | **275.0 → 83.1** (−70 %) |
| wake latency on that socket, p50 (µs) | 3.36 → 3.93 (+0.57: half the interval, by design) | 17.7 → 18.9 (inside the spread; the host's ~18 µs is the writer thread and AFD, bimodal on both sides) |
| wake latency, p99 (µs) | 25.1 → 23.2, level | noisy on both, no conclusion |
| push / timed ask / no-watcher pass | 23.7 → 23.9 / 67.9 → 68.6 / 12.4 → 12.3, level | 31.1 → 31.1 / 90.2 → 89.6 / 15.8 → 15.7, level |
| the same candidate with the interval set to 0 | pass 126.1, p50 3.31 — the control | pass 272.5, p50 20.1 — the control |

The trade is stated, not hidden: a QUIET socket's first byte waits up to one interval more (half
of it on average, and the g++ p50 moved by exactly that); a burst pays nothing, because the loop
is hot for the pass after every delivery; and a core that owns a socket pays 36 ns a pass on g++
(67 on MSVC) for it instead of 112 (259). A 20 µs cadence of bytes is 400 passes apart and finds
the loop cold every time — hot is one pass, which is the design (a burst is bytes back to back).
The knob is per engine, and 0 restores the old behaviour, measured to the control on both hosts.

What remains in the cold pass — the loop's own clock read and bookkeeping, ~22 ns of the 48 on
g++ — needs the earliest timer deadline and the loop's clock readable inline to skip `ev_run`
altogether when nothing is due; that is the next cut, packed with QB-190 (the embedder's clock).

Found on the way, in qev (Huly QB-194): the standalone's wepoll suite — the only dedicated
coverage of the fork's headline backend — had never measured wepoll. Its five cases wrote a raw
winsock `SOCKET` cast to `int` as the fd, registered nowhere in qev's `SOCKET ↔ fd` registry;
`EPOLL_CTL_ADD` failed, `fd_kill` fed the watcher `EV_ERROR | EV_READ | EV_WRITE`, and callbacks
that never looked at `revents` took the kill for a delivery (`revents` 0x80000003 measured, the
loop's io count 0 after the pass). The sixth case, written for this phase, asks whether a pass
LOOKED — it counts what wepoll fed — and so could not be fooled. The sockets go through
`ev_io_init_sock` now, every verdict requires `EV_ERROR` absent, the raw form replanted is
rejected (3 FAIL), and the floor is 5 → 13. Same lesson on the qb side: the cadence test's five
fd cases were behind `#ifndef _WIN32` (a pipe) and run on Windows now over a loopback pair
through wepoll — the platform where the cadence buys the most is the one that must prove it.

### 17.7 Phase 3 delivered — the pass without the loop, and the embedder's clock

QB-190 (`results/<host>/qb-branch-perf-pass-clock/`; qev `ev_now_set`, `ev_clock_now`,
`ev_timer_count_addr`, `ev_timer_next`, `ev_wake_pending_addr`). Once the poll was on a cadence
(§17.6), what a non-blocking pass still paid was `ev_run` itself — its clock read, the timer heap,
the pending walk, `bench-pass`'s `timer` shape: 21.8 ns on g++-14, 28.6 on MSVC — on every pass of
every core holding one far timer (a `sleep`, a retry, a keep-alive) or one quiet socket, to find
nothing. The `io-pass` probe gained a `timer` mode for exactly that shape: a busy core (a self-event
chain) holding one `async::callback` an hour out, no socket.

The listener now asks the loop, inline, before calling it — an event fed since the last pass
(`ev_pending_count_addr`), a wake an `ev_async_send` from another thread left while no pass was
blocking (`ev_wake_pending_addr`), a poll the cadence is due to make (`ev_io_count_addr`), a timer
within reach (`ev_timer_count_addr`, `ev_timer_next`) — and a pass with none of the four does not
enter the loop. "Within reach" is judged the way the cadence is, on the CPU's counter: the listener
anchors each reading of the loop's clock against `tsc_ticks()` and estimates now as the anchor plus
the counter's advance (a 0.1 % rate margin, 2 ms of slack, a fresh anchor every second at most), so
a deadline beyond the estimate costs no clock read, and one within it costs the precise read —
handed to the loop (`ev_now_set`) so the pass that fires the timer reads the clock once. A timer is
never judged against the estimate, only against a real reading.

| one core, medians of five | WSL2 / g++-14 | Windows / MSVC 19.51 |
|---|---|---|
| busy pass with a far timer (ns) | **36.9 → 26.2** (−29 %) | **47.7 → 29.5** (−38 %) |
| pass with a quiet socket, cold (ns) | **48.6 → 28.5** (−41 %) | **82.9 → 49.5** (−40 %; both bimodal) |
| what those pay over a plain pass (12.4 / 15.6) | 24.5 → 13.8 ; 36 → 16 | 32 → 14 ; 67 → 34 |
| wake p50 on the socket, push, timed ask, no-watcher pass | level (all inside their spreads) | level |
| qev `bench-pass`: `timer` / `timer+set0` (a free sample) / `gate` (no `ev_run`) | 21.6 / **10.3** / **1.8** | 28.6 / **15.0** / **0.8** |

The roadmap's phase-3 target (`timer` ≤ 10 ns on a core that supplies its clock) is the
`timer+set0` row — the loop's bookkeeping floor given its time, 9.7–11.0 on g++, 15.0 on MSVC —
and qb does better than take it: on the passes where nothing is due it takes the `gate` row, and
pays `timer+set` (the read moved, not saved: 21.2–22.1) on the one that fires. What is left in a
skipped pass, 14 ns over a plain one, is the counter read (`rdtsc`, ~8) and the loads and the call
of the gate; below that is a shared pass clock for every consumer at once, which is 3.3's question,
not this milestone's.

**The gate had to go out of line, and Windows is where that was measured.** Written inline in
`listener::run()` — which every core's pass inlines — the gate cost MSVC's `push` **31.0 → 33.1 ns**
(+6.5 %, five alternations, fully separated) on a core whose pass never enters it: two actors
exchanging events, no timer, no socket. Bisected in five builds: the control's `listener.h` in the
candidate tree read level, so the file was the cause; the loop struct's growth (moved to its end)
and the listener's (its fields moved last; the control plus 48 bytes of padding: +0.5) were not; the
gate's double arithmetic out of line (`_timer_due` `QB_NOINLINE`) recovered the timed ask and most
of push, and the whole gate as one out-of-line call (`_nowait_gate`) closed it, 31.2 against 31.2.
What MSVC did to the pass around an inlined gate the pass never took cost more than the call the
gate now is; g++ showed nothing of it in either form.

**Found on the way (Huly QB-195): the `QueryPerformanceCounter` clock of QB-193 had never been
compiled in.** The first `ev_now_set` case failed on MSVC alone — `ev_clock_now()` read Unix
seconds — and the reason is libev's "fixes any misconfiguration" block, which forces
`EV_USE_MONOTONIC` to 0 wherever `CLOCK_MONOTONIC` is undefined, 450 lines after QB-193 had set it
to 1 for `_WIN32`; MSVC defines it nowhere. So `get_clock` was `ev_time`, the loop's "monotonic"
time was the precise SYSTEM time (QB-193's other half) — stepped by every wall-clock adjustment —
and the QPC path was dead code in qev and in qb's copy alike; `test_clock_resolution` could not
tell, a precise system clock also moving a thousand times in 50 ms without stepping back inside
them. §17.4's Windows figures are therefore those of the precise system time; with the block
exempting Windows, QPC reads cheaper (`timer` 31.5 → 28.6) and the loop is monotonic for the first
time. The corollary for every raw timer user, tested with the raw C arm as the negative control: a
loop that runs only when something is due has a stale clock the rest of the time, so
`event::timer::start()` / `again()` refresh it before every arm, as `sleep`, `callback` and
`with_timeout` already did by hand.

### 17.8 Phase 4 delivered — io_uring measured, and at parity

QB-81 (qb `readme/6_guides/performance_tuning.md`, "io_uring, measured (3.2.0)"; qev `5fe159b`).
`QB_EV_BACKEND=iouring` had shipped since 3.1 with no figure behind it, and the `io-pass` probe was
the first to ask: a core's pass over one quiet socket read **1345 ns against epoll's 28.5** — a
syscall storm inside the backend, not a slow backend. `iouring_poll` armed its deadline timerfd at
"now" on every timeout-0 poll (`>=` where a sleep needs `>`; upstream libev carries the same line),
the timerfd expired at once, its one-shot `POLL_ADD` completed, was drained and re-armed through
`io_uring_enter`, and the next pass armed "now" again: three syscalls a cycle, ~300k cycles a second,
whatever the embedder's cadence. Armed only for a poll that sleeps, the pass fell to 25.8 ns — and
exposed the second defect: the ring is `COOP_TASKRUN`, the kernel's completion work waits for the
task's next syscall, and a loop that now made none saw a ready fd at the scheduler tick (wake p50
2.0 ms against 3.9 µs); `IORING_SETUP_TASKRUN_FLAG` + a `GETEVENTS` enter on `IORING_SQ_TASKRUN`
in the post-drain flush restores prompt delivery at one syscall per completed event. Running qb's
whole suite on the backend found the third: the loop's own timerfd watcher counted in `iocnt`, so a
timers-only loop never took the no-poll pass of §17.4 nor the gate of §17.7 (40.5 ns against 25.8);
`io_is_loop_own()` keeps it and the wake pipe out of the count.

Final figures against epoll, one pinned core, medians (WSL2 6.6, g++-14): a pass with one quiet
socket at the default 1 µs cadence **28.8 / 26.4 ns**, polled on every pass **126.9 / 40.9**,
timers-only 26.5 / 26.4, a byte every 100 µs 55.2 / 50.6, wake p50 3.98 / 4.18 µs (p99 17.0 /
16.6), a parked core woken by a socket p50 41.6 / 41.3 µs (p99 137 / 161), syscalls on a quiet
socket 960k/s / **1/s**. Parity with a different shape: io_uring checks readiness by reading its
memory-mapped completion ring, so a quiet pass costs no syscall at all where epoll pays
`epoll_wait(0)`; it pays ~0.2 µs more per delivered event (the completion, the one-shot re-arm) and
two syscalls per park (`timerfd_settime`, `poll`) against `epoll_wait`'s one. **epoll stays the
default** — a single-digit gain at the default cadence does not buy a younger backend with more
kernel-version and seccomp surface for every Linux deployment — and io_uring is the right choice for
a core that polls on every pass, the one configuration where sub-microsecond wake latency is
affordable. qb's Linux CI runs its suite on both (`-DQB_IO_EV_TEST_BACKENDS=epoll;iouring`), with a
guard that fails an io_uring variant the kernel refused rather than let it test epoll under that
name; the whole suite forced onto io_uring under both sanitizers: 385/385.

## 18. The dispatch under a population: the actor's line, and what the core already hides

QB-198 (`results/<host>/qb-branch-perf-dispatch-prefetch/`, the `dispatch-population` probe — a
negative result, measured on both sides and kept) and QB-199 (`results/<host>/qb-branch-perf-loop-listener-ref/`).

After the pass reached its floor (§15, §17), `perf` on savina/bank-transaction at one core — 1 000
accounts, transfers to accounts drawn at random, ~1 MB of live objects against a 1.25 MB L2 —
showed where a core serving a POPULATION pays: the account's `on(Deposit&)` handler was 18 % of
the core and **78 % of its samples sat on the trampoline's `is_alive()` load**, the destination
actor's first line, an L3 miss of ~20 ns on a 150 ns transfer; `EventResolver<Deposit>::resolve`
had 64 % of its samples on the load of the router's handler slot (32 bytes a subscriber, 32 KB a
type). Each account is touched every ~1 000 events, so its lines have left L1 and L2 by the time
its next event arrives — the shape of any core serving one actor per connection. (The transfer
coroutine's own 25 % sat on the adapter's `std::deque` chunk: the user's structure, left alone;
CAF and SObjectizer pay it too.)

### 18.1 The prefetch, and the four measurements that shaped it

`__receive_events__` walks a sequential pipe — the headers of the events ahead are in cache, and a
destination's slot in the core's actor table is one load from a table that stays resident — so
the loop can prefetch the actor of an event ahead before routing the one in hand, and the miss
overlaps the handlers in between. Each design decision was measured, and three of the four went
the other way from the plan:

- **One line, not two.** Prefetching the liveness line AND the first line of user data hid the
  trampoline's miss (13.2 → 5.0 % of the core under `perf`) but put `qb::ask` +2.7 points on the
  same profile: two fills a peek take the fill buffers the handler's own misses need.
- **Not the handler slot.** The same peek into the router's slot through a virtual
  `IEventResolver::prefetch` cost `big` **+19 %**, a plain pass **+21 %** and `push` **+13 %** with
  the gate CLOSED — the code the call added around the loop, not the prefetch.
- **A gate of 512, read per batch — and no lambda.** At 64, savina/big's 120 hot actors paid +4 %
  for a peek that hid nothing. A per-event compare never taken cost counting **+1 %** of a 7.7 ns
  message in three sessions; the obvious way to make it free — the loop as a C++20 templated lambda
  instantiated with and without the peek, "byte for byte" — compiled WORSE (ping-pong 1c **+12 %**,
  `push` +9 %, the probe at 16 actors +13 %: by-reference captures of the loop's locals and a
  doubled body). Reverted; a hoisted bool it was.
- **Two events of lead paid on bank-transaction (−3.6 %) and hid nothing at 2 ns a handler.** The
  cursor went to eight events ahead (40 ns of lead at the cheapest handler) — and bank-transaction
  fell back to −0.5 %: 800 ns of its own traffic evicts the fetched lines before use.

### 18.2 The probe's verdict

`qvoprobe-dispatch-population <actors> [batch] [s] [cpu]`: N actors of ~300 bytes on one core, a
driver pushing batches of 256 events to actors drawn at random, ns per event. Its CONTROL curve
is the fact that decides the question (WSL2 g++-14, medians of three): **5.0 ns an event at 16
actors, 5.1 at 64, 5.5 at 256, 5.7 at 512, 6.1 at 1 024, 9.3 at 4 096, 11.2 at 16 384** — the
population costs a core ~6 ns an event on this probe, against an L3 miss of 40–60 ns on this
host. The out-of-order engine already overlaps the misses of a dozen short, predictable handlers
(one trampoline target, one loop, a 512-entry reorder window). So on every cheap workload the
software prefetch adds its instructions and hides nothing: with the gate closed **+6 %** an event
(16–256 actors: the per-batch bool, the cursor's bookkeeping), on a hot population of 512–1 024
actors **+21–25 %** (300 KB of actors sit in L2; the peek's own loads and fills cost more than the
L2 hits they pre-empt), −3.5 % where the misses finally start (4 096), +7.7 % again at 16 384. Where
it paid — a ~100 ns handler with dependent misses and unpredictable control flow (a coroutine
resumed inline, indirect calls to different targets) — it paid at two events of lead and not at
eight, and no cheap gate (population, batch size) tells such a handler from a hot population of
the same size; the one that would (the time an event costs, read per batch) needs an `rdtsc` a
batch, which a ping-pong's batch of one cannot afford, and a threshold tuned on this host.
**Not shipped.** Windows/MSVC's curve for the record, identical code on both sides: 6.2 → 16.6 ns
an event from 16 to 16 384 actors, with ±1–10 % between two identical binaries at three points —
more rounds are needed there before the probe can judge an A/B. What stays: the probe, and
`messaging-dispatch-batch` — one batch mixing live destinations, a slot the reap emptied, a
broadcast, a never-assigned service id and a mid-batch kill, delivered in one pass, and a batch
over three segment links with kills scattered through it.

### 18.3 QB-199 — one reference to the loop

`listener::current` is an inline thread_local with a non-trivial constructor, so g++ routes every
access through its TLS wrapper (the init guard, `__tls_init`), and `__workflow__` reached it three
times a pass (`has_work()`, `run()`, `nb_invoked_event()`) plus once on the idle path: 2.2 % of
ping-pong 1c. One reference taken at the top of the loop — the object lives for the thread, and
the loop is the thread. Measured alone at twelve interleaved rounds, one quiet session each:
**ping-pong 1c 23.1 → 22.8 ns (−1.4 %, quartiles separated)** on WSL2 g++-14, every other cell and
the three probes level; Windows/MSVC, which initialises TLS at thread start, level everywhere
(ping-pong 1c 31.4 / 31.5). Small, pure, and the one thing QB-198's four sessions delivered to
`develop`.

## 19. The park's wait, and how fine it is — QB-196

QB-196 (`results/<host>/qb-196-park-cap/`, the `parked-timer-wake` probe, the second qb-only
instrument after §10's `parked-io-wake`). The issue was filed from a reading of the code: on
Windows the park of a core that owns io watchers — `ev_run(EVRUN_ONCE)` capped at `latency`,
axis N — waits through wepoll's `epoll_wait`, whose timeout is whole milliseconds that libev
rounds UP, and a kernel wait "is honoured at the 15.6 ms system tick unless the process raised
its timer resolution"; so a `latency` of 100 µs was said to park 1–16 ms, with `timeBeginPeriod`
and a high-resolution waitable timer as the candidates. The rule on the issue was the right one:
measure before touching. The measurement refuted half of the reading, found the other half on
Linux too, and moved one thing — on Linux.

### 19.1 The instrument

`tools/probes/parked-timer-wake.cpp` (`qvoprobe-parked-timer-wake`): one core, one actor, and
nothing on the core but a timer. Each round arms one `qb::io::async::callback` of `delay` from
inside the previous one and records its lateness — fired-at minus due-at — in microseconds; 2000
rounds after 50 warm-ups (400 at `delay = 5 ms`), min / p50 / mean / p90 / p99 / max on one
line. The core is pinned by the probe (CPU 0); `latency`, `delay`, the idle-spin floor and, on
Windows, a `timeBeginPeriod(period)` around the run are its arguments. Two controls bound the
reading: `latency = 0` (the core never parks and judges the timer on the busy pass's clock —
what the timer costs with no wait at all) and `idle_spin = 0` (the core parks on its first idle
pass, so the wait is the whole story). A parked core's wait is `min(latency, time to the next
timer)`, so `delay` under `latency` measures one wait and `delay` over it measures a chain of
`latency`-long parks ending in a short one.

### 19.2 What Windows said — the tick that was not there, and the idle state that was

Windows 11 / MSVC 19.51, i9-12900K, Docker Desktop quit, the WSL2 side idle, 60 s after the last
build, one run per cell (`parked-timer-wake.txt`). Lateness p50 in µs, `period = 0` (nothing
asked of the timer resolution) → `period = 1 ms` (`timeBeginPeriod(1)` around the run):

| `latency` \ `delay` | 100 µs | 1 ms | 5 ms |
|---|---:|---:|---:|
| 0 (never parks) | 0.2 → 0.2 | 0.2 → 0.2 | 0.2 → 0.2 |
| 100 µs | **1417 → 1013** | 997 → 887 | 966 → 783 |
| 1 ms | **1442 → 1006** | 994 → 910 | 967 → 870 |
| 10 ms | **998 → 985** | 923 → 906 | 799 → 666 |

Every parked cell's p90 is 1.9 ms and its p99 2.0–2.4 ms, `period` or not; `idle_spin = 0`
reads the same as the default (1009 / 981 / 1010 µs at the three latencies). Two facts, and
neither is the one the issue was written on:

- **The 15.6 ms tick is not what a 1 ms wait costs here.** `NtQueryTimerResolution` on this box,
  read in the same session, says the system resolution is at its coarsest — **15.625 ms**, nothing
  running has raised it — and a wepoll wait of 1 ms still returned in 1.0–1.5 ms at p50 and 2.4 ms
  at p99. The kernel's waits are tickless; what it adds to the requested millisecond is its own
  coalescing, 0.5–1.5 ms. `timeBeginPeriod(1)` moves the p50 by 1–30 % from cell to cell and the tails not at all:
  a lever, not the floor. Neither of the issue's candidates was pursued.
- **What a parked core pays to be woken by a socket is the CPU's idle-state exit, and it is the
  same on every tree.** §10 had recorded 62–69 µs for the `latency = 1 ms, gap = 2 ms` cell on
  axis N; a first run of the same cell this session read 122 µs, and by the protocol that is an
  A/B, not a number. `parked-io-wake-ab.txt`: qb at axis N (`7761b0c5`) against `develop`
  (`67f4efb6`), five interleaved repetitions per cell, p50 in µs, control → candidate:

  | cell | control (5 reps) | candidate (5 reps) |
  |---|---|---|
  | `latency=1000 gap=2000` | 118 / 88 / 118 / 118 / 66 | 118 / 119 / 86 / 113 / 105 |
  | `latency=1000 gap=200` (polling) | 23.0 ×5 | 23.1–23.2 ×5 |
  | `latency=100 gap=2000` | 67 / 103 / 120 / 75 / 117 | 103 / 72 / 120 / 120 / 72 |
  | `latency=10000 gap=2000` | 180 / 185 / 187 / 189 / 183 | 189 / 188 / 185 / 188 / 188 |
  | `parked-timer-wake 1000 100` | 1014 / 1134 / 1030 / 1496 / 1436 | 1077 / 1031 / 999 / 1459 / 1411 |

  The `gap = 2 ms` cells are bimodal on BOTH sides — a mode near 65 µs and one near 120, the p50
  landing on either from one launch to the next, p90 127–130 and min 22–24 on every run of the two
  sub-millisecond-cap cells — and
  the 10 ms cell sits at 185 on both: the longer the CPU has been idle before the wake (a 10 ms
  cap lets it sleep the whole 2 ms gap; a 1 ms cap wakes it every millisecond), the deeper the
  state it has to leave. §10's 62–69 was the lower mode in a session where the CPU happened to
  stay shallow. Not a regression, and not qb's: the polling cell and the spinning control do not
  move, and the instrument that would show a qb cost — the spinning timer at 0.2 µs — is flat.

So on Windows the floor of a parked core's wait is libev's millisecond ceiling plus the kernel's
coalescing, ~1 ms at p50 and ~2.4 at p99, whatever `latency` says under a millisecond; a
sub-millisecond timer on a parked core fires at the millisecond, and only `latency = 0` goes
below. That is now the contract `CoreInitializer::setLatency` states.

### 19.3 What Linux said — libev's own millisecond

WSL2 Debian 13 / g++ 14.2 (Linux 6.6), same probe, qb `develop` (`parked-timer-wake.develop.txt`),
lateness p50 in µs:

| `latency` \ `delay` | 100 µs | 1 ms | 5 ms |
|---|---:|---:|---:|
| 0 (never parks) | 0.1 | 0.2 | 0.2 |
| 100 µs | **1010** | 110 | 357 |
| 1 ms | **1012** | 111 | 358 |
| 10 ms | **1010** | 110 | 113 |

The 100 µs column is exactly one millisecond late, on every latency, with a p90 within 5 µs of
the p50 — no coalescing, no tick, a precise wait for the wrong duration. The source is in
`ev_epoll.c` and `ev.c`, and it is the same code on both hosts: `epoll_wait` takes an `int` of
milliseconds, `EV_TS_TO_MSEC` is `a * 1e3 + 0.9999` (a ceiling), and the epoll backend sets
`backend_mintime = 1e-3`, which `ev_run` applies before the poll, so a wait of 100 µs is asked
for as 1 ms before the syscall is even made. The 1 ms column's 110 µs is the same ceiling seen
from the other side: the core spins its 50 µs idle floor, asks for the 950 µs that remain, gets
1 ms, and the kernel's 50 µs timer slack (`/proc/sys/kernel/timer_slack` — 50 000 ns on this box,
the default for a normal thread) is the rest; the 5 ms column at `latency = 1 ms` chains four
1 ms parks, each late by that slack and a pass, into 357. Linux never had the 15.6 ms problem; it
had this one, and so did Windows underneath its coalescing.

### 19.4 `epoll_pwait2` — the A/B

Linux 5.11 added `epoll_pwait2`, the same wait with a `struct timespec`, declared by glibc 2.35.
qev's epoll backend now makes every BLOCKING wait through it where the libc declares it and the
kernel answers (asked once per loop at init; `ENOSYS` keeps `epoll_wait` and the millisecond
minimum; a non-blocking poll keeps `epoll_wait` on every kernel — both enter the same path and
`epoll_wait` copies nothing in, so the NOWAIT pass of §17 pays nothing), with `backend_mintime`
at the select backend's microsecond. Windows compiles the same backend over wepoll, whose
`epoll_wait` is the millisecond one, and keeps it. `probe-196-wsl-ab.txt`: qb `develop`
(`67f4efb6`) against `perf/epoll-pwait2` (`9c401070`), five interleaved repetitions per cell,
60 s after the build, p50 in µs (ns per pass for `io-pass`), medians of the five:

| cell | control | candidate |
|---|---:|---:|
| timer 100 µs, `latency` 100 µs | 1011 | **59.7** |
| timer 100 µs, `latency` 1 ms | 1010 | **59.5** |
| timer 100 µs, `latency` 10 ms | 1010 | **59.5** |
| timer 100 µs, `idle_spin` 0 | 960 | **59.0** |
| timer 1 ms, `latency` 1 ms | 110 | **60.0** |
| timer 5 ms, `latency` 1 ms | 357 | **59.7** |
| timer 100 µs, `latency` 0 (spinning) | 0.1 | 0.1 |
| `parked-io-wake 1000 2000` | 37.8 | 38.0 |
| `parked-io-wake 1000 200` (polling) | 30.8 | 30.2 |
| `parked-io-wake 100 2000` | 37.7 | 38.0 |
| `io-pass timer` (NOWAIT pass, one far timer) | 25.62 | 25.56 |
| `io-pass pass` (NOWAIT pass, one quiet socket) | 27.62 | 27.70 |

Every timer cell lands on the same 59–60 µs whatever `latency` and `delay` asked, with p90 65–73
and the five repetitions within 1 µs of each other: the wait is honoured to the duration asked
and the lateness that remains IS the 50 µs slack plus a pass. The socket wake, the polling cell,
the spinning control and both NOWAIT passes do not move. What a qb server gets from it on Linux:
a `setLatency` under a millisecond means what it says (a core parked at 100 µs re-checks every
~150 µs instead of every millisecond), and a keep-alive, a retry or a poll timer under a
millisecond on an otherwise idle core fires when due instead of at the millisecond. Pinned by
qev's `test_epoll_ns_wait` (a blocking run over a 200 µs timer, the best of twenty rounds under
800 µs: 257 measured; its negative control with the nanosecond path switched off fails at 1058)
and by qb's `core-park-wake`
(`ASubMillisecondTimerFiresUnderAMillisecondOnACoreParkedInItsLoop`, the same bound through the
core's own park, a SKIP under io_uring and wherever the call is missing).

Three levers were looked at and left, each with its reason recorded: `prctl(PR_SET_TIMERSLACK)`
on the core thread would take the 60 down toward 10 µs, but it is a per-thread policy a server
should set knowingly rather than a framework default (a tighter slack is a busier CPU on every
sleep in the process's threads that inherit it) — a knob, if ever, not a change; io_uring's wait
is already a timespec but keeps libev's 1 ms `backend_mintime`, unmeasured here and the recorded
gap of this section; and Windows' millisecond, where the wait primitive underneath
(`GetQueuedCompletionStatusEx`) takes milliseconds and the kernel coalesces on top — a
high-resolution waitable timer could halve the floor at the cost of a second wait object and a
restructured wepoll wait, for a platform whose contract is now stated honestly instead.

### 19.5 The idle cadence — what `setLatency` under a millisecond really sleeps (QB-48)

The tuning guide's fourth QB-48 fact was written from the code ("MSVC rounds `wait_for` to the
millisecond and hands it to `SleepConditionVariableSRW`, so a `latency` under 1 ms parks at the
15.6 ms tick"), and §19.2 had just shown what such a reading is worth. `tools/probes/parked-cadence.cpp`
(`qvoprobe-parked-cadence`) asks the plain question: one core, one actor with a `LoopEvent` callback,
`setIdleSpin(0)` so that every wake is exactly one pass (a wait that returns with nothing to do parks
again on the next pass), wakes counted over 2 s — through the CONDITION-VARIABLE park (no io watcher:
`std::condition_variable::wait_for`) and through the LOOP park (a far `async::callback` on the core:
`ev_run(EVRUN_ONCE)` capped at `latency`). Mean sleep between two looks at the mailbox, three runs
per cell, both hosts on qb `develop` `70354b35` (`results/<host>/qb-196-park-cap/parked-cadence.txt`):

| `setLatency` | Windows cv park | Windows loop park | WSL2 cv park | WSL2 loop park |
|---|---:|---:|---:|---:|
| 100 µs | 1.46–1.53 ms | 1.42–1.44 ms | 163 µs | 162 µs |
| 1 ms | 1.79–1.83 ms | 1.41–1.44 ms | 1.07 ms | 1.07 ms |
| 10 ms | 10.5–10.6 ms | 10.3–10.4 ms | 10.05 ms | 10.05 ms |

On Windows both parks take whole milliseconds (MSVC's `wait_for` rounds up, wepoll's `epoll_wait`
takes an `int`) and the kernel adds 0.3–0.8 ms of coalescing: `setLatency(1us)` and `setLatency(1ms)`
sleep the same 1.4–1.8 ms, and 15.6 ms appears nowhere. On Linux both parks honour the value to the
50 µs slack plus a pass — the condition variable always did (`pthread_cond_timedwait` takes an
absolute nanosecond deadline), the loop park since §19.4. That is the sentence
`readme/6_guides/performance_tuning.md` now carries, with this table.

## 20. The footprint — what N cores hold, and when they hold it (QB-63)

QB-63 (`results/<host>/qb-63-footprint/`, the `pipe-footprint` probe) was filed from a 3.1 reading
of the tree: "the send pipe grows by doubling and never shrinks; 22.5 MiB at rest on 8 cores;
`MaxCores = 256`, so a large engine costs gigabytes at rest". QB-43 (§9, the segmented pipe) had
already changed the first half — a pipe allocates nothing before its first push, a core's pool grows
in 2 MB slabs to its high water, `shrink()` hands idle slabs to the process-wide `slab_cache` and
`trim()` returns them to the OS — and the probe measures what is left of the second.

`tools/probes/pipe-footprint.cpp` (`qvoprobe-pipe-footprint`): N cores, one actor each, settled at
`latency` 100 µs, the process's resident set and private commit read from the main thread at four
moments — before `Main::start()`, once the engine is idle after the traffic, after `stop()` +
`join()`, after the `Main` object is destroyed — in three modes: `idle` (no event), `broadcast`
(core 0 broadcasts once, N pipes used), `mesh` (every actor pushes once to every other, N × (N − 1)
pipes used). Each line carries the model it is read against: `N² × 64 KiB` of mailbox rings (one
SPSC ring of `MaxRingEvents` buckets per producer core in every mailbox, value-initialised) plus
256 KiB per pipe that carried an event. Both hosts on qb `develop` `c15d9d9d`, 2026-09-09, resident
growth over the baseline (Windows: private commit in brackets where it differs):

| cores | idle | broadcast | mesh | destroyed, mesh |
|---|---:|---:|---:|---:|
| 8 | 4.1 / 5.1 MiB | 6.2 / 5.2 [7.1] MiB | 20.3 / 5.4 [21.0] MiB | 16.3 / 1.2 [16.7] MiB |
| 32 | 65 / 68 MiB | 74 / 68 [77] MiB | 324 / 72 [325] MiB | 258 / 4.6 [257] MiB |
| 64 | 263 / 266 MiB | 279 / 266 [283] MiB | 1.26 / 0.28 [1.26] GiB | 1.00 / 0.02 [1.00] GiB |
| 128 | 1.02 / 1.02 GiB | 1.05 / 1.02 [1.06] GiB | 5.02 / 1.09 [5.04] GiB | 4.01 / 0.06 [4.01] GiB |

(WSL2 / Windows; the WSL2 figure is `VmRSS`, Windows `WorkingSetSize` with `PrivateUsage` in
brackets.) Three readings:

- **At rest the cost is the rings, and it is resident from `Main::start()`.** `N² × 64 KiB` within
  4 % on both hosts at every N: 4 MiB at 8 cores, 1 GiB at 128, ~4 GiB at the 256 `MaxCores` allows
  (arithmetic; not measured). The mailbox value-initialises every ring (`std::vector<Producer>(n)`,
  a `std::array<EventBucket, 1024>` per producer), so every page is written before the first event.
- **With traffic the cost is 256 KiB per pipe that ever carried an event**, up to `N × (N − 1)`; on
  Linux the slab a segment comes from is populated when it is mapped (§9's `MADV_POPULATE_WRITE`,
  chosen so that a burst pays no page faults), so the memory is resident the moment the pipe grows
  — 5 GiB for a 128-core mesh that exchanged one event per pair; on Windows the slab is committed
  and becomes resident page by page as events land (1.09 GiB resident, 5.04 committed).
- **When the cores stop, the rings are freed and the slabs go to the cache**: 4 GiB of the 128-core
  mesh stay mapped after `Main` is destroyed, on both hosts, warm for the next engine; nothing calls
  `slab_cache::trim()` for you.

**What was decided.** The premise ("never returned") is answered by §9's design: the cache is the
return path, and `trim()` the lever for a process that wants its memory back; the readme's cost
table now says what the probe measured instead of the 3.1 figure. The one mechanism worth an issue
of its own is the rings' eager touch: default-initialising the producer array instead of
value-initialising it would leave a ring's pages untouched until its producer writes them — a
128-core engine at rest at ~64 MiB (one page per ring for the indices) instead of 1 GiB, and no
1 GiB memset at start — at the price of moving those page faults to the first ~1 000 events of each
(producer, consumer) pair, exactly the trade §9 refused for the segments because the faults landed
inside a measured burst. It is a decision for the backpressure axis (QB-53), not a quick win: filed,
with these figures, not done.

### 13.7 The ask frame (QB-212, point 2 → QB-214): `qb::ask` as an awaitable in the caller's frame, measured

Point 2 of QB-212 named "the ask frame" as where `bank-transaction` still lost. The registry rework measured
nothing (2026-09-17, both hosts: bank censuses overlapping, the probe unchanged), which isolated the remaining cost
of a `co_await qb::ask` to the `task<E>` coroutine wrapped around an awaiter that already did all the work: a pooled
frame allocated and freed per ask, the initial suspend and the symmetric transfer into it, a `co_return` moving the
64-byte reply into the promise's `variant`, the final suspend and the transfer back, a second move out of the
`variant`. QB-214 (qb `91276be0` on `perf/ask-frameless`) makes `qb::ask` return the exchange itself as an
awaitable — `ask_operation<E>` / `ask_emplace_operation<E, Args...>`, a prvalue built into the awaiting frame that
engages the `ask_awaiter` in place at `await_suspend()`; lazy, cancel-aware, implicitly convertible to `task<E>`
so every existing shape keeps compiling.

Control `7296ac8d`, candidate `91276be0`, one quiet session per host, same-length executable paths, both sides on
the harness's emplace idiom (see the harness note below); full tables in
`results/<host>/qb-branch-perf-ask-frameless/README.md`:

| instrument | WSL2 g++-14 | Windows MSVC 19.51 |
|---|---:|---:|
| probe `ask`, ns per round trip (median of 7, alternated) | 45.2 → 35.6 (−21 %) | 64.0 → 48.5 (−24 %) |
| the ask mechanics above a bare push/reply | 21.5 → 12.4 ns (−43 %) | 32.8 → 16.1 ns (−51 %) |
| `bank-transaction` 1c-spin (census ×12, ns/unit) | 136.2 → 97.2 (−29 %) | 228.8 → 161.4 (−30 %) |
| `bank-transaction` 1c-park | 141.0 → 98.5 (−30 %) | 231.9 → 162.5 (−30 %) |
| `bank-transaction` 2c-spin | 82.3 → 72.7 (−12 %) | 142.0 → 106.1 (−25 %) |
| `bank-transaction` 2c-park | 83.5 → 73.2 (−12 %) | 142.7 → 108.8 (−24 %) |
| `dev/bench` ask-roundtrip same-core / cross-core | 5.5 → 5.1 ms / 14.5 → 14.3 ms | — |
| anchors (ping-pong, fib, counting 1c) and the bimodal cells re-censused | flat | flat |

Every bank distribution is disjoint (control minimum above candidate maximum) on both hosts. The 2c cells gain
less on WSL2 than on Windows because there the round trip is dominated by the cross-core hop, not by the ask's own
mechanics; on Windows, where MSVC's coroutine code is the slower half, removing a frame and two transitions per ask
is worth as much on two cores as on one.

**The harness note — a trap worth its own paragraph.** The first pass measured the candidate +17 % on every
bank-transaction config, distributions disjoint the wrong way. `frameworks/qb/savina/bank-transaction.cpp` detected
the emplace-ask idiom with a concept keyed on the exact return type (`-> std::same_as<task<Deposit>>`); the
candidate's `qb::ask` returns an operation, the concept went false, the code fell back silently to the by-value form
and `deposit()` wrapped it in the `task<Deposit>` it promised: two 64-byte copies and a frame more than the control.
The harness now detects by callability and returns what `qb::ask` returns (`85e4277a`); the ask-free anchors were
flat in both passes, which is what pointed at the harness rather than at the framework. A version-detection concept
must never constrain on the exact type an API returns.

### 13.8 The deque tax (QB-215): `qb::growable_ring` under the coroutine layer, measured

The ask-cost probe's `stream` mode was the tell: 69 ns per chunk on Windows against 31 for a bare push, while WSL2
sat at 24 vs 24 — a factor of two on one platform over identical code is a data structure, not codegen. MSVC's STL
packs `sizeof(T) <= 1 ? 16 : <= 2 ? 8 : <= 4 ? 4 : <= 8 ? 2 : 1` elements per deque block (`<deque>`,
`_Deque_val::_Block_size`): one 64-byte event per block, two coroutine handles per block, a heap allocation and a
free per chunk or per park; libstdc++ packs 512 bytes per block. qb's own benchmarks confirmed it: the sync
primitives with 64–512 parked waiters and the channel's try-send / try-recv ran 1.3–2× behind g++ where the
deque-free cells sat at the ordinary MSVC ratio. QB-215 (qb `6632987e` on `perf/stream-ring`) replaces every
`std::deque` of the coroutine layer with `qb::growable_ring<T>` (`qb/src/qb/system/container/growable_ring.h`): a
power-of-two ring over storage aligned for `T`, doubled when full with the elements moved, one allocation per
doubling and none per element, pointer cursors, deque-shaped names — `ask_stream`'s chunk buffer, `channel<T>`'s
value buffer and its three waiter lists, the waiter lists of `semaphore`, `async_mutex`, `async_rw_lock` and
`async_event` (`barrier` and `async_latch` already kept a `std::vector`).

Control `6712ef30`, candidate `6632987e`, one quiet session per host, same-length executable paths; full tables in
`results/<host>/qb-branch-perf-stream-ring/README.md`:

| instrument | WSL2 g++-14 | Windows MSVC 19.51 |
|---|---:|---:|
| probe `stream`, ns per chunk (median of 7, alternated) | 24.67 → 24.65 (−0.1 %) | 72.25 → 36.26 (−49.8 %) |
| probe `push` / `ask` (no ring on the path) | −1.5 % / −0.2 % | +1.7 % / +2.4 % (placement; the harness censuses below are flat) |
| `BM_Sync_AsyncMutex` coros 8 / 64 / 512 | +0.5 / +2.8 / +0.8 % | −17.1 / −24.6 / −22.9 % |
| `BM_Sync_RwLock_Write` coros 8 / 64 / 512 | +1.2 / +3.2 / +2.0 % | −16.3 / −22.1 / −21.5 % |
| `BM_Sync_Semaphore_Contended` coros 8 / 64 / 512 | −1.2 / −1.4 / −0.4 % | −2.2 / −0.1 / +1.7 % |
| `BM_Sync_Latch` arrivers 8 / 64 / 512 (a `std::vector`, untouched) | +3.1 / +2.6 / +1.0 % | +1.3 % at 512 |
| `BM_Channel_TrySendTryRecv` messages 64 / 1024 / 8192 | −4.6 / −27.1 / −30.3 % | −24.7 / −24.9 / −16.0 % |
| `BM_Channel_SendRecv` messages 64 / 512 / 2048 | +0.5 / −1.5 / −1.3 % | −30.1 / −9.6 / −5.6 % |
| `bank-transaction` 1c-spin / 2c-spin (census ×12, the ask path) | — | +0.4 % / −1.6 %, overlapping |
| ping-pong 1c-spin / fib 1c-spin (census ×8) | — | −1.0 % / −1.4 % |

Three things the table says. On Linux the sync primitives sit inside the ±3 % band the untouched latch cell draws
on the same binary — a ring and a 512-byte-block deque cost the same per park, as they should — while the channel's
try-send / try-recv loop gains −27 / −30 % past 64 messages, where libstdc++'s deque starts walking its block map
on every push and pop (a two-level indirection) and the ring keeps bumping a pointer. On Windows the mutex and the
rw-lock gain a fifth to a quarter from 8 parked coroutines up and the channel at every size; the semaphore, whose
list held the same 8-byte element as the mutex's, is flat on both hosts — an observation this run does not explain
and the text does not guess at. And the cells that use no ring drift: `BM_Generator_MapFilter` +5 to +8 % on
Windows (flat on Linux), `BM_Stream_MapCollect` +5 to +7 % on Linux (−0.4 to −4.4 % on Windows), each systematic
across its three passes, each on code the diff never touched and whose twin cell on the same machinery sits flat —
the placement of a rebuilt binary, recorded as such in both READMEs rather than netted out.

**The lesson that cost a pass.** The first shared ring addressed its slots by index (`_buf[(_head + i) & _mask]`):
identical on Windows, +21 to +33 % on `BM_Channel_TrySendTryRecv` under g++ — five member loads and a mask per push
against a deque's two-pointer cursor. Replacing a container that a libstdc++ deque already served well has to match
the deque's instruction budget, not merely its allocation count: the pointer-cursor ring (`_head`, `_tail`, `_end`;
a construct, one compare, one increment) is what both hosts were then measured with. A candidate is measured on the
platform where it is NOT expected to win before it is believed on the one where it is.

### 13.9 The final candidate on the two arm64 hosts (2026-09-19) — macOS and a Linux guest, and the two cells the arena costs

§13.6 – §13.8 measured the four changes that landed after the release candidate of §13.5 — the actor
arena (QB-212), `pin_frame_copy` (QB-213), the frame-free `qb::ask` (QB-214), `qb::growable_ring`
(QB-215) — on the two x86-64 hosts that are one machine. On 2026-09-19 the candidate that carries all
four, qb `develop` **`174e515a`**, was measured on the two hosts that were not there: the macOS
M4 Pro (`results/macbook-m4pro-macos-clang21/`, AppleClang 21, unpinned) and, an hour later, a
native-arm64 Linux guest on the same machine (`results/utm-debian13-arm64-g++14/`, UTM / QEMU,
Debian 13, g++ 14.2, vCPUs 2 and 4). Same protocol on both, §13.5's with one leg more: (A) the
candidate, `--only qb`, 9 + 2; (B) shipped 3.1.0; (B′) **`f2779605`**, the control for the four
changes; (A2) the candidate again; (C) every framework, 132 cells, a fresh manifest; (D) the field
census (ping-pong and thread-ring, two cores, qb / CAF / floor, 12 interleaved launches of 3 + 1) and
a second census of candidate / `f2779605` / shipped on the two-core cells of ALL eight shapes — on
an unpinned host, and on a guest whose vCPUs float, a two-core grid cell is one launch. 132 / 132
cells verified on each host (2 declared `n/a`), 0 unverified launch in 2 × 696. The whole tree had
passed its macOS validation immediately before (Huly QB-44).

**Against 3.1.0, in the same session, no cell is slower on either host**: geometric mean of
candidate / shipped 0.24 (macOS) and 0.23 (the guest); the guest's two park cells that cross a core
per message read 29.56 µs → 0.17 µs and 14.81 µs → 0.08 µs — the WSL2 result (§13.5) on another
hypervisor and another architecture, because the guest's floor is a hypervisor's too
(`baseline__2c-park` 20.8 µs a round trip; macOS's condition variable reads 4.42). **Against the
field qb is the fastest framework in all 64 cells**; qb / best rival is 0.105 (macOS) and 0.139 (the
guest) in geometric mean, the narrowest cell on both is ping-pong 2c-spin (0.54 and 0.57, against
CAF), and qb is below the raw-thread floor in 14 and 15 of the 16 two-core cells — the exceptions
are fib (47 / 44 against 32 / 30 on macOS, 50 against 32 on the guest).

**The field census** (median of the twelve launch medians, [min … max], ns per unit):

| cell | qb | CAF | floor |
|---|---|---|---|
| ping-pong 2c-spin, macOS | **196.4** [181.2 … 213.2] | 386.6 [382.4 … 391.3] | 237.7 [210.3 … 250.2] |
| ping-pong 2c-park, macOS | **203.9** [177.8 … 219.0] | 388.0 [376.9 … 398.9] | — |
| thread-ring 2c-spin, macOS | **91.6** [79.7 … 96.7] | 191.9 [184.6 … 199.9] | 120.0 [99.1 … 140.0] |
| thread-ring 2c-park, macOS | **88.7** [78.0 … 99.7] | 189.9 [185.7 … 198.5] | — |
| ping-pong 2c-spin, arm64 guest | **172.0** [161.7 … 180.5] | 297.4 [291.0 … 307.7] | 207.1 [183.9 … 224.3] |
| ping-pong 2c-park, arm64 guest | **178.7** [161.1 … 189.6] | 297.3 [293.1 … 302.7] | — |
| thread-ring 2c-spin, arm64 guest | **83.4** [71.5 … 95.0] | 143.7 [141.1 … 148.7] | 113.5 [94.0 … 130.2] |
| thread-ring 2c-park, arm64 guest | **83.1** [77.5 … 87.3] | 144.5 [142.1 … 147.2] | — |

Under the floor on all four spin cells, where the two x86-64 hosts read "on it or under it".

**The four late changes, against `f2779605`, carry over to arm64 with their signs and their sizes.**
fib at one core 90.7 → 69.9 (−23 %) on macOS and 89.0 → 80.9 at the guest's census (−9 %), at two
cores by census 60.3 → 46.7 (−22 %) and 62.5 → 50.0 (−20 %): the arena removes a `malloc` / `free`
pair per actor on a platform whose allocator is fast and whose thread-local access is a call.
bank-transaction at one core 100.6 → 82.0 (−18 %) and 94.3 → 78.6 (−17 %), at two 76.2 → 65.1 and
73.0 → 63.9; the ask-cost probe reads **ask 46.08 → 37.44 ns (−18.8 %) on macOS and 51.69 → 37.49
(−27.5 %) on the guest**, push level on both. Every other two-core cell is level on macOS, and every
other cell but one on the guest.

**What the arena costs, found here because these two hosts were not there when it was measured.**
Two cells, both small, both attributed by a census over builds along `f2779605..174e515a` and over
two scratch variants of the arena commit `a134ccd6` — `global-new`, which keeps the commit whole
and routes `qb::Actor`'s four class-level operators to the global allocator, and `granule64`, which
starts every actor on its own cache line:

| cell (12 launches of 5 + 1; chameneos 24 of 3 + 1) | `f2779605` | `a134ccd6` (arena) | `global-new` | `granule64` | `174e515a` |
|---|---|---|---|---|---|
| thread-ring 1c-spin, macOS | 17.2 [16.8 … 17.5] | **18.4** [18.1 … 18.7] | 17.2 [16.6 … 17.2] | 18.4 [18.0 … 18.7] | 18.3 [18.0 … 18.7] |
| fib 1c-spin, macOS | 84.8 [82.9 … 100.3] | **70.1** [69.0 … 72.9] | 86.0 [82.4 … 91.2] | 70.6 [69.3 … 73.8] | — |
| thread-ring 1c-spin, arm64 guest | 24.8 [21.0 … 25.0] | 24.1 [23.9 … 25.4] | 24.8 [24.6 … 25.0] | — | 24.1 [24.0 … 26.0] |
| chameneos 2c-park, arm64 guest, launches ≥ 58 ns of 24 | 1 | **12** | 6 | — | 9 |
| chameneos 2c-spin, arm64 guest, launches ≥ 58 ns of 24 | 6 | **9** | 4 | — | 7 |

- **thread-ring at one core on macOS, +1.2 ns a hop (+7 %)**: the step is at the arena commit, nothing
  after it moves the cell, ping-pong and big (the two other static shapes) are flat over the same
  five builds, and `global-new` takes it away — so it is where the hundred actor objects LIVE, not
  the code around them, and `granule64` says it is not their alignment. What is left is the
  neighbourhood: under the system allocator an actor object sits beside what its constructor
  allocates, in the arena it sits in a chunk of its own, and a ring that visits a different actor
  every 18 ns pays for the second stream. The guest, on glibc, does not show it (24.8 → 24.1).
- **chameneos at two cores on the guest: the same fast mode (43 – 45 ns a meeting), a slow mode
  visited more often** and deeper (to ~100 ns against 66) with the arena; medians overlap. macOS
  does not show it (41.8 against 40.8 by census), nor did the two pinned hosts (§13.6). Attributed,
  not explained: the guest has no `perf`.

Neither is a regression against anything shipped — the two cells are 18.6 against 3.1.0's 42.2, and
45.7 – 66.9 against 81.7 – 84.0 — and the arena's gain is a quarter of an actor's lifetime on four
hosts; both are recorded with their instrument under each host's
`qb-branch-develop/bisect-f2779605-174e515a/`, and the design question they share (the actor's
satellites in the arena too) is Huly's. **And one the ring costs**: on libc++ the stream chunk reads
19.87 → 21.16 ns (+6.5 %; the five-build probe puts the step at `174e515a`) — libc++'s deque packs
4096 bytes a block and never paid the tax §13.8 removed on MSVC; on the guest's libstdc++ it is
level (19.82 → 19.79). §13.8's last sentence, applied to a third standard library.

**The parked timer, the one kqueue question QB-196 left**: `qvoprobe-parked-timer-wake 1000 100
2000` — a 100 µs timer on a core parked at `setLatency(1 ms)` — reads lateness p50 **16.8 µs** (p99
22.7, max 35.5) on kqueue, which takes a `timespec`, and **55.4 µs** (p99 67.5) on the guest's
`epoll_pwait2`, the thread's 50 µs timer slack plus the wake: §19.4's figure on another kernel.
