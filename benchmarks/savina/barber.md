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
(eight with a paced factory), and above all **whether production overlaps haircuts when there are
two cores**: a framework
that lets the room and the barber run while the factory is still producing finishes in roughly the
time of the longer side, one that does not pays both sides one after the other.

It does **not** measure the full room at its default parameters — see the deviation below: nobody
is turned away when the room has a seat per customer, so the `Full` / `Returned` path is
implemented and verified (a small `room` exercises it) but not timed. It does not measure fairness
either: the room is FIFO by construction, in every implementation.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `haircuts` | 5 000 | 5 000 | no deviation; 5 000 customers created, served and ended per repetition |
| `room` | 5 000 | 1 000 | **deviation — see below**; one seat per customer |
| `apr` | 1 000 | 1 000 | no deviation; `uniform[0, apr) + 10` busy-work iterations after each customer |
| `ahr` | 1 000 | 1 000 | no deviation; `uniform[0, ahr) + 10` busy-work iterations per haircut |
| `cores` | 2 | n/a | factory and customers on one core, room and barber on the other, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's room, and why

The reference was written for a many-threaded runtime, where the factory's and the barber's
busy-works race in wall time and, with `apr = ahr`, the room seldom fills. On ONE thread there is no
race: a runtime orders the factory's and the room's turns by its queues, the factory gets ahead,
the room fills, and every customer turned away goes back to the factory and is sent again at once —
so the run becomes customers bouncing off a full room until the barber drains it, and **how many
bounces is decided by each framework's scheduling order, not by the problem**. Measured at Savina's
own parameters (`haircuts=5000 room=1000 cores=1`, one repetition each): qb and the floor turned
customers away **1 500 500** times, SObjectizer **5 334 667** times, CAF **4 000** times — the same
problem with a thousandfold different amount of work, which no ratio between the cells could mean
anything about. A one-thread FIFO model of the protocol predicts the first two exactly (1 500 500
for a factory that sends one customer per turn, 5 334 667 for the reference's factory that sends
them all at once); CAF's scheduler happens to turn each customer away once. With two cores, at
1 000 seats, qb, SObjectizer and the floor turned nobody away and CAF 4 000.

So the default room has a seat per customer and the default run is the same amount of work in
every framework. Every implementation still implements the full room: run with a small `room` and
every cell verifies (measured at `room=5`, and up to 333 667 rejections in one cell) — those cells
are not comparable between frameworks, for the reason above, and are not published.

### The factory's shape, per framework

The reference's factory sends every customer from ONE handler, busy-working between them. CAF and
SObjectizer keep that shape: a mail or a send is queued at its receiver at once, so the room starts
while the factory is still producing, and a factory paced by one self-addressed Start per customer
measured slower in both, at both core counts. **qb paces itself**: a qb push to another VirtualCore
is published by the pass's flush, after the handler returns, so a looping factory released all its
customers only when production ended — measured, its two-core cell took as long as the one-core
cell, the barber idle until the factory finished — and a producer written as one short handler per
item is qb's idiom. The floor paces too: measured level with the loop, and a worker inside a loop
drains none of its inbound rings, which on the floor's bounded rings is a hang at a large enough
`haircuts`. The `n − 1` extra Starts of the two paced factories are in their reported message
counts.

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
haircuts, `r` rejections and `a` wake-ups of the barber (`n − 1` more for the paced factories), and
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

A qb handler that runs long holds every push it makes to another core until it returns: the pass
flushes the outgoing pipes after the handlers, and there is no primitive to publish them from
inside one. That is qb's batching design and its documented idiom says to keep handlers short, so
it is recorded here as a characteristic of the shape, not as a defect: a producer that busy-works
between items is written as one event per item, at the price of one self-addressed event each.
