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
