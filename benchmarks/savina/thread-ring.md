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

The token carries a countdown and an accumulator. The ring actor at index `i` that receives it
with `remaining = k` adds `term(i, k) = mix((i << 40) ^ k)` as a **wrapping sum** before passing
it on — one function in the spec, `thread_ring::term`, that every adapter folds through. The sink
injects `k = hops` at actor 0 and actor `i` hands `k - 1` to actor `i + 1`, so the hop of rank `h`
is folded by actor `h % actors` with `k = hops - h`: the expected sum, computed from the
parameters alone, names **which actor folds every hop**. A term skipped or folded twice — an actor
that decrements without adding, or adds without decrementing — changes the total, and so does
any defect that puts a term under the wrong actor (below).

`expected_messages = hops + 1` — the token deliveries, the injection being the first, plus the
result to the sink — is asserted **exactly**, and it is **counted**: every ring actor counts the
tokens its handler ran, the sink the results its handler ran, and the adapter reports their sum.
Each ring actor hands its count over when the framework tears it down — qb from the actor's
destructor, CAF from its state's (destroyed when the actor terminates), SObjectizer from
`so_evt_finish`, the floor from a per-actor slot read once its workers are joined — so the
collection is outside the window; inside it, each hop adds one increment of the receiving actor's
own count and one XOR of its index into the term, the same two operations in every adapter. (Until
Huly QB-1017 every term was `mix(k)` and every adapter's sink *declared* `hops + 1`: the four
defects below verified. Result documents measured before that change carry the earlier
`expected_checksum`.)

In qb the token is **one event for its whole life** — each actor mutates it in place and
`forward()`s it, the same recycling ping-pong's `reply()` does — which is qb's cheapest hand-off
and the idiom `Actor.h` documents `forward()` for. CAF re-sends a bare `(remaining, acc)` pair;
SObjectizer re-sends a `msg_token`; the floor pushes a two-word struct into a ring.

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). The token is
the only message in flight, and the sink ends the run on the first result. A token or a result
**lost** leaves nothing to end the run, and so does a token that reaches the sink (which has no
handler for it) or one whose `remaining` arrives as 0 (the decrement wraps): the run **blocks**,
the repetition outlives the harness's deadline and fails with exit 1 and no timing. An `acc`
altered, or a `remaining` altered to another non-zero value, moves the sum. So do three defects
the first version of this checksum could not see, because every term now carries the index of
the actor that folds it. A token delivered to the **wrong ring actor** goes on from that actor's
successor: its term and every later one sit under another index. An actor the ring **routes
around** never folds its share, and its successors fold theirs one index off. A token
**forwarded untouched** is folded, for the same `k`, by the next actor, under the next index. The
fourth, a token **delivered twice**, gives two tokens, each carrying its own countdown and
accumulator, each folding every remaining hop under the right index: both reach the sink with the
right total and the run ends on the first — the count sees it, the duplicated delivery being one
more than `hops + 1` even when it is the last one. A planted version of each is rejected by the
harness's negative control (`tools/negative-control.py`, section K, on
`tools/control/thread-ring-subject.cpp`), which also confirms that a correct ring verifies. What
reaches neither verdict is a copy no handler ever runs — one still queued when the first result
tears the run down: it changes nothing the run did, and nothing it reports.

## The measured window

Opens when the sink injects the token into an already-running ring, closes when the token comes
back with `remaining = 0`. Every framework implementation waits for all `actors` ring members to
report ready before opening it, so every worker thread is running and every actor has been
scheduled at least once — a hundred actors' first-touch scheduling is a visible fraction of a run
otherwise. The floor has no actors to wait for: its second thread is created just before the
window and its start-up, tens of microseconds once, is below resolution at a million hops.
