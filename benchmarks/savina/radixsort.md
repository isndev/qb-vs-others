# savina/radixsort

Savina's Radix Sort, one of the "parallelism" benchmarks (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `RadixSortConfig.java` and `RadixSortAkkaActorBenchmark.scala`) — a
source draws `values` integers and sends them, one message each, down a **linear pipeline** of
`bits` sort stages, one per bit, into a validation actor that receives them sorted.

## What it measures, and what it does not

It measures **sustained throughput along a chain**: every value crosses every stage — 6.1 M
hand-offs per repetition at the defaults — and each hand-off is one message to the next actor of
the chain. A stage passes a value whose bit is 0 on at once and holds one whose bit is 1 until it
has received every value; then it sends what it held, in arrival order. Each stage is a stable
partition, so the chain is an LSD radix sort, and each stage that holds anything is a **barrier**:
its held values cannot leave before its last arrival. So the pipeline streams the values that pass
and moves the held ones in bursts, one per stage; what a framework pays is a message per hand-off,
the bursts' queue depth (a stage flushing ~50 000 values from one handler) and, at two cores, how
the chain's stages share the cores.

It does **not** measure sorting: a stage's work per value is a bit test and an append. It does not
measure fan-out or fan-in either — every actor has one predecessor and one successor — which is
what `filterbank.md` adds.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `values` | 100 000 | 100 000 (`N`) | no deviation |
| `bits` | 60 | `M` = 2^60 | **a re-spelling, not a deviation** — see below; the stages and the values' range |
| `seed` | 2 048 | 2 048 (`S`) | no deviation; Savina's own `PseudoRandom` |
| `cores` | 2 | n/a | the chain cut into `cores` contiguous blocks for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### `bits` for `M`, and Savina's data, kept

Savina's parameter is the maximum value `M`, and its stages test `radix = M/2, M/4, ..., 1` — a
radix sort only when `M` is a power of two, which every Savina configuration is. Here the exponent
is the parameter: `bits = 60` is Savina's `M = 2^60`, sixty stages, and a value is drawn below
`2^bits`. The source's generator is Savina's `PseudoRandom` (`value = (value * 1309 + 13849) &
65535`, the source keeping `abs(nextLong()) % M`), whose values are **16-bit**: with Savina's
`M = 2^60` only the first sixteen stages ever hold a value, and the other forty-four pass every value
on at once. That is the reference's own data and it is kept — the cell measures the chain Savina
measures: sixteen barriers, then forty-four stages of pure streaming. `--param bits=16` measures
the sixteen alone.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the source, the 60 stages and the validation on VirtualCore 0; every hand-off a `push<Value>` into the core's own pipe, the source's 100 000 from one handler | the chain cut in two contiguous halves (`radixsort::core_of`): the source and stages 0–29 on VirtualCore 0, stages 30–59 and the validation on VirtualCore 1 — **a value crosses a core once** |
| CAF | `max-threads=1` | `=2`, both pinned; every actor in the work-stealing pool, which runs a receiver on the sender's worker (`worker::delay`) unless the idle worker steals it |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; whichever thread is free takes the next demand |
| floor | one thread, its own ring | qb's placement: positions 0–30 on worker 0, 31–61 on worker 1 |

Why contiguous halves and not thread-ring's every-hop-crosses placement: a pipeline is the shape
whose natural static placement IS contiguous blocks — that is how a pipeline is parallelised — and
with actor `i` on core `i % 2` all 6.1 M hand-offs would cross a core, which `thread-ring.md`
already measures. With halves, qb's two-core cell measures pipeline parallelism through one
crossing; the pools' measures where their schedulers put sixty-two actors.

## The adapters

- **qb**: every hand-off is `push<Value>` — ordered per sender and receiver, published by the
  pass's flush after the handler. Not `forward()`: it is eager and unordered across cores (the
  answer published at once, ahead of the handler's earlier pushes, and into the pipe when the
  peer's ring is full), and a stage's output must keep its arrival order — the order IS the sort.
  A stage's `Counts` are the actor's own members, handed to its tally slot when the engine tears
  it down (thread-ring's arrangement).
- **CAF**: a value is a bare `uint64`; the actors are spawned as the reference spawns them — the
  validation, then the stages from the last to the first, each handed its successor, then the
  source. A stage holds integers.
- **SObjectizer**: a stage **re-sends the message it received** — `so_5::send(mbox, mhood_t)` for
  a value passed on, a `message_holder_t` kept for a held one, re-sent at the flush — as Savina's
  SortActor re-sends its `ValueMessage` (`nextActor ! vm`) and stores the objects themselves. No
  message is allocated after the source's 100 000. This is SObjectizer's documented redirection
  (`dev/so_5/send_functions.hpp`, 5.5.19 and 5.6.0) and the reference's semantics, not an
  optimisation chosen for this cell; the allocating form (`so_5::send<msg_value>(mbox, value)` per
  hand-off) is not implemented.
- **floor**: integers, one ring push and pop per hand-off, qb's placement — and one more thing,
  below.

**The floor's gate.** `qvobase::Mesh::send` resolves a full ring owned by the sending worker by
draining that worker's inbound rings inline, from inside the handler that is sending. That can run
another handler of the **same actor** before the first one has sent what it must send first, and
reorder the actor's outputs. This shape found it: at `cores=2` the second worker drains the first
worker's ring before its own, every value it takes from there is pushed into its own ring, and once
that ring fills, the next push drains the other ring again, from inside the handler — re-entering
the stage. Measured on Windows/MSVC before the gate: 130 000 values failed the checksum in 6
launches of 7 (a stage's outputs reordered), and 1 000 000 overflowed the stack (`0xC00000FD`).
So the floor runs one handler at a time per worker: a message the inline drain delivers while a
handler runs is deferred, in arrival order, and run when that handler returns — what an actor
runtime's mailbox does. The published B1 adapters used a local `Gate` in radixsort, filterbank and
sieve; that handoff records 130 000, 400 000 and 1 000 000 values (`bits=62`) verified at `cores=2`
in 16 of 16 repetitions each. The default cell had verified 72 of 72 repetitions without it —
the defect is one of volume. The combined candidate moves this ownership and FIFO rule into
`baseline_support.h` for every Mesh caller and removes those three local copies. The earlier
B1 outcome does not validate the shared candidate: its actual ordering control, all earlier
floor shapes, sanitizer checks and quiet-host performance comparison remain integration gates.

## The verified answer

Two parts, both fixed by the parameters (`radixsort::expected`):

- **every stage's tally** — how many values it received, their wrapping sum, how many it held:
  every stage receives all `values`, their sum is the input's, and it holds exactly the values whose
  bit `k` is set. The three are digested with the stage's index, so a tally carried by another
  stage moves the total.
- **the sequence the validation received, position by position**: an LSD radix sort of stable
  partitions is the stable sort, so the `i`-th value received is the `i`-th of the sorted input.
  Savina's ValidationActor compares each value with the previous one and adds it to a sum; here the
  validation stores it at the next position of a sequence the body owns (one store), and the
  sequence is checked whole after the window — so an order one comparison would let through, two
  values exchanged across a third, cannot.

`expected_messages = values × (bits + 1) + 1` — every stage's and the validation's deliveries and
the source's Start — is **counted** at the receivers (`radixsort::delivered`), never declared.

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). A stage
receives exactly `values` values. One **more** — a value delivered twice — fails the run at once
as a protocol violation (`radixsort::overflow`, `qvo::fail`: exit 1 and a document), at the stage
that receives it or at the validation. One **fewer** — a value lost, or sent past the next stage —
leaves that stage waiting for its last arrival: it never flushes, the validation never completes,
the run **blocks** and the harness's deadline fails it. A stage that **passes on a value it should
hold**, that **flushes its held values out of order**, or that the chain **routes around**, leaves
the sequence unsorted and its own tally wrong; a value altered in flight moves the sums and the
sequence. A **consistent misroute** — two stages wired in each other's place — leaves every tally
right (each still partitions its own bit, receives every value, holds its own ones) and sorts by
the wrong bit order: only the sequence sees it, which is why the sequence is checked whole. A
planted version of each is rejected by the harness's negative control (`tools/negative-control.py`,
section L, on `tools/control/radixsort-subject.cpp`, the consistent misroute shown passing every
tally first), which also confirms that a correct pipeline verifies. What reaches neither verdict
is a copy no handler ever runs — one still queued when the validation's last value tears the run
down.

## The measured window

Opens when the source — having heard from every stage and the validation that they are up — sends
itself Savina's Start; closes when the validation takes the `values`-th value. Every draw, every
hand-off and every flush is inside it. The sequence is reduced, and the tallies with it, after the
framework is gone. The stages' buffers are reserved for `values` before the window (Savina's
`orderingArray(numValues)`): what a stage holds is written inside the window, and the pages it
touches are faulted in there, in every framework alike. The floor has no actors to wait for: its
second thread is created just before the window.
