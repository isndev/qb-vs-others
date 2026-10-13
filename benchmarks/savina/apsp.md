# savina/apsp

Savina's `apsp` benchmark of the "parallelism" group (Imam & Sarkar, *Savina — An Actor Benchmark
Suite*, AGERE 2014; `ApspConfig.java`, `ApspUtils.scala`, `ApspAkkaActorBenchmark.scala`) —
Floyd-Warshall all-pairs shortest paths over a complete graph, the distance matrix cut into a
**grid of block actors** that exchange their tiles with the blocks of their row and of their column,
phase after phase. It is the first shape of wave C (Huly QB-247), and the first whose handlers are
**long**: every phase of a block is 2 500 min-plus steps before its ten sends.

## What it measures, and what it does not

It measures **computation over a grid of actors, synchronised by the data it exchanges**. At the
defaults 36 blocks each run 300 phases — 10 800 block phases, 27 million min-plus steps — and send
108 000 tiles; a block computes generation `g + 1` of its tile only once it holds generation `g`
from each of its ten neighbours, so no block runs more than one generation ahead of a neighbour and
every phase of the run waits on tiles from the blocks around it. There is no barrier and no
coordinator: the phases are held together by the tiles alone. The kernel is the spec's `relax()`,
the same object code in every implementation, so what differs between two cells is the framework's:
where the phases run, and **how soon a phase's tiles reach the blocks that wait for them** — the
time between a block becoming ready and its phase starting, which is what a long handler on a busy
core can delay. The tiles themselves are pointers (a deviation, below), so a message costs what the
framework's delivery costs, not what copying 20 KB does.

It does **not** measure a framework's way of sharing a large payload, nor its allocator: Savina
allocates a fresh tile per phase, here every block owns two (below). It does not measure load
balancing either: 36 blocks split into two bands of 18 is exactly balanced, so a scheduler that
moves work has nothing to move — only somewhere to move it to.

## What it can and cannot show about scheduling under computation

Wave C's question for qb (Huly QB-247) is whether a scheduling cost shows when the cores compute:
the latency to resume after a long handler, and fairness between the actors of one core.

- **The resume latency after a long handler — it can show it, through `form`.** In qb an event
  pushed to another core leaves at the core's next flush, and the pass flushes before it receives
  (`VirtualCore::__workflow_loop__`: `__flush_all__()`, then `__receive__()`): a core that runs
  several block phases in one pass holds the first phase's tiles for the other core until the last
  has finished. At `cores=2` every block waits, every generation, for three column neighbours on
  the other core, so that hold sits on the critical path of every generation. `send` hands the tile
  to the peer's ring at once (`Actor.h`, `send()`), so the difference between the qb cell at
  `form=0` and at `form=1` is the price of the pass-end flush under long handlers; the floor
  publishes at once, like `form=1`. Both forms are verified; which one is the table cell is decided
  by the quiet-host form sweep (below).
- **Fairness between the actors of one core — it cannot show unfairness; it shows order.** Every
  block gets exactly one phase per generation and the protocol holds every block within one
  generation of its neighbours, so no scheduler can starve a block here, whatever it does. What
  differs is the ORDER in which ready blocks run, and the observation `ahead` reports it: the tiles
  that reached a block one generation ahead of its own — a neighbour already past the phase this
  block is still collecting. In the unpinned correctness runs of the four adapters at the defaults,
  qb's blocks never received one (`ahead = 0` at one core and at two: a core finishes every block's
  generation in the pass that completes it, before any tile of the next generation arrives), the
  floor's and SObjectizer's none at one core and some thousands at two, and CAF's ~29 900 at one
  core and at two. Those are counts of what happened, not timings, and they are the sub-row the
  report prints beside every cell.
- **What it cannot show**: a handler preempted (no framework here preempts one), the tail latency of
  OTHER work sharing a core with a long handler (every actor on a core is a block waiting for the
  same generation), or anything a dynamic placement wins on an unbalanced load (the load is
  balanced). `savina/sor` is the opposite shape — tiny handlers, thousands of actors — and the two
  pages are meant to be read together.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `nodes` | 300 | 300 (`ApspConfig.N`) | vertices; no deviation |
| `block` | 50 | 50 (`ApspConfig.B`) | the tile's edge: a 6 × 6 grid of 36 blocks, 10 neighbours each; no deviation |
| `weight` | 100 | 100 (`ApspConfig.W`) | the largest edge weight; no deviation |
| `form` | 0 | n/a | how a qb block publishes a generation, a **declared axis** — see below |
| `cores` | 2 | n/a | block-row `bi` on core `bi * cores / grid` for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

The graph is Savina's, bit for bit: `ApspUtils.generateGraph`, a complete undirected graph whose
edge `(i, j)`, `i < j`, weighs `nextInt(W) + 1` drawn in row-major order from Savina's own
`PseudoRandom` seeded with `N` (`value = (value * 1309 + 13849) & 65535`) — reproduced in the spec's
`graph()`, outside the window. The spec refuses (exit 2) a grid of one block (it has no neighbour
and would wait forever, as Savina's would), a `block` that does not divide `nodes` (Savina silently
drops the remainder), and sizes past 2 048 nodes or 128 blocks a side.

### Deviations from Savina, and why

- **Two tiles per block, published by pointer.** Savina's actor allocates a fresh tile per phase
  and sends a reference to it; the JVM collects the old ones. Here each block owns two tiles in
  memory the body allocates before the window (the spec's `Storage`: every block's state, inbox and
  tiles in a region of whole 128-byte lines of its own), computes generation `g + 1` into the one
  that held `g − 1`, and every message carries a pointer to the tile it publishes. It is safe by the
  protocol: a block computes `g + 1` only once every neighbour has sent it generation `g`, so every
  neighbour has computed its own `g` — the only computation that reads this block's `g − 1` — and
  each of those sends is a release/acquire hand-over in every framework here. Allocating a tile per
  phase would put 10 800 allocations of 20 KB, and as many frees on whichever thread drops the last
  reference, inside the window: the cell would measure the system allocator under cross-thread
  frees, the same for every framework and none of their doing. Every adapter shares one message per
  generation the way its framework does — qb pushes one small event per neighbour, CAF mails one
  `caf::message` to all ten, SObjectizer sends one `message_holder_t` to all ten, the floor pushes
  its fixed message — as Savina's actor sends one `ApspResultMessage` to all ten.
- **The last generation is not sent.** Savina's blocks send generation `N` to their neighbours and
  exit; the neighbours have exited too, and the tiles are dead letters. Nobody reads them, so
  nobody sends them here.
- **Every tile carries its generation, and a block keeps two inboxes.** Savina's actor keys the
  tiles it holds by sender only (`neighborDataPerIteration.put(sourceId, ...)`): a neighbour one
  generation ahead replaces its current tile with the next one, and the `clear()` after the phase
  throws that away, so a phase can compute with a newer tile than its generation's. Floyd-Warshall's
  answer survives that — a newer distance is still a real path's — but the generations in between do
  not, and nothing could check them. Here a tile names its generation, a block holds the two
  generations a neighbour can be at in two inboxes (`Block::accept`), and anything outside them is a
  protocol failure. The work is Savina's; its phases are exact.

### The publication: `form`, a declared axis

How a block's generation reaches its neighbours is the one place a framework's delivery model
meets the long handler, and qb has two ways to publish. Following FAIRNESS.md §1.1 both are
implemented and measured, and the faster is the table cell:

- **qb**: `form=0` publishes with `push`, ordered, the default qb's own reference names
  (`qb/llm/qb.llm.md`: "Use `push` unless you have a specific reason — and 'faster' is not one":
  `send` wins for one event with nothing behind it to batch and loses on sustained traffic): the
  tile leaves at the core's next flush. `form=1` publishes with `send`, handed to the peer's ring at
  once and unordered — the protocol needs no order between two generations of one sender (a block
  holds both). A phase's ten tiles are a short burst behind a long handler, between the two cases
  the reference names, which is why neither form is assumed. Until the quiet-host sweep the
  adapter's cell is `form=0`.
- **CAF, SObjectizer, the floor**: a mail, a send or a ring push is in the receiver's queue when it
  returns, so `form=0` is already a publication at once, and `form=1` is **not applicable** (exit 3,
  the reason in the document).

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the sink and all 36 blocks on VirtualCore 0 | block-rows 0–2 on VirtualCore 0 with the sink, rows 3–5 on VirtualCore 1: a block's row neighbours share its core, three of its five column neighbours do not |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places every block — a block made ready by a tile is prepended to the sender's worker (`worker::delay`) unless the idle worker steals it |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; whichever thread is free takes the next demand |
| floor | one thread, every block in its own ring | two pinned threads, qb's placement (block-row `bi` on worker `bi * cores / grid`) |

## The verified answer

Every block keeps, in its `State`, the generations it computed, the tiles it accepted, and a
**witness** of its phases: after each phase, `witness = witness × K + Σ(the tile just computed)`.
Once the framework is gone the body reduces, outside the window, through the spec's `checksum()`:
per block, `mix` of its id, phases, accepted tiles and witness, plus a digest of its final tile in
which every distance is weighted by its place in the matrix. The expected value comes from an
independent reference — the textbook in-place Floyd-Warshall over the whole matrix, sharing no code
with the blocks' kernel — which computes the same per-block witnesses because, in place, the matrix
after pivot `k` IS generation `k + 1` (row and column `k` do not move during pivot `k`). The
reference refuses itself if its matrix is not a shortest-path matrix (diagonal zero, symmetric,
never above an edge). Every block computes `nodes` phases from `10 × nodes` tiles;
`expected_messages = blocks × (neighbours × nodes + 2)` — the tiles, each block's Initial and its
Done — is counted at the receivers and asserted exactly (108 072 at the defaults).

Why the witness: the final matrix does not prove the phases ran right. On a complete graph
Floyd-Warshall reaches its answer long before its last pivot and through many tied paths: in the
negative control, a block that throws away the first phase that changed its tile — 282 distances —
still ends on the right matrix, with every count right. The witness of that block, from that phase
on, is not the reference's. The witness costs one add per distance inside the window — the only
work this cell adds to Savina's, in the shared kernel, for every framework alike.

**What blocks, what fails at once, what moves the sum, and what neither sees** (FAIRNESS.md
section 0). A tile, an Initial or a Done **lost** leaves a phase that never completes, or a sink a
Done short: the run **blocks**, and the harness's deadline fails it with exit 1 and no timing. A
tile **delivered twice**, delivered to a block outside its sender's row and column, carrying a
generation its receiver cannot be at (more than one ahead, or behind — a phase that ended without
it ended early), arriving after the receiver's last phase, or a second Initial, stops the run at once
through `qvo::fail()` (exit 1 and a document naming the violation); a tile **misrouted** to another
block of its sender's row or column is a duplicate there, and the block it was meant for blocks. A
phase computed with an inbox short — an adapter calling `compute()` before every tile is in — is
refused the same way ("a phase that ends early"). A phase computed wrong or thrown away moves the
witness; a block that skipped or repeated a phase, or accepted a tile too many or too few, moves its
tally; a tile filed under another block moves the tile digest. What neither sees: a phase thrown
away that would have changed nothing — its tile is the right tile, and the work it skipped had no
effect to observe — and a copy of a last-generation tile that lands after its receiver's actor is
gone.

The negative control watches each of these (`tools/negative-control.py`, section T, on
`tools/control/apsp-subject.cpp`, the grid simulated on one thread through the spec's own `Block`):
a correct run and a run delivering every message in a seeded random order both verify; a duplicate,
two misroutes, a generation two ahead and a phase computed one tile short are rejected by the check
meant to see each; a lost tile ends at the deadline; and the thrown-away phase is shown passing
every count and the final matrix before the witness rejects it.

The actual qb and floor adapters also build as `qvoctl-qb-apsp` and
`qvoctl-baseline-apsp`. Their compile-only plants operate at the real tile-dispatch and
phase-compute boundaries; the independent expected reference is unmodified. Per-block plant
state emits `control plant fired: NAME` when the chosen fault occurs. Dispatch-origin corruption
exercises the neighbour and occupied-row-slot checks, and a skipped changing phase reports
the actual count and final-matrix comparisons after teardown. These targets are excluded from
benchmark discovery; the ordinary benchmark binaries compile no plant code. Native execution
must establish each rejection, complete return, timing suppression and the masked-error
diagnostics before this closes the actual-adapter proof obligation.

## What is observed, not asserted

`ahead` — the tiles that reached a block one generation ahead of its own, summed over the blocks.
How far the blocks drift apart inside the one generation the protocol allows depends on the
interleaving, so it is reported beside the cell (FAIRNESS.md section 0), never asserted; the
section on scheduling above says what the correctness runs read.

## The measured window

Opens when the sink, having heard from every block that it is running (outside the window), sends
every block its Initial; closes when the sink has the last block's Done. The graph, the `Storage`
and every generation-0 tile are built before the framework, outside the window and outside its
setup, by the same thread for every adapter; the reduction — the tiles' digests, the reference's
comparison — is after the framework is gone. The floor has no ready handshake: its second thread is
created just before the window, as in every floor here.
