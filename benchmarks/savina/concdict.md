# savina/concdict

Savina benchmark "Concurrent Dictionary" (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014), one of the "concurrency" group — **twenty workers, one dictionary actor, and every
request waits for its answer before the next is sent**. Where `bank-transaction` nests a request
inside another and spreads it over a thousand accounts, this shape puts twenty request/reply chains
on ONE serving actor: the cost of answering the sender, times two hundred thousand, through a
single mailbox.

## What it measures, and what it does not

It measures **the round trip to a shared actor**: a worker sends a read or a write to the
dictionary, the dictionary does a hash-map lookup (and, for a write, a store) and answers the
sender, and the worker sends its next request only once it has the answer. Each framework's
answer-the-sender path is what is priced — qb's `reply()` (or `qb::ask`), the result a CAF handler
returns (or `request().then()`), a SObjectizer message sent back on a direct mbox — together with
the twenty-writer fan-in into the dictionary's mailbox, and how much of one worker's round trip a
runtime overlaps with the others' while the dictionary is busy. The dictionary is the serial
bottleneck by design: at most twenty requests are ever in flight, one per worker.

It does **not** measure the data structure. The dictionary is the spec's `Store` — one
`std::unordered_map` of 524 287 entries — compiled into every adapter as the same object code, so
its lookups are a constant every cell pays (and a large one: a random key in a map that size is a
cache miss). It does not measure contention on the map either: one actor owns it, which is the
point of writing a shared dictionary as an actor. And it does not measure actor creation — the
master, the dictionary and the workers are built before the window opens (`fib` is the creation
shape).

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `workers` | 20 | 20 (`NUM_ENTITIES`) | no deviation |
| `messages` | 10 000 | 10 000 (`NUM_MSGS_PER_WORKER`) | requests per worker; no deviation — 200 000 round trips per repetition |
| `write` | 10 | 10 (`WRITE_PERCENTAGE`) | percent of requests that write; no deviation |
| `keys` | 524 287 | 524 287 (`DATA_LIMIT` = `Integer.MAX_VALUE / 4096`) | the pre-filled key space; no deviation in its size — **which keys a worker uses is the deviation below** |
| `form` | 0 | n/a | the reply path, **a declared axis — see below** |
| `cores` | 2 | n/a | the master and the dictionary on core 0, worker *w* on core (1 + *w*) % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's key space, and why

Savina's twenty workers draw their keys from the whole key space, so a read returns whatever the
last write to that key left — and whether another worker's write landed first is the scheduler's
business, different run to run and framework to framework. No framework-free value could assert
those answers, and a checksum that ignored them would accept a dictionary that answered anything.
Here worker *w* uses only the keys of its **own partition** (`slot × workers + w`): no other worker
ever writes a key *w* reads, so every answer *w* receives is fixed by its own earlier requests — it
has one in flight at a time and the dictionary serves one at a time — and so is the final content of
the map. The dictionary is still ONE map holding all 524 287 entries and serving every worker, a
lookup costs what it costs in Savina, and every key touched is one of Savina's.

The draws themselves follow Savina's worker (`nextInt(100) < writePercent` decides a write, then a
key and, for a write, a value) from a mix of (worker, request index) instead of a
`java.util.Random` per worker: the same distribution, and a stream no implementation can pick. The
map is pre-filled with every key mapped to itself, as Savina's `DATA_MAP` is — before the window,
by the thread that runs the repetition, before the framework starts (Savina copies it inside the
master's constructor, inside its timing; here construction is setup in every cell).

### The reply path: `form`, a declared axis

Savina's Akka reference answers with an ordinary message to the sender (`sender ! result`), and
every framework here can do that; two of them also have a request/response primitive that keeps the
continuation for the caller. Which is faster is not obvious and differs per framework, so the reply
path is a declared parameter recorded in every result document: `form=0` is the reference's shape,
`form=1` the framework's primitive. Following FAIRNESS.md §1.1, each adapter's table cell runs the
FASTER of its forms as measured on the quiet host, and its `main()` says which; the other form is
publishable as a side document. Until that measurement every adapter runs `form=0`.

- **qb**: `form=0` bounces ONE event per worker between the worker and the dictionary for the whole
  run — the dictionary writes the answer into the event and `reply()`s it, the worker folds it,
  rewrites the same event into its next request and `reply()`s it back (qb's documented reply idiom,
  the one its ping-pong uses); `reply()` hands the event to the peer at once, as `send` does.
  `form=1` is one coroutine per worker awaiting `qb::ask<OpAsk>(ctx, dictionary,
  qb::duration::zero(), key, value, write)` per request — a timeout `<= 0` waits indefinitely
  (`request.h`) — the answer routed by `resolve_ask` in the worker's handler.
- **CAF**: the dictionary's handlers RETURN the answer and CAF sends it to the sender — for an
  ordinary message as an ordinary message (`response_promise::respond_to`; the response id of an
  asynchronous message is asynchronous). `form=0` sends the request with `mail().send()` and takes
  the answer in the worker's behavior; `form=1` is `mail().request(dictionary, infinite).then()`,
  `then` and not `await`, a continuation allocated per request. Every request and every answer is a
  new CAF message either way: CAF messages are immutable and cannot be sent back.
- **SObjectizer**: `form=0` resends ONE `mutable_msg<msg_op>` per worker, rewritten in place, from
  worker to dictionary and back on the direct mboxes — SObjectizer's own redirection of a mutable
  message (`dev/sample/so_5/mutable_msg_agents`), which allocates only the first request where the
  documentation's lead, a new `so_5::send<msg>()` per request and per answer, allocates two per round
  trip. SObjectizer has no asynchronous request/reply with a continuation (`request_value` was removed
  in 5.6; `so_5::extra::async_op` is a separate library), so `form=1` is **not applicable**.
- **The floor**: a request is one ring push carrying the worker's index, the answer one push back.
  There is no primitive to compare, so `form=1` is **not applicable** and the `form=0` cell is the
  floor for both forms.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | master, dictionary and every worker on VirtualCore 0 | master and dictionary on VirtualCore 0, worker *w* on core (1 + *w*) % 2 — fixed before start: the dictionary shares its core with ten workers and the other ten are one pipe away |
| CAF | `max-threads=1` | `=2`, both pinned; the pool places the master, the dictionary and the workers and steals |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads, `fifo_t::individual`; the dispatcher places |
| floor | one thread, every role in its own ring | two pinned threads, the same static placement as qb; workers are vector slots with no mailbox, and the dictionary's fan-in is a scan of one SPSC ring per thread |

## The verified answer

Worker *w* folds `reply_key(w, j, answer)` for the answer to its *j*-th request — *j* being its own
count of the answers it has received — and reports the total `F(w)` to the master when it has its
last answer; the master adds `done_key(w, F(w))` for every report, and `digest_key(D)` for the
dictionary's digest `D`, as wrapping sums. `D` is kept by the `Store` itself: every write adds
`entry_key(k, new) − entry_key(k, old)`, which telescopes to a function of the map's final content,
so it is reported at the end without walking half a million entries, and `write_key(k, value)`,
which counts the write itself — the final content alone cannot see a write that was answered but
never stored when the same worker rewrites that key before reading it. Because nobody but *w*
touches *w*'s keys, every answer, every write and every final value is plain arithmetic: the spec
header replays each worker's requests against its own partition, with no framework linked.

The total moves for an answer carrying the wrong value, a write answered but not stored, a write
stored under the wrong key, an answer the worker does not fold, a report counted twice and two
reports that swap their workers' names — each planted alone, at `cores=1` and `cores=2`, in all
four adapters, and failing every one of those cells on the checksum.
`expected_messages = 2 × workers × messages + 2 × workers + 2` — every request and every answer,
each worker's start and report, the dictionary's exit and report — is counted at the receivers and
asserted alongside (a master that counts one message too many fails every adapter's cells on the
count alone); `form=1` sends the same requests and answers, so the count is the same.

An answer duplicated mid-run shifts every later term of its worker's fold and leaves that worker
two requests in flight: it reaches its last answer early, and the answer still in flight then
reaches it — a worker that receives an answer after its last request stops the run loudly
(`fail()`) when it is still there to receive it, and that is how the planted duplicate (qb and the
floor, `cores=1` and `2`) ended every time; a worker already gone leaves the shifted fold to fail
the sum. What neither can see is a duplicate of a worker's LAST answer, which lands after that
worker's report: `fail()` catches it only when the worker is still there to receive it.

What a checksum cannot report is a message that never arrives: a request or an answer lost, an
answer delivered to the wrong worker, a worker or the dictionary that never reports. Each worker
has one request in flight and the master waits for every report, so each of those leaves an actor
waiting for a message that never comes — the run **hangs**, and the run's timeout
(`tools/run.py --timeout`) fails the cell.

Nothing in this shape depends on the interleaving once the keys are partitioned, so it reports no
`observed` counts and asserts no `observed_at_least`: every quantity it has is asserted.

## The measured window

Opens when the master sends the twenty starts into an already-running system — in the three
frameworks every worker and the dictionary have reported ready, so every thread is up — and closes
when the master has the twentieth worker's report: every request answered. The floor has no
ready handshake: at `cores=2` its window opens right after its second thread is created, before
that thread is confirmed running, so its start-up, once, is inside the floor's window — a cost
only the floor pays, as in `bank-transaction`; a start barrier for every floor is a harness-wide
decision, not this benchmark's. The dictionary's exit and its report (the digest) are after the
window and inside the message count. The map is built before the framework, outside the window
and outside the framework's own setup, by the same thread for every adapter.
