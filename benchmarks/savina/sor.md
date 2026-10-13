# savina/sor

Savina's `sor` benchmark of the "parallelism" group (Imam & Sarkar, *Savina — An Actor Benchmark
Suite*, AGERE 2014; `SucOverRelaxConfig.java`, `SucOverRelaxAkkaActorBenchmark.scala`) —
successive over-relaxation on a square grid with **one actor per cell**, coloured like a
checkerboard: a cell relaxes its value once it holds one value from each of its 2 to 4 neighbours,
and sends the new value to all of them. Two owners — Savina's runner and peer — each create, start
and collect half of the grid. It is wave C's stencil (Huly QB-247), and the opposite of
`savina/apsp`: thousands of actors whose handlers are a few instructions long.

## What it measures, and what it does not

It measures **fine-grained message passing between many small actors that depend on their
neighbours**. At the defaults 6 400 cell actors make 100 (red) or 99 (black) relaxations each —
636 800 updates — and exchange 2 515 360 values: every update is one value received from each
neighbour and one sent to each, and a cell can only ever be one exchange ahead of a neighbour. The
handlers are tiny — an integer add per value, a multiply-add and up to four sends per update — so
the cell is the framework's cost of delivering a message to one of thousands of actors and running
it, with the dependencies keeping every actor's progress tied to its neighbours'.

It does **not** measure computation: the relaxation is three integer operations. It does not
measure actor creation either — Savina's iteration creates its 400 actors inside its timing, the
cell actors here are created and wired before the window, as the harness does for every shape whose
point is not creation (`harness.h`, `Watch`). And at `cores=2` it measures two placement models side
by side, by design: qb and the floor fix each half of the grid on its own core, so only the values
across the middle column cross a core (2 × 80 per round out of ~25 000 at the defaults); CAF's and
SObjectizer's pools place every cell as it becomes ready.

## What it can and cannot show about scheduling under computation

Wave C's question for qb (Huly QB-247) is whether a scheduling cost shows when the cores compute:
the latency to resume after a long handler, and fairness between the actors of one core.

- **The resume latency after a long handler — it cannot show it.** No handler here is long: an
  update is a few nanoseconds. What it shows instead is the other end of the same mechanism: a
  qb core runs thousands of short handlers per pass, and the values they push to the other core
  leave at the next flush (`VirtualCore::__workflow_loop__`: `__flush_all__()`, then
  `__receive__()`), so a boundary value waits for the end of the pass that produced it — a pass of
  many cells' updates. `savina/apsp` measures that hold behind long handlers, with `send` as the
  control; here the boundary is 2 × 80 cells out of 6 400 and the batching is the point of `push`.
- **Fairness between the actors of one core — it shows it only as progress.** With 3 200 or 6 400
  cells on a core, a scheduler that served some cells much more often than others would not get
  more work done for them: a cell can only be one exchange ahead of a neighbour, so a cell ahead
  waits, and the whole grid moves at the pace of its slowest region. An unfair order therefore
  shows as time, not as a wrong answer or a different amount of work — every amount of work here is
  fixed and asserted. The shape has no observation to report: nothing in it depends on the
  interleaving once the values are exact (below).
- **What it can show**: the per-message cost of dispatching to one of thousands of actors on a
  core, with a working set — 6 400 actors and their mailboxes — larger than any core's first-level
  cache, and how well a framework batches the traffic of thousands of tiny handlers.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `size` | 80 | 20 (`DATA_SIZES[n]`, default `n = 0`) | the grid's edge; **deviation — see below**; 80 is Savina's own `n = 1` |
| `iterations` | 100 | 100 (`JACOBI_NUM_ITER`, a constant there) | red cells make `iterations` updates, black cells `iterations − 1`; no deviation |
| `cores` | 2 | n/a | the runner's columns on the first half of the cores, the peer's on the rest, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

The input is Savina's: `SucOverRelaxConfig.randomMatrix`, `A[i][j] = 1e-6 / (nextLong() + 1)` drawn
row-major from Savina's `PseudoRandom(10101010)` — the same numbers here, in fixed point (below).
`omega` is Savina's 1.25. The spec refuses (exit 2) a grid under 2 × 2 or over 1 024 a side, and an
iteration count outside 1 – 100 000; Savina's own protocol blocks at `iterations = 1` (its black
cells report only from a value handler), this one does not.

### Deviation from Savina's grid size, and why

Savina's default, `n = 0`, is a 20 × 20 grid: ~150 000 values per run, 1.4 ms for qb at one core in
the correctness runs on the development host — the regime in which `thread-ring`'s and
`ping-pong`'s counts were raised, a repetition shorter than the spread between two launches. The
default here is Savina's next data size, `n = 1`: 80 × 80, 6 400 cells, 2.5 M values. `--param
size=20` reproduces Savina's default exactly, and every adapter verifies it.

### Deviation from Savina's arithmetic: exact values, and why

Savina relaxes doubles and validates the grand total against `REF_VAL` to 1e-12. Two things in its
protocol make that total depend on the scheduler: a cell adds its neighbours' values **in the order
they arrive** (`sum += message.v`), and each owner adds its cells' final values **in the order they
report** — floating-point addition is not associative, so two frameworks, or two runs of one, can
disagree in the last bits, and no bit-exact checksum could be written over them. So the values are
**fixed-point integers** and the relaxation is integer arithmetic (the spec's `relax()`):

- `omega = 5/4` exactly, so `omega / 4 = 5/16` and `1 − omega = −4/16`, and
  `value' = ⌊(5 · sum − 4 · value) / 16⌋` — an arithmetic shift, which C++20 defines on a negative
  value as the floor;
- the sum is integer addition, the same whatever order the values arrive in, formed in unsigned
  (wrapping) arithmetic so that no order and no defect can make it undefined;
- the initial value of cell `(i, j)` is Savina's draw scaled to fixed point:
  `⌊2^40 / (nextLong() + 1)⌋`, the same numbers in units of `1e-6 / 2^40`;
- the answer is reduced after the window, cell by cell in index order, from what each cell reported
  to its owner — never in report order.

An update is at most 1.5 × the largest value it reads, the relaxation contracts, and the reference
refuses any parameter set whose values ever reach 2^56; none of the domain does. The negative
control's `shuffle` plant delivers every message of a run in a seeded random order and still
verifies: the answer does not depend on the interleaving.

### Other deviations

- **A value carries its sender's generation and index.** Savina's `SorValueMessage` carries the
  value alone and a cell counts what it receives. Two neighbours exchange strictly in turn — a cell
  sends again only after its neighbour has answered — so a cell can only ever receive the generation
  it is waiting for, from each neighbour once; the two words more are what lets a cell refuse a
  duplicate, a misrouted value or a value of the wrong round when it arrives (`Cell::accept`).
- **The red cells do not send their last generation.** Savina's red cells send it to black cells
  that have already exited — dead letters. Nobody reads it, so nobody sends it here.
- **A cell may receive values before its Start.** Savina's cell buffers them until Start and replays
  them; here a red cell takes them as they come (the values carry their generation), and a cell that
  finishes before its owner's Start reaches it reports on that Start, so the run never ends with a
  Start in flight.
- **Each cell keeps a witness of its updates** (below): one multiply-add per update, the only work
  this cell adds to Savina's.
- **The peer reports its half as a signal.** Savina's peer sends the runner its half's `gTotal` and
  message count; here each owner records its cells' reports and the body reduces both, in cell
  order, after the window.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the runner, the peer and all 6 400 cells on VirtualCore 0 | the runner and its 40 columns on VirtualCore 0, the peer and its 40 on VirtualCore 1: only the values across the middle column cross a core |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places every cell — a cell made ready by a value is prepended to the sender's worker (`worker::delay`) unless the idle worker steals it |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; whichever thread is free takes the next demand |
| floor | one thread, every cell in its own 128-byte slot | two pinned threads, qb's placement |

Each adapter publishes an update the way its framework shares one message with several receivers:
qb pushes one small event per neighbour, CAF mails one `caf::message` to each, SObjectizer sends one
`message_holder_t` to each, the floor pushes its fixed message per neighbour. A report is one
message to the owner in the three frameworks; the floor's carries the cell's index and the owner
reads the finished cell's slot, handed over by the ring's release/acquire like the message itself.

## The verified answer

Every cell counts the messages it received (its Start and its values) and its updates, and keeps a
**witness** of its updates: after each one, `witness = witness × K + value` (wrapping). Its report
carries its index, value, update count, received count and witness to its owner, who files it under
the cell's index — refusing a cell it does not own, or one that reports twice. Once the framework is
gone the body reduces, in cell order, the spec's `checksum()`: per cell, `mix` of its index, update
count, received count, witness and final value. The expected value comes from the same red-black
relaxation run sequentially in the spec (`reference()`), sharing only `relax()`, the witness and the
input with the cells, and from the closed forms of the counts: a red cell makes `iterations`
updates, a black one `iterations − 1`, and each receives its Start and one value per neighbour per
update. `expected_messages = 2 × cells + 2 + edges × (2 × iterations − 1)` — every Start (one per
cell and the peer's), every value, every report and the peer's — is counted at the receivers and
asserted exactly (2 528 162 at the defaults).

Why the witness: the final values do not prove the updates ran right. The relaxation contracts and
the floor rounds, so an error made half-way through dies out before the end: in the negative
control, one cell's value one unit off at one update, half-way through the run, leaves every count
and **every final value** as the reference says. The witnesses of that cell and of the cells the
error reached are not the reference's.

**What blocks, what fails at once, what moves the sum, and what neither sees** (FAIRNESS.md
section 0). A value, a Start, a report or the peer's half **lost** leaves a round that never
completes, a cell that never starts or never reports, or a runner a report short: the run
**blocks**, and the harness's deadline fails it with exit 1 and no timing. A value **delivered
twice**, from a cell that is not a neighbour, of a generation its receiver is not waiting for, or to
a cell that has finished, a second Start, a report to an owner that does not own the cell or a
second report of one cell, stops the run at once through `qvo::fail()` (exit 1 and a document naming
the violation). A value **misrouted** to another neighbour of its sender is a duplicate there, and
the cell it was meant for blocks. An update computed wrong, or a value wrong on its way, moves the
witnesses; a cell that skipped or repeated an update, or received a value too many or too few, moves
its counts; a report filed under another cell's index is a second report of that cell. What neither
sees: a copy of a cell's last value that lands after its receiver's actor is gone.

The negative control watches each of these (`tools/negative-control.py`, section U, on
`tools/control/sor-subject.cpp`, the grid and its owners simulated on one thread through the spec's
own `Cell` and `Owner`): a correct run and a run delivering every message in a seeded random order
both verify; a duplicate, a misroute and a value of the wrong round are rejected by the check meant
to see each; a lost value ends at the deadline; and the value one unit off is shown passing every
count and every final value before the witnesses reject it.

The actual qb and floor adapters also build as `qvoctl-qb-sor` and
`qvoctl-baseline-sor`. Their compile-only plants operate at the real value-dispatch and
publication boundaries; the independent expected reference is unmodified. Per-cell plant
state emits `control plant fired: NAME` when the chosen fault occurs, and the final
diagnostics compare the real reports' counts and values against the unchanged reference.
The actual adapter's `off-by-16` plant perturbs one raw transmitted value by 16 integer units
at the midpoint update, for all its neighbours. Their `5/16` relaxation changes by exactly
five units, so the fault necessarily reaches a computed update and its existing witness.
A transmitted increment of one can round away before any update changes; the initial native
control exposed that fixture weakness in both actual adapters. The synthetic subject's
`off-by-one` remains its separate fault placement. The actual control still requires the
counts and final values to match the unchanged reference, demonstrating a masked intermediate
error; it never alters a witness or the expected answer to manufacture a rejection.
That actual masked-error control uses `size=4`, `iterations=100`, `cores=1`, `wait=1`: both
native adapters reject its checksum while their counts and final values remain the reference's.
The positive and protocol-fault controls keep `size=20`, `iterations=100`; only the blind case
changes its fixture. At `size=20`, `iterations=100`, the +16 fault still changes final values,
so that case proves rejection but does not prove why the intermediate witness is needed.
These targets are excluded from benchmark discovery; the ordinary benchmark binaries compile
no plant code. Native execution must establish each rejection, complete return, timing
suppression and the masked-error diagnostics before this closes the actual-adapter proof
obligation.

## The measured window

Opens when the runner, having heard from every cell and from the peer that they are running
(outside the window), sends Start to its cells and to the peer, which starts its own; closes when
the runner holds its half's reports and the peer's. The input and every actor are built before,
and the reduction is after the framework is gone. The floor has no ready handshake: its second
thread is created just before the window, as in every floor here.
