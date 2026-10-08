# savina/barber

Savina's Sleeping Barber, one of the "concurrency" benchmarks (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `SleepingBarberConfig.java` and
`SleepingBarberAkkaActorBenchmark.scala`) — a factory creates `haircuts` customers, each an actor,
and sends them to a waiting room of `room` seats; a barber who sleeps when the room is empty and is
woken by the first arrival serves them one at a time. Factory and barber both busy-work between
messages.

## What it measures, and what it does not

It measures **a producer and a consumer coordinated through a bounded buffer, with dynamic actor
creation on the producer's side**: 5 000 customers are created inside the window, each told two or
three things, each reporting once and dying, while the factory busy-works the production delay and
the barber the haircut. The busy work is the harness's `qvo::spin_work` — the same object code for
every framework, the production and the haircut equal on average — so what differs between the
frameworks is the cost of creating and ending a customer, of the seven messages each one takes
(eight with `pace=1`), and above all **whether production overlaps haircuts when there are two
cores**: a framework that lets the room and the barber run while the factory is still producing
finishes in roughly the time of the longer side, one that does not pays both sides one after the
other.

**The table cell does not time a full room.** Its room has a seat per customer (the deviation
below), so nobody is ever turned away in it; the `Full` / `Returned` path is implemented by every
adapter and verified (a small `room` exercises it) but not timed in any published cell. It does not
measure fairness either: the room is FIFO by construction, in every implementation.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `haircuts` | 5 000 | 5 000 | no deviation; 5 000 customers created, served and ended per repetition |
| `room` | 5 000 | 1 000 | **deviation — see below**; one seat per customer; Savina's 1 000 is a declared side experiment |
| `apr` | 1 000 | 1 000 | no deviation; `uniform[0, apr) + 10` busy-work iterations after each customer |
| `ahr` | 1 000 | 1 000 | no deviation; `uniform[0, ahr) + 10` busy-work iterations per haircut |
| `pace` | 0 | n/a | the factory's shape, a **declared axis** — see below; 0 = the reference's |
| `cores` | 2 | n/a | factory and customers on one core, room and barber on the other, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's room, and why

The reference was written for a many-threaded runtime, where the factory's and the barber's
busy-works race in wall time and, with `apr = ahr`, the room seldom fills. On ONE thread there is no
race: a runtime orders the factory's and the room's turns by its queues, the factory gets ahead,
the room fills, and every customer turned away goes back to the factory and is sent again at once —
so the run becomes customers bouncing off a full room until the barber drains it, and **how many
bounces is decided by each framework's scheduling order, not by the problem**. Measured at Savina's
own parameters (`haircuts=5000 room=1000`, one repetition each, unpinned): at `cores=1` qb, the
floor and SObjectizer turned customers away **5 334 667** times with the reference's factory
(`pace=0`) and **1 500 500** times with the paced one, CAF **4 000** times with either — the same
problem with a thousandfold different amount of work, which no ratio between the cells could mean
anything about. A one-thread FIFO model of the protocol predicts the first two exactly (5 334 667
for a factory that sends every customer at once, 1 500 500 for one that sends one per turn); CAF's
scheduler happens to turn each customer away once. At `cores=2` the floor and SObjectizer turned
nobody away, CAF 4 000 again, and qb nobody with `pace=1` but **3 625 825** with `pace=0` — for the
reason in "What the shape found in qb" below.

So the default room has a seat per customer and the default run is the same amount of work in
every framework. Savina's `room=1000` remains a **declared side experiment**: run with
`--param room=1000`, it verifies in every cell, and its rejections and wake-ups are what it
reports (every implementation prints them after each repetition — on stderr until the harness
carries an observation field for them); it is never a table cell. Every implementation implements
the full room: run with a small `room` and every cell verifies (measured down to `room=5`, and up
to 333 667 rejections in one cell) — those cells are not comparable between frameworks either.

### The factory's shape: `pace`, a declared axis

The reference's factory produces every customer from ONE handler, busy-working between them. A
runtime can only overlap production with haircuts if what that handler sends reaches the room
while the handler is still running, and the frameworks differ on exactly that — so the shape is a
declared parameter, implemented by all four adapters and recorded in every result document:
`pace=0` is the reference's factory, `pace=1` produces one customer per self-addressed `Start`
(`n − 1` more messages, in the reported count). Following FAIRNESS.md §1.1, each adapter's table
cell runs the FASTER of its two forms as measured on the quiet host, and its `main()` says which;
the other form is publishable as a side document. Until that measurement every adapter runs the
reference's `pace=0`. The unpinned correctness runs on a busy host decide nothing about speed; they
do show what each runtime does with the shape:

- **qb**: the factory hands each customer to the room with `send<Enter>`, which publishes it into
  the room's core at once (`qb.llm.md`: "hands the event to the peer's ring at once instead of the
  pass's batched flush"); a `push` is published by the pass's flush, after the handler returns, so
  with `pace=0` it would hold every customer until production ended. With `send`, the room does
  receive its customers during production — and the run still loses the overlap past a few hundred
  customers, for the reason in "What the shape found in qb".
- **CAF**: a mail is enqueued in the room's mailbox at once, but when the room RUNS is the
  pool's decision: at the default parameters and `cores=2`, CAF woke the barber exactly once in
  every correctness run, with either `pace` — the room did not run until production had ended.
- **SObjectizer**: a send is queued at the receiver at once, and at `cores=2` the room ran during
  production in both shapes (thousands of wake-ups of the barber).
- **The floor**: a worker inside the `pace=0` loop drains none of its inbound rings. At `cores=1`
  that is safe — `Mesh::send` drains a full ring it owns inline. At `cores=2` the room's worker can
  fill the ring back to the factory's (Wait, Start and Done for every customer it handles, 65 536
  messages: about 21 800 customers served during production) and then spin on it; the loop still
  finishes as long as all its Enters fit in the ring to the room's worker. So the floor's `pace=0`
  cannot hang at `haircuts` ≤ 65 536, thirteen times the default, and the adapter refuses
  (`not_applicable`) `pace=0` at `cores≥2` above that, rather than allowing a configuration that
  could.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | everything on VirtualCore 0; a customer is an `addRefActor` child of the factory, killed after it reports | factory and customers on VirtualCore 0 (a child lives on its creator's core), room and barber on VirtualCore 1: an arrival, its Wait or Full, its Start and its Done cross the cores, the barber's Next → Enter turn stays on his core |
| CAF | `max-threads=1` | `=2`, both pinned; every customer is `self->spawn`ed by the factory and the work-stealing pool places everything |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; the factory, the room and the barber are one coop, every customer its own child coop, deregistered after it reports |
| floor | one thread, every actor in its own ring | two pinned threads: factory and customers on thread 0, room and barber on thread 1 — qb's placement, so the floor bounds the placing frameworks and not the pools |

## The verified answer

Which customers find the barber asleep — and, with a small room, which are turned away and how
often — depends on the interleaving, so the checksum is built from what every interleaving must
deliver, and every term that concerns a customer is weighted by that customer's identity
(`identity(i)`, a mix of its number; the number travels in every message that names a customer).
Customer `i` (numbered in production order) reports `mix(i) + h` plus `identity(i)` times a
weighted count of everything it was told (`Start`, `Wait`, `Full`), where `h` is the haircut it
received; the barber's `k`-th haircut is `haircut_work(k)` whoever gets it. The factory adds every
report, its own production work and, per `Returned` of customer `i`, a `Returned` weight times
`identity(i)`; the room adds, customer by customer, an `Enter` weight for each one it lets in, a
`Wait` weight for each one whose arrival wakes the barber, a `Next` weight for each one the
barber's `Next` names, and subtracts the `Full` and `Returned` weights for each one it turns away;
the barber adds a `Cut` weight for each one he serves. Each term is paired with its counterpart
FOR THE SAME CUSTOMER — every customer let in is either told `Wait` or wakes the barber, every
rejection of `i` is one `Full` at `i` and one `Returned` of `i`, the barber serves every customer
once and names it in the `Next` that follows; the naps and the room's own wake-up `Next`s cancel
by count — so the total is the same for every interleaving, and a delivery dropped, duplicated
**or delivered to the wrong customer** moves it, the no-op `Wait` included. All three are asserted
to fail on every qb and floor cell: a room that skips its 100th `Wait`, a barber that tells his
100th customer `Start` twice, and a room whose 100th `Wait` sent while two or more customers wait
goes to the oldest of them instead — the last one passed the checksum of the first version of this
benchmark, which counted what each customer was told without saying which customer.

The room leaves for `Exit` only once the barber's `n`-th `Next` is in, and a run in which it
receives an `(n+1)`-th stops with an explicit protocol failure rather than a number: a duplicated
`Next` could otherwise let `Exit` leave one haircut early. Asserted too: a barber that sends the
`Next` after his 100th haircut twice fails every qb and floor cell that way.

No message count is asserted, because it is not determined: it is `7n + 3r + a + 3` for `n`
haircuts, `r` rejections and `a` wake-ups of the barber (`n − 1` more with `pace=1`), and
what an asserted count would check is checked kind by kind by the checksum. The number reported is
the messages the actors received. Every implementation prints its rejections and wake-ups to
stderr after each repetition — measured, never asserted, as Savina itself only tracks its
"CustomerAttempts".

## The measured window

Opens when the factory, having heard from the room and the barber that they are up (outside the
window), sends itself Savina's `Start`; closes when the barber receives `Exit`, the end of the
chain factory → room → barber. The room forwards `Exit` only once it has received the barber's
`n`-th `Next`: the factory's `Exit` and the barber's last `Next` come from two senders and nothing
orders them. Every customer's creation, every production delay, every haircut and every customer's
end is inside the window. The floor's second thread is created just before it.

## What the shape found in qb

The first version of this page said qb had no primitive to publish a cross-core event from inside
a long handler. That was wrong: `send<>` is that primitive — it hands the event to the peer's ring
at once instead of the pass's flush (`qb.llm.md`, `Actor.h`'s `send`, `VirtualCore::send` →
`try_send`), and the qb adapter uses it. What this shape found is one level down.

With `pace=0` at `cores=2`, the factory's core runs one handler for the whole production and
drains none of its mailbox meanwhile. The barber's core keeps sending to the customers on that core
— a Wait or nothing, then a Start and a Done per customer, one 64-byte bucket each — and after
about 340 customers the factory core's 1023-bucket ring from the barber's core is full. From then
on every pass of the barber's core tries to flush into that full ring: the flush retries each event
up to 512 times per pass (64 spins, then `std::this_thread::yield()`, `VirtualCore.cpp`'s bounded
backoff) before bailing to the next pass, and the barber's core spends its passes there instead of
cutting hair. Measured with qb's own counters (`getCoreStats().sends_blocked` on the barber's core,
one unpinned repetition, a print added for the measurement and not committed): **94 285 blocked
sends** at `haircuts=5000 pace=0`, **0** at `pace=1`, and 0 at `haircuts=300` (857 events, under
one ring). The overlap follows the fill point: on the unpinned runs the two-core cell took 55 % of
the one-core time at 200–300 customers, 73 % at 600, 86 % at 1 200 and 99 % at 5 000 — and with
`room=1000` the throttled room turns 3 625 825 customers away where `pace=1` turns none.

The backoff exists so that two cores with full rings for each other cannot deadlock (its comment
says so), and a full ring of a peer that is merely busy is the case it was not shaped for: the
sending core yields its time slice hundreds of times per pass while its own work waits. Whether
the flush should give up sooner when the core has its own work pending is a question for qb, not
for this benchmark; it is recorded here as the shape's finding, and `pace` keeps both forms
measurable.
