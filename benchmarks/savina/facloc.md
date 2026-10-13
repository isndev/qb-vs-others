# savina/facloc

Savina's Online Facility Location, `facloc`, of the "parallelism" group (Imam & Sarkar, *Savina —
An Actor Benchmark Suite*, AGERE 2014; `FacilityLocationConfig.java` and
`FacilityLocationAkkaActorBenchmark.scala` in `shamsimam/savina`). A producer draws customers on a
square and sends them, one at a time, into a tree of **quadrant** actors that partitions the square
recursively: a quadrant keeps the customers that fall in it and adds each one's cost — its distance
to the nearest facility — and when the cost exceeds a threshold it **opens its facility** (the
midpoint of its region), splits its region in four and **creates four child quadrants**, handing
each the customers inside it; from then on it routes every customer to the child whose region holds
it.

## What it measures, and what it does not

It measures **a stream of small messages routed down a tree that grows where the stream falls**:
100 000 customers, each delivered 5.6 times on average on its way down (561 533 customer
deliveries at the defaults), 1 364 quadrants created inside the window — each by the quadrant whose
region it splits — and one customer in flight at the root at a time (the producer sends the next
one when the root asks for it), so every level below the root is a stage of a pipeline. What a
framework pays per customer is a delivery per level and a handler that routes or costs it; what it
pays per quadrant is a creation with its share of customers.

It does **not** measure the facility-location algorithm's quality: the cost here uses fewer
facilities than Savina's (below), and nothing compares the facilities to an optimum. And it does
not measure computation beyond the kernel's: a distance per facility of the quadrant (its depth
plus one), a sum and a comparison per customer, identical object code in every framework.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `points` | 100 000 | 100 000 (`NUM_POINTS`) | no deviation |
| `grid` | 500 | 500 (`GRID_SIZE`) | the square's side; coordinates are integers of 1/65 536 of a unit (below) |
| `alpha` | 2 | 2.0 (`ALPHA`) | the threshold is `alpha × F`, `F = √2 × grid`, Savina's own; an integer here |
| `seed` | 123 456 | 123 456 (`SEED`) | the customers' stream; Savina declares it and its actor implementations never apply it (below) |
| `cores` | 2 | n/a | the producer and the root on core 0, the subtree of the root's child `k` on core `k % cores`, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Savina's `CUTOFF_DEPTH` (3), `N` and `C` are parsed by its configuration and read by no actor
implementation of the benchmark; they have no counterpart here.

At the defaults the customers are uniform on the square and the quadtree comes out complete: 341
quadrants open their facility (the root and every quadrant down to depth 4) and 1 024 leaves at
depth 5 hold the customers, 1 365 quadrants in all, 341 under each of the root's four children.
Other parameters grow uneven trees (`alpha=1`: 1 377 quadrants to depth 6, 341 / 341 / 349 / 345
under the root's children).

### The customers

Savina's producer draws each customer with `Point.random(GRID_SIZE)` (`:75`), two `nextDouble()`
of a generator that is a **static** of `Point` seeded once by its default constructor
(`FacilityLocationConfig.java:63`; `setSeed` is never called by the actor implementations), so every
iteration of a Savina run sees a different stream. Here customer `i` is a function of `seed` and
`i` — two uniform draws from `mix()` — drawn before the window into a table the producer reads.

Coordinates are **integers**: one unit of the grid is 65 536 sub-units, the square
`[0, 32 768 000)²` at the defaults. A distance is the square root of an exact integer — both squares
stay below 2⁵² when the side is below 2²⁶ (`grid ≤ 1 000`), so their sum converts to a double
exactly, fused multiply-add or not — and IEEE 754 rounds the root correctly on every platform; a
quadrant's cost is a sum of those in its own customer order. The same bits everywhere, which is
what lets the reference's splits be the adapters' splits on every host. Regions are half-open,
`[x1, x2) × [y1, y2)`: Savina's `Box.contains` is closed (`:209`), so a customer exactly on a split
line would belong to two children at a split — never with Savina's doubles, possibly with integers
— and here every customer has exactly one quadrant.

### The facilities, for every interleaving

Savina's quadrant costs a customer against `localFacilities` — "all the local facilities from
corner ancestors" (`:105`): its own midpoint and its ancestors', inherited when it is created — and
also against every facility it is TOLD about: a quadrant that opens tells its parent, the parent
passes it up and tells the child **diagonally opposite** the one that opened (`:186`–`:197`), and
a quadrant told by its parent passes it to all its children if it has any (`:201`–`:205`); each
receiver adds the point to `localFacilities` (`:178`). Those messages land at whatever point of
the receiver's customer stream the scheduler puts them, so the cost of the customers after them,
when a quadrant opens, and which facilities exist at the end are decided by the interleaving: two
runs of one framework open different facilities, and no framework-free value could check any of
them.

The customers themselves are deterministic: one channel from the producer to the root and one from
each parent to each child, each FIFO in every framework here, so every quadrant receives its
customers in the producer's order. So here a quadrant's facilities are **its corner ancestors'
and its own** — Savina's own comment on `localFacilities` — fixed when it is created, and the
messages whose only effect was to add to them are not sent: the sibling notification and its
forwarding down the sibling's subtree. The notification up to the root is kept — it is always
delivered before the ConfirmExit that summarises it, and its count is fixed (one delivery per level
between the facility and the root, 1 252 at the defaults) — and each receiver counts it, as
Savina's `knownFacilities`. With that, which quadrants open, when, and which customers each of the
others holds are the same for every interleaving, and the spec computes them in advance by running
the protocol sequentially through the same kernel (`reference()`).

What it costs the shape: on this quadtree, delivered in the reference's sequential order, the
sibling notifications would be 1 252 messages and their forwarding 75 624 more — 11.6 % on top of
the 665 514 kept — a count the interleaving changes in any parallel run (a sibling forwards only to
the children it has when the message lands); and with fewer facilities to be near, a customer costs
more here than in Savina, so a quadrant opens sooner. What it keeps: every customer delivery, every
split and every creation, Savina's ConfirmExit answer (`numFacilities`, `numCustomers`) and its
flow control.

### The kernel

A quadrant's algorithm — the costed add, the split, the routing — is the spec's `Quadrant` class,
Savina's `QuadrantActor` without its messages, linked into every adapter. One detail differs from
Savina's code and not from its semantics: at a split, Savina hands each child a copy of all the
support customers and each child keeps those in its region (`:283`–`:301`, `:127`); here the parent
hands each child its own share, in the same order. A child created with a cost over the threshold
splits on its next customer, as in Savina (`:148`: the check is made only when a customer arrives).

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the producer, the root and every quadrant on VirtualCore 0; a split creates the four children with `addRefActor` from their seeds, a quadrant ends with `kill()` after its ConfirmExit | the producer and the root on VirtualCore 0; the root's children 0 and 2 (the left half) are created there, 1 and 3 on VirtualCore 1 by a **planter** created there before the window — one `Plant` event each, the child's seed behind a `std::unique_ptr` (an event payload must be trivially relocatable; smart pointers are) — and the root holds the customers it routes to such a child until the planter names it (`Planted`). A quadrant then lives on its parent's core: 682 quadrants a core, the producer and the root on core 0 besides, and nothing balances them |
| CAF | `max-threads=1` | `=2`, both pinned; the producer spawns the root, a quadrant spawns its four children with their seeds as spawn arguments, and the work-stealing pool places every quadrant |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; a split registers **one child coop of the four** (they are created together and end together), and the parent deregisters it once all four have confirmed — 341 coops per repetition |
| floor | one thread; a quadrant is a heap record holding the kernel, freed by its parent once it has confirmed | two pinned threads; the producer and the root on worker 0, a quadrant on its parent's worker and the root's child `k` on worker `k % 2`, allocated by the root and handed over with its first message — the same static placement qb has, without the planter, so the floor bounds the placing frameworks and not the pools |

## The verified answer

Savina's own end: after the last customer the producer sends RequestExit, it travels down the
tree, and every quadrant answers its parent with ConfirmExit once its children have — carrying here,
besides Savina's `numFacilities` and `numCustomers`, the identities of the quadrants that opened
(each quadrant's identity is its position in the complete quadtree: the root 0, child `k` of `q`
`4q + k + 1`), the indices of the customers each of the others holds, weighted by its identity, the
facility identities each quadrant was told of, the quadrants' depths and every delivery each
handled. Every field is a plain wrapping sum — no `mix()` inside the window (Huly QB-1017) — and the
root's total is mixed, field by field, into the checksum after the window. `reference()` computes
the same sums; `expected_messages` — every customer delivery, a NextCustomer per customer, the
Facility deliveries, a RequestExit per quadrant and a ConfirmExit per quadrant but the root, 665 514
at the defaults — is **counted at the receivers**.

**What blocks, and what moves the sum** (FAIRNESS.md section 0).

* A customer lost between the producer and the root, a NextCustomer, a RequestExit or a ConfirmExit
  **lost** leaves the producer or a parent waiting: the run **blocks**, the repetition outlives the
  harness's deadline and fails with exit 1 and no timing.
* A customer lost below the root — the producer's flow control sees only the root, so nothing
  blocks —, delivered twice or routed to the wrong child ends in the wrong place or nowhere: the
  support counts and their identity-weighted sums move, and so do the splits it caused or
  prevented. A quadrant that skips a customer's cost, or ignores its threshold, splits at another
  customer or never: the facilities and the support sums move. A Facility notification lost or
  repeated on its way up moves the notification sum. A delivery a handler did not count moves the
  message count.
* What the protocol itself can see is a **failure at once** (`qvo::fail()` through the spec's
  `fail()`): a customer after RequestExit, a second RequestExit, a fifth ConfirmExit.

The negative-control battery plants each of these classes in `tools/control/facloc-subject.cpp` —
the protocol on one thread through the spec's own `Quadrant`, `Tally` and `checksum()` — and asserts
each is rejected by the check meant to see it (section W of `tools/negative-control.py`).

What neither sees: a message delivered to a quadrant after it has ended — qb and CAF drop it. And,
as for every shape that creates actors (`fork-join-create.md`), the checksum does not prove that a
quadrant was an actor: that is established by reading the four adapters.

SObjectizer's dispatcher binder is owned by the quadrant agents that create child coops.
The launch initializer and the body-owned Field retain no owner across environment teardown.
Windows repeated teardown remains part of validation.

## The measured window

Opens when the producer sends the first customer into an already-running system — for qb once the
root and every planter have reported ready, for CAF on the producer's first message (the root
spawned before it), for SObjectizer in the producer's `so_evt_start()`, for the floor on the calling
thread once its workers are up — and closes when the root has the last of its children's
ConfirmExits. Every customer, every split and creation, both exit waves and every quadrant's end
are inside it; the customers' table is drawn before it, once per repetition. How much of a
quadrant's end lands inside the window differs by framework as `uct.md` says.

## Forms

No adapter declares a second form. CAF's `caf::lazy_init` (a spawned actor not scheduled until its
first message) does not fit a quadrant, which computes its initial costs as it is created.
