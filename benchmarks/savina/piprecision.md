# savina/piprecision

Savina's `piprecision` benchmark of the "parallelism" group (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `PiPrecisionConfig.java` and `PiPrecisionAkkaActorBenchmark.scala`)
— π to 5 000 decimal places by the Bailey–Borwein–Plouffe series, its terms computed by a **pool
of workers** that a master keeps fed: the master hands out term indices, each worker answers with
the term — **a number of 5 000 decimal places** — and the master adds the terms, handing the
answering worker the next index until a term at or below 10⁻⁵⁰⁰⁰ comes back.

## What it measures, and what it does not

It measures **the fan-out / fan-in of savina/trapezoid with heavy messages and a master that keeps
the workers fed**. At the defaults a run needs 4 147 terms (k = 0 … 4 146) and hands out between
4 147 and 4 346 of them (below): every one is a work message and an answer, and every answer
carries the term — 1 041 bytes on average, 2 080 at the first terms (260 limbs of 64 bits), 4.3 MB
per run — which the master adds into a 261-limb sum. The term itself is the same code in every
implementation (`benchmarks/specs/qvospec/savina/piprecision.h`, `bbp_numerator()` /
`bbp_shift()`): four long divisions of a 260-limb integer and a shift, about 12 µs per term in the
functional runs on Windows/MSVC. So the cell is the computation first, and then **how a payload crosses from a
worker to the master** — which is the axis the shape was chosen for (Huly QB-246), and which each
adapter declares (*The payload*).

It does **not** measure actor creation (the master and its 20 workers are created before the
window), nor load balancing beyond what the protocol gives: the master answers each term with the
next index to the same worker, so a faster worker gets more terms in every framework. And at Savina's
defaults the computation is most of the cell: two payload forms that differ by a copy of a kilobyte
differ by a few hundred nanoseconds per term against twelve microseconds of division.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `precision` | 5 000 | 5 000 | decimal places, Savina's `scale` (`PRECISION`) |
| `workers` | 20 | 20 | the pool (`NUM_WORKERS`); worker `w` on core `w % cores` where the framework places |
| `form` | 0 | n/a | how an answer's payload travels — a **declared axis**, *The payload* below |
| `cores` | 2 | n/a | worker-thread budget |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

No deviation in the protocol: the master's start hands out `min(precision, 10 × workers)` terms
round-robin (200), every answer is added, a term at or below the tolerance stops the hand-outs, the
answering worker otherwise gets the next index, and the run's last answer is the one that brings
the answers received to the terms handed out. Savina's `StopMessage` exchange after it is the
shutdown, measured as teardown like every benchmark's here.

### The arithmetic: Savina's BigDecimals, exactly, on binary integers

Savina computes each term with `java.math.BigDecimal` at `scale` decimal places — four divisions
rounded `HALF_EVEN`, three subtractions, a division by 16ᵏ rounded `HALF_EVEN` — and adds the terms
exactly. A BigDecimal at a fixed scale is an integer U worth U × 10⁻ˢᶜᵃˡᵉ, so every one of those
operations is an operation on integers: `4/(8k+1)` at the scale is `round_half_even(4 × 10⁵⁰⁰⁰ /
(8k+1))`, the subtractions are exact, the division by 16ᵏ at the scale is `round_half_even(U /
2⁴ᵏ)` — a shift — and the sum is an integer sum. The spec computes exactly that on binary integers
of 64-bit limbs: **Savina's values, digit for digit**. Checked independently while this page was
written: Python's `decimal` module (Java's BigDecimal semantics: `quantize` at the scale,
`ROUND_HALF_EVEN`) computes the same 4 147 terms, the same first term at the tolerance (k₁ = 4 146,
value 1 at the scale) and **the same sum, bit for bit**, which is π × 10⁵⁰⁰⁰ within 16 units of
its last place (five roundings per term, 4 147 terms; a Machin-formula π agrees on the leading
4 990 digits). What differs from Savina is the cost of a term, not its value: a division by 16ᵏ is a shift
of a binary integer and a long division of a decimal one, so Savina's per-term work is heavier than
this one — a cell's absolute time is not comparable with Savina's published figures.

### The payload: `form`, a declared axis

qb refuses at compile time an event wider than its cross-core mailbox ring
(`qb::detail::event_fits_ring`, `.claude/rules/qb-api-constraints.md`), and points bulk data to one
of two places: behind a pointer member, or in a `Pipe::allocated_push` tail. Both are implemented
and verified; `form` selects one, and every result document records it.

- **`form=0`, the payload behind a pointer — every framework.** A worker computes the term into a
  fresh heap buffer sized to it and MOVES that buffer into its answer: qb's `Result` event carries a
  `std::vector` (one 64-byte bucket wherever it goes, the buffer freed on the master's core when the
  event is disposed); CAF's answer is `(add_atom, term, worker, proof, std::vector<uint64_t>)`, the
  vector moved into the message (CAF's custom type-ID block gives `std::vector<std::uint64_t>` the
  ID CAF's core module does not); SObjectizer's `msg_result` holds the moved vector; the floor sends
  the pointer of one heap block (length, proof, limbs) in its ring message and the master frees it.
  The same data movement in all four: the limbs written once, by the worker, into the buffer the
  master reads — no framework copies them, none is made to.
- **`form=1`, the payload in the message's own storage — qb only.** A worker reserves
  `getPipe(master).allocated_push<ResultTail>(bytes)` with room for the term behind the event and
  computes the term straight into that tail: no allocation, and the payload travels inside the event
  — through the pipe on the worker's core and, across cores, copied by the flush into the master
  core's mailbox ring (up to 34 buckets of 64 bytes at the defaults; 1 023 is the ring). CAF and
  SObjectizer have no variable-length inline message (a message element, a message object, is of a
  fixed type: a fixed array sized to the largest term would allocate and copy 2 KiB for every
  one-limb term), and the floor's ring carries a fixed three-word message: their `form=1` documents
  are **not applicable** (the harness's third verdict, exit 3), and their `form=0` cells are the
  comparison for both forms.

qb's table cell runs **`form=0`** until a quiet-host form sweep says otherwise (FAIRNESS.md §1.1:
the faster form enters the table): it is the form whose data movement every framework shares, and it
never fills a ring. Functional runs while this page was written (one busy host, pinned CPUs 8 and
10, Windows MSVC 19.51, 7 repetitions — not a measurement) put the two forms within 2 % of each other
at the defaults (11.8–12.1 µs per term at one core, 6.3–6.5 at two), and `form=1` ahead at
`precision=300` (0.59–0.62 against 0.73–0.75 µs per term at two cores), where a term is a tenth of
the work and the messages a larger share. The sweep is the measurement campaign's (*What the
campaign must sweep*).

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the master and the 20 workers on VirtualCore 0 | master on VirtualCore 0, worker `w` on `w % 2`: the answers of the ten odd workers cross a core — `form=0` as one bucket and a pointer the master's core reads through, `form=1` as the whole term copied into the master core's mailbox ring |
| CAF | `max-threads=1` | `=2`, both pinned; which thread runs a worker's next term is the work-stealing pool's decision |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; one coop, and the pool decides which thread runs a worker's next demand |
| floor | one thread | two pinned threads; the master is actor 0 on thread 0 and worker `w` is actor `cores + w` on thread `w % 2` — qb's placement exactly. **Its two-core cell does not halve the one-core cell** — *The floor at two cores* |

### The floor at two cores

The floor's runtime (`frameworks/baseline/baseline_support.h`, `Mesh::drain`) drains each inbound
ring until it is EMPTY, events its own handlers push into it meanwhile included. In this shape the
master and the ten even workers share thread 0, and an even worker's answer, the master's next
index for it and the term after that all go through thread 0's own ring: that ring never empties
while the run lasts, and the odd workers' answers wait in the ring from thread 1 until the hand-outs
stop. Measured functionally (MSVC, pinned CPUs 8 and 10, 5 repetitions): **11.5 µs per term at two
cores against 11.9 at one**, thread 1's workers answering little beyond their share of the first
batch (`terms` 4 246 where every framework hands out 4 346). The same adapter on a copy of the
runtime whose drain stops at what each ring held when the drain began measures **6.2 µs per term**
at two cores, every term handed out. That copy was a diagnosis, not a change: the runtime is shared
by every floor cell published here, and the fix is the integrator's decision (Huly QB-246), so this
page records the cell as it is — read the floor's two-core row of this shape as one thread's.

## The verified answer

Savina's master adds the terms as they arrive, and so does the spec's `Fold`: the sum is an integer
sum, the same for every order. What makes the answer exact for every interleaving is the series
itself — **every term past the first one at the tolerance rounds to 0 at the scale**: the master
stops handing out at the first answer at or below 10⁻⁵⁰⁰⁰ (k₁ = 4 146 at the defaults), but the up
to 200 terms already in flight and the indices handed out while k₁'s answer was on its way all
complete, and how many there are is a race. The reference asserts, for every index a run can reach
(up to `k₁ + batch`, and a margin), that the term is 0 there, so a correct run's sum is
**exactly** Σ term(k) for k = 0 … k₁, whatever the race handed out.

Every answer carries, beside the term, its **proof of work**: the low limb of the term's numerator —
4/(8k+1) − 2/(8k+4) − 1/(8k+5) − 1/(8k+6) at the scale, before the division by 16ᵏ — which only the
four long divisions produce. A term past k₁ is 0 whether it was computed or not; its proof is not.
The master keeps, per index, the answers received and the proof the answer carried — written by its
handlers alone — and what is compared after the window is: the sum, limb by limb; the indices whose
answer count is not the protocol's (1 for every index handed out, 0 for every other); the indices
handed out whose proof is not the reference's; the workers' work messages, counted in each worker's
128-byte slot (for qb in the actor, handed to the slot at teardown), against the terms handed out;
and the answers received against them too. The reference computes all of it — every term up to the
cap, the exact sum, every proof — once per process before the first repetition, about one
repetition's work at one core.

`terms`, the indices the master handed out, is **observed, not compared**: it depends on the
interleaving, so it is reported beside the cell (FAIRNESS.md §0), and the minimum the answer needs is
asserted — at least k₁ + 1 = 4 147 (`Spec::observed_at_least`). At the defaults the functional runs
read 4 346 for qb, SObjectizer and the floor at one core (the whole batch in flight when k₁'s answer
arrives), 4 156 for CAF at one core, 4 166 for CAF at two, 4 246 for the floor at two (above). No
message count is asserted — it is `2 × terms` — and what a count would assert is asserted index by
index instead.

**What blocks, and what moves the sum** (FAIRNESS.md section 0). The master ends the run when every
index it handed out has its answer. A work message or an answer **lost** never ends the run: the
repetition outlives the harness's deadline and fails with exit 1 and no timing. An answer delivered
twice, or a work message delivered twice (two answers to one index), moves the per-index count and,
below k₁, the sum; a worker that answers without computing, or with another term, moves the proofs
and, below k₁, the sum — past k₁, the proofs alone; an answer for an index never handed out is a
protocol violation the run declares at once (`qvo::fail()`: exit 1 and a document naming it).
`tools/negative-control.py` section M plants each in a one-thread subject that runs Savina's protocol
through the spec's own functions (`tools/control/piprecision-subject.cpp`) — among them a duplicated
answer of value 0, which leaves the sum unchanged and is rejected by the per-index count alone, and
a term past k₁ answered 0 without being computed, rejected by its proof alone — and watches every
one rejected, and deliveries drawn at random verify. Each of those two checks was also removed from
a scratch copy of the spec, and the plant it alone sees then passed: it is what sees it. The real qb
and floor adapters were planted too while this shape was written (a term answered twice, a term
answered with the next one's value), and rejected at `cores=1` and `2`.

**What neither sees.** The proof is one limb of the numerator: a worker that computed the four
divisions and skipped only the shift of a term past k₁ — a shift of zero significant limbs — is not
seen, and skips nothing worth measuring.

## The measured window

Opens when the master has received *ready* from all 20 workers and hands out the first batch from
one handler. Closes when the answers received reach the terms handed out after the stop. The
shutdown — qb's `broadcast<qb::KillEvent>()`, CAF's `close_atom` to each worker, SObjectizer's
`environment_t::stop()`, the floor's `Mesh::stop()` — runs after the window, where Savina's own
timing includes its `StopMessage` round; it is measured as teardown, as for every benchmark here.

## What the campaign must sweep

The qb `form` axis, `form=0` (the cell) against `form=1`, at the four configurations, in the quiet
window, as wave A swept its declared forms (`docs/TUNING.md` §21.4): the faster form enters the
table, and the other is kept as a side document.
