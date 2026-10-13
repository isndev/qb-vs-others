# savina/filterbank

Savina's Filter Bank, one of the "parallelism" benchmarks (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `FilterBankConfig.java` and `FilterBankAkkaActorBenchmark.scala`,
StreamIt's FilterBank ported to actors) — a **branching pipeline**: one stream of samples fanned
out to `channels` banks of filters, each bank a chain of seven actors, and the banks' outputs fanned
back in, round by round, to one sink.

## What it measures, and what it does not

It measures **a fan-out, eight parallel chains of real work, and a fan-in that joins them by
position**. A producer paces a source one sample at a time (a Boot for each, a Next back); the
source sends each sample — 0, 1, …, 999, 0, … — to the branches, which send it to every bank; a
bank's own actor passes it to a delay of `columns − 1`, an FIR filter over the last `columns`
samples with the bank's H coefficients, a sampler that keeps one value in `columns` and zeroes the
rest, a second delay, a second FIR with the bank's F coefficients, and a tagger that names the
channel; the integrator makes round `k` of the `k`-th value of every channel, the combine sums each
round and the sink receives the sums. An FIR outputs nothing until it has seen `columns` values,
then one value per input, each a `columns`-term dot product — at Savina's defaults the sixteen FIRs
evaluate **2.68 G multiply-adds per repetition**, most of the cell. So what differs between the
frameworks is how they spread eight banks of work over the cores and what ~1.42 M messages cost on
top of it; at one core the cell is mostly the FIRs, everywhere.

It does **not** measure a balanced pipeline: the first FIRs do 147 000 of the 164 000 dot products,
the second FIRs the rest, everything else is a hand-off. It does not measure the producer's
pacing in isolation either — the Boot/Next round trip runs beside the banks' work.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `simulations` | 34 816 | 34 816 (`NUM_SIMULATIONS`) | no deviation; Savina computes it as `2048 + max(2048, NUM_COLUMNS) * 2` before reading its arguments, so its `-col` never moves it |
| `columns` | 16 384 | 16 384 (`NUM_COLUMNS`) | no deviation; the FIR length, the delays' length + 1 and the sampling rate |
| `channels` | 8 | 8 (`NUM_CHANNELS`) | no deviation; Savina clamps its argument into [2, 33], here a value outside is refused |
| `cores` | 2 | n/a | for the frameworks that place: see the mapping table |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

The arithmetic is Savina's, term for term: `H[j][i] = i·columns + j·channels + j + i + j + 1` and
`F[j][i] = i·j + j² + j + i` in doubles (every term an integer below 2^53, so exact), the FIR's
`Σ data[i] · coefficients[columns − i − 1]` accumulated from `i = 0` over its circular buffer read
from index 0 — not from its write position, which is the reference's arithmetic and is kept — the
combine's sum in channel order (the order Savina's `HashMap<Integer, Double>.values()` yields for
keys 0–32). The dot product is the spec's `fir_dot`, out of line (`QVOSPEC_FILTERBANK_NOINLINE`): each
binary carries it as ONE function, called by `expected()` and by every adapter's filters, so the
reference and the run compute the same bits in the same process. No reassociation is allowed: the
build has no fast-math flag, and a reduction in doubles is evaluated in order.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | all 62 actors on VirtualCore 0 | the producer, the source and the branches on VirtualCore 0, bank `j`'s seven actors on VirtualCore `j % 2`, the integrator, the combine and the sink on VirtualCore 1 (`filterbank::core_of`): four banks' FIRs per core |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places every actor |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual` |
| floor | one thread | qb's placement |

## The adapters

- **qb**: every value and every Exit is a `push` (ordered — the Exit must follow every value an
  actor sent); a round reaches the combine as one event carrying its `channels` values in a
  `std::vector<double>`, the reference's collection. Each actor's `Counts` are its own members,
  handed to its tally slot at teardown.
- **CAF**: the actors spawned from the end of the chain back, as the reference nests them, each
  handed its successor; a sample is a bare `double`, a tagged value `(uint32, double)`, a round a
  `std::vector<double>` — given a type id with `CAF_BEGIN_TYPE_ID_BLOCK` / `CAF_ADD_TYPE_ID`, CAF's
  own mechanism — and Next, Boot and Exit built-in atoms. Every actor quits behind its Exit, as the
  reference's `onPostExit` does.
- **SObjectizer**: where the reference re-sends the message it received — the branches to every
  bank, a bank to its first delay, a sampler keeping its sample — the agent **redirects** the
  immutable `msg_value` it received (`so_5::send(mbox, mhood_t)`), and a sampler zeroing a value
  sends one preallocated zero (`message_holder_t`), the reference's `SampleFilterActor.ZERO_RESULT`;
  a delay, an FIR, a tagger and a combine send a new message, as the reference does.
- **floor**: integers; a value is its IEEE-754 bits in the ring message; a round reaches the combine
  as its index into a table the integrator fills (the two share a worker): the least a message
  carrying `channels` values can be. A worker runs one handler at a time (`radixsort.md`, "The
  floor's gate").

## The verified answer

The whole run is fixed by the parameters — every filter is deterministic, every channel delivers in
order and the join rounds by position — so `filterbank::expected` replays it on one thread with the
same filters (`Delay`, `Fir`, `Sampler`, `Join`, `combine`), and the checksum is:

- **every actor's tally**, digested with its number: how many values it received (the producer its
  Nexts, the source its Boots, the combine its rounds), the wrapping sum of their bit patterns, and
  its Exits — order-independent, so the integrator's interleaving cannot move it;
- **the sink's sequence, position by position**: the sums the run produced, bit for bit. The sink
  stores each at the next position of a sequence the body owns (one store); the sequence is checked
  after the window.

`expected_messages` is closed form (`filterbank::expected_messages`: 1 419 393 at the defaults) and
**counted** at the receivers (`filterbank::delivered`).

**What blocks, what moves the sum, and what neither sees** (FAIRNESS.md section 0). Nothing blocks
on a lost value: an actor passes Exit on behind its last value through the same FIFO, so a value
lost, duplicated, zeroed, sent to the wrong filter or tagged with the wrong channel ends the run on
a wrong sum — the count or the bit sum of the actor that should have had it moves, and the FIR
state, the rounds and the sequence downstream with it. A sampler that keeps a value it should zero,
or a bank's second FIR routed around, moves them too. **Two banks filtering with each other's
coefficients** — a consistent fault — leaves every count right and every sum wrong. A value past
the last round reaching the sink fails the run as a protocol violation (`filterbank::Sequence`,
`qvo::fail`). What blocks is an **Exit lost** (the integrator waits for one per channel): the
harness's deadline fails it. A planted version of each is rejected by the harness's negative control
(`tools/negative-control.py`, section M, on `tools/control/filterbank-subject.cpp`), which also
confirms that a correct bank verifies. What reaches neither verdict is a message no handler runs
before the sink's Exit tears the run down — and with Exit behind every value, only a duplicated
Exit can be one.

## The measured window

Opens when the producer — having heard from every other actor that it is up — sends itself the
reference's first Next; closes when the sink receives its Exit, the last message of the cascade.
Every pacing round trip, every filter step, every round and every Exit is inside it. The coefficient
tables are built before it (Savina builds them while parsing its arguments) and the reference is
computed before the first repetition — at the defaults that replay costs about as much as a
one-core repetition, once per process, outside every timing. The floor's second thread is created
just before the window.
