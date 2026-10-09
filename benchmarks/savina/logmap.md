# savina/logmap

Savina's Logistic Map Series, one of the "concurrency" benchmarks (Imam & Sarkar, *Savina — An
Actor Benchmark Suite*, AGERE 2014; `LogisticMapConfig.java` and
`LogisticMapAkkaManualStashActorBenchmark.scala`) — a master asks each of `series` series workers
for `terms` terms of the logistic map x' = r·x·(1 − x), and **every term is a request/reply round
trip** between the worker and the rate computer that holds its series' rate: the worker sends its
current term, the computer answers the next one, and until that answer arrives the worker holds
every other request.

## What it measures, and what it does not

It measures **the latency of one ask-and-answer, chained**: a series is `terms` strictly
sequential round trips — the next request cannot leave before the previous answer lands — and the
`series` chains are independent, so a repetition costs about `terms` round trips of the slowest
chain, with `series`-way parallelism to spread over the cores. It also measures **a burst that
lands in deep mailboxes**: the master sends all `series × terms` NextTerm requests at once (250 000
at the defaults), most of which find their worker waiting for an answer and are held — so what a
framework pays to accept, queue and set aside a request it cannot act on yet is in the window too.
The computation itself is three floating-point operations per term, identical object code in
every adapter (`next_term` in the spec header); it is not what is measured.

It does **not** measure a request primitive: every adapter asks with plain messages — Savina's own
"manual stash" variant — and `qb::ask`, CAF's `request().then()` and their continuations are
priced by `bank-transaction`. It does not measure balancing at `cores=2` for the frameworks that
place: qb puts series *i* — its worker and its computer, which talk to nobody else — on core
(1 + *i*) % 2, so every round trip stays on its core and the ten chains split five and five; the
pools place each actor freely and may split a pair across cores. It does not measure actor
creation: every actor exists before the window opens.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `terms` | 25 000 | 25 000 | no deviation; 750 040 messages per repetition |
| `series` | 10 | 10 | no deviation; as many rate computers, computer *i* serving worker *i* (Savina: `computers(i % numComputers)`) |
| start rate | 3.46 | 3.46 (`-r`) | fixed, not a parameter (qvo parameters are integers); **see "The rates" below** |
| rate increment | 0.0025 | 0.0025 | fixed; series *i* has rate 3.46 + 0.0025·*i* and first term 0.0025·*i* |
| `cores` | 2 | n/a | the master on core 0; series *i* (worker + computer) on core (1 + *i*) % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Series 0 starts at 0 and stays there — Savina's own parameters (0·anything is 0). Its chain is
still `terms` round trips and is verified like the others: the fold counts its steps, not its
values (see "The verified answer").

### Which Savina variant, and the held requests

Savina ships the shape several ways for Akka alone. `…AkkaAwaitActorBenchmark` blocks the
worker's thread in `Await.result` for every term — a blocked pool thread, which no framework here
offers and none should be measured doing. `…AkkaBecomeActorBenchmark` switches behaviour and
`stash()`es every other message until the answer, then `unstashAll()`. `…AkkaManualStashActorBenchmark`,
the one this page follows, keeps a flag and a list by hand: a request that arrives while the
worker waits is added to the list, and after each answer ONE stashed message is re-sent to the
worker itself. That is the shape every framework can write the same way.

**Deviation: the held requests are a count.** A NextTerm carries nothing, so holding one is
remembering that it came; every adapter keeps a counter where the reference keeps a list of
identical message objects and re-sends each to itself — a second delivery of every held request
(≈ 250 000 per repetition), which would measure the reference's workaround for Akka rather than
the shape. GetTerm is the one request that is not a count: it is answered only when nothing is
held and nothing is in flight, exactly the reference's rule ("do not reply to master if stash is
not empty"), and because every mailbox here is FIFO per sender it arrives after every NextTerm the
master sent before it.

**Deviation: the computers' stop is answered.** Savina's master sends StopMessage to every
computer and every worker and exits. Here each computer answers its stop with a report of how many
requests it served, which is folded into the checksum (a request served twice or never fails the
run); the workers' stop is the teardown, after the window, and is not counted.

**Deviation: no floating sum.** The reference's master adds the final terms as doubles in the
order they arrive, which differs run to run in the last bits. The checksum below is exact.

### The rates, and why every adapter computes the same bits

Savina computes `3.46 + i * 0.0025` and `i * 0.0025` in doubles. Written that way the first is a
multiplication feeding an addition — exactly what a compiler may contract into one fused
multiply-add on a target that has one (every arm64 target, with clang's default
`-ffp-contract=on`), skipping a rounding: the last bit of a rate, and with it every term of the
series, would then differ between hosts. So the spec computes each as **one correctly rounded
division of exact integers** — 3.46 = 1384 / 400, 0.0025 = 1 / 400 — the IEEE double nearest
Savina's decimal value, the same on every compiler and target. For some *i* it can differ in the
last bit from the JVM's two-rounding result; the problem is the same problem.

The map step is Savina's own evaluation order, (r·x)·(1 − x): no product feeds an addition, so
there is nothing to fuse, and each of the three operations is one correctly rounded IEEE operation
on every target qvo builds for — SSE2 on x86-64 (no `-march`, no fast-math: `CMakeLists.txt`),
NEON on arm64. In every adapter a term crosses a message between the multiplication that makes it
and the subtraction that consumes it; the spec's reference loop gets the same rounding barrier
explicitly (`settle`, a volatile round trip, outside the window). The checksum is therefore the
same value in every document, on every host.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | everything on VirtualCore 0 | master on VirtualCore 0, series *i*'s worker AND computer on core (1 + *i*) % 2 — fixed before start, so no round trip crosses a core (qb.llm.md: put the actors that talk to each other most on the same core). The ask is ONE event per chain: the worker `push`es `Compute{term}`, the computer overwrites the term and `reply()`s the same event, and a worker still owing terms `reply()`s it straight back — nothing is allocated per hop. `push`, not `send`: the burst must reach each worker in order, and a same-core hop has nothing to publish early. The master's burst reaches the remote core at the end of its handler, through the pass's flush |
| CAF | `max-threads=1` | `=2`, both pinned; the pool places the master, the workers and the computers. The worker sends its term as a bare `double`, the computer's handler RETURNS the next term and CAF delivers it to the sender (`response_promise::respond_to`: a stack-held promise for an asynchronous message, no allocation beyond the message) — Savina's `sender() ! result`. A computer made ready by its worker's message is prepended to the worker's own queue, so a pair usually shares a thread; which pairs share one, and whether a hop crosses a core, is the scheduler's decision |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads, `fifo_t::individual`; the dispatcher places. The ask is ONE `mutable_msg<msg_term>` per chain, redirected computer → worker → computer with `so_5::send(mbox, std::move(cmd))` — SObjectizer's own idiom for a message passed along (`sample/so_5/mutable_msg_agents`), no allocation per hop. A SObjectizer message has no sender, so the term carries the worker's index, as Savina's ComputeMessage carries its sender |
| floor | one thread, the master and every series in its ring | two pinned threads, qb's placement: a series is ONE slot holding its worker's and its computer's state; a request is a push into the owner's own ring and the answer a push back. The per-actor mailbox and the per-role dispatch are engineered out — that is what the floor is for. The burst is sent from the caller's thread, which is worker 0: at `cores=1` a full ring is drained inline, so the chains run in slices of a ring while the burst is still being sent |

## The verified answer

Every series is strictly sequential — one request in flight per series, ever — so its worker
folds every term it receives, IN ORDER, by its exact bit pattern: `chain = (chain ^ bits(x)) ·
0x100000001b3` (FNV-1a's 64-bit step), seeded with the series' identity. An ordered fold and not a
sum, and every term and not only the last: at these rates the orbit settles on a period-4 cycle, so
a run that computed four terms too few would end on the same final term, and series 0 ends on 0
however many it computes. When worker *i* answers GetTerm the master adds `series_key(i, chain)`;
when computer *i* reports the master adds `computer_key(i, served)` — wrapping sums of `mix()`.
The expected total is the same recurrence run with no framework linked (`series_chain` in the spec
header). A term dropped, computed twice, answered to the wrong worker, computed at the wrong
series' rate, or answered by GetTerm before the chain ended changes a fold; a request served twice
or never changes a report. `expected_messages = series × (3·terms + 4)` — per worker `terms`
NextTerm, `terms` answers and one GetTerm, per computer `terms` requests and one stop, and the
master's `series` answers and `series` reports — is counted at the receivers and asserted
alongside.

**Reported beside the cell, not asserted: `held`**, the NextTerm requests that found their worker
waiting for an answer. It says how the master's burst interleaved with the chains — every request
but each series' first when the whole burst is delivered before any answer, fewer when answers
overtake its tail (the floor at `cores=1`, a pool that runs a pair while the master is still
sending) — and it does not change the amount of work: `terms` requests reach each computer either
way.

## The measured window

Opens when the master sends the burst into an already-running system — every worker and every
computer has reported ready (wired, for CAF), so every thread is up — and closes when the master
has received the last computer's report. The burst, every hold, every round trip of every chain,
the GetTerm answers and the computers' stop-and-report are inside it. The actors are created
before the window and torn down after it.
