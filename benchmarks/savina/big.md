# savina/big

Savina benchmark 7 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014). N actors, all-to-all: each pings a randomly chosen peer, waits for the pong, pings
the next.

## What it measures, and what it does not

It measures **many-writer mailbox contention**. Every actor is at once a pinger with exactly one
request in flight and a ponger for the other 119, so every mailbox is a real many-producer queue
with `actors` writers — the case `counting` (one producer) and `thread-ring` (one token) never
reach. It is also the only benchmark here with `actors` messages in flight at the same time
instead of one, so the scheduler is never starved and the two-core column measures real
concurrency rather than a hand-off.

It does **not** measure a burst: each actor sends one ping per pong it receives, so no queue
grows beyond what 120 in-flight pings can put in it. And its "random" target is a deterministic
per-actor sequence declared in the spec, so the checksum can be computed without a framework.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `actors` | 120 | 120 | no deviation |
| `pings` | 20 000 | 20 000 | per actor — 2 400 000 round trips per repetition; no deviation |
| `cores` | 2 | n/a | actor a lives on core a % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Savina's own counts are large enough here — 2.4 million round trips is 60–1200 ms per repetition
across the frameworks measured — and the target choice is the one deviation in kind: Savina's
implementation draws from a per-actor PRNG too, and this one names the generator (xorshift64
seeded from the actor index, mapped onto the *other* actors so no actor ever pings itself) so
that every framework and the expected value draw the same sequence.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | all 120 actors on VirtualCore 0; a ping is a same-core pipe write | actor a on VirtualCore a % 2: about half of all pings and pongs cross a core, and **each core's inbound queue is written by the ~60 actors of the other core** — there is one cross-core pipe per (source core, destination core), not one per actor, so the 120-writer mailbox of the spec collapses to a 2-writer pipe |
| CAF | `max-threads=1` | `=2`, both pinned; the 120 actors are placed by the work-stealing pool and every actor has its own mailbox, so a ponger's mailbox really is written by up to 119 actors on two threads |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`: one demand queue per agent, written by whichever thread runs each of its 119 peers |
| floor | one thread, one ring per (worker, worker) pair | two pinned threads; actor a owned by thread a % 2 — like qb, the per-actor mailbox does not exist and a worker's inbound ring has one writer per peer thread |

The table says what the contention cell actually compares. CAF and SObjectizer have a **mailbox
per actor** and pay for its many-writer synchronisation on every ping; qb and the floor route
every message through a **queue per (core, core) pair** with exactly one writer thread each, and
dispatch to the actor on the receiving side. That is the architectural bet of a shard-per-core
design — contention is engineered out by construction, at the price of never rebalancing — and
`big` is the benchmark on which it pays most visibly. The caveat on every qb document says so, and
the `cores=1` column, where every framework's mailboxes have one writer thread, is the
like-for-like dispatch comparison.

## The verified answer

The **ponger** computes `pong_value(pinger, ponger, k)` — `mix()` of the three packed together —
and the pinger accumulates it as a **wrapping sum** over its `pings` pongs, then reports the
sum in its `done`. A reply produced by the wrong actor, a ping delivered to a peer other than the
one the sequence chose, or a round trip delivered twice changes the total; a round trip LOST blocks
the run instead (below).

`expected_messages = 2 x actors x pings + 2 x actors` — the pings and pongs, plus one `start`
and one `done` per actor. The **pinger** counts: two per pong it receives (the pong and the ping
it proves), plus its own `start`; the sink adds one per `done`. A ponger must **not** count the
pings it receives, because peers keep pinging an actor after it has reported done and that count
depends on a race — measured on the floor, 4 726 888 and 4 747 980 of 4 800 240 on two consecutive
runs of the same binary. The spec header states the rule so that every framework counts the same
way.

**What blocks, and what moves the sum** (FAIRNESS.md section 0). Every pinger has one ping in
flight and reports after its last pong; the sink ends the run on the `actors`-th `done`. A ping, a
pong, a `start`, a `ready` or a `done` **lost** leaves the sink a `done` short: the run **blocks**,
the repetition outlives the harness's deadline and fails with exit 1 and no timing. A ping or a pong
delivered twice runs a second chain, and a `start` delivered twice or to another actor makes one
actor report twice: the run ends on a wrong sum. A pong delivered to the wrong actor depends on the
adapter: CAF, SObjectizer and the floor fold it and ping on, moving the sum; qb's actors answer
whoever pinged them (`reply()`), and the two actors bounce it between them for good — a run that
blocks while busy.

## The measured window

Opens when the sink sends the 120 `start`s into an already-running system — those starts are
inside the window and counted, because actors that start themselves have no single instant at
which a window can open — and closes when the sink has received the 120th `done`. Every
framework implementation waits for all 120 actors to report ready before opening it, so every
thread is running and every actor has been scheduled once. In qb a ping is answered by mutating
it into its own pong and `reply()`ing it — one event object per round trip, the ping-pong idiom
— and the next ping is a fresh `push<>`.
