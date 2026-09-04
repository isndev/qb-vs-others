# savina/counting

Savina benchmark 2 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014). One producer sends N increments to one counter, then asks for the total.

## What it measures, and what it does not

It measures **single-producer mailbox throughput**: how fast one side can enqueue and the other
dequeue when nothing ever waits for a reply. Ping-pong never has more than one message in flight,
so it measures a hop and hides the queue; counting is the complement — the queue is the whole
benchmark. With `cores=2` the counter's mailbox is a real cross-core queue written by one thread
and drained by another, and the number reported is the steady-state cost of one message through
it, sender and receiver included.

It does **not** measure contention. There is one producer. `big` is the many-writer case.

What it exposes that ping-pong could not is the cost of a **burst**: the producer's single handler
emits a million messages before the framework gets a chance to do anything else with them. A
framework that batches (qb's growable per-core pipe, flushed in bulk) pays for the buffer it grows;
one that hands each message to a mailbox eagerly pays a queue operation per message. Both costs
are real and both are in the cell.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `messages` | 1 000 000 | 1 000 000 | no deviation |
| `cores` | 2 | n/a | 1: producer and counter on one thread; 2: one pinned thread each |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Savina's own count is large enough here — a million increments is 30–300 ms per repetition across
the frameworks measured, well above the noise floor that made ping-pong's 40 000 unusable.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | both actors on VirtualCore 0; the producer's million `push<>`es go into the core's own pipe and are drained after the handler returns | producer on VirtualCore 0, counter on VirtualCore 1, each pinned; the pipe is swapped across cores and flushed in bulk |
| CAF | `max-threads=1`: the counter runs on the producer's worker once the producer's handler returns | `=2`, both pinned; the producer never yields its worker while streaming, so the idle worker steals the counter and every increment crosses a thread |
| SObjectizer | `one_thread` dispatcher, two agents on it | `active_obj`: one pinned work thread per agent, the counter's mailbox an MPSC queue with one writer |
| floor | one thread, one bounded SPSC ring, drained inline when full | two pinned threads, one SPSC ring between them |

The ordering requirement matters for qb: the retrieve must arrive **after** every increment, and
`send<>` (unordered) may deliver ahead of events still sitting in the pipe, so the adapter uses
`push<>` (ordered). SObjectizer's `send<>` and CAF's `mail().send()` are FIFO per mailbox and
need no such choice.

## The verified answer

The i-th increment carries i; the counter accumulates `mix(i)` as a **wrapping sum** (see
ping-pong.md for why a sum and not an XOR). A framework that coalesced two increments, dropped
one, delivered one twice, or let the retrieve overtake an increment would return a different
total. `expected_messages = messages + 2` (the increments, the retrieve, the result) is asserted
alongside.

## The measured window

Opens when the producer emits its first increment into an already-running system, closes when it
receives the total. Each implementation performs a handshake round trip before the window so that
the counter has been scheduled at least once; the window contains message passing, not first-touch
scheduling.

Note what "first increment" means for a batching framework: in qb the window opens before a
handler that pushes a million events, and closes only after the pipe carrying them has been
grown, swapped and drained. The buffer growth is inside the window because it is the price of the
batching, not an artefact of the harness — a burst is the workload.
