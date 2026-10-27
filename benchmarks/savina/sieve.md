# savina/sieve

Savina's Sieve of Eratosthenes, one of the "parallelism" benchmarks (Imam & Sarkar, *Savina — An
Actor Benchmark Suite*, AGERE 2014; `SieveConfig.java` and `SieveAkkaActorBenchmark.scala`) — a
**pipeline that grows while it runs**: a producer sends every odd candidate down a chain of filter
actors, and the last filter, finding a new prime it has no room for, creates the next filter.

## What it measures, and what it does not

It measures **a chain of filters created inside the window, each doing real work per message**. A
filter holds up to `local` primes and tests every candidate it receives against them (trial
division, Savina's `isLocallyPrime`); a candidate none of them divides is passed to the next filter,
or — at the last filter — is a new prime: kept there while the filter has room, else the filter
creates the next filter with it. A composite dies at the filter holding its smallest prime factor.
At Savina's defaults that is 49 999 candidates, 9 592 primes, ten filters — nine of them **created
inside the window** — 91 329 messages and 46.3 M trial divisions per repetition: the divisions are
most of the cell, the creations nine. With `--param local=1`, the one-actor-per-prime sieve, the
same run creates 9 591 filters and every prime passes through every filter before it: the
messages, not the divisions, become the cell.

It does **not** measure creation cost at the defaults — nine creations against 46 M divisions;
`fork-join-create.md` and `fib.md` measure that — and it does not measure a balanced pipeline:
filter 0 does 9.4 M divisions, filter 9 0.17 M, and the chain is only as long as the run has grown
it.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `limit` | 100 000 | 100 000 (`N`) | no deviation; the candidates are the odd numbers from 3 below it |
| `local` | 1 000 | 1 000 (`M`) | no deviation; the primes one filter holds — `local=1` is the one-filter-per-prime sieve |
| `place` | 0 | n/a | where the frameworks that place put the filters, a **declared axis** — see below |
| `cores` | 2 | n/a | the producer on core 0, the first filter on core `1 % cores` |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Placement: `place`, a declared axis

Savina's filter creates its successor itself (`context.system.actorOf`). In a pool that places
every actor, where the successor runs is the scheduler's decision; in a framework whose actors are
placed — qb, and the floor that bounds it — it is the creator's, and qb's creation primitive,
`addRefActor`, creates **on the creator's core**: qb has no cross-core spawn. So the reference's
shape, followed in qb, puts every filter on the first filter's core, and the second core runs only
the producer. That is `place=0`, the cell's default. `place=1` is what a qb program that wants the
chain spread does: filter `f` lives on core `(f + 1) % cores`, created by a `Spawner` actor that
core holds (one per core, made before start) — the creator sends `Spawn`, holds the candidates and
the Exit it would pass on until the answer names the new filter, then pushes them in order: one
round trip per creation. Following FAIRNESS.md §1.1 both forms are implemented and the table cell
runs the faster as measured on the quiet host (the measurement campaign's form sweep, `place=0`
against `place=1` at two cores); until then the cell runs the reference's `place=0`, and qb's and
the floor's `main()` say so. CAF and SObjectizer place by their pools: `place=1` has no
counterpart there, and their adapters declare it **not applicable** (the harness's third verdict)
rather than run the pool under a label that says otherwise.

The first sweep, on macOS (2026-10-27, unpinned, shipped qb 3.3.0, `results/macbook-m4pro-macos-clang21/wave-bc-form-sweep/`,
`docs/TUNING.md` §22.4), answers it one way for qb and the other for the floor: **qb at `place=1`
reads +8.6 % (2c-spin, separate) and +1.4 % (2c-park, overlapping)** over its `place=0` cell — the
creation round trip per filter buys nothing on a chain whose work is the divisions — while **the
floor at `place=1` reads −39.9 / −39.0 %**, its rings costing nothing to spread. So `place=0` is
qb's cell everywhere, and the floor's cell stays `place=0` too until the x86 sweep says what this
one did: a one-host, unpinned sweep does not move a default the two pinned hosts measure at. The
`local=1` stress (one filter per prime: 9 591 creations, 46 M messages) is a labelled side document
in the same directory: qb 5.40 / 5.42 µs per candidate at one core and 5.89 / 5.68 at two, under the
floor's 6.60 at every configuration, against CAF's 47.7–71.7 µs and SObjectizer's 28.4–95.4.

At `place=1` the floor's filters alternate between its workers, so its rings form a cycle, and
workers that each spin on a full ring to the next (`Mesh::send`) would wait forever if every ring of
the cycle filled at once. A ring asked for fewer messages than its capacity over the whole run can
never fill, and then the cycle cannot close; so the floor counts, from the reference, what each ring
of the cycle carries and declares the run not applicable only when every one reaches the capacity —
`local=1` at the defaults does (each direction carries millions), `local=1000` does not.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | everything on VirtualCore 0; a filter is an `addRefActor<FilterActor>` child of its predecessor | `place=0`: the producer on VirtualCore 0, every filter on VirtualCore 1; `place=1`: filter `f` on VirtualCore `(f + 1) % 2`, created by that core's `Spawner` |
| CAF | `max-threads=1` | `=2`, both pinned; every filter `self->spawn`ed by its predecessor, placed by the work-stealing pool |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; every filter its own top-level coop on that pool |
| floor | one thread | qb's placement for the run's `place`; a filter is a heap node its worker allocates when its first message, Create, arrives |

## The adapters

- **qb**: every candidate and every Exit is a `push` — ordered: a composite that overtook its
  smallest factor would reach the end of the chain and be kept as a prime. A filter's `Counts` are
  its own members, handed to its slot at teardown; the primes it holds are a vector the body owns.
  qb's actor id is a 16-bit slot per VirtualCore: with `place=0` every filter is on one core, so a
  run needing more than 65 534 filters fails the cell (`qvo::fail`) rather than report.
- **CAF**: a candidate is a bare `uint64`, the Exit `close_atom`; a filter spawns its successor
  with `self->spawn`, as `fib.md`'s nodes do, and quits behind its Exit.
- **SObjectizer**: a filter that needs a successor introduces a new **top-level** coop holding it —
  the reference's `context.system.actorOf` makes an actor of the system, not a child — and reads its
  direct mbox at once (`so_define_agent()` runs during registration). A passed candidate is the
  `msg_candidate` the filter received, **redirected** (`so_5::send(mbox, mhood_t)`), as the reference
  re-sends its `LongBox`.
- **floor**: creating the next filter is one Create message to the id the creator chooses
  (`f * W + worker`), the candidates behind it on the same ring — the least creation on another
  worker can cost, no round trip. A worker runs one handler at a time (`radixsort.md`, "The floor's
  gate").

## The verified answer

The run is fixed by the parameters: the candidates arrive at every filter in order, so every filter
holds the same primes in the same order in every run. `sieve::reference` computes it without trial
division — a sieve of each odd number's smallest prime factor: a prime `p_i` (`p_0 = 2`) reaches
filters `0 … (i − 1) / local`, passed on by all but the last, which keeps it or creates filter
`i / local` with it; a composite reaches the filters up to the one holding its smallest prime
factor and dies there. The checksum is, per filter, digested with the filter's number:

- **its tally** — the candidates it received, their wrapping sum, how many it passed on, its Exits;
- **the primes it holds, position by position**, read from the body's vectors after the window.

So the number of filters is part of the answer: a filter the chain never created leaves its slot
empty, and a creation past the last one the primes need fails the run as a protocol violation
(`sieve::too_many_filters`, `qvo::fail`). `expected_messages` — the producer's Start, every
candidate's visit to every filter, one Exit per filter — comes from the reference and is **counted**
at the receivers (`sieve::delivered`).

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). Nothing blocks
on a lost candidate: the Exit follows the last candidate down the chain, so a candidate lost,
duplicated or sent past a filter ends the run on a wrong sum — the filter that should have seen it
is short or long, and a prime missed or kept twice shifts every later filter's primes. A candidate
**out of order** — a composite ahead of its smallest factor — or a filter that **passes candidates
on untested** keeps composites as primes: with exactly as many filters as the primes need, the
first extra one asks for a filter that has no slot, and the run fails as a protocol violation; with
fewer, the primes' positions move. What blocks is an **Exit lost**: the last filter never ends the
run, and the harness's deadline fails it. A planted version of each is rejected by the harness's
negative control (`tools/negative-control.py`, section N, on `tools/control/sieve-subject.cpp`),
which also confirms that a correct chain verifies. What reaches neither verdict is a message no
handler runs before the last filter's Exit tears the run down — with the Exit behind every
candidate, only a duplicated Exit can be one.

What the checksum does **not** prove is that a filter is an actor: an adapter that ran every filter
inside one actor would fold the same total. That each filter is created by its framework's creation
primitive — `addRefActor`, `self->spawn`, a coop introduced to the environment, a Create message on
the floor — is established by reading the four adapters, as `fork-join-create.md` says of every
creation shape.

SObjectizer's dynamically growing filters own the dispatcher binder, following the fib
adapter. The body-owned Field holds no binder across environment teardown; the same lifetime
review as UCT/facloc applies, with repeated native teardown still to validate.

## The measured window

Opens when the producer — having heard from the first filter (and, for qb at `place=1`, from every
`Spawner`) that it is up — sends itself Savina's start; closes when the last filter receives the
Exit. Every candidate, every trial division, every creation and every Exit is inside it. The first
filter is created before it, once. In SObjectizer every filter's coop registration is inside it,
and so is CAF's and qb's actor construction; the floor allocates a node.
