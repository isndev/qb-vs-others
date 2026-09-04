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

This is `harness/include/qvo/verdict.h`, and it is not optional: a benchmark that never reaches a
`qvo::verify()` call is reported as `unverified` and excluded from every table.

## 1. The seven mechanisms

### 1.1 Idiomatic implementation, sourced from the framework's own documentation

Every implementation carries a header block naming the framework document, example or test it is
modelled on:

```
// @framework     caf 1.1.0
// @idiom-source  CAF manual, "Message Passing" — actor-framework.org/docs (accessed 2026-09-04)
// @idiom-note    Uses the blocking-free event-based idiom the manual leads with. A typed-actor
//                variant is in alt/ and is reported alongside.
```

The rule this encodes: **qb's implementation may not be the only tuned one.** Where a framework
offers a faster idiom than the one its documentation leads with, both are implemented, both are
reported, and the *faster* one is what enters the comparison table.

### 1.2 A floor, not just a field

`frameworks/baseline/` is not an actor framework. It is the same workload written with raw
`std::thread` and a bounded MPSC queue, doing the minimum the benchmark's semantics allow.

It exists so that "qb is 3× faster than X" cannot be reported without also reporting "and the
floor is 2× faster than qb". Without a floor, a table in which every framework is slow reads as a
win for whoever is least slow. With a floor it reads as what it is: a measure of how much of the
gap is inherent to *being a framework at all*.

### 1.3 Same everything, and the differences are enumerated

| Held equal | How |
|---|---|
| Compiler & flags | One toolchain per host, one optimization level, recorded in every result file |
| Dependency provenance | vcpkg manifest pinned to `builtin-baseline a900048467…` — the same baseline qb-dev itself pins |
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

### 1.5 Distributions, never a single number

A benchmark reports **all samples**. The report renders median, IQR, min and p99, and a difference
smaller than the overlap of two distributions is rendered as *"no measurable difference"* rather
than as a percentage. Percentages computed from two means with no spread are how most framework
comparisons are made to say whatever their author wanted.

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
python3 tools/run.py --all --repetitions 11 --out results/<host-id>
python3 tools/report.py --results results/<host-id> > REPORT.md
```

Every number in every table is regenerated from the JSON in `results/`. No figure anywhere in this
repository is hand-written, and `tools/check-report.py` fails if one appears.

The harness has its own negative control (`tools/negative-control.sh`): it plants a framework that
drops 1 message in 10⁷, one that returns a wrong answer, a run whose pinning was refused, and a
benchmark made 5 % slower — and asserts that each is **rejected**. A verifier nobody has watched
reject anything is not known to work.
