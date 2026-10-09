# savina/a-star

Savina's Guided Search benchmark, `astar`, of the "parallelism" group (Imam & Sarkar, *Savina —
An Actor Benchmark Suite*, AGERE 2014; `GuidedSearchConfig.java` and
`GuidedSearchAkkaActorBenchmark.scala` in `shamsimam/savina`). A master and 20 search workers
explore one random graph on a 30 × 30 × 30 lattice: a worker walks it breadth-first from the node
it was given, claims every neighbour it reaches, does a fixed amount of busy work per node, and
after 1 024 nodes hands its whole frontier back to the master, one message per node, for the
master to deal out again.

## What it measures, and what it does not

It measures **a fixed amount of computation redistributed by messages**. Every reachable node is
searched exactly once and costs the same busy work in every framework — the search step is the
spec's own code, linked into every binary — so what differs between the rows is what it costs to
move a node from a worker to the master and on to the next worker, and how well the framework (or
its placement) spreads 20 workers' chunks over the cores. It is the first benchmark here in which
a message carries **work** by default: `fib`'s nodes compute nothing and `fork-join` runs at
`work=0`. `docs/ROADMAP.md` lists it beside `nqueens` as "creation with WORK per actor"; it
creates no actor inside the window — the 20 workers exist before it opens — so what it adds is
work per MESSAGE. The master is a real hot spot: every handed-back node goes through it, ~1 800
of them per repetition on one thread.

It does **not** measure message throughput: 100 rounds of busy work per node against one message
per handed-back node make plumbing a small share of the window by design, and a framework's
per-message cost shows up here only where it delays a worker that has nothing else to do. And it
does not measure a priority mailbox: Savina's `-p` (`PRIORITIES`) feeds only the priority-actor
variants (`GuidedSearchAkkaPriorityActorBenchmark`), not the plain Akka benchmark this one
follows.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `workers` | 20 | 20 (`NUM_WORKERS`) | no deviation |
| `grid` | 30 | 30 (`GRID_SIZE`) | 27 000 nodes, 78 712 edges, 20 515 reachable from the origin; capped at 248 like Savina's |
| `threshold` | 1 024 | 1 024 (`THRESHOLD`) | nodes one work message searches before handing its frontier back |
| `work` | 100 | 100 (`busyWait()`) | **deviation in kind — see below** |
| `cores` | 2 | n/a | the master is actor 0 and search worker w actor w + 1; actor a on core a % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### The graph, bit for bit

The graph is Savina's own. `GuidedSearchConfig.initializeData()` is transcribed in the spec:
java.util.Random's 48-bit LCG seeded with 123456, the six lattice steps per node visited in Java's
loop order, the seventh step kept unconditionally when no earlier one was — and drawing no bit
then, because Java's `||` short-circuits — and the nodes drawing their bits in the order Savina's
`HashMap<Integer, GridNode>` iterates them, which is ascending `id ^ (id >> 16)`: ascending id up
to grid 40, a fixed permutation above. Checked against Savina's own Java (the `initializeData()`
body copied verbatim, OpenJDK 21) at grid 2, 3, 7, 10, 30, 40, 41, 64 and 100: the edge count, an
order-sensitive fold over every adjacency list and the reachable count are identical at all nine.
Savina's target node, (24, 24, 24), is among the 20 515 reachable at grid 30.

### The one deviation in kind: the search runs to exhaustion

Savina's worker sends `DoneMessage` the moment it claims the target, and the master stops the run.
Which worker claims what is a race, so where the target falls in the search — and with it how many
nodes are searched, how many messages are sent and how long the run takes — changes from run to
run; a run that stopped on the target would have no answer anyone could compute in advance. Here
the search never stops on the target: it runs until the master has as many acknowledgements as it
sent work messages, which is Savina's own **other** termination (`numWorkCompleted ==
numWorkSent`, the one it falls back to when the target is unreachable). The amount of work is then
fixed — every reachable node searched exactly once, whoever claims it — and so is the answer.
What is lost is the early exit; what Savina measures besides it is all here: the graph, the
master, the round-robin, the 1 024-node chunks, the frontier handed back one node per message,
the acknowledgements and the claims.

Two smaller departures follow from it. Savina's claim also records a distance from the root
(`distanceFromRoot`, set after the compare-and-swap); nothing reads it, its value depends on which
worker won the claim, and in C++ the plain field Savina writes from several threads would be a data
race, so the claim here is the compare-and-swap alone. And Savina's worker drops the rest of its
queue when it finds the target; with no early exit, nothing is dropped.

### The busy work

Savina's `busyWait()` is 100 calls to `Math.random()`. Its generator is ONE `java.util.Random`
shared by the whole JVM, advanced by a compare-and-swap on a single `AtomicLong`, so on more than
one thread Savina's per-node work is partly a contention benchmark on one cache line. Here it is
`qvo::spin_work(2n + 1, 100)` — 100 rounds of the harness's `mix()`, private to the thread, the
function every Savina benchmark in this repository uses for per-message computation — and its
result is folded into the checksum, so it can be neither skipped nor hoisted.

### The claims are shared memory

The one input not passed by message. Savina's `GridNode.parentInPath` is an `AtomicReference`
every worker compare-and-swaps from `null`, and that is the design of the benchmark, so the spec
keeps it: one `std::atomic<uint32_t>` slot per node (`Claims` in `a-star.h`), the same class in
every framework and in the floor. A claim publishes nothing — the graph is immutable and the
claimed node travels by message — so it is a relaxed compare-and-swap; on x86 every
compare-and-swap is a locked instruction regardless.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the master and the 20 workers on VirtualCore 0 | actor a on VirtualCore a % 2, the master being actor 0 and search worker w (w = 0 … 19) actor w + 1: the master and workers 1, 3, …, 19 on core 0, workers 0, 2, …, 18 on core 1, so the master's round-robin alternates cores and every other relayed node crosses one. The master relays with `forward()`: the event a worker pushed is the one the next worker receives |
| CAF | `max-threads=1` | `=2`, both pinned; the master spawns its workers with `self->spawn` and the work-stealing pool places them. A relay is a fresh mail: CAF has no forward of the current message outside the request/response path (`delegate` is deprecated in 1.1) |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; the master redirects the `msg_work` instance it received with `so_5::send(mbox, mhood)`, SObjectizer's own zero-copy relay |
| floor | one thread, one ring per (worker, worker) pair | two pinned threads; actor a owned by thread a % 2, the placement qb has |

In every framework a worker sends its frontier and then its acknowledgement to the master from
one handler, and each delivers one sender's messages to one receiver in order — qb's pipe, CAF's
per-sender ordering, SObjectizer's per-agent demand queue, the floor's per-pair ring — so the
frontier always lands before the acknowledgement that counts it, and the master never sees its
count of acknowledgements reach its count of work messages while a node is still in flight.

The static placement has a price the pools do not pay. With `cores=2`, qb and the floor put the
master on core 0 with 10 of the 20 workers, and a core runs one handler at a time: while one of
those workers searches a chunk (up to 1 024 nodes of busy work), every node handed back to the
master waits in core 0's queue, and so does every worker on core 1 that is waiting for one. CAF's
and SObjectizer's pools have no such constraint — the master is just another runnable actor that
either thread may pick up next — so on this shape the placing rows pay for the master's placement
and the pool rows do not. The cell records that asymmetry; it is the same one `big.md` names from
the other side, where static placement is what pays.

## The verified answer

Searching node n yields `node_value(n) = mix(spin_work(2n + 1, work))`; a work message's
acknowledgement carries the **wrapping sum** of those over the nodes it searched, and the master
adds the acknowledgements up. `expected()` is the same sum over the reachable set, computed by a
breadth-first walk of the graph with no framework linked. `spin_work` is a chain of bijections
from a distinct seed, so no two nodes yield the same term: a work message lost — master to worker
or worker back to master — drops at least its node from the sum, a duplicated one counts a node
twice, and both fail the run. Planted in the qb and floor adapters (the master losing the first
node handed back; the master delivering its first relayed node twice), each exits 1 with a wrong
checksum at `cores=1` and `cores=2`.

There is **no `expected_messages`**. How many work messages a run sends is decided by which worker
wins each claim — a node claimed late in a worker's chunk is handed back as a message, the same
node claimed early by another is searched in place — so it is fixed on one thread and varies on
two: measured, 1 790 work messages on every repetition at `cores=1` for both qb and the floor, and
1 338 to 1 630 over ten repetitions at `cores=2`. A count that depends on a race is not a count
(`big.md` says the same of a ponger's pings). What is fixed is the WORK, every reachable node
searched exactly once, and the checksum asserts it node by node. The report's unit is therefore
the node — `work_units` is the reachable count, 20 515 — not the message.

What the checksum does **not** prove is that the work was redistributed: an adapter whose workers
ignored `threshold` would search the whole graph from the first work message and still verify.
The spec derives the floor that closes it, with no framework: one work message searches at most
`threshold` nodes and every reachable node is searched once, so a run sends at least
`min_work_messages = ceil(reachable / threshold)` work messages, 21 at the defaults. It is
**asserted** (next section): a run that reports fewer work messages, or reports none, fails like a
wrong checksum. The harness checks the bound only after the checksum has verified — once every
reachable node is known to have been searched exactly once — so a count below the bound can only
mean that some work message searched more than `threshold` nodes.

## What is observed, not asserted — and the one bound that is

FAIRNESS.md section 0: work whose amount depends on the interleaving is reported beside the cell,
never silently compared; a minimum the semantics requires is asserted. Every adapter — the three
frameworks and the floor — returns the master's count of work messages sent as the observation
`work_messages` (`qvo::Answer::observed`; `kObservedWorkMessages` in the spec): the origin's
message plus one per frontier node a worker handed back, since the master relays each handed-back
node as exactly one work message — so it is the hand-back count plus one. Every measured
repetition's value is written into the result document (`"observed": {"work_messages": {samples,
min, p50, max}}`) and `tools/report.py` prints the median and the range in a sub-row under the cell,
so a row whose interleaving handed back half as many nodes as another's is never compared with it
silently (the counts above: 1 790 on one thread, 1 338 to 1 630 on two).

Every adapter's `main()` also declares `min_work_messages` as the name's lower bound
(`qvo::Spec::observed_at_least`), and the document records it (`"observed_at_least":
{"work_messages": 21}` at the defaults). The proof that no correct run can go below it, with no
framework: the checksum has established that each of the `R` reachable nodes was searched exactly
once; the spec's `search()` stops a work message after `threshold` nodes, so `W` work messages
search at most `W · threshold` nodes; hence `W · threshold ≥ R` and `W ≥ ceil(R / threshold)`. An
adapter that ignored `threshold` would search the graph from the origin's message alone and report
`W = 1`, below 21: it fails. What the bound does not catch is a threshold merely too large — a
doubled one still sends hundreds of messages — and that stays held by the adapters calling the
spec's `search()` with the parameter, which is a reading of the source.

## The measured window

Opens when the master sends the origin to the first worker, once all 20 workers have reported
ready — every thread running and every worker scheduled once — and closes when the master has as
many acknowledgements as it sent work messages. The graph is built and the claim slots allocated
before the framework starts, once per repetition, outside the window; every search, claim, hand-back,
relay and acknowledgement is inside it.
