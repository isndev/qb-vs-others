# savina/concsll

Savina's Concurrent Sorted Linked List, one of the "concurrency" benchmarks (Imam & Sarkar,
*Savina — An Actor Benchmark Suite*, AGERE 2014; `SortedListConfig.java`, `SortedLinkedList.java`
and `SortedListAkkaActorBenchmark.scala`) — twenty workers send requests to **one actor that owns a
sorted singly-linked list**, one request at a time each, and every request is answered only after
the list has been walked: an insert walks to its place, a membership test walks until it finds the
value or runs out of list, a size query walks the whole list.

## What it measures, and what it does not

It measures **a hot server actor**: one mailbox written by every worker, and behind it a long,
serial, cache-bound computation per message. No other shape of the suite has it — chameneos' mall
and bank-transaction's accounts answer in a few instructions. Here the computation dominates by
construction: a run at the defaults walks about **1.2 billion list nodes** (1 155 676 458 contains
steps, 64 958 357 insert steps and 1 234 size steps if the workers' requests interleave
round-robin, 1 224 495 386 in all if they run one worker after the other — counted by a
framework-free model of `SortedList`'s walks; every run reports its own counts), against 160 000
requests and 160 000 answers. So what differs between the cells is not the walk — it is the **same
source in every framework**, out of line, on nodes from the list's own arena — but three things
around it:

- the cost of 160 000 request/answer round trips (one message each way, a worker never has two
  requests in flight);
- **where the list actor runs**: a placing framework keeps it on one core, whose cache keeps the
  list (16 126 nodes of 16 bytes at the end of a run — 252 KiB); a pool may run it on either of its
  threads, and a list actor that changes thread finds its list in the other core's cache;
- how far the interleaving lets the list grow before each walk — a contains issued after more
  inserts walks a longer list. That amount of work is not the same in every cell, so it is
  **reported beside the cell** (`contains_walk`, `write_walk`, `size_walk`) and never compared
  silently (FAIRNESS.md § 0). `contains_found` is reported beside them but is not an observation
  of the interleaving: Savina's generator fixes it at 0 (below), and every contains answer is
  asserted, so a cell that found a value has already failed verification.

It does **not** measure parallelism: the walk is one actor's, serial by the problem's definition,
and a second core can only take the workers and the messaging off the list's core. It does not
measure actor creation (the 22 actors are built before the window opens; `fib` is the creation
shape), and it does not measure a concurrent data structure — the list is one actor's private state,
which is the point of writing it as an actor.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `workers` | 20 | 20 (`NUM_ENTITIES`) | no deviation |
| `messages` | 8 000 | 8 000 (`NUM_MSGS_PER_WORKER`) | no deviation; 160 000 requests per repetition |
| `write_percent` | 10 | 10 (`WRITE_PERCENTAGE`) | no deviation — a THRESHOLD of the generator, see below |
| `size_percent` | 1 | 1 (`SIZE_PERCENTAGE`) | no deviation — a THRESHOLD of the generator, see below |
| `form` | 0 | n/a | the reply path, **a declared axis — see below** (the same axis as `concdict`'s) |
| `cores` | 2 | n/a | the list alone on core 0, the master and the workers on core 1, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### The requests are Savina's, exactly — and so is their mix

Each worker draws its requests from Savina's own `PseudoRandom`, seeded as the reference seeds it
(`id + numMessagesPerWorker + writePercent + sizePercent`), with the reference's rule: `nextInt(100)`
below `size_percent` is a size query, below `size_percent + write_percent` an insert of
`nextInt()`, otherwise a contains of `nextInt()` (`Script` in the spec header). The generator is a
16-bit linear congruential one whose low bits are strongly correlated, so the kind drawn and the
value drawn after it are not independent, and the mix it produces is not the nominal one. At the
defaults, computed framework-free:

| | requests | share |
|---|---|---|
| inserts | 16 126 | 10.08 % |
| contains | 143 864 | 89.92 % |
| size queries | **10** | **0.006 %**, not 1 % |

The 16 126 inserts write only 3 345 distinct values, and **no contains ever looks for a value any
worker inserts — for any parameters, not only the defaults**: a value is always the generator's
successor of the state that drew the request's kind, the states that draw "insert" and those that
draw "contains" are disjoint (their `% 100` differ), and the generator is a bijection on its 65 536
states, so the values inserted and the values looked for never meet. Every contains walks the whole
list and answers false. These are properties of the reference's workload and they are reproduced
exactly, not corrected — a "fixed" generator would be a different benchmark, and Savina's own
numbers were measured on this one. The checksum asserts every answer that follows from them
(below), and `contains_found`, reported beside every cell, is the run's own confirmation: 0, by
construction rather than by interleaving.

### The reply path: `form`, a declared axis

Savina's Akka reference answers with an ordinary message to the sender (`sender ! new
ResultMessage(...)`), and every framework here can do that; two of them also have a
request/response primitive that keeps the continuation for the caller. Which is faster is not
obvious and differs per framework, so the reply path is a declared parameter recorded in every
result document — the same axis, with the same values, as `concdict`'s, the other request/reply
shape of the suite: `form=0` is the reference's shape, `form=1` the framework's primitive.
Following FAIRNESS.md §1.1, each adapter's table cell runs the FASTER of its forms as measured on
the quiet host, and its `main()` says which; the other form is publishable as a side document.
Until that measurement every adapter runs `form=0`.

- **qb**: `form=0` bounces ONE event per worker between the worker and the list for the whole run —
  the list writes the answer into the event and `reply()`s it, the worker folds it, rewrites the
  same event into its next request and `reply()`s it back (qb's documented reply idiom, the one its
  ping-pong uses); `reply()` hands the event to the peer at once, as `send` does, which is what a
  request its sender then idles for wants. `form=1` is one coroutine per worker awaiting
  `qb::ask<ListAsk>(ctx, list, 0, id, kind, value)` per request, the answer routed by `resolve_ask`
  in the worker's handler.
- **CAF**: the list's handlers RETURN the answer and CAF sends it to the sender — for an ordinary
  message as an ordinary message (`response_promise::respond_to`; the response id of an
  asynchronous message is asynchronous), CAF's own calculator example. `form=0` sends the request
  with `mail().send()` and takes the answer in the worker's behavior; `form=1` is
  `mail().request(list, infinite).then()`, a continuation per request. Every request and every
  answer is a new CAF message either way: CAF messages are immutable and cannot be sent back.
- **SObjectizer**: `form=0` resends ONE `mutable_msg<msg_op>` per worker, rewritten in place, from
  worker to list and back on the direct mboxes — SObjectizer's own redirection of a mutable message
  (`dev/sample/so_5/mutable_msg_agents`, `chameneos_prealloc_msgs`), which allocates only the first
  request where the documentation's lead, a new `so_5::send<msg>()` per request and per answer,
  allocates two per round trip. SObjectizer has no asynchronous request/reply with a continuation
  (`request_value` was removed in 5.6; `so_5::extra::async_op` is a separate library), so `form=1`
  is **not applicable**.
- **The floor**: a request is one ring push carrying the worker's identity, the answer one push
  back. There is no primitive to compare, so `form=1` is **not applicable** and the `form=0` cell is
  the floor for both forms.

On this shape the reply path is a small part of the window — the walk dominates it — so the two
forms may well measure level; the axis is declared all the same, so that whichever is faster is the
one in the table and the other is on record.

### Deviations, all in the list's memory, none in the work

1. **The item lives in the node.** The reference stores a boxed `Integer`, one more pointer per
   step of a walk; the C++ node holds the `int32` itself (16 bytes a node). Every framework walks
   the same nodes.
2. **The nodes come from the list's own arena** — blocks of 4 096 nodes, taken in insertion order —
   rather than from the process heap. On the heap, the nodes would be interleaved with whatever the
   framework allocates between two inserts (a CAF message per request, a SObjectizer message), and
   the list's cache footprint — the thing the walk is bound by — would differ by framework. With the
   arena it is the problem's, the same in every cell. The first block is allocated with the list
   actor, before the window; the three more a run needs at the defaults are allocated inside it, by
   the same code everywhere.

Savina's master creates the list and the workers in its start hook, inside its timed region; here
they are created before the window, as in every benchmark of the suite that does not measure
creation. A worker that has sent its last report quits at once in CAF (`quit()`, the reference's
`exit()`), and waits for the end of the run in qb, SObjectizer and the floor; it receives nothing
more either way.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the list, the master and every worker on VirtualCore 0 | the list **alone** on VirtualCore 0, the master and every worker on VirtualCore 1 — fixed before start (worker *w* on core 1 + *w* % (cores − 1) above two cores). Every request and every answer crosses the cores, and the core that walks the list runs nothing else and keeps the list in its cache — see "Placement" below |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places every actor, the list included, and may move the list between its threads |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; the dispatcher places, the list included |
| floor | one thread: the list, the master and the workers in one ring | two pinned threads: the list alone on thread 0, the master and the workers on thread 1 — qb's placement, so the floor bounds the placing frameworks. The list is the same `SortedList` |

### Placement

The list is the only actor with real work: its core walks, every other actor only draws, sends and
folds. Alone on its core — chameneos' mall placement — the walk has the core and its cache to
itself, at the price of every request and every answer crossing the cores. `concdict`, the other
request/reply shape, puts its dictionary on core 0 WITH half the workers (bank-transaction's
placement), where a request is one hash probe and the workers' turns are a fair share of that core.
Neither argument settles which is faster for this shape, so before a cell is published the other
placement is measured once on the quiet host, for qb and for the floor together (they share the
placement), and the faster one is the cell's; the page will say which, and by how much against the
A/A spread.

## The verified answer

Every request carries its identity — the worker's number and the request's index in that
worker's sequence — and the list echoes it, with the request's kind, in the answer. A worker knows
what it asked without reading the answer (its sequence is the generator's), and for each answer it
adds `answer_term`: `reply_term(what it asked, what the answer says it answers, the asserted
result)` — equal in a correct run, so the term is fixed by the request alone — plus, for a size
query, the length the answer reports (below). An answer delivered to the wrong worker, an answer
delivered twice (every later answer of that worker is then one index off), an answer of the wrong
kind, or a wrong result where the result is fixed, moves the worker's sum. In qb, SObjectizer and
the floor, a copy that reaches a worker after its last request — its sum already reported — stops
the run with an explicit protocol failure (`fail()`, as in `concdict`) rather than a number, and
so does, with `form=1`, a qb answer that no ask of the worker waits for. A CAF worker has quit
after sending its report (`quit()`, the reference's `exit()`), so a late copy is dropped with its
mailbox and reaches no handler: it changes nothing the run reports.

Which results are fixed, whatever the interleaving:

- an **insert** answers the value it inserted (the reference's `ResultMessage`) — asserted;
- a **contains of a value no worker inserts** answers false — asserted. With Savina's generator that
  is every contains (above);
- a contains of a value some worker inserts is fixed in two cases — the asker inserted it earlier
  (true: that insert was applied before the asker could ask again) and only the asker inserts it,
  later (false) — and otherwise depends on whether another worker's insert came first. The rule
  asserts none of the three, the conservative choice: the generator never produces one, so nothing
  is lost, and the table of inserted values is computed, not assumed, so the rule does not depend
  on the generator. Such an answer is taken as 0 in `reply_term`;
- a **size query** answers the length at that moment, which the interleaving decides — taken as 0
  in `reply_term`, and asserted all the same, as a SUM: `SortedList::size` adds to the size walk
  exactly the length it answers, so the lengths the workers receive add up to the list's
  `size_walk` in every interleaving. Each worker adds every length it receives, the list subtracts
  its `size_walk` from what it reports, and a length altered on its way — or answered without its
  walk — is left in the checksum.

The list adds a term per request it RECEIVES — `request_term(identity, kind, payload)`, as the
request arrived — since the workers' terms only check what comes back: a contains of a value
nobody inserts answers false whatever value it carried, so without it a payload altered on its way
to the list would still verify. The sum of those terms is fixed by the requests, whatever order
they arrive in. After the window the list reports `list_term`: those request terms, minus its size
walk, plus the fold of its final contents — element *k* of the list, in order, contributes
`content_term(k, item)`, so the fold asserts the order as well as the multiset. The contents are
fixed by the multiset of inserts, which no interleaving changes (each worker's inserts are its own
and each is applied once); an insert lost or applied twice moves the fold. The master adds every
worker's sum and the list's term. An answer LOST does not move a number: its worker waits for it,
the run cannot end, and the harness's caller sees a hang — every message of this shape is on the
critical path.

`expected_messages = 2 × workers + 2 × workers × messages` is asserted alongside: the workers' DoWork,
the requests, the answers and the workers' reports, counted at the receivers; `form=1` sends the
same requests and answers, so the count is the same.

**The walks are asserted from below.** The amount of walking depends on the interleaving, so it is
an observation, reported beside the cell — but some of it every interleaving must do, and that part
is asserted (`Spec::observed_at_least`): when worker *w*'s request *j* reaches the list, every insert
*w* made before it has been applied, so an insert of *v* walks at least past *w*'s own earlier values
not greater than *v*, and a contains of a value nobody inserts, or a size query, walks at least *w*'s
own earlier inserts. At the defaults the three bounds are 57 777 849 contains steps, 3 248 656 insert
steps and 65 size steps — about a twentieth of what a run walks. They exist for an adapter that
would answer without the list: a contains answered from a hash set still reaches the right
checksum, and its walk falls short of the bound.

## The measured window

Opens when the master, having heard from the list and from every worker that they have started
(outside the window), sends each worker its DoWork; closes when the master receives the last
worker's report — the reference's terminal condition, the master's last `EndWorkMessage`. Every
request, every walk and every answer is inside it. The list's term — its final contents' fold, its
request terms, its size walk — is computed after it (the master's Finish and the list's Report, not
counted; the floor reads its list once its threads have joined), and the table of values any worker
inserts — which tells a worker which contains answers are fixed — is built before it, the same
framework-free code in every adapter.

The walk is compiled into each binary with that binary's flags. It is out of line
(`QVOSPEC_CONCSLL_NOINLINE`) so that no framework's handler can inline it into a different shape and
so that its code can be compared across the four binaries — and it has been, on the three
toolchains of the report (MSVC, g++-14, clang-19; the MSVC listings compiled with `/FAs` from each
adapter's own command line, the ELF binaries disassembled): each binary holds exactly ONE copy of
`add`, `contains` and `size`, and within a toolchain the four copies are the same instructions,
once addresses are set aside (g++'s inlined arena growth names a string constant at a different
offset in each binary). Where each copy lands in memory is the linker's, as for any code.
