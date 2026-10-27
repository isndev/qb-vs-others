# savina/uct

Savina's Unbalanced Cobwebbed Tree, `uct`, of the "parallelism" group (Imam & Sarkar, *Savina —
An Actor Benchmark Suite*, AGERE 2014; `UctConfig.java` and `UctAkkaActorBenchmark.scala` in
`shamsimam/savina`). A root and a tree of node actors that **grows while the run lasts**: the root
creates ten children, every node asks the root whether it may have children of its own, the root
grants some requests and ignores the others, and a granted node creates ten children that ask in
turn. Once the tree is complete the root sends a traversal down it.

## What it measures, and what it does not

It measures **creation on demand into an unbalanced tree**: 59 990 node actors created inside the
window at the defaults, each by a parent the root picked, plus two traffic patterns no earlier
shape has together — a **funnel** into one actor (every node's request lands on the root: the
"cobweb", every node holding a reference to the root and talking to it) and two **waves** over an
irregular tree (the traversal down, the reports up). Half the nodes are leaves, and the subtrees
under the root's ten children differ by four orders of magnitude (below), so where a framework
places what it creates — and whether it can move it later — is what the cell asks.

It does **not** measure computation at the defaults: Savina's busy loops are dead code (below), and
`work=0` runs none. It does not measure a priority mailbox either: Savina's urgent nodes are kept
as data (below), and the only behaviour that would read them is unreachable in Savina's own actor
implementation.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `nodes` | 60 000 | 200 000 (`MAX_NODES`) | **deviation — see below**; the tree has 59 991 nodes, the root included |
| `binomial` | 10 | 10 (`BINOMIAL_PARAM`) | the children of a node that has any |
| `avg` | 500 | 500 (`AVG_COMP_SIZE`) | the mean computation size a grant hands the children |
| `stdev` | 100 | 100 (`STDEV_COMP_SIZE`) | its standard deviation |
| `urgent` | 50 | 50 (`URGENT_NODE_PERCENT`) | a grant is urgent when a draw in [0, 100) is at most this — 51 % of grants, as in Savina |
| `work` | 0 | dead loops | **deviation in kind — see below** |
| `cores` | 2 | n/a | the root on core 0, the subtree of the root's child i on core i % cores, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

At the defaults the tree has 5 998 grants (3 072 of them urgent), 53 992 leaves, a height of 7
and these levels (`plan()` in the spec):

| depth | nodes | coin says "children" | granted |
|---:|---:|---:|---:|
| 1 | 10 | 5 | 5 |
| 2 | 50 | 19 | 19 |
| 3 | 190 | 104 | 104 |
| 4 | 1 040 | 539 | 539 |
| 5 | 5 390 | 2 704 | 2 704 |
| 6 (the cap depth) | 27 040 | 13 434 | 2 627 |
| 7 | 26 270 | 12 984 | 0 |

The ten subtrees under the root's children hold 1, 1, 10 811, 1, 13 261, 13 311, 7 071, 15 531, 1
and 1 nodes: half the root's own children are leaves, which is the imbalance the shape's name
promises.

### Deviation from Savina's cap, and why

Savina's `MAX_NODES` is 200 000, and every node of the tree is alive until the end — a node is
created, asks, is answered or not, and waits for the traversal; nothing ends before the tree is
complete. The delivered qb 3.2.1 `ActorId` carries a **16-bit** per-core index (`ServiceIdPool`): at most 65 534 actors
alive on one VirtualCore. At `cores=1` the whole tree is on one core, and at `cores=2` qb's static
placement puts 103 685 of Savina's 199 990 nodes on core 0: Savina's cap does not fit, in any
configuration on that delivered identity domain — measured: its qb adapter at `nodes=200000` fails both core counts with
`qvo::fail()` naming the pool, where CAF, SObjectizer and the floor verify. 60 000 is a round cap
with headroom under the per-core limit at `cores=1` (59 991 nodes against 65 534; `nodes=65000`,
64 991 nodes, still verifies); it applies to every framework, and
`--param nodes=200000` reproduces Savina's tree (199 990 nodes, 19 998 grants, cap depth 7). The
60 000 case is the common comparison variant across all four frameworks and every host; the
canonical 200 000 case is a separate document beside it.

**qb 3.3.0 holds the canonical tree.** The 64-bit `ActorId` that shipped with 3.3.0 (Huly QB-970 /
QB-971) carries a **24-bit** ordinary slot per VirtualCore —
[`MaxOrdinarySlot`](https://github.com/isndev/qb/blob/e18022dd9cba98e3fac4a4b6c3adbdcda8567ad7/src/qb/core/ActorId.h#L438)
at the release, 16 711 679 live actors against 3.2.1's 65 534 — and the first measurement of this
shape (macOS, 2026-10-27, unpinned, `results/macbook-m4pro-macos-clang21/wave-bc-form-sweep/`,
`docs/TUNING.md` §22.4) verifies qb at `nodes=200000` in every configuration: 136.5 / 138.5 ns per
node at one core (spin / park) against 128.6 / 129.0 at the 60 000-node cell, 102.5 / 101.3 against
91.8 / 90.8 at two — 6–12 % more per node for a tree 3.3 times larger — where CAF reads 1.21–1.27 µs,
SObjectizer 1.30–1.66 and the floor's heap node 68–77 ns. The qb adapter's caveat says so since that
session; the cell stays at 60 000 so that every host's table compares the same tree, the x86 waves
having been measured at it — moving the cell to Savina's cap is a roster decision, not a host's.

### The tree, for every interleaving

Savina's root draws from one `java.util.Random(2)` (`UctAkkaActorBenchmark.scala:49`) **in the
order the requests arrive** — `nextBoolean` for "children or not", `getNextNormal` for the
computation size, `nextInt(100)` for urgency, `nextInt(BINOMIAL_PARAM)` for the urgent child — and
grants while `size + BINOMIAL_PARAM <= MAX_NODES` (`:108`), `size` counting every node created so
far, in arrival order; it even numbers the new nodes with its running count (`currentId = size`).
Which nodes get children, and the whole shape of the tree, are therefore decided by the scheduler:
two runs of the same framework build two different trees, and no framework-free value could check
any of them.

Here every draw is a function of the **node**, never of an arrival order:

* a node's identity is its position in the complete `binomial`-ary tree — the root is 0, child `i`
  of node `v` is `v × binomial + i + 1` — a function of its path;
* the root's draw about node `v` is `mix(salt ^ v)`: bit 0 is the coin, the next bits the urgency
  roll and the urgent child; a grant's computation size is an Irwin–Hall normal variate (twelve
  16-bit lanes of three `mix()` words, centred and scaled — the same mean and deviation, bounded
  at six deviations, integer arithmetic) drawn again until positive, as `getNextNormal` does. It
  replaces `nextGaussian`, whose `log` and `sqrt` are not bit-portable across C libraries;
* the cap is applied in a **fixed order** the spec computes before the run (`plan()`): level by
  level from the root, every node whose coin says "children" is granted while the cap allows a
  whole level; at the first level that does not fit — the cap depth, 6 at the defaults — the
  nodes whose coin says "children" are granted in the order of a per-node priority (`prio(id)`,
  the identity breaking a tie) until the cap is reached; nobody deeper is granted.

So the tree has exactly the size Savina's has — `1 + B + B × g` nodes, `g` the largest number of
grants the cap allows — whenever the coins allow that many, and the same shape in every run of
every framework. The fixed order stands in for Savina's arrival order, which in practice is also
level after level: each level waits for the round trip of the one above. The root still does the
deciding — `decide()` is the root's draw, one `mix()` per request and three more per grant, the
counterpart of Savina's `Random` calls — but the plan's two numbers that ration the cap depth are
computed before the window, outside every framework.

### The busy work

Savina's node runs `UctConfig.loop(100, 40 000)` before it asks (`:246`) and
`loop(compSize, 40 000)` in a traversal (`:308`): a loop counting to `busywait × dummy` into a
local nobody reads (`UctConfig.java:68`) — 4 000 000 iterations per request and about 20 000 000
per traversal. On a JVM that is dead code a JIT deletes; executed, it would make one repetition of
the default tree last hours. So the default here runs **no busy work** (`work=0`), and the cell
measures the tree's creation, the funnel and the two waves. `--param work=k` puts a deterministic
computation back with Savina's ratio — `qvo::spin_work` for `100 k` iterations before each request
and `comp × k` in each traversal — folded into the answer, so a node that skipped it fails.

### Deviation from Savina's end, and why

Savina's root starts the traversal at the **first request the cap refuses** (`:137`) — while other
parts of the tree may still be growing — then sends `TerminateMessage` down the tree right behind
it and exits; each node passes Terminate on and exits, requests that arrive later go to dead
letters, and the driver waits for the actor system to terminate. Two things change here, for the
same reason — the answer has to be asserted (FAIRNESS.md §0):

* the traversal starts when the tree is **complete**: the root counts the requests, and once it has
  heard from `binomial × (grants + 1)` nodes — every node there is, since every node asks once and
  every grant adds `binomial` — every grant has been acted on and no node will be created again;
* the Terminate wave is replaced by a **report wave** of the same size going up: a node reports to
  its parent once it has traversed and heard from its children, and ends there. The root's last
  report closes the window. One message per node either way.

### Savina's urgency, kept and inert

Half the grants are `UrgentGenerateChildrenMessage`, which marks one child urgent; the urgent
traversal that would read the mark (`urgentTraverse()`, `:323`) is triggered only by
`UrgentTraverseMessage`, which only `urgentTraverse()` itself sends (`:333`) — Savina's traversal
starts with plain `TraverseMessage` and never reaches it. Every adapter here sends the two grant
messages as Savina does and records the urgent flag; the answer checks both, and nothing else reads
them.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the root and every node on VirtualCore 0; a node creates its children with `addRefActor` (same core, synchronous `onInit`), reports with one `push<Report>` and `kill()`s itself | the root on VirtualCore 0 creates its children `i` with `i % 2 == 0`; the others are created on VirtualCore 1 by a **planter** — an actor created there before the window, asked once with one `Plant` event: qb has no cross-core spawn, and asking an actor that lives on the other core is how a qb actor creates there. A node then lives on its parent's core: core 0 holds 31 145 of the 59 990 nodes, core 1 28 845, and nothing balances them as they grow |
| CAF | `max-threads=1` | `=2`, both pinned; every node is `self->spawn<>` from its parent's behavior and the work-stealing pool places it — a runnable node is stolen by the idle worker |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; a grant registers **one child coop holding the whole sibling group** (they are created together and end together), and the parent deregisters it once all of them have reported — 5 999 coops per repetition |
| floor | one thread; a node is a heap record, freed by its parent once it has reported | two pinned threads; a node on its parent's worker, the root's child `i` on worker `i % 2` — allocated by the root and handed over with its first message, without a planter; the same static placement qb has, so the floor bounds the placing frameworks and not the pools |

## The verified answer

Every node reports once, to its parent, after its traversal and its children's reports: what it
is (its identity, its depth, the computation size its parent's grant gave it, whether it is urgent,
whether it was granted), what it learnt (the identities of its children that told it they were
granted — Savina's `UpdateGrantMessage`), its busy work and every delivery it handled, added to its
children's reports. Every field is a plain wrapping sum — no `mix()` inside the window (Huly
QB-1017) — and the root's total is mixed, field by field, into the checksum after the window.
`plan()` computes the same sums from the planned tree; `expected_messages` = 4 per node + 2 per
grant (one TryGenerateChildren, one request, one Traverse and one report per node; one grant and
one UpdateGrant per grant): 251 956 at the defaults, **counted at the receivers**. So the answer is
the tree — its node count and every node's visit — and is the same for every interleaving.

**What blocks, and what moves the sum** (FAIRNESS.md section 0).

* A request, a grant, a TryGenerateChildren, a Traverse or a report **lost** leaves the root or a
  parent waiting: the run **blocks**, the repetition outlives the harness's deadline and fails with
  exit 1 and no timing.
* A grant delivered to the wrong node grows children under another identity, which moves the
  identity sums; a grant carrying another computation size or urgency moves `comp_w` or
  `urgent_w`; an UpdateGrant lost or misrouted — which blocks nothing — moves `update_w` against
  `grant_w`; a traversal that skipped its busy work misses its result (`work > 0`); a delivery a
  handler did not count moves the message count.
* What the protocol itself can see is a **failure at once** (`qvo::fail()` through the spec's
  `fail()`): a request after the tree is complete (a request delivered twice), a second grant at a
  node, a second Traverse at a node, a report beyond a node's children, an UpdateGrant repeated.

The negative-control battery plants each of these classes in `tools/control/uct-subject.cpp` — the
protocol on one thread through the spec's own `decide()`, `Tally` and `checksum()` — and asserts
each is rejected by the check meant to see it (section V of `tools/negative-control.py`).

What neither sees: a node delivered a duplicate it never acts on after it ended — in qb and CAF a
message to an actor that has ended is dropped. And, as for every shape that creates actors
(`fork-join-create.md`), the checksum does not prove that a node was an actor: that each is a real
actor of its framework, created by that framework's creation primitive, is established by reading
the four adapters.

SObjectizer's dispatcher binder is owned by the spawning root/node agents and their coops,
following the fib adapter. The launch initializer retains no binder after initialization,
and the body-owned Field holds no binder: agent/cooperation teardown releases every retained
owner while the environment is alive. Windows repeated teardown remains part of validation.

## The measured window

Opens when the root starts Savina's `generateTree` in an already-running system — for qb once the
root and every planter have reported ready, for CAF on the root's first message, for SObjectizer
in the root's `so_evt_start()`, for the floor on the calling thread once its workers are up — and
closes when the root has the last of its children's reports. Every creation, every request, both
waves and every node's end are inside it; the plan the root decides by is computed before it, once
per repetition, and the root and the planters are created before it.

How much of a node's END lands inside the window differs by framework, as in `fork-join-create.md`:
qb, CAF and the floor reclaim a node on the thread that ran it, and only the last few nodes to
report can finish after the window closes; SObjectizer runs the final deregistration of every coop
on a thread of its own outside the `cores` budget, and the coops still in its chain when the root
receives its last report are released after the window.

## Forms

No adapter declares a second form. SObjectizer's one-coop-per-node lifecycle (`fib.md`) would only
add work here: a sibling group is created together and ends together. CAF's `caf::lazy_init`, tried
on `fork-join-create`, showed no gain there and is not used.
