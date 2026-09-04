# savina/fork-join

Savina benchmark 5 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014) — the *throughput* fork-join, where a master streams jobs at a fixed set of workers.
Not benchmark 4, the actor-creation fork-join, which spawns a worker per job and is not
implemented here (ROADMAP.md, "actor creation cost").

## What it measures, and what it does not

It measures **fire-and-forget dispatch into many mailboxes**: the master sends `messages` jobs to
each of `actors` workers, round-robin, and nothing replies per job. That is the cheapest thing a
framework can be asked to do — no reply path, no round trip, no per-message wait — with sixty
runnable workers sharing two threads, which is where the scheduler's placement policy shows.

With `work=0` (the default) a worker does one `mix()` per job and the cell is pure dispatch cost.
With `work>0` the worker spins for `work` iterations per job and the same cell becomes a
**parallel speed-up** measurement: does the framework spread sixty workers over two cores well
enough to halve the time. A document measured under `work>0` carries the value in its params and
is never mixed into the dispatch table.

It does **not** measure fan-in: the sixty `done` messages at the end are one per worker.
`counting` is the single-queue fan-in case and `big` the many-writer one.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `actors` | 60 | 60 | no deviation |
| `messages` | 10 000 | 10 000 | per worker — 600 000 jobs per repetition; no deviation |
| `work` | 0 | one `sin()` | **deviation — see below** |
| `cores` | 2 | n/a | master on core 0; worker w on core w % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's per-job work, and why

Savina's worker computes one `sin()` per job — a few nanoseconds whose purpose in a JVM benchmark
is to keep the JIT from deleting an actor whose result nobody reads. Here every job is observable
through the checksum, so nothing can be deleted, and the default is `work=0`: the cell reports
what a job costs to *deliver*, not to deliver plus compute. `--param work=N` turns the
computation back on, deterministically (`qvo::spin_work`, folded into the checksum so that a
framework which skipped it would fail verification), which is the shape Savina's own number has.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | master and all 60 workers on VirtualCore 0; a job is a same-core pipe write | master on VirtualCore 0, worker w on core w % 2: **half the jobs stay on the master's core and half cross**; the master's own core also runs 30 workers, and qb never rebalances |
| CAF | `max-threads=1` | `=2`, both pinned; the sixty workers are placed by the work-stealing pool — the idle worker steals runnable workers off the master's thread, which is the case work stealing exists for |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; a free thread takes the next agent with pending demands |
| floor | one thread; the master's own ring is drained inline when it fills | two pinned threads; worker w owned by thread w % 2, the master is thread 0 — the same split qb gets, with nothing balancing it either |

This is the benchmark where **static placement costs qb something it cannot tune away**: the
master's core does the dispatching *and* runs half the workers, so at `cores=2` qb's second core
is idle whenever the master is the bottleneck, while CAF's and SObjectizer's second thread takes
whatever is runnable. The cell records that honestly — a qb caveat says it on every document —
and `work>0` is where the difference would grow: the more a job costs, the more balancing buys.
The `cores=1` column, again, is the like-for-like dispatch comparison.

## The verified answer

Worker `a` folds `job_value(a, i, work)` into its accumulator for its i-th job — `mix((a << 32) |
i)`, plus `spin_work` of the same key when `work>0` — as a **wrapping sum**, and reports it once
in its `done`. A job delivered to the wrong worker, dropped, or delivered twice changes the total;
so does a worker that skipped the spin. `expected_messages = actors x messages + actors` (the
jobs, plus one `done` per worker) is asserted alongside.

## The measured window

Opens when the master emits its first job into an already-running system, closes when it has
received the sixtieth `done`. Every framework implementation waits for all sixty workers to report
ready before opening it, so every thread is running and every worker has been scheduled once. The
floor's master is the calling thread and its second thread is created just before the window; its
start-up, once, is below resolution at 600 000 jobs.

For qb the window contains the growth of the per-core pipes the 600 000 pushes are batched into
(see counting.md, "The measured window"); that is the price of batching a burst and it belongs
inside.
