# ROADMAP.md — what is not done

Stated plainly, because a benchmark repository that lets its coverage be inferred from its
ambition has already misled the reader.

## Done

- The harness: verified checksums, verified CPU pinning, distribution reporting, JSON results.
- Build discipline: all frameworks from source, one flag set, pinned refs matching qb-dev's own
  vcpkg baseline.
- Worker placement through each framework's own public API.
- `savina/ping-pong` for qb 3.1.0, CAF 1.1.0, SObjectizer 5.8.5.1 and the floor. 16/16 verified.
- `tools/run.py`, `tools/report.py`. Every published figure is regenerated.
- The tuning sweeps, including the one that had CAF handicapped.
- `tools/negative-control.py` — **7 CAUGHT / 4 CONFIRMED / 0 MISSED**. The verifier has been
  watched rejecting a 1-in-10^7 message loss, a single lost message, a duplicate, a wrong
  checksum, a right checksum reached by the wrong amount of work, an unmarked measurement window
  and a refused CPU pin — and watched NOT rejecting the four shapes that are legitimate.

## Not done

### The other 24 Savina benchmarks

`savina/ping-pong` is one of the suite's twenty-five, and the least representative: it has no
parallelism, no fan-in, no dynamic actor creation and no contention. The ones that would change
the picture most, roughly in order of what they would teach:

| benchmark | what it adds that ping-pong cannot show |
|---|---|
| `thread-ring` | many actors, one token — scheduling fairness and hand-off cost at scale |
| `counting` | pure fan-in to a single mailbox — the contention case |
| `fork-join` | fan-out with no reply — cheapest possible dispatch |
| `chameneos` | rendezvous through a shared broker — mailbox contention with state |
| `big` | all-to-all — the quadratic messaging case |
| `fib` / `nqueens` / `a-star` | dynamic actor creation and destruction, which ping-pong never exercises |
| `bank-transaction` | request/response with a reply promise |
| `philosophers` / `barber` / `smokers` | blocking-shaped coordination |
| `radixsort` / `sieve` / `trapezoid` | pipelines and data-parallel shapes |

Each needs one spec header in `benchmarks/specs/qvospec/savina/` and one implementation per
framework. The per-framework support headers (`frameworks/<fw>/*_support.h`) exist so that
placement and spin/park do not have to be re-decided twenty-four more times.

### Actor creation cost and memory footprint

Neither is measured at all. "How much does an actor cost to spawn, and how many bytes does it
occupy" is one of the first questions anyone asks of an actor framework, and this repository
currently cannot answer it. It needs a different measurement shape from the Savina timings —
`savina/fib` and `savina/nqueens` would give the spawn side; the footprint side needs an RSS probe
the harness does not have.

### The Linux axis

**WSL2 Debian 13 / g++ 14.2 is run** — 16/16 cells verified, `results/wsl-debian-g++14/`, read
with `docs/TUNING.md` §6: the 2-core park row measures the hypervisor's ~13 µs futex wake, not the
frameworks (the raw condition-variable floor is 26.6 µs there). Two things remain:

- **Native Linux.** The same matrix on bare metal or a non-nested VM, where a futex wake is
  2–5 µs and the park row becomes a framework measurement. Nothing in the C++ path needs root.
- **Two CAF coherence fixes** found while reading the CAF adapter against CAF's source
  (TUNING.md §1 correction, README point 3): the `wait=1` profile equals CAF's shipped defaults,
  so CAF needs a *real* spin profile (a sweep of `aggressive-poll-attempts` above 100 at a fixed,
  non-degenerate steal interval), and CAF's `cores=2` ping-pong never crosses a core
  (`worker::delay` prepends to the sender's own queue), so an honest cross-core CAF cell needs
  the receiver forced onto the other worker — or the table has to say that CAF's 2-core figure is
  a 1-core figure.

### Seastar

The closest architectural peer to qb — shard-per-core, message passing, no shared mutable state —
and therefore the most informative comparison available. Not done: Linux-only, and its build
dependencies (DPDK-adjacent, hwloc, fmt, c-ares, protobuf) need packages this host cannot install
without the user's password. Needs `docs/SEASTAR.md` and a provisioning step before it can be a
framework here rather than a wish.

### Cross-language reference points

Erlang/OTP, Pekko (JVM), Actix (Rust), Orleans (.NET). These are runtime comparisons rather than
framework comparisons and belong in a separately labelled section — an Erlang figure answers "what
does the language that invented this model cost", not "is qb faster than CAF". None of the four
runtimes is installed on this host; Rust and .NET install per-user without administrator rights,
the JDK unpacks from a zip, and Erlang is the awkward one.

### Guards this repository promises and does not yet have

Named because FAIRNESS.md and README.md refer to them, and a document that cites a guard that does
not exist is exactly the drift qb-dev's own tooling was built to catch:

- `tools/check-report.py` — assert no hand-written figure has appeared in a committed Markdown
  table. Referenced by FAIRNESS.md 3 and README.md. **Not written yet.**
- `docs/FEATURES.md` — the non-performance comparison (supervision, distribution, typed actors,
  message priorities, delivery filters, maturity), where qb does not lead everywhere. Referenced
  by FAIRNESS.md 2. **Not written yet.**
- A roster cross-check: every spec header has an implementation in every framework, in both
  directions. Nothing currently notices a framework silently missing from a benchmark.
