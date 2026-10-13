# savina/bitonicsort

Savina's Bitonic Sort, one of the "parallel" benchmarks (Imam & Sarkar, *Savina — An Actor Benchmark
Suite*, AGERE 2014; `BitonicSortConfig.java` and `BitonicSortAkkaActorBenchmark.scala`, a port of
StreamIt's iterative bitonic sort) — a **sorting network built of actors**: tens of thousands of
small actors wired into a fixed topology before anything flows, and a stream of values pushed
through it one small message at a time.

## What it measures, and what it does not

It measures **many small messages through a fixed topology**: at the defaults 1 024 values cross a
network of 44 772 actors, 458 deliveries per value — 541 869 messages in all, each a value or a value
stamped with a lane number, an Exit, or the one Start — and each actor does almost nothing with what
it receives: forwards it, deals it round-robin, compares two values, or puts a value back in
round-robin order. The cell is the cost of a hop, through a population of actors far larger than any
cache, along a pipeline 55 steps deep.

It does **not** measure sorting: the network's 28 160 compare-exchanges are a sort a sequential
library does in microseconds. It does not measure actor creation either — the network is built, and
every actor has started, before the window. And it is not a parallel shape in the usual sense: a
value goes through the steps in order, so two cores can only overlap different values at different
steps, and the last stage's first step waits for half the stream before it sends anything.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `n` | 1 024 | `N` = 4 096 | **deviation — see below**; a power of two |
| `max` | 2^60 | `M` = 2^60 | no deviation; a value is `abs(nextLong()) % max` — and never binds, below |
| `seed` | 2 048 | `S` = 2 048 | no deviation: the input IS Savina's |
| `cores` | 2 | n/a | for the frameworks that place, the steps cut in two halves of equal message weight |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

**The input.** Savina's `PseudoRandom` (`value = (value * 1309 + 13849) & 65535`), reproduced exactly
in the spec as in savina/quicksort: values in [0, 65 535], so `max` never binds, and the first 65 536
draws of the full-period generator are distinct — 1 024 distinct values.

### Deviation from Savina's size, and why

The reference network for `n` values has `log2(n) (log2(n) + 1) / 2` steps of `n / 2` compare-exchanges
each, and around them the split-joins' splitters, joiners, adapters and forwarders. Counted by the
spec's `topology()`, it is **44 772 actors at `n` = 1 024, 104 720 at 2 048 and 241 984 at Savina's
4 096**. qb's actor id is a 16-bit slot per VirtualCore — 65 534 actors on one core (`Max number of
Actors reached`, qb `src/qb/core/Main.h:1170`) — so 4 096 fits neither one core nor two, and 2 048
not one: **1 024 is the largest network every framework in the field can hold at `cores=1`**, which is
savina/fib's deviation (n = 23 for Savina's 25) for the same cap. The cap is qb's and is reported as
qb's: the qb adapter declares a cell whose network does not fit **not applicable** (exit 3, the
reason in the document) before building anything, so `--param n=2048` runs every framework but qb's
one-core cells, and Savina's `--param n=4096` every framework but qb.

### The other deviations

- **The actors are ended after the window.** The reference's actors `exit()` as the Exit wave passes
  them; here the wave is forwarded the same way, and every actor is torn down once the sink has closed
  the window — 44 772 ends would otherwise be the last part of the cell for some frameworks and not
  for others, depending on where each puts an actor's end.
- **The window opens on a started network.** Every actor reports to the sink once it has started
  (qb's `onInit`, SObjectizer's `so_evt_start`, CAF's answer to a tick from a driver spawned last), and
  the sink sends the source its Start only when all 44 771 have — the reference starts its source as
  soon as the actors are created.
- **A joiner's lanes are sized before the window.** The reference buffers each lane in a growing
  `ListBuffer`; here each lane is a fixed slice of one buffer sized at construction to the values the
  run sends it (`Joiner`, `Node::cap`), and a lane receiving more stops the run with a named failure.

## The network, and the code every framework runs

The spec builds the reference's network as data (`topology()`, `bitonicsort.h`), exactly as the
reference's constructors nest — the kernel's merge stages for `P` = 2, 4, … `n`/2 and the last merge
stage of the whole sequence, each a chain of steps, each step a split-join of partitions (or of
compare-exchanges directly when its sequences are two long), each partition a split-join of
compare-exchanges — in **dependency order**: every actor's targets have smaller indices, so an adapter
creating actors in index order always holds the address of what it sends to. At the defaults: 1
source, 4 163 forwarders (the kernel, the stages, the steps, the partitions), 4 152 splitters, 28 160
compare-exchanges, 4 152 joiners, 4 097 ValueData and 46 DataValue adapters, 1 sink. What an actor does
with a message is the spec's code too — `Splitter`, `Comparator`, `Joiner` — so no framework's network
differs from another's in anything but the messaging:

| framework | a value, a stamped value, the Exit | idiom |
|---|---|---|
| qb | `Value{value}`, `Data{order, value}`, `Exit{}` events, `push` | one actor class per kind; a value that passes through unchanged (a forwarder, a splitter, a DataValue adapter) is the received event `push_forward`ed in place (qb `src/qb/core/Actor.h:1332`, "Forward a received event to a new destination with push's ordering, reusing the event object"); a compare-exchange, a joiner and a ValueData adapter push a new one. Every hop is ordered — never the unordered `forward` — because a joiner's lanes and the Exit wave rely on per-sender FIFO |
| CAF | a bare `uint64`, `(uint32 order, uint64 value)`, `close_atom` | one behavior per kind, every actor spawned before the window with the handles it sends to; CAF's mailbox is FIFO per sender |
| SObjectizer | `mutable_msg<msg_value>`, `mutable_msg<msg_data>`, a `msg_exit` signal | one agent class per kind on direct mboxes; a value that passes through unchanged is the received mutable message **redirected** (`so_5::send(next, std::move(cmd))`, SObjectizer's own `mutable_msg_agents` sample), only a compare-exchange, a joiner and a ValueData adapter allocate one |
| floor | the four-field `Msg` in a ring (`b` = the order) | every actor an integer, its state a vector slot, its kind a switch |

**SObjectizer declares two bindings** (FAIRNESS.md 1.1): every agent on the `thread_pool` with
`fifo_t::individual` — the cell's form until a quiet host decides — or, with `QVO_SO_GROUP_COOPS=1`,
each STEP of the network one coop bound with `fifo_t::cooperation` on the same pool, one demand queue
per step served by one work thread at a time: SObjectizer's spelling of qb keeping a step on one core
(`so_support.h`, the group binding; savina/logmap's grouped form is its table cell since the sweep of
2026-10-11). A grouped document carries the sweep caveat first. Which of the two enters the table is
for the measurement session's form sweep to decide.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | all 44 772 actors on VirtualCore 0 | each step whole on one VirtualCore, the steps cut into two contiguous runs of equal message weight (`placement()`: before step 27 of 55 at the defaults), every actor outside the steps with the step it feeds or is fed by — a value crosses a core once, never inside a step. Only the cores the placement populates are configured: qb refuses to start a VirtualCore that holds no actor (`Core Init Failed`), and the network of `n` = 2 is one step |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places all 44 772 actors |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads: `fifo_t::individual` (the default), or each step's coop on `fifo_t::cooperation` (`QVO_SO_GROUP_COOPS=1`) |
| floor | one thread | two pinned threads with qb's placement: an actor's mesh id is `slot * 2 + worker`, so the mesh routes it to the worker its step is on. The floor runs `n` up to 2^14: at most 1.5 `n` messages are ever in flight in this network (counted on the reference's FIFO), and a worker that finds one of its own rings full drains its inbound rings inline (`Mesh::send`), which could run a splitter's next value before the current one is queued; up to 2^14 the 65 536-slot rings hold 2.6 times what can be in flight, and above it the floor declares the cell not applicable |

## The verified answer

Every actor **tallies** what it receives (Huly QB-1017's verification): how many messages of any kind
— the Start, values, stamped values, Exits — and the wrapping sum of the values they carried. The
network is data-oblivious, so both numbers are **fixed for every actor** by the parameters: a
compare-exchange always sees the same two values, a step's actors always see the same stretch of the
stream, a joiner always gets one Exit per lane. Each tally is a **128-byte slot** of a vector the body
owns (thread-ring.md says why 128); CAF, SObjectizer and the floor add into the slot in the handler,
qb into two members of the actor, handed to the slot by the actor's destructor on its core after the
window. The sink records the values it receives. After the window the body reduces both through the
spec's `checksum()` — the wrapping sum of `mix(mix((i << 40) ^ count) ^ sum)` over the actors, plus a
term of the sink's sequence **measured** (its length, the multiset of its values, its descents) — and
`delivered()`, the counts. The expected value is the same reduction over the **reference network**:
the spec runs `topology()` on one thread through the same `Splitter`, `Comparator` and `Joiner`, and
refuses to run at all if that reference does not deliver its input sorted (`reference()`).
`expected_messages` = 541 869 at the defaults — 366 592 values, 102 400 stamped values, 72 876 Exits
and the Start — asserted exactly against the counted total. After teardown every adapter calls the
spec's `explain()`, which names on stderr the first actor whose tally is off, whether every COUNT is
right, and the output's length and descents.

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). The Exit wave is
what ends the run: a joiner forwards its Exit once every lane has closed, so **an Exit lost** leaves a
joiner open forever and the run **blocks** — the harness's deadline fails it with exit 1 and no timing
(the Start lost likewise). **A value lost does not block**: the compare-exchange waiting for its pair
never answers, its joiner's round-robin stalls at that lane, but the Exit wave still closes every
joiner, so the run ENDS — with every value the stalled lane held back missing (planted on the first
splitter, the sink received none). A value **duplicated** into a joiner overflows the lane the run
sizes for it, and the run stops with a named failure (`qvo::fail()`, exit 1); one duplicated at the
sink, or an Exit too many, moves a tally. A value delivered to the **wrong compare-exchange** puts it
in another actor's sum; a **consistent misroute** — a partition splitter pairing neighbours, values
`2k` and `2k + 1`, instead of `k` and `k + L/2` — hands every compare-exchange exactly two values, so
every COUNT is right and only the sums see it. A compare-exchange that orders its pair **the wrong
way** is the case the output alone misses: planted on the first compare-exchange whose pair differs,
the later stages repair it and the sink receives a SORTED sequence — the actors downstream see a
different pair, and their tallies reject it. A compare-exchange whose values pass on **without its handler running** keeps a
short tally. A planted version of each is rejected by the harness's negative control
(`tools/negative-control.py`, section S, on `tools/control/bitonicsort-subject.cpp`, the reference
network run on one thread with one defect at a time, refusing a plant whose victim the network lacks),
which also confirms that a correct network verifies and that an `n` not a power of two is refused.
What reaches neither verdict is a compare-exchange that skips its comparison when its pair already
arrived in order — it sends what a correct one sends — and a message still queued when the sink
closes the run, which changes nothing the run did.

## The measured window

Opens when the sink, having heard from all 44 771 other actors that they have started (outside the
window), sends the source its Start; closes when the sink receives its first Exit, the end of the
wave. Every value's journey through the 55 steps and every Exit is inside it; building the network,
starting its actors and ending them are outside.

## Spin and park

`wait=1` and `wait=0` are each framework's spin and park, as everywhere (the support headers). With
the whole stream injected at once, a core in this shape rarely runs dry inside the window; at two cores
the first half's core finishes before the second's, which is where a park could land.

## What the correctness runs showed

Correctness runs, not measurements — unpinned, on a host other agents were using; every cell
verified, on WSL2/g++-14 and Windows/MSVC, at the defaults and at `n` = 2, 4 (WSL2), 64 and 2 048
(`max=1000`; qb's two one-core cells not applicable, as declared). For the campaign to confirm or
refute:

- qb runs the 541 869 messages in 4.7–5.0 ms on Windows at both core counts — ~9 ns a message —
  beside the floor's 5.1–7.2 ms, and in 5–24 ms on WSL2, where other agents' builds shared the host;
  its two-core cell is no faster than its one-core one: the cut puts each value across a core once,
  and the stream is 1 024 values through a pipeline 55 steps deep.
- At the defaults, the wrong-way compare-exchange of the negative control leaves the sink's sequence
  sorted and moves 16 actors' tallies.
- CAF and SObjectizer are an order of magnitude above (50–190 ms per repetition), and SObjectizer's
  individual binding at two cores was slower than its one core on both hosts, where its grouped binding
  at two cores was faster than its individual one (44–54 ms on WSL2): the form sweep this page asks
  for is not a formality.
