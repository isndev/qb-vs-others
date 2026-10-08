# savina/nqueens

Savina's `nqueenk` benchmark of the "parallelism" group (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014) — the N-Queens search split across a **master and a fixed pool of
workers**: the master hands out partial boards round-robin, a worker either extends a board by one
row and sends every valid extension back to the master as new work, or — past a depth threshold —
searches the rest of the tree itself and reports each solution. It is the first benchmark in this
repository whose time is dominated by **work done inside the handlers**, not by the messages
between them.

## What it measures, and what it does not

It measures **where a framework runs a CPU-bound search that arrives as messages**, and what a
relay through one master costs. At the defaults the master forwards 4 959 work items and receives
14 200 results, and between them the workers re-validate every board of a complete 12-queens
search (Savina's `boardValid` checks every pair of queens for every candidate column). The search
kernel is the same code in every implementation (`benchmarks/specs/qvospec/savina/nqueens.h`,
`process()`), so the difference between two cells is the framework's: how evenly its scheduler
spreads the items over the cores it was given, the relay through the master, and the 29 076
messages of the run.

It does **not** measure actor creation: the master and its 20 workers are created before the
window and nothing is created inside it — a work item is a message to an existing worker, not an
actor. And at `cores=2` it measures two different placement models side by side, by design: qb and
the floor keep worker `w` on one core and the master's rotation decides where each item runs; CAF's
work-stealing pool and SObjectizer's thread pool move a runnable worker to an idle thread. A cell
where the rotation leaves one core with more search than the other shows that, and the caveats say
so next to the number.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `size` | 12 | 12 | the board; 14 200 solutions (`NQueensConfig.SOLUTIONS`) |
| `threshold` | 4 | 4 | depth from which a worker searches sequentially instead of splitting |
| `workers` | 20 | 20 | the pool; worker `w` on core `w % cores` where the framework places |
| `cores` | 2 | n/a | worker-thread budget |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |
| `SOLUTIONS_LIMIT` | — | 1 500 000 | **not implemented — see below** |
| `PRIORITIES` | — | 10 | **not implemented — see below** |

### Savina's early stop, and why it is not here

Savina's master counts results and asks every worker to stop once it has seen `SOLUTIONS_LIMIT` of
them — the "first K solutions" of the benchmark's name. At Savina's own defaults the limit is
1 500 000 and a 12×12 board has 14 200 solutions, so the stop never fires: Savina's default run
searches the whole tree, and so does this one — the work measured here is the work Savina's
defaults measure. Below the solution count the stop would make *which* solutions were counted, and
how many more arrive after the stop was sent, a race between the workers; a race has no answer that
can be asserted (FAIRNESS.md §0), so the parameter is left out rather than implemented and never
verified. (Savina's own check of its run, `actSolution >= solutionsLimit`, rejects its default run
for the same reason: 14 200 is below 1 500 000.)

### Priorities

`PRIORITIES` belongs to Savina's priority-mailbox variant (`NQueensAkkaPriorityActorBenchmark`);
the plain actor variant every framework here is compared on carries the field and ignores it.

### The board

Savina allocates a fresh `int[]` per candidate column. Here a board travels packed in two 64-bit
words (5 bits per row, the depth in the top byte) — one event in every framework, the two payload
words of the floor's ring message — and the kernel works on a stack array. The search, the order
of the candidates and the full-board validity check are Savina's.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the master and the 20 workers on VirtualCore 0 | master on VirtualCore 0, worker `w` on `w % 2`; the master's rotation alternates the cores and **an item runs where the rotation sends it** — qb has no work stealing, so nothing moves an item to an idle core |
| CAF | `max-threads=1` | `=2`, both pinned; the workers are pool actors and an idle thread steals a runnable one, so the search is balanced dynamically |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; the master and the workers are one coop, and the pool decides which thread runs a worker's next demand. The master relays a child item by redirecting the received message instance (`so_5::send(mbox, mhood)`), SObjectizer's copy-free relay |
| floor | one thread | two pinned threads; the master is actor 0 on thread 0 and worker `w` is actor `1 + w` on thread `(1 + w) % 2` — the same static placement qb has, so the floor bounds the placing frameworks and not the pools |

## The verified answer

Every RESULT carries the solution's identity — a fold of its columns, row by row, through
`qvo::mix` — and every DONE the identity of the item it completes, salted so that a board that is
both an item and a solution counts twice, distinctly. The master sums both, as **wrapping sums**,
and the checksum is that sum. A work item lost, duplicated or delivered twice changes the set of
boards searched and therefore both sums; a result lost or duplicated changes the first; a done lost
never ends the run. A worker that answered without searching would have to produce every
solution's identity, which is the search.

`expected()` computes the same sum with **no shared search code**: an independent enumeration over
column and diagonal bitmasks, the identity folded incrementally, which also asserts its solution
count against Savina's `NQueensConfig.SOLUTIONS` table — the kernel the frameworks run and the
reference that checks them cannot agree by construction. `expected_messages = 3 × items − 1 +
solutions` (29 076 at the defaults) is asserted alongside: each worker counts the item it received
and says so in that item's DONE, and the master counts every child item it relays (all but the
first, which it made), every DONE and every RESULT. The ready handshake and the shutdown are not
counted.

## The measured window

Opens when the master has received *ready* from all 20 workers — every thread is up and every
worker holds its behaviour — and hands the empty board to the first worker of its rotation. Closes
when the master has received as many DONE as it has forwarded items. That end is exact because
every channel here is per-sender FIFO: a worker sends an item's children and results before that
item's DONE, so the master cannot count the last DONE before the last item was forwarded. The
shutdown — qb's `broadcast<qb::KillEvent>()`, CAF's `close_atom` to each worker, SObjectizer's
`environment_t::stop()`, the floor's `Mesh::stop()` — runs after the window, where Savina's own
timing includes its stop protocol; it is measured as teardown, as for every benchmark here.
