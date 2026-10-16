# `caf-detached` — CAF with every actor on a private thread

The same adapter as [`frameworks/caf/`](../caf/), compiled with `QVO_CAF_DETACHED`, so every
`sys.spawn(...)` becomes `sys.spawn<caf::detached>(...)`. Nothing else differs: same messages,
same behaviors, same checksum, same harness. The sources here are one-line wrappers that
`#include` the plain adapter, so the two variants cannot drift apart.

## Why a second CAF row exists

CAF's work-stealing scheduler delivers a message sent **from a worker thread** by prepending the
receiver to the **sender's own queue** (`work_stealing::worker::delay` → `queue.prepend`, reached
from `scheduled_actor::enqueue`). The sender's worker then runs the receiver next, unless another
worker steals it first. In CAF 1.1.0, another worker may take even that runnable receiver with
`try_take_tail()` (`libcaf_core/caf/scheduler.cpp`, `work_stealing::worker::delay` and
`work_stealing::worker::try_steal`). Local execution is favored, not guaranteed by the two-actor
shape. No steal trace in these historical captures establishes how often a message crossed
cores; the plain `caf` row remains the idiomatic pool measurement.

That pool row therefore does not guarantee the per-message cross-core hop of two actors fixed
to different cores. The detached variant makes that placement explicit without inferring the
pooled run's actual topology from its timing. `caf::detached` is CAF's own, documented way to give an actor a thread of its own
(`spawn_options.hpp`), which makes it the only public placement primitive CAF has; the adapter
pins each `thread_owner::pool` thread on its own rotation over the harness CPU set, so the two
actors land on two different cores.

## How to read its cells

| config | what the row means |
|---|---|
| `2c-park` | CAF's honest **cross-core, parked** cost: a `private_thread` parks on a condition variable between messages (`caf/detail/private_thread.cpp`, `await()` is an unconditional `cv_.wait()`), so every message pays a `notify` + wake-up on another core. Compare with `qb` 2c-park and the `baseline` cv floor. **It is bimodal** — see below — and the report marks it so. |
| `1c-park` | both detached threads pinned to the SAME core: the OS switches between them on every message. Not a configuration anyone would deploy; recorded because the matrix is complete or it is nothing. |
| `2c-spin`, `1c-spin` | **not applicable** — a detached actor has no spin mode. The binary calls `qvo::not_applicable()` and the harness records the reason (exit 3); the report renders `n/a` with that reason in the cell. Inventing a spin number here would mean measuring the pool under a detached label. |

## The 2c-park cell is bimodal, and the report says so rather than averaging it

Measured on both platforms, same binary, same idle host, 1 000 000 round trips per repetition:

| platform | fast mode | slow mode | split observed |
|---|---:|---:|---|
| Windows 11 / MSVC 19.51, CPUs 0 and 2 | ~0.93 µs per round trip | ~10.6 µs | 2 of 9 repetitions fast, then 7 of 9 |
| WSL2 Debian 13 / g++ 14.2, vCPUs 0 and 2 | ~3.3–3.9 µs | ~25.7 µs | 5 of 5 slow on one run, 3 of 5 fast on the next, 2/5, 3/5, 3/5 on three re-runs |

A repetition lands in one mode and stays there for its whole 10 s; the mode is chosen at the
start, not averaged over. The slow mode is the OS cost of waking a parked thread on another
core — the same ~10.6 µs / ~25.7 µs that `qb` 3.1.0 and SObjectizer's `simple_lock` pay in their
2c-park cells, and on WSL2 the raw `std::thread` + condition-variable floor itself. The fast mode
costs about what the same-core `1c-park` cell costs, which is consistent with the two threads
locking into a phase where each message lands before its receiver has reached the futex /
`WaitOnAddress` wait — the condition variable is signalled every time but never slept on. That
mechanism is inferred from the numbers, not traced; what IS measured is that the cell has two
answers and that the median of a 9-repetition sample is whichever answer won the coin toss that
run. `tools/report.py` splits any sample at a gap wider than 2× and prints both modes under the
row with a note not to quote the median; it also refuses to claim an ordering against a bimodal
cell. The published `caf-detached` 2c-park is therefore read as **"~1 µs or ~10.6 µs on Windows,
~3.5 µs or ~26 µs on WSL2"**, and never as one number.

## Only ping-pong is mirrored, on purpose

This directory's `savina/` holds one file. The other Savina shapes are **not** given a detached row, and the reason is the one the
row exists for: `caf::detached` is CAF's placement primitive for *one actor that deserves a
thread*, and the variant borrows it to make a two-actor exchange cross a core. On a hundred ring
actors, sixty workers or a hundred and twenty all-to-all actors it would create that many OS
threads over two CPUs, and the cell would measure the kernel's scheduler switching between them —
a configuration CAF's own documentation steers away from and nobody would deploy. `counting` has
only two actors. Its published single-producer mailbox-throughput cell uses the pool at
`cores=2`; the counter can be stolen by another worker, but this adapter does not record a
placement census proving a cross-thread queue for the whole window. Omitting a detached
counting row does not establish that detaching would have no effect. The plain `caf` row is the
selected idiomatic configuration for those shapes, and the per-benchmark
docs under `benchmarks/savina/` say what its scheduler does with the same two-CPU budget.

## What was tried before this and why it was not enough

The first attempt at "CAF cross-core" was a spin profile on the pool
(`caf.work-stealing.aggressive-poll-attempts` / `steal-interval`), swept in `docs/TUNING.md` §1.
The more aggressive profiles were **slower** in that recorded sweep. They change polling and
steal opportunities, but the captures do not contain a steal count that establishes the cause.
A scheduler profile does not guarantee cross-core placement; the detached variant supplies
explicit placement through CAF's own API.
