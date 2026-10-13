# FAIRNESS.md — why you should believe these numbers, and where you should not

This repository benchmarks **qb against other actor frameworks**, and it is written and run by
**qb's own maintainer**. That is a conflict of interest. It is not resolved by promising to be
careful; it is resolved by making the comparison *checkable by someone who wants it to come out
the other way*. Everything below exists for that reason.

If this document and the code disagree, the code is the defect and the document is the
specification.

---

## 0. The single fact that matters most

> **A benchmark that does not verify its own result measures the wrong thing.**

Almost every published actor-framework benchmark reports a wall-clock number and nothing else. A
framework that silently drops messages under load, coalesces them, delivers them out of order, or
returns before the work is done will *win* such a benchmark. It will win it by a lot.

So in this repository **every benchmark computes an answer, and the answer is asserted**. Not
"the program exited 0" — a specific value, derived from every message that was supposed to be
delivered, compared against a value computed independently of any framework. A framework that
drops one message in ten million fails the run and produces **no timing at all**.

This is `qvo::Spec::expected` in `harness/include/qvo/harness.h`, and it is not optional: `qvo::run`
refuses to start a benchmark that declares no `expected()`, and a body that never marks its
measurement window is reported `unverified` and excluded from every table.

Some shapes do an amount of work that depends on the interleaving — a philosopher's refused
requests, a barber's turned-away customers, how often a search hands its frontier back — and no
framework-free value can assert it. Such work is **reported beside the cell, never silently
compared**: the body returns it in `qvo::Answer::observed`, every measured repetition's value is
written into the result document, and the report prints it next to the row it belongs to. Where
the semantics requires a minimum of that work, the spec declares it in
`qvo::Spec::observed_at_least` and it is asserted like the checksum: a missing or short
observation fails the run, with no timing.

A wrong answer is one way a run fails; a run that never ends is the other. Every shape here waits
for the end of a protocol — N "done" messages, a report, the end of a chain — so a message the
protocol waits for, when it is **lost** (and, in many shapes, when it is delivered to the wrong
actor or carries a corrupted control field), does not move the checksum at all: the run
**blocks**. `qvo::run` therefore bounds every repetition with a deadline of its own
(`--deadline`; by default 300 s, ten times the slowest repetition any document here records,
scaled up with the work the parameters ask for — `harness/include/qvo/harness.h`). A repetition
that outlives it ends the process with exit 1 and a result document that says so —
`verified:false`, the repetition named, no timing — so a planted defect that blocks reads as
failed, never as still running, and a binary run by hand cannot wait forever. `tools/run.py
--timeout` stays the outer bound of a whole process. A protocol violation a spec or an adapter
detects before any checksum could report it — an answer after the last request, an `(n+1)`-th
`Next` — ends the run the same way, through `qvo::fail()`: exit 1 and a document naming it, from
whichever thread saw it. Never `std::abort()`, which writes no document and owns none of the
harness's exit codes: the Windows runtime ends it with `0xC0000409`, or with 3 — the code of a
cell declared not applicable — under another abort behaviour. Which defects of a shape block and
which move its sum is said on its page under `benchmarks/savina/` ("What blocks, and what moves
the sum"), and so are the few that reach neither verdict — a duplicate of a `concdict` worker's
last answer, for one, lands after its report and is caught only while the worker is still there
to receive it. (A duplicated `thread-ring` token was one, its copy carrying a right total, until
each ring actor kept a tally — how many tokens, the sum of their countdowns — checked against its
closed form after the window, and the deliveries were counted rather than declared.)

## 1. The seven mechanisms

### 1.1 Idiomatic implementation, sourced from the framework's own documentation

Every implementation carries a header block naming the framework document, example or test it is
modelled on:

```
// @framework     sobjectizer 5.8.5.1
// @idiom-source  dev/sample/so_5/ping_pong{,_minimal}/main.cpp -- SObjectizer's OWN samples
// @idiom-note    DEVIATION, in SObjectizer's favour: both shipped samples route through one
//                SHARED mbox. This uses each agent's DIRECT mbox, the framework's fast path.
```

The rule this encodes: **qb's implementation may not be the only tuned one.** Where a framework
offers a faster idiom than the one its documentation leads with, both are implemented, both are
reported, and the *faster* one is what enters the comparison table. The same rule covers the one
knob every framework has on the spin/park axis, and it is enforced by a SWEEP, not by a reading of
the defaults: CAF's work-stealing profile was swept over ten points (`docs/TUNING.md` §1.1 — its
defaults are its fastest setting) and SObjectizer's `combined_lock` spin budget over nine on each
host (§1.2 — nothing above 100 µs moves outside the launch spread, the framework's own 1 ms default
included, so the adapter's 10 s profile is its best), and each framework's spin cell is the best
profile that sweep found. A document measured under a sweep override says so in its first caveat
(`SWEEP DOCUMENT, NOT A TABLE CELL`) and never enters a table.

### 1.2 A floor, not just a field

`frameworks/baseline/` is not an actor framework. It is the same workload written with raw
`std::thread` and a hand-written bounded SPSC ring, doing the minimum the benchmark's semantics
allow. The ring is written *here* rather than taken from qb's `lockfree::spsc`: a floor that
belongs to a contestant is not a floor.

It exists so that "qb is 3× faster than X" cannot be reported without also reporting "and the
floor is 2× faster than qb". Without a floor, a table in which every framework is slow reads as a
win for whoever is least slow. With a floor it reads as what it is: a measure of how much of the
gap is inherent to *being a framework at all*.

The `big` floor keeps naturally aligned, compact slots with one worker and separates each actor
slot onto cache-line boundaries with two or more workers. The `bank-transaction` floor keeps
compact account slots at every worker count under MSVC-compatible compilers (`_MSC_VER`, including
clang-cl); under GCC and other Clang toolchains it uses compact slots with one worker and separate
cache-line slots with multiple workers. Each compiler applies the same policy in spin and park.
Padding increases the bank's working set as well as separating adjacent owners, so it is not a
free improvement. The selection happens before the runtime starts, with no per-message layout
branch. The bank's deque storage remains a separate system allocation. An accepted quiet-host
comparison remains pending. The published result JSON and generated reports still describe the
earlier binaries; no old floor figure or framework-to-floor ratio is a measurement of this source
policy. The `chameneos` floor needs no such separation: all creatures belong to one worker.

### 1.3 Same everything, and the differences are enumerated

| Held equal | How |
|---|---|
| Compiler & flags | One toolchain per host, one optimization level, recorded in every result file |
| Dependency provenance | vcpkg manifest pinned to `builtin-baseline a900048467…` — the same baseline qb's development tree pins |
| Allocator | System allocator for all targets. A framework bundling its own is flagged in the result file, never silently allowed |
| CPU set | Identical affinity mask, applied by the harness before the framework starts |
| Warmup, timing, repetition | Shared harness code, framework-agnostic, one implementation |
| Process isolation | One process per (framework × benchmark × repetition). No two frameworks share a process |

Anything that *cannot* be held equal is recorded as a machine-readable **caveat** in the result
JSON and rendered into the report next to the number it affects — not as prose a reader may miss.

### 1.4 The hybrid-CPU trap, stated because it invalidates most desktop benchmarks

The primary host is an **Intel i9-12900K: 8 performance cores + 8 efficiency cores**. A thread on
an E-core runs at roughly half the clock of one on a P-core. Two runs of the same binary can
differ by more than any framework difference this repository could report.

Every measured run therefore pins to an explicit CPU set, recorded in the result. A run whose
pinning was **requested and silently refused** is discarded rather than reported. That is a real
failure mode, not a hypothetical: qb's own documentation records `setAffinity` returning success
on Apple Silicon while doing nothing at all.

Pinning is necessary and not sufficient: **the host must be quiet, and this was measured rather
than assumed.** A pass over qb's `perf/core-hot-path` branch taken while a build and a test suite
ran on the other cores moved the two-core cells by 20–30 % (Windows 2c-spin 319 → 262 ns, WSL2
270 → 206) and the one-core cells by under 7 % — the one-core cell never leaves its core, the
two-core cell is priced in remote cache-line reads, and a busy sibling core is what perturbs those
(on WSL2 the pinned vCPUs land on whichever host cores the hypervisor picks, so "other cores" is
the whole machine). The rule that follows: a number is published only from a run with nothing
else executing on the host, and a comparison is published only from **shipped and candidate
measured in the same session** — a figure quoted from an earlier table is not a control.
`docs/TUNING.md` §7 "With the branch" is the record of the pass that taught this.

### 1.5 Distributions, never a single number

A benchmark reports **all samples**. The report renders median, IQR, min and p99, and a difference
smaller than the overlap of two distributions is rendered as *"no measurable difference"* rather
than as a percentage. Percentages computed from two means with no spread are how most framework
comparisons are made to say whatever their author wanted.

For a small control/candidate delta, `tools/launch-census.py --alternate-order` runs the two
verified binaries in AB/BA order on alternating launches within each configuration. Use an
even launch count and read `launch-order.jsonl` beside the raw JSON; two sequential blocks
with opposite orders confound order with host drift and are not an equivalent control.

### 1.6 Externally-chosen problems

The benchmark set is the **Savina suite** (Imam & Sarkar, AGERE 2014) — the standard academic
actor benchmark suite. It was chosen by neither qb nor its competitors, it predates qb, and its
problems cover shapes qb is *not* optimized for as well as ones it is.

Choosing your own benchmarks is the oldest way to win a comparison. This is the mitigation.
`benchmarks/savina/` records, per benchmark, the original parameters and a note wherever this
repository deviates from them.

### 1.7 The right of reply

`docs/CHALLENGE.md` is an open invitation: if you maintain, or merely know, one of these
frameworks and believe an implementation here is not how it should be written — send a better
one. The rule, written down in advance so it cannot be relitigated once a result is inconvenient:

> **A submitted implementation that is correct, idiomatic and faster replaces the one in this
> repository, and the tables are regenerated. Including when that makes qb lose.**

## 2. What this repository cannot tell you

Stated up front, because a benchmark's honest limits are load-bearing and are usually buried.

- **It does not measure your application.** Actor micro-benchmarks measure message plumbing. Real
  systems spend their time in parsing, I/O, serialization and database round-trips, where these
  differences frequently vanish.
- **It does not measure correctness, safety or maturity.** CAF has a decade of production use and
  a distributed runtime; SObjectizer has message priorities, delivery filters and a mature
  dispatcher taxonomy. Neither shows up in a nanosecond count. `docs/FEATURES.md` is the
  non-performance comparison, and qb does not lead it everywhere.
- **It does not generalize across hardware.** An 8-core ARM laptop, a 128-core server and a hybrid
  desktop rank these frameworks differently. This repository has measured only the hosts it lists.
- **One version, one date.** Every framework is pinned. A comparison is a photograph.

## 3. Reproducing, and disagreeing

```sh
python3 tools/run.py --build build/final --out results/<host-id> --repetitions 9 --cpus 0,2
python3 tools/report.py --results results/<host-id> > REPORT.md
```

Every number in `REPORT.md` is regenerated from the JSON in `results/`. The figures quoted in
`README.md` are transcribed from those runs, and `tools/check-report.py` **checks that they
still match**: every `REPORT.md` must be byte-identical to `report.py`'s render of its
directory, and every table in `README.md` that follows a `<!-- check-report: <dir> ... -->`
marker is re-derived from the JSON, cell by cell, to the renderer's own formatting and its own
ratio — a figure that is right to the wrong number of decimals is a finding. The figures in
`docs/TUNING.md` are **not** checked, by design: they come from side experiments that are not
cells of a published table, each subsection names the directory they came from, and
`docs/ROADMAP.md` records the gap. `tools/check-roster.py` closes the other silence — a
framework missing from a benchmark, or a results document that no roster cell explains.

`tools/run.py` gives each process a fresh output file beside its cell, checks that the document
names the requested framework and configuration and carries the verdict matching the exit code,
then atomically replaces the cell. A failed launch replaces any older timing with an unverified
document carrying no samples. `run.json` is marked `in_progress` before any cell changes and
`complete` only after every cell is published; `tools/report.py` refuses an interrupted run or a
canonical cell that the completed manifest does not list. Each run has a new `run_id`, and the
report compares the manifest before and after loading and rendering so a concurrent rerun cannot
mix its cells into the prior report.
An atomic directory lock covers discovery as well as measurement, so even a second runner's
`--describe` processes cannot disturb an active run. A filter that selects no cells is refused
before the manifest changes; every named `--only`, `--benchmark`, and `--config` value must also
exist in the discovered roster or configuration list. Every prior cell selected by those filters
must have a current binary and be remeasured. When both `--only` and `--benchmark` are given,
every named framework/benchmark pair must exist; with either axis omitted, the runner uses the
supported discovered pairs without inventing unsupported combinations. If a run is interrupted,
measure again into a new directory, or discard the entire old output directory before a complete
rerun. A partial rerun cannot recover an interrupted directory.

The runner's file and verdict boundary has a portable control with a synthetic process; it runs
without building or measuring a framework: `python3 tools/test_run.py`.

For a launch census over physical binary copies and A/A controls, `tools/launch-census.py
--shuffle-seed N` archives one reproducible family order in `launch-order.jsonl` and executes
that exact plan. The default fixed order and the two-binary `--alternate-order` mode remain
available; these ordering modes cannot be combined.

The harness has its own negative control. Its last complete **base** census was recorded on
2026-10-12, on WSL2 g++-14 and Windows MSVC 19.51; this historical base census remains recorded:

```
python3 tools/negative-control.py --build build/final
    CAUGHT=28 CONFIRMED=11 MISSED=0
```

It plants one defect at a time — a message lost 1 time in 10⁷, a single lost message, a duplicate
delivery, a wrong checksum, a right checksum reached by the wrong amount of work or reported with
no message count at all, a body that never marked its measurement window, an observation below
its asserted lower bound, a bounded observation never reported, a CPU pin that cannot be
applied, a defective repetition before or after a success, including during warmup, a repetition
that never returns — inside its window, in its teardown, after an earlier failure — a
`--deadline` that is not a number of seconds, a protocol violation the body declares through
`qvo::fail()` — from its own thread or another, with the watchdog on or off, after an earlier
failure — a refusal from another thread (`qvo::refuse()`, exit 2), and, on a `thread-ring`
subject that tallies like every adapter, a token delivered twice, a token delivered to the wrong
actor, a token forwarded untouched, an actor the ring routes around, a consistent misroute that
leaves every per-actor count right, and a ring of zero hops — and asserts each is **rejected**, by
the check meant to see it where it names one, with no
partial samples or summary emitted; a blocked repetition must end inside its deadline, and a
declared failure at once, with a document `tools/run.py` publishes as an unverified cell. It also
asserted eleven legitimate shapes were **not** rejected in that base census — among them a run inside its deadline,
`--deadline 0`, a correct ring, and a correct run whose document is the same, clock and timings
aside, with the watchdog armed and without it — because a battery that fails everything is not a
working battery.

The combined source now contains 30 Savina specifications, 30 pages and 30 adapters in each of
qb, CAF, SObjectizer and the floor. The B/C battery additions occupy unique sections L–W and
preserve A–K, including shuffled-arrival positives and faults only a checksum witness can see.
APSP and SOR also have compile-only controls built from their actual qb/floor adapter sources;
normal `qvo-*` binaries contain no plant path. These controls require a plant-fired marker, the
intended rejecting check, empty timing arrays and a document accepted by `tools/run.py` under the
adapter's real identity. The separate Mesh owner-dispatch control has functional JSON verdicts
and no timing fields; its compile-only reentrant mutant must fail while the fixed dispatcher
preserves FIFO over every delivered message.

The merged battery was executed on macOS arm64 with CLT 21 on 2026-10-13:
**151 CAUGHT / 43 CONFIRMED / 0 MISSED**, 194 unique case identities. Its floors are
read from the successful complete `--census-out` export, not summed branch constants.
The first two runs remain archived as failed controls: a transmitted SOR +1 rounded away;
+16 changed a real trajectory but left a final-value difference on the 20×20 grid. The
actual blind control uses a legal 4×4 grid over 100 iterations: the trajectory witness
rejects the +16 publication while final values and update/message counts still match.
Normal SOR positive and protocol controls stay at 20×20 over 100 iterations. Mesh controls
exercise self-ring overflow on owner 0 and owner 1 under both wait modes, delivering all
65,537 messages; cross-worker sender 1 to owner 0 also preserves FIFO in both wait modes.

Windows and WSL must validate this merged candidate separately. This census establishes
neither their verdicts nor performance. Retain every stdout, exit code and exported case
ledger; a new successful census must have zero MISSED and explain any changed identity
before its applicable floors are recorded.

The workload variants are explicit across the four main frameworks. UCT's `nodes=60000` is the
common capacity-limited variant; Savina's canonical `nodes=200000` remains a separate case with
qb's declared actor-pool failure visible. The Actor64 design is under review; it changes no
runtime here and supplies no capacity proof. Sieve keeps canonical `local=1000`, with `local=1`
labelled separately as one-filter-per-prime stress. Facility location retains its documented
omission of sibling notifications in every framework, and radixsort retains Savina's 16-bit
input generator. See each shape's page for the resulting work and deviation.

The two document guards have theirs, and it plants in a sandbox copy while hashing the real
checkout before and after. The following is the last recorded base census:

```
python3 tools/guards-negative-control.py
    CAUGHT=34 CONFIRMED=3 MISSED=0
```

The recorded base counts remain floors. A run below them fails: a battery that quietly stopped planting anything
looks exactly like one that passes. A verifier nobody has watched reject anything is not known to
work. B/C results have not been published: existing eighteen-shape datasets remain historical,
and the complete-results roster/report controls stay red until complete new datasets exist.
Do not weaken those guards to accept missing result documents.
