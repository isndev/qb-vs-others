# CHALLENGE.md — the right of reply

This repository benchmarks qb against CAF and SObjectizer, and it is written by qb's maintainer.
Its author knows qb far better than he knows the others. That asymmetry does not go away by being
careful about it; the only thing that removes it is somebody who knows the other framework
sending a better implementation.

## The rule, written down in advance

> **A submitted implementation that is correct, idiomatic and faster replaces the one in this
> repository, and the tables are regenerated. Including when that makes qb lose.**

It is written down in advance so that it cannot be relitigated once a result is inconvenient.

"Correct" has a precise meaning here and it is the only hard requirement: your implementation must
return the checksum that `benchmarks/specs/qvospec/<suite>/<name>.h` computes, with no framework
linked. A faster implementation that reaches a different answer is not a faster implementation.

## What to send

A single `.cpp` under `frameworks/<framework>/<suite>/<name>.cpp`, carrying the header block every
implementation here carries:

```
// @benchmark     savina/<name>
// @framework     <framework> <version>
// @idiom-source  <the document, example or test in that framework's own tree you modelled it on>
// @idiom-note    <what you did differently from that source, and why>
```

The `@idiom-source` is not decoration. Every implementation here names the framework's own example
it was written from — CAF's `examples/hello_world.cpp` and `dancing_kirby.cpp`, SObjectizer's
`sample/so_5/ping_pong`, qb's `ping-pong-latency.cpp` — so that a reviewer can check the code
against what the framework's authors actually recommend rather than against this author's taste.

## What you do not have to argue about

These are already conceded, in writing, in [FAIRNESS.md](../FAIRNESS.md):

- **Configuration knobs are swept, not chosen.** If you think a framework was given a bad profile,
  you are probably right — it has already happened once, and [TUNING.md](TUNING.md) records the
  spin profile this repository handed CAF that made CAF 2x slower than its own defaults.
- **Placement is symmetric.** Every framework's workers are pinned through that framework's own
  public API. If yours has an API this repository missed, that is a defect here.
- **The faster idiom wins.** Where a framework offers something faster than what its documentation
  leads with, the faster one is what belongs in the table. SObjectizer's direct mbox is used here
  rather than the shared mbox both its shipped samples use, for exactly that reason.
- **One benchmark is not a comparison.** Only `savina/ping-pong` exists so far, and it is the
  narrowest workload in the suite.

## What will not be accepted

- An implementation that reaches the right checksum by doing less work than the benchmark
  specifies. `expected_messages` exists to catch this and is asserted.
- An implementation that measures a different window. `Watch::start()` opens when the first
  workload message is injected into a warm system; `Watch::stop()` closes when the terminal
  condition is observed. Framework construction and teardown are reported separately, as
  `outside_window_ns`, and are not something to hide work in.
- A build configuration only your framework gets. One `CMAKE_CXX_FLAGS_RELEASE`, everything from
  source, no prebuilt packages.

## If you would rather just tell us we are wrong

That is welcome too, and it is cheaper for you. Open an issue saying which number you do not
believe and why. "Your CAF implementation should have used X" is a useful issue even without a
patch, and it is how the CAF tuning defect in TUNING.md was found — by the author re-reading his
own configuration and not believing the result.
