# savina/bndbuffer

Savina's Producer-Consumer with Bounded Buffer, one of the "concurrency" benchmarks (Imam & Sarkar,
*Savina — An Actor Benchmark Suite*, AGERE 2014; `ProdConsBoundedBufferConfig.java` and
`ProdConsAkkaActorBenchmark.scala`) — a manager owns a bounded buffer between `producers`
producers and `consumers` consumers; a producer is asked for one item at a time and is **parked**,
not asked again, while the buffer is near full; a consumer takes one item at a time and tells the
manager when it is free. It is the suite's **backpressure** shape: the only thing that keeps the
producers from running ahead of the consumers is the protocol, and this page says what each
framework offers for that natively (Huly QB-53, the open qb question).

## What it measures, and what it does not

It measures **a hub coordinating 80 busy actors through a bounded buffer**: 40 producers and 40
consumers each do real work per item and talk only to the manager, four messages per item —
`ProduceData` to the producer, `DataItem` to the manager, `DataItem` on to a consumer,
`ConsumerAvailable` back — 160 080 messages for 40 000 items per repetition. The busy work is the
harness's `qvo::spin_work`, the same object code for every framework, 2 500 steps on each side of
an item at Savina's costs: **at the default parameters the work dominates the messages**, and what
differs between the frameworks is how well each keeps every core busy while one manager hands out
the work — how fast the hub answers when the actors around it are inside long handlers, and how the
81 actors are placed on the cores. With `prod_cost=0 cons_cost=0` (Savina's own branch for a cost
of zero: one step each) the same run is almost pure coordination — the manager's protocol and the
messages — which is the side experiment for the cost of keeping the bound itself.

It does not measure a framework's own flow control: the bound is the manager's protocol in every
implementation (the next section but one says why no framework's native primitive fits this shape),
so a framework is never asked to throttle anything. It does not measure fairness between producers
or consumers either: every list the manager keeps is FIFO by construction, in every implementation.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `buffer` | 50 | 50 | no deviation; must exceed `producers` (see below); the buffer never holds more than 49 |
| `producers` | 40 | 40 | no deviation |
| `consumers` | 40 | 40 | no deviation |
| `items` | 1 000 | 1 000 | no deviation; per producer — 40 000 items per repetition |
| `prod_cost` | 25 | 25 | no deviation in the value; `cost × 100` busy-work steps per item — see below |
| `cons_cost` | 25 | 25 | as `prod_cost` |
| `cores` | 2 | n/a | the manager on core 0, each kind split evenly over the cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Savina's `numMailboxes` exists only for its Habanero selector variants and has no counterpart here.

### The threshold, and the bound it keeps

The manager parks a producer when its buffer holds `buffer − producers` items or more — Savina's
`adjustedBufferSize`, 10 at the defaults — so that every producer that delivered just before the
threshold was reached still finds a slot: the buffer then holds at most `buffer − producers − 1 +
producers = buffer − 1` items, and **every implementation asserts it** (an append that leaves the
buffer holding `buffer` items or more puts a non-zero weight into the checksum, so such a run cannot
verify). Each un-parking frees one buffered item and lets one producer deliver one, so the bound
holds for the whole run. The parameter must exceed `producers`: at `buffer ≤ producers` Savina's
threshold is 0 or less, every producer is parked after every item, only a buffered item un-parks
one, and the run stops forever on the first item handed straight to a waiting consumer — the spec
refuses that configuration before anything is built.

### Deviations from the reference, and why

- **The busy work.** Savina's `processItem(cost)` runs `cost × 100` iterations of one
  `PseudoRandom.nextDouble()` and one `Math.log()` (one iteration when the cost is 0 or less).
  Here each iteration is one step of `qvo::spin_work` — the mapping savina/barber makes for one
  `Math.random()` call — so the parameter is Savina's and the work is the harness's. Savina seeds
  its generator with the cost on every call, so every item does the same arithmetic; here the work
  is seeded by the item, so it cannot be skipped without the checksum seeing it.
- **The consumer works on each item on its own.** Savina's consumer chains its items
  (`consItem = processItem(consItem + data)`), which is order-free in the reference only because
  `processItem` adds to its argument; a mix chain is not order-free, and which consumer receives
  which item is the scheduler's decision, so a chained consumer would make the answer depend on the
  interleaving. Each item gets the same amount of work, from its own value. The producer keeps
  Savina's chain (`prodItem = processItem(prodItem)`): a producer works in order, so its chain is
  deterministic, and an item cannot be produced without the ones before it.
- **The messages carry identities.** `DataItem` carries the producer's number and the item's
  index beside the value (Savina: the value and the producer's reference), `ConsumerAvailable`
  the consumer's number and its contribution to the checksum, `ProducerExit` the producer's
  receipts, and `ProduceData` — a payload-free singleton in Savina — a sequence number. They are
  what the checksum is built from (below); no message is added or removed.
- **The end of the consumers is after the window.** Savina's manager sends `ConsumerExit` to
  every consumer when it exits; here the window closes at the manager's exit decision — the
  reference's `tryExit` — and the consumers end after it (qb: the engine-wide `KillEvent`; CAF:
  `close_atom` then `quit`; SObjectizer: the environment's stop; the floor: its threads join).
  The producers end inside the window, as the reference's do — except in SObjectizer, which ends an
  agent only with its whole coop: there the producers go silent after their exit message and are
  deregistered with the coop after the window.
- **The manager's lists.** Savina keeps three `ListBuffer`s; every implementation here keeps the
  same three FIFOs, from one ring in the spec header (`qvospec::savina::bndbuffer::Fifo`), sized
  to the protocol's bound so a correct run never allocates inside the window — the container is
  the same object code in every framework's manager, never a difference between them.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the manager, every producer and every consumer on VirtualCore 0 | the manager on VirtualCore 0, producer i on core (1 + i) % 2 and consumer j on core (1 + 40 + j) % 2: each core runs 20 producers and 20 consumers, core 0 the manager besides. Every hand-over is `send<>` — published into the peer's ring at once rather than by the pass's flush, which here would come after every 2 500-step handler the pass runs; each actor has at most one message in flight to or from the manager, so the unordered primitive changes no outcome. The manager answers only between the handlers of its own core |
| CAF | `max-threads=1` | `=2`, both pinned; the manager spawns its producers and consumers, and the work-stealing pool places all 81 |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; the 81 agents are one coop |
| floor | one thread | two pinned threads, actor a on thread a % 2 (the manager 0, producers 1–40, consumers 41–80) — qb's placement, so the floor bounds the placing frameworks and not the pools |

## The verified answer

Which consumer takes which item, how often a producer is parked and how full the buffer gets
depend on the interleaving, so the checksum is built from what every interleaving must deliver:

- **every item, once**: the consumer of item `k` of producer `p` adds `identity(p, k) +
  consume(v)`, where `v` is the value at that point of the producer's chain — a dropped item misses
  its term, a doubled one counts twice, an item produced without its chain carries the wrong value;
- **every hand-over to the consumer it was handed to**: the manager adds `route(c, item)` when it
  hands an item to consumer `c`, and the consumer that receives it subtracts `route(itself, item)`
  (a mix of the consumer's number times the item's identity). The two cancel exactly when the item
  reached the consumer it was handed to — any consumer may consume any item, so the item terms
  alone could not see a hand-over delivered to another one;
- **every request to its producer**: the manager numbers the `ProduceData` it sends each producer
  from 1, and the producer adds `receipt(itself, seq)` for each it receives and reports the sum in
  its exit. A producer receives exactly `items + 1` requests — one per item and the one that ends
  it — so every interleaving gives the same sum, while a duplicated request makes the producer end
  on its `items + 1`-th receipt with one number twice and the last one missing.

The total is therefore the same for every interleaving. Asserted alongside: the message count,
`4 × producers × items + 2 × producers` (who receives which message depends on the interleaving,
how many does not), counted at the receivers. Reported beside the cell, never asserted:
`producer_waits` (how many times the manager parked a producer), `consumer_waits` (how many times a
free consumer found the buffer empty and joined the available list — each consumer does at least at
its last item) and `buffer_peak` (the most items the buffer held, at most `buffer − 1`).

What the checksum cannot see, said plainly: a duplicate of a producer's LAST request reaches a
producer that has already ended and changes nothing it did; and a request delivered to the wrong
producer starves the one it was meant for, so that run never reaches its end — a hang, which
produces no number, rather than a wrong one.

## The measured window

Opens when the manager — having heard from every producer and consumer that it is up (outside the
window) — marks every consumer available and asks every producer for its first item, Savina's
`onPostStart`; closes at the reference's `tryExit`, when the manager has every producer's exit and
every consumer available. All 40 000 productions, hand-overs and consumptions are inside it, and so
are the producers' ends, as in the reference (SObjectizer's excepted, above). The floor's second
thread is created just before it.

## Backpressure: what each framework offers, and why the buffer is the manager's here

Savina's bounded buffer is a protocol, and every implementation here keeps it as one — the
manager's FIFO and its park / un-park rule, the same code in all four adapters. What each framework
offers natively, read from its own source:

- **qb** offers no bound a program can lean on between actors. `push` and `send` always succeed
  (`noexcept`); the mailbox between two VirtualCores is a bounded ring (1 023 64-byte buckets per
  sending core, `qb::detail::max_deliverable_buckets`), but a full ring is not a signal to the
  sending ACTOR: the sending core keeps the event in its own outbound queue and retries the flush on
  its next passes, and `getCoreStats().sends_blocked` counts the attempts that found the ring full
  — observability, not control. `qb::EventQOS0` is the one event the flush may drop when the ring
  is full. `qb::io::async::channel` is bounded, with a send that suspends while it is full, but it
  is single-threaded — coroutines of one VirtualCore — not a channel between actors on two cores.
  So in qb a bounded buffer between actors is written as a protocol, exactly as here; a public
  `try_push`, soft caps or a per-pipe policy are Huly QB-53, open. This benchmark is the shape such
  a primitive would have to beat.
- **CAF**'s mailboxes are unbounded; its native backpressure is the flow API (`caf/flow`, over
  `caf::async::spsc_buffer`, a soft-bounded buffer that signals demand to its producer whenever its
  consumer takes data out). A flow implementation would replace the manager by a stream pipeline —
  a different benchmark, not implemented here.
- **SObjectizer**'s message limits (an agent's `limit_then_drop`, `limit_then_abort`,
  `limit_then_redirect` and `limit_then_transform`, `dev/so_5/message_limit.hpp`) bound an agent's
  queue by dropping, aborting, redirecting or transforming what exceeds it — overload control, not
  a producer held back — and a
  size-limited mchain can make `send` WAIT on a full chain (`make_limited_with_waiting_mchain_params`),
  which blocks the sending work thread, possibly the very thread a consumer on the same pool needs
  to drain it. Neither is this shape.
