# `caf-detached` — CAF with every actor on a private thread

The same adapter as [`frameworks/caf/`](../caf/), compiled with `QVO_CAF_DETACHED`, so every
`sys.spawn(...)` becomes `sys.spawn<caf::detached>(...)`. Nothing else differs: same messages,
same behaviors, same checksum, same harness. The sources here are one-line wrappers that
`#include` the plain adapter, so the two variants cannot drift apart.

## Why a second CAF row exists

CAF's work-stealing scheduler delivers a message sent **from a worker thread** by prepending the
receiver to the **sender's own queue** (`work_stealing::worker::delay` → `queue.prepend`, reached
from `scheduled_actor::enqueue`). The sender's worker then runs the receiver next, unless another
worker steals it first — and on a two-actor ping-pong nothing else is ever runnable, so the
receiver runs on the sender's thread every time. CAF's "2 cores" ping-pong is a **same-core
hand-off**: no cache-line transfer, no wake-up, no cross-core anything. That is CAF's design —
locality is the point of work stealing — and the plain `caf` row is the right idiomatic figure.

But it is not the same measurement as a framework whose two actors sit on two pinned cores and
pay a real cross-core hop per message. A reader comparing the `caf` row to the `qb` 2-core row
would be comparing a one-core hand-off to a two-core one and reading the difference as
architecture. `caf::detached` is CAF's own, documented way to give an actor a thread of its own
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

This directory's `savina/` holds one file. The other four Savina benchmarks (`counting`,
`thread-ring`, `fork-join`, `big`) are **not** given a detached row, and the reason is the one the
row exists for: `caf::detached` is CAF's placement primitive for *one actor that deserves a
thread*, and the variant borrows it to make a two-actor exchange cross a core. On a hundred ring
actors, sixty workers or a hundred and twenty all-to-all actors it would create that many OS
threads over two CPUs, and the cell would measure the kernel's scheduler switching between them —
a configuration CAF's own documentation steers away from and nobody would deploy. `counting` has
only two actors, but its question (single-producer mailbox throughput) is answered by the pool row
at `cores=2`: the producer never yields its worker while it streams, so the idle worker steals
the counter and the mailbox between them is a real cross-thread queue for the whole window; the
detached row would add nothing but a second number to explain. The plain `caf` row is the right idiomatic figure for all four, and the per-benchmark
docs under `benchmarks/savina/` say what its scheduler does with the same two-CPU budget.

## What was tried before this and why it was not enough

The first attempt at "CAF cross-core" was a spin profile on the pool
(`caf.work-stealing.aggressive-poll-attempts` / `steal-interval`), swept in `docs/TUNING.md` §1.
Every profile more aggressive than CAF's defaults was **slower**, because polling harder only makes
the idle worker steal the receiver away from the sender's warm cache — CAF's fastest ping-pong is
the one where nothing ever crosses a core. A knob cannot produce the cross-core cell; only a
placement primitive can, and this variant is CAF's.
