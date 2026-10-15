# savina/quicksort

Savina's Quicksort, one of the "parallel" benchmarks (Imam & Sarkar, *Savina — An Actor Benchmark
Suite*, AGERE 2014; `QuickSortConfig.java` and `QuickSortAkkaActorBenchmark.scala`) — a recursive
decomposition whose data travels in the messages: an actor told to sort a sequence either sorts it
itself, when it is short, or splits it around a pivot, creates a child for each side, sends each its
part, and joins their sorted answers as they come back.

## What it measures, and what it does not

It measures **a parallel divide-and-conquer whose actors are created inside the window and whose
messages carry bulk data**: at the defaults one sequence of 1 000 000 values is split by 969 actors
into 970 leaves (1 939 actors in all, the deepest 21 levels below the root), every split creates two actors and sends each
a part of its sequence, every answer carries a sorted part back up, and the parent copies it into
its own as it arrives. The splitting, the leaf sorts and the joins are the spec's own code
(`split()`, `sort_leaf()`, `join()` in `benchmarks/specs/qvospec/savina/quicksort.h`), identical
object code for every framework, and they are most of the cell: what differs between the frameworks
is the cost of creating and ending 1 939 actors, of moving 3 878 messages that each own a sequence,
and above all **whether the two halves of the first split are sorted at the same time when there
are two cores** — and, below them, how the tree is spread.

It is not a message-rate shape: 3 878 messages against ~60 ms of sorting on one core is a few
microseconds of messaging per millisecond of work. And it does not measure a balanced tree: the
pivot is Savina's, the value at the middle of each sequence, so the two halves of a split differ by
whatever the input puts there.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `n` | 1 000 000 | `N` = 1 000 000 | no deviation |
| `max` | 2^60 | `M` = 2^60 | no deviation; a value is `nextLong() % max` — and never binds, below |
| `threshold` | 2 048 | `T` = 2 048 | no deviation; a sequence shorter than this is sorted by the actor itself |
| `seed` | 1 024 | `S` = 1 024 | no deviation: the input IS Savina's |
| `cores` | 2 | n/a | the two seeds on two cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

**The input.** Savina's generator is its own `PseudoRandom` (`value = (value * 1309 + 13849) &
65535`), reproduced exactly in the spec (the reference's arithmetic is Java's wrapping `long`, and
only the low 16 bits survive the mask, which unsigned 64-bit arithmetic gives the same). It draws
from [0, 65 535], so `max = 2^60` never binds and the input is the generator's full period — all
65 536 values — repeated about 15 times: a sort with many duplicates, which the reference's
three-way split (below, equal to, above the pivot) is built for. `--param max=16` narrows it further.

### Deviations, and why

- **The input is built before the window.** Savina's `runIteration()` generates it inside the timed
  iteration; generating a million values is not the shape, and every adapter builds them before the
  window from the same spec function (`input()`), the driver handing them to the root as the window
  opens.
- **The root's two children are created before the window** — the two SEEDS, one for each half of
  the first split, placed by the adapter (seed `s` on core `s % cores` for the frameworks that
  place). Savina's root creates them in its first handler. Every other actor — 1 935 of 1 939 at the
  defaults — is created by its parent inside the window. The reason is qb's: qb has no cross-core
  spawn, so without a pre-placed seed a qb cell could never use a second core (savina/fib's
  arrangement, for the same reason); every framework gets the same two seeds, and for the pools they
  are just two actors their scheduler places.
- **The split is one counting pass and one filling pass.** Savina makes three passes over the
  sequence, each filtering into a new list reserved at the sequence's full length; here one pass
  counts the three parts and a second fills three vectors of exactly their size — the same three
  order-preserving sequences, which is what the protocol sends.
- **The leaf sort is in place.** Savina's `quicksortSeq` recurses by filtering into three new lists
  per call and concatenating them back: on a JVM each list is a bump allocation, in C++ a call into
  the system allocator, and ~6 000 of them per leaf would make the cell measure the C runtime's
  `malloc`. `sort_leaf()` keeps the algorithm — Savina's pivot rule (the middle element) and its
  three-way split — and does it by exchanges inside the leaf's own sequence (Dijkstra's partition),
  recursing on the smaller side. The sorted result is the same; the comparisons are not counted.
- **The joins are Savina's.** A Result is joined into what the actor holds as it arrives, a left part
  before and a right part after, into a NEW sequence of both (`join()`, the reference's `new
  ArrayList` + two `addAll`) — the copies the decomposition pays on its way up, kept as the shape.

## The data in the messages

qb's event rule decides the carrier. An event wider than the cross-core mailbox ring (1 023 64-byte
buckets, ~64 KiB) is a compile error (`static_assert(event_fits_ring<T>)`, qb
`src/qb/core/Event.h:1077`), and an `allocated_push` tail must fit that ring together with its event
or the flush drops it as a dead letter (`DeadLetterReason::oversize`, `src/qb/core/DeadLetter.h:58`):
the first split alone hands each seed ~500 000 values, ~4 MB. So **qb's Sort and Result carry the
values behind a pointer member**, a `std::unique_ptr<std::vector<uint64_t>>` — eight bytes through
the pipe; the sender moves its vector in, the receiver moves it out of the event, and the receiving
core runs the event's destructor exactly once (`Event.h:1069`), so the event may own heap. The other
three are given the same movement — the sequence's buffer changes owner, the elements are never
copied in transit:

| framework | a Sort / a Result carries | the receiver |
|---|---|---|
| qb | `std::unique_ptr<std::vector<uint64_t>>` in the event (+ the counts and the position inline) | moves the pointer out; `push` for every message |
| CAF | the `std::vector<uint64_t>` itself, moved into the mail (`self->mail(std::move(v))`), a type id of its own (`CAF_ADD_TYPE_ID`) | takes it by mutable reference and moves it out: CAF copies a message's data only when it is shared (`libcaf_core/caf/intrusive_cow_ptr.hpp:17`), and a mail to one actor is not |
| SObjectizer | a `mutable_msg` holding the vector, moved in (`so_5::send<so_5::mutable_msg<msg_sort>>(mbox, std::move(v))`) | moves it out through `mutable_mhood_t` — a mutable message may go to an MPSC mbox only, which a direct mbox is |
| floor | one heap payload (the vector and, on the way back, the counts) as a pointer in the message's first word | takes ownership and frees it |

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | everything on VirtualCore 0; a child is an `addRefActor` child of its parent, killed after it answers | the root and seed 0 on VirtualCore 0, seed 1 on VirtualCore 1; a child lives on its parent's core (`addRefActor`), so each half of the first split is sorted on its own core and **never rebalanced** — the window closes when the larger half and the root's last join are done |
| CAF | `max-threads=1` | `=2`, both pinned; every child is `self->spawn`ed by its parent into the work-stealing pool, which places and balances the tree |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; every child is its own child coop of its parent, deregistered after it answers, bound to the same pool, which places it |
| floor | one thread | two pinned threads: the root on thread 0, seed `s` on thread `s`, a child on its parent's thread — qb's placement, so the floor bounds the placing frameworks and not the pools |

## The verified answer

The run's answer is reduced **after the window**, from what the driver received and what the actors
counted (`quicksort.h`):

- the driver's sequence, **measured**: its length, the wrapping sum of a mix of every value (the
  multiset of the output, order-independent), and its **descents**, the positions where a value is
  below its predecessor. A sequence with the input's length and multiset and no descent is the
  sorted input;
- the messages, **counted where they were received** and carried up the tree beside the values: an
  actor counts the Sort it handled and, on each Result, adds its child's counts and the one Result it
  just received; the driver does the same with the root's. The whole run reports one Sort and one
  Result per actor.

The checksum is one mixed term per component (`answer()`), and the expected value is the same terms
over the **sequential reference**: the input, its multiset, and the tree Savina's protocol builds over
it — one actor per Sort, a split for every sequence of `threshold` or more values — walked with the
same `split()` every adapter calls (`reference()`). The tree is fixed by the input and the pivot rule,
so the actor count is too: `expected_messages` = 2 × 1 939 = 3 878 at the defaults, asserted exactly
against the counted total. After teardown every adapter calls the spec's `explain()`, which names on
stderr what differs — length, descents, multiset, counts — when the run is not the reference's.

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). Every actor waits
for exactly two Results and the driver for one, so a Sort or a Result **lost** — or delivered to an
actor that has already answered — leaves a parent waiting: the run **blocks**, and the harness's
deadline fails it with exit 1 and no timing. A Result **delivered twice** completes its parent early
with that part joined twice and the other side never: the output is short and its multiset wrong. A
Result joined on the **wrong side**, or a leaf that answers **without sorting**, keeps every value and
breaks the order: descents. An actor that **sorts a sequence it should have split** reaches the right
output with fewer actors: only the counted Sorts and Results see it. A planted version of each is
rejected by the harness's negative control (`tools/negative-control.py`, section R, on
`tools/control/quicksort-subject.cpp`, the decomposition run on one thread through the spec's own
functions, the less-work plant shown to produce the sorted input first), which also confirms that a
correct decomposition verifies and that fewer values than the threshold is refused. What reaches
neither verdict is work no message reveals: a leaf that sorts its sequence twice, a split that runs
its passes twice — the output and the counts are the same.

## The measured window

Opens when the driver, having heard from the root and the two seeds that they are up (outside the
window), sends the root its Sort; closes when the driver receives the root's Result. Every split,
every leaf sort, every join and every actor below the seeds — created, answered, ended — is inside
it; the root's and the seeds' own ends land at its edge. SObjectizer's final deregistration of a
coop — one per actor here — runs on a thread its environment starts for itself, outside the `cores`
work threads (savina/fib.md, "The measured window", says how); the coops still in its chain when the
window closes are released after it.

## Spin and park

`wait=1` and `wait=0` are each framework's spin and park, as everywhere (the support headers). In this
shape a core waits only when its half of the tree is done before the other's, and at the end of the
run; the column is published for completeness, not because it is expected to differ.

## What the correctness runs showed

Correctness runs, not measurements — unpinned (one pinned, said so), on a host other agents were
using; every cell verified, on WSL2/g++-14 and Windows/MSVC, at the defaults and at `n=2048
threshold=64`, `n=5000 threshold=1` (a split per value), `n=100000 max=16 threshold=64` (16 distinct
values) and, on WSL2, `n=4000000`. What they show about the shape, for the measurement campaign to
confirm or refute:

- qb and the floor sort the two halves at once at two cores — 55–70 % of their one-core time,
  by host and adapter — as their placement promises; SObjectizer's pool does too (66–68 %).
- CAF's two-core cell halved its one-core time on WSL2 but **not on Windows**: 74–76 ms at both core
  counts, pinned to CPUs 0 and 2, where its bitonicsort cell on the same build does use both workers.
  A tree whose second half is one actor waiting in the busy worker's queue is stolen only when the
  idle worker looks — CAF's relaxed polling sleeps 10 ms between steals
  (`libcaf_core/caf/defaults.hpp`, `relaxed_sleep_duration`) — which is a reading, not a trace; the
  campaign's two-core census is what says whether the Windows cell is CAF or this host.
