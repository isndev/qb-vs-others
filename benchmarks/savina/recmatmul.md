# savina/recmatmul

Savina's `recmatmul` benchmark of the "parallelism" group (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `MatMulConfig.java` and `MatMulAkkaActorBenchmark.scala`) — the
product of two 1 024 × 1 024 matrices by **recursive decomposition** over a master and a fixed pool
of workers: a task above a block threshold is split into eight tasks of half the dimension, sent
back to the master and forwarded to the worker Savina's routing names; a task at or below it is
multiplied into its quarter of C.

## What it measures, and what it does not

It measures **work created by the work, relayed through one master, and the work at its leaves**.
At the defaults the root task splits three times: 73 splits create 584 tasks, and the 512 leaves
are block products of 128 × 128 × 128 multiply-adds — 1.07 × 10⁹ in all, Savina's own triple loop
(`benchmarks/specs/qvospec/savina/recmatmul.h`, `multiply()`, the same code in every implementation).
The messages — 585 tasks to workers, 584 created tasks and 585 DONEs to the master, 1 754 in all —
are a vanishing share of a cell of a second; what differs between frameworks is where each leaf runs.

The creation in this shape is the **tasks'**, not the actors': Savina's master creates its 20
workers once, before the first task, and the recursion creates messages — the Huly line's
"création + travail" is read that way here, and no actor is created inside the window.

It does not measure balanced work, and that is Savina's own routing: a task for the quarter of C at
`(srC, scC)` goes to worker `(srC + scC) % workers`, which keeps every product of one quarter on one
worker (the reason workers add into C without a lock) — and at the defaults, every corner being a
multiple of 128 and 128 ≡ 8 (mod 20), **every task of the run goes to workers 0, 4, 8, 12 and 16**:
five workers of twenty, with 96, 104, 104, 96 and 112 leaves. That is the shape every framework is
given; what it does with five busy workers and two cores is the cell.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `length` | 1 024 | 1 024 | the matrices' dimension (`DATA_LENGTH`), a power of two (below) |
| `threshold` | 16 384 | 16 384 | the block count (dim²) at or under which a task is multiplied (`BLOCK_THRESHOLD`): leaves of 128 × 128 |
| `workers` | 20 | 20 | the pool (`NUM_WORKERS`); placement below |
| `cores` | 2 | n/a | worker-thread budget |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |
| `PRIORITIES` | — | 10 | **not implemented** — the priority-mailbox variant's; Savina's plain Akka actors ignore it, as here |

`length` must be a power of two: Savina halves the dimension at every split (`dim / 2`) and would
drop a row of an odd one. It is capped at 2 048, where every entry of C is still an exact double
(below).

### The data: a deviation in the values

Savina fills `A[i][k] = i` and `B[k][j] = j` and checks `C[i][j] = N·i·j` after the run. With those
values every inner range gives the same product — the A row is constant, the B column is constant —
so a task carrying the wrong inner corners (`scA`, `srB`) still produces Savina's C, and Savina's
own check accepts it. Here `A[i][k] = i + k + 1` and `B[k][j] = 2k + j + 1`: every inner range gives
a different sum, C is not symmetric, and every entry is a closed form,

    C[i][j] = n·a·b + (2a + b)·S1 + 2·S2,   a = i + 1, b = j + 1,
    S1 = n(n − 1)/2,  S2 = (n − 1)·n·(2n − 1)/6,

an integer below 2³⁵ at `length=2048` — as is every partial sum, so every addition is exact, C does
not depend on the order the products are added in, and the comparison is bitwise. The work — the
multiplies and the adds, their number and their order — is Savina's. The negative-control subject
replays every product on Savina's own data beside the spec's and shows the difference: a leaf
multiplying the other half of the inner index passes Savina's check and fails this one (section N
below).

### The layout

A, B and C are `length` × `length` doubles with each row padded by 16 doubles, so consecutive rows
start 8 320 bytes apart and every row starts on a 128-byte boundary: 8 192 bytes apart, the kernel's
column walk of B would map every element of a column to one L1 set, which Savina's `double[][]` —
rows that are separate JVM arrays, a header apart — does not do. A leaf's quarter of C (128 doubles
per row, 1 KiB) then never shares a cache line, or a 128-byte pair, with another worker's. The same
layout in every framework, built before the window, C zeroed before each repetition.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the master and the 20 workers on VirtualCore 0 | master on VirtualCore 0, worker `w` on `⌊w · 2 / 20⌋` — workers 0–9 on core 0, 10–19 on core 1 — so core 0 holds workers 0, 4 and 8 (**304 leaves**) and core 1 workers 12 and 16 (**208 leaves**): the best split of those five workers over two cores, fixed before start and never rebalanced; the master relays each created task with `forward()`, the same event, no copy |
| CAF | `max-threads=1` | `=2`, both pinned; a worker is a pool actor and an idle thread steals a runnable one, so the five busy workers are spread over the two threads dynamically |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; one coop, and the pool decides which thread runs a worker's next demand. The master relays a created task by redirecting the received message instance (`so_5::send(mbox, mhood)`) |
| floor | one thread | two pinned threads; the master is actor 0 on thread 0 and worker `w` on thread `⌊w · 2 / 20⌋` — qb's placement exactly, so the floor bounds the placing frameworks and not the pools |

**Why not `w % cores`**, the rule of every other shape here: with Savina's routing at the defaults
every busy worker has an even index, and `w % 2` would put all 512 leaves on core 0 — a two-core
cell that measures one core. Contiguous blocks put the five busy workers 3 / 2, and a static
placement cannot do better than 304 / 208 (59 % of the leaves on one core) with five workers of 96–
112 leaves: the pools, which move a runnable worker to an idle thread, can come closer to half.

## The verified answer

The task tree is fixed by the parameters, and so is the worker every task is routed to: what each
worker must receive, split and multiply is a closed count, and C is a closed form. What is compared,
after the window: C, entry by entry, against its closed form; each worker's tally — the tasks it
received, the leaves it multiplied, the tasks it split, in a 128-byte slot of its own (for qb in the
actor, handed to the slot at teardown) — against the tree's counts for that worker; and the
master's counts — the tasks it received from the workers to forward (the created ones) and the
DONEs. `expected()` computes all of it with no product: the tree walked once, C's closed form,
O(length²). `expected_messages = 3 × tasks − 1` (1 754) is asserted alongside, counted at the
receivers. The ready handshake and the shutdown are not counted.

**What blocks, and what moves the sum** (FAIRNESS.md section 0). The master ends the run when it has
received one DONE per task it sent. A task lost between the master and a worker, or a DONE lost,
never ends the run: the repetition outlives the harness's deadline and fails with exit 1 and no
timing. A created task lost on its way to the master is never sent, so the run ENDS — every task
sent has its DONE — with a product missing from C, and the master's count short; a task delivered
twice adds its product twice and brings one DONE too many; a task routed with the wrong corners puts
a product in the wrong place; a worker that answers DONE with no product leaves its quarter short;
a task delivered to the wrong worker is multiplied where it should be — C is right — and moves two
tallies. `tools/negative-control.py` section N plants each in a one-thread subject that runs Savina's
protocol through the spec's own functions with a mailbox per sender
(`tools/control/recmatmul-subject.cpp`) — the misroute rejected by the tallies alone with C right,
the wrong inner half rejected where Savina's data would accept it — and watches deliveries
interleaved at random, each sender's order kept, verify. The tallies were removed from a scratch
copy of the spec and the misroute then passed: they are what sees it. The real qb and floor adapters
were planted too while this shape was written (the first leaf misrouted, the first leaf multiplied
twice), and rejected at `cores=1` and `2`.

**What neither sees.** Two workers adding into one quarter at once would be a race on C that the
routing rules out; a misroute that made one — the planted one does not overlap — could still end
on a right C by chance of timing, and is rejected by the tallies whatever C holds.

## The floor's bound

The floor's runtime drains a full ring that the sending thread owns inline, a handler running inside
the send, and a tree whose messages outnumber one ring (65 536) nests those drains until the stack
runs out — measured at `length=128 threshold=4`, 299 593 tasks, a stack overflow on Windows. Every
message of a run fits in one ring below that bound (1 754 at the defaults), so no ring ever fills;
above it the floor's adapter declares the cell **not applicable** (exit 3) rather than run a
configuration that can crash. The three frameworks have unbounded mailboxes and run it.

## The measured window

Opens when the master has received *ready* from all 20 workers and sends the root task. Closes when
the master has as many DONEs as it has sent tasks; that end is exact because every channel is
per-sender FIFO — a worker's eight tasks reach the master before the DONE of the task that made
them. Building A, B and C and zeroing C are outside it, and so is the shutdown — qb's
`broadcast<qb::KillEvent>()`, CAF's `close_atom` to each worker, SObjectizer's
`environment_t::stop()`, the floor's `Mesh::stop()` — where Savina's own timing includes its actor
system's start and its `StopMessage` round; they are measured as setup and teardown.

## Cost of a repetition

About 1.3 s at one core and 0.8 s at two in the functional runs on Windows/MSVC (pinned CPUs 8 and
10, a busy host — not a measurement): 2.6 ms per leaf at one core, against 0.89 ms per leaf with
g++ 14 on WSL2 — the two compilers' code for the same triple loop, the same for every framework on
one of them. The harness's default deadline, 300 s per repetition, holds it with room.
