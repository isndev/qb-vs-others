# savina/thread-ring

Savina benchmark 3 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014). N actors in a ring pass one token around it H times.

## What it measures, and what it does not

It measures **a hand-off between two actors that are not the same two every time**. At any
instant exactly one actor is runnable and no mailbox ever holds more than one message, so — like
ping-pong — there is no parallelism and no queue depth. What the ring adds is *breadth*: the
scheduler has to find the next actor a hundred times over, and the state the token lands on is
cold more often than warm. Ping-pong's two actors live in L1 for the whole run; a hundred actors,
their mailboxes and their dispatch tables do not.

It does **not** measure throughput: with one token, per-hop latency is the whole number. And it
does not measure fairness in any strong sense — every actor gets exactly one message per lap.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `actors` | 100 | 100 | no deviation |
| `hops` | 1 000 000 | 100 000 | **deviation — see below** |
| `cores` | 2 | n/a | actor i lives on core i % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's hop count, and why

Savina's default is 100 000 hops. At the ~40–250 ns per hop measured here that is a 4–25 ms
repetition, in the same noise regime that made ping-pong's 40 000 round trips unusable: a
run-to-run IQR wider than the framework differences the cell is meant to show. 1 000 000 is used
for the same reason, and `--param hops=100000` reproduces Savina's figure exactly.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | all 100 actors on VirtualCore 0; a hop is a same-core pipe write, drained on the next loop turn | actor i on VirtualCore i % 2, so **every hop crosses a core** — the worst placement a shard-per-core design can be given, and deliberately the one published |
| CAF | `max-threads=1` | `=2`, both pinned; a hop is delivered by prepending the receiver to the sender's own worker queue (`worker::delay`), so the hop stays on the sender's thread unless the idle worker steals it — CAF measures mostly same-thread hand-offs |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; whichever thread is free takes the next demand, so how many hops cross a core is the dispatcher's decision |
| floor | one thread, one ring per (worker, worker) pair, a hop is a push and a pop | two pinned threads; actor i owned by worker i % 2, so every hop crosses a core like qb's |

That table is the reason the `cores=2` column must not be read as one contest. qb and the floor
pay a cross-core hop on every one of the million hand-offs; CAF pays it only when a steal happens,
which on a one-token workload is rare; SObjectizer pays it whenever its two threads alternate.
The qb figure is a **cross-core hand-off cost**, the CAF figure is mostly a **same-thread
hand-off cost**, and the difference between them at `cores=2` is architecture — a scheduler that
follows the token against one that pins the actors — before it is anything else. The `cores=1`
column is the like-for-like comparison of dispatch cost with a hundred actors in play, and it is
the one to read first.

Why qb is published on the worst placement rather than the best: with actor i on core (i / 50)
every hop but two per lap would be same-core, and qb's `cores=2` cell would read as a same-core
number wearing a two-core label — exactly the confusion the `caf-detached` variant exists to
avoid in the other direction. The rule is that a framework's cell says what its cell does.

## The verified answer

The token is a **countdown and nothing else** — Savina's own shape. The sink injects
`remaining = hops` at actor 0 and actor `i` hands `remaining - 1` to actor `i + 1`, so the hop of
rank `h` (from 0) reaches actor `h % actors` carrying `hops - h`. What a ring actor keeps of the
tokens it received is its **tally** (`thread_ring::Tally`): how many, and the wrapping sum of the
countdowns they carried — two independent adds per hop, no multiply, nothing folded into the token.
Each actor's tally is a **128-byte slot** of a vector the body owns — two lines, because x86's
adjacent-line prefetcher and Apple's cores fetch lines in pairs — so no two actors, whichever
threads run them, ever write one line or one pair. CAF, SObjectizer and the floor add into the
slot on every hop. qb adds into two members of the actor itself, on the line its handler already
reads — an actor object lives in its own core's thread arena, so no actor of another core shares
that line — and the actor hands them to its slot when the engine destroys it, on its core, after
the window: measured with the slot written on every hop instead, qb's one-core cells ran 7 %
slower on Windows/MSVC, the hop touching one line more. Once the framework is gone the body
reduces the tallies, **outside the window**, through the spec's `checksum()` — the wrapping sum
of `mix(mix((i << 40) ^ count) ^ sum)` over the actors — and `delivered()`, the counts plus the
results the sink's handler ran. Both numbers are fixed for every actor by the parameters: actor
`i` receives exactly the ranks `i, i + actors, i + 2·actors, …` below `hops`, so its count and
its sum have a closed form (`expected_count`, `expected_sum`), and `expected()` is the digest of
that closed form, O(actors). `expected_messages = hops + 1` — the token deliveries, the injection
being the first, plus the result — is asserted **exactly** against that counted total.

Until Huly QB-1017 the token also carried an accumulator, each actor folded `mix(remaining)` into
it, and every adapter's sink *declared* `hops + 1` messages: the four routing defects below
verified, and a multiply chain ran inside the window on every hop. Result documents measured
before that change carry the earlier `expected_checksum`, and their per-hop work included that
fold; the per-hop work now is the hand-off plus the receiver's two adds.

In qb the token is **one event for its whole life** — each actor decrements it in place and
`forward()`s it, the same recycling ping-pong's `reply()` does — which is qb's cheapest hand-off
and the idiom `Actor.h` documents `forward()` for. CAF re-sends a bare `uint64` countdown;
SObjectizer re-sends a one-field `msg_token`; the floor pushes its fixed four-field message into a
ring. The result to the sink is a signal (CAF's `close_atom`).

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). The token is
the only message in flight, and the sink ends the run on the first result. A token or a result
**lost** leaves nothing to end the run, and so does a token that reaches the sink (which has no
handler for it) or one whose `remaining` arrives as 0 (the decrement wraps): the run **blocks**,
the repetition outlives the harness's deadline and fails with exit 1 and no timing. Everything
else that changes which actor receives which countdown moves a tally. A token **delivered twice**
adds a count and a countdown to one actor twice — even the last delivery, whose copy carries the
right countdown. A `remaining` altered puts other countdowns into every later tally. A token
delivered to the **wrong ring actor** puts a countdown into the tally of an actor that should not
have it, and every later one with it. An actor the ring **routes around** keeps an empty tally,
and one that **forwards the token untouched** keeps one short. A **consistent misroute** — every
hop two actors on, on a ring of odd size — hands every actor exactly as many tokens as it should
get: every count is right, which is why a per-actor count alone would not do, and every actor's
sum is someone else's. A planted version of each is rejected by the harness's negative control
(`tools/negative-control.py`, section K, on `tools/control/thread-ring-subject.cpp`, the
consistent misroute shown passing every count first), which also confirms that a correct ring
verifies. What reaches neither verdict is a copy no handler ever runs — one still queued when the
first result tears the run down: it changes nothing the run did, and nothing it reports.

## The measured window

Opens when the sink injects the token into an already-running ring, closes when the token comes
back with `remaining = 0`. Every framework implementation waits for all `actors` ring members to
report ready before opening it, so every worker thread is running and every actor has been
scheduled at least once — a hundred actors' first-touch scheduling is a visible fraction of a run
otherwise. The floor has no actors to wait for: its second thread is created just before the
window and its start-up, tens of microseconds once, is below resolution at a million hops.
