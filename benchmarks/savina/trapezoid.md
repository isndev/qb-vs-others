# savina/trapezoid

Savina's `trapezoid` benchmark of the "parallelism" group (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `TrapezoidalConfig.java` and `TrapezoidalAkkaActorBenchmark.scala`)
— the trapezoidal approximation of an integral split across a **master and a fixed pool of
workers**: the master hands each worker one contiguous range of the trapezoids, each worker
integrates its range and answers once, and the master adds the areas.

## What it measures, and what it does not

It measures **a fan-out and a fan-in of pure computation**: 100 work messages and 100 results
against 10 000 000 trapezoids, two evaluations of Savina's integrand each — a cube, a sine, a
quotient, two square roots and an exponential, 20 000 000 times. The integration kernel is the same
code in every implementation (`benchmarks/specs/qvospec/savina/trapezoid.h`, `integrate()`), so what
differs between two cells is where each framework runs the 100 ranges on the cores it was given:
at `cores=2`, whether each core gets half of them.

It does **not** measure messaging: 200 messages are a vanishing share of a cell (one per 100 000
trapezoids). Nor actor creation — the master and its workers are created before the window. Nor
load balancing under uneven work: every range holds the same number of trapezoids within one, and
the integrand costs the same everywhere on the interval, so a static placement that splits the
workers evenly splits the work evenly. It is the suite's control for its other parallelism shapes:
a cell where the frameworks are not level is a cell where one of them did not use its cores.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `pieces` | 10 000 000 | 10 000 000 | trapezoids (`N`) |
| `workers` | 100 | 100 | the pool (`W`); worker `w` on core `w % cores` where the framework places |
| `left`, `right` | 1, 5 | 1, 5 | the interval (`L`, `R`); integers here, as every qvo parameter is — Savina's two defaults are |
| `cores` | 2 | n/a | worker-thread budget |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### The partition: a deviation in rounding only

Savina's master sends worker `i` the interval `[L + i·(R−L)/W, L + (i+1)·(R−L)/W]` as two doubles
and the step `h = (R−L)/N`, and the worker recovers its count of trapezoids as
`((r − l) / h).toInt` — a truncation of a floating-point quotient, which can come out one short at
a range's edge. Here the master sends each worker its range of trapezoid INDICES,
`[⌊w·N/W⌋, ⌊(w+1)·N/W⌋)`, and trapezoid `i` spans `[i·h + L, i·h + L + h]` — the same arithmetic
as Savina's `lx = i·h + l, rx = lx + h` over a global index instead of a per-worker one. Every
trapezoid is integrated exactly once, by exactly one worker; the last bits of an `x` move with that
choice, the work does not.

### The answer: exact, where Savina's is not

Savina's master adds the 100 areas **in the order they arrive** (`resultArea += rm.result`), and
floating-point addition is not associative: its printed area moves in the last bits from one run to
the next, and no framework-free value could be asserted against it. Here the master keeps one
record per worker and, once the last result is in, folds the records **in worker order** — the same
100 additions, in an order no interleaving can change (`fold()`; Savina's own reduction moved to the
end of the protocol, inside the window). Each worker's area is `integrate()` over a range the
parameters fix, in a fixed order, and `integrate()` is compiled ONCE per binary and never inlined
(`QVOSPEC_TRAPEZOID_NOINLINE`), so the reference and every worker of a binary run the same machine
code: a framework that ran the work answers exactly the bits the reference computes, and the
comparison is bitwise. At the defaults the area reads 0.2710807519529585 on MSVC 19.51 and on
g++ 14.2; the workers' partial areas differ between the two compilers' mathematical functions in
their last bits, so each compiler has its own reference, and the four frameworks' references agree
to the bit on each (the `expected_checksum` of their documents is one value per compiler in every
functional run).

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the master and the 100 workers on VirtualCore 0 | master on VirtualCore 0, worker `w` on `w % 2`: each core integrates the ranges of its 50 workers — half the trapezoids — and the master's one fan-out handler publishes the other core's 50 work messages at the end of its pass |
| CAF | `max-threads=1` | `=2`, both pinned; the workers are pool actors, and which thread runs each one's single message is the work-stealing pool's decision |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; the master and the workers are one coop, and which thread runs each worker's single demand is the pool's decision |
| floor | one thread | two pinned threads; the master is actor 0 on thread 0 and worker `w` is actor `cores + w` on thread `w % 2` — qb's placement exactly, so the floor bounds the placing frameworks and not the pools |

The placement axis has nothing to decide here: 100 equal ranges over two cores split evenly under
every rule. At `cores=1` every framework runs the whole integration on one thread.

## The verified answer

What is compared, after the window: for every worker, its index, the results the master received
naming it, the bits of their summed area, and the work messages the worker itself received — the
worker's `Tally`, a 128-byte slot of its own (no two workers, whatever thread runs them, write one
line or one prefetched pair of lines; for qb, the count lives in the actor and reaches the slot when
the engine destroys the actor) — digested together, and the bits of the fold. `expected()` computes
the same digest from the same kernel, worker by worker, folded in the same order: one repetition's
work at one core, once per process, before the first repetition. `expected_messages = 2 × workers`
(200) is asserted alongside, counted at the receivers: every work message a worker's handler ran and
every result the master's ran. The ready handshake and the shutdown are not counted.

**What blocks, and what moves the sum** (FAIRNESS.md section 0). The master ends the run at its
100th result. A work message or a result **lost** never ends the run: the master waits for it, the
repetition outlives the harness's deadline and fails with exit 1 and no timing. A work message
delivered twice integrates its range twice — two results, twice the area — and a result delivered
twice reaches the 100th result one real answer early, with the last worker's record empty; a work
message delivered to another worker puts one range under another name, the right number of messages
and the wrong records; a worker that answers without integrating, or integrates another range, moves
its area. Each of those moves the sum, and `tools/negative-control.py` section L plants each one in a
one-thread subject that runs the protocol through the spec's own functions
(`tools/control/trapezoid-subject.cpp`) and watches the checksum reject it — and watches the results
folded in reverse arrival order verify, which is what the worker-order fold is for. The real qb and
floor adapters were planted too while this shape was written (a duplicated result, a worker that
answers 0), and rejected at `cores=1` and `2`.

**What neither sees.** A result duplicated when it is the LAST of the run lands after the master's
fold: it is counted only if the master is still there to receive it. The comparison is within one
binary — each framework's answers against the reference compiled into the same executable — and
the agreement of the four binaries' references with each other is shown by their documents, not
asserted by the harness.

## The measured window

Opens when the master has received *ready* from all 100 workers — every thread up, every worker
holding its behaviour — and sends the fan-out from one handler. Closes when the master has folded its
100th result. The shutdown — qb's `broadcast<qb::KillEvent>()`, CAF's `close_atom` to each worker,
SObjectizer's `environment_t::stop()`, the floor's `Mesh::stop()` — runs after the window, where
Savina's own timing includes its actor system's start and termination; it is measured as setup and
teardown, as for every benchmark here.

## Spin and park

Neither column has much to wait for: a core that integrates never idles until its ranges are done,
and the one wait of the run is the master's for the last result. `wait=0` measures what the cores
that finish first cost while they park; the floor's parked threads use its condition variable.
