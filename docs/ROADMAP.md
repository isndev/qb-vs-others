# ROADMAP.md — what is not done

Stated plainly, because a benchmark repository that lets its coverage be inferred from its
ambition has already misled the reader.

## Done

- The harness: verified checksums, verified CPU pinning, distribution reporting, JSON results.
- Build discipline: all frameworks from source, one flag set, pinned refs matching qb-dev's own
  vcpkg baseline.
- Worker placement through each framework's own public API.
- **Five Savina benchmarks** — `savina/ping-pong`, `counting`, `thread-ring`, `fork-join`,
  `big` — for qb 3.1.0, CAF 1.1.0, SObjectizer 5.8.5.1 and the floor, plus the `caf-detached`
  variant on ping-pong (CAF's only cross-core placement; the other four declare it omitted with
  the reason in `frameworks/caf-detached/CMakeLists.txt`): **84 cells per platform, 82 verified
  + 2 `n/a`**, both platforms measured in one quiet session each on 2026-09-04, with the
  candidate qb branch measured through the same adapters minutes after the shipped build (the
  20-cell grids under `results/*/qb-branch-perf-core-hot-path/M-f5c20eeb/`, with the
  same-session shipped control beside each; `L-ba051409/` is the previous candidate, kept).
  Every qb-side cost the four new shapes exposed is in `docs/TUNING.md` §9, and the one
  the candidate itself exposed — a dispatch made fast enough to flip the cross-core pipe
  into a per-event publish regime — is 9.10, fixed on the same branch.
- **The two CAF coherence defects, closed** (`docs/TUNING.md` §1.1 and §8). The spin-knob sweep
  was run on both axes — poll budget 100 → 10⁶ at fixed steal interval, steal interval 1 → 10⁶ at
  fixed budget; ten documents in `results/desktop-b67osn6-win-msvc/caf-spin-sweep/` — and no
  profile beat CAF's shipped defaults, so the adapter now declares `wait=1` ≡ `wait=0` in its
  caveats instead of pretending a second column. The cross-core question is answered by
  `frameworks/caf-detached/` through `caf::detached`, CAF's own placement primitive, with its
  spin cells reported as **not applicable** (a third harness verdict, exit 3) rather than invented.
  What that cell revealed — a bistable ~1 µs / ~10.6 µs (Windows) and ~3.5 µs / ~26 µs (WSL2)
  park cost — made `tools/report.py` grow a bimodality detector that prints both modes and
  refuses to rank against such a cell.
- The harness records its toolchain (`env`) in every document, the n/a ones included; `run.py`
  merges a filtered re-run into the manifest and refuses to merge across hosts, CPU sets or
  repetition counts; `report.py` skips side-experiment directories by name and treats two
  documents for one cell as a hard stop.
- The qb park-cost experiment behind `QVO_QB_IDLE_SPIN_US` (`docs/TUNING.md` §8): the branch's
  idle-spin floor forced to 0 on both platforms, which is the only way to make a qb ping-pong
  actually block, and the measurement of what it then pays.
- `tools/run.py`, `tools/report.py`. Every published figure is regenerated.
- The tuning sweeps, including the one that had CAF handicapped.
- `tools/negative-control.py` — **7 CAUGHT / 4 CONFIRMED / 0 MISSED**. The verifier has been
  watched rejecting a 1-in-10^7 message loss, a single lost message, a duplicate, a wrong
  checksum, a right checksum reached by the wrong amount of work, an unmarked measurement window
  and a refused CPU pin — and watched NOT rejecting the four shapes that are legitimate.
- **The document guards**, and their battery. `tools/check-roster.py`: every spec has an adapter
  in every framework or a declared omission with a reason, every adapter binds to exactly its
  spec, every spec has its page, and — given a results directory — every roster cell has a
  document and every document is a roster cell. `tools/check-report.py`: every `REPORT.md` is
  byte-identical to `report.py`'s render of its directory, and every figure in a marked README
  table (`<!-- check-report: <dir> ... -->`, including the one-framework `framework=qb` grids
  of the candidate branch) equals the JSON it summarises, to `report.py`'s own formatting and
  its own ratio. `tools/guards-negative-control.py` plants a defect at a time in a sandbox copy
  and hashes the real checkout before and after: **33 CAUGHT / 3 CONFIRMED / 0 MISSED**. Its
  first full run found two of its own controls planting nothing (one written for a `N.NN ns`
  the renderer never emits) and one guard answering "inconclusive" where a named results
  directory that does not exist is a finding — all three fixed before the figure was recorded.
- `docs/FEATURES.md` — the non-performance comparison, every claim cited `path:line` into the
  three source trees, with the eight things qb should be honest about at the end.

## Not done

### The qb branch — the 3.2.0 pipeline, in order

Everything performance-side is aimed at qb **3.2.0** (a minor: the 2c-park collapse is a
user-reachable defect, and the branch changes no observable behaviour). All of it is local
until the two platforms this host cannot see have run it; nothing on qb or qev is pushed.

1. **Record and re-read** — done at each step: `docs/TUNING.md` §7 and §9 carry every axis with
   its A/B, the shipped control of the same session beside every candidate grid, and the burst
   sweep (§9.11) that says what the one-core cell measures.
2. **`perf/event-pipe-segmented`** — the pipe's growth (§9.11) on a branch of its own, off
   `perf/core-hot-path`, A/B'd against it on both hosts with the burst sweep as the instrument
   and the five-benchmark grid as the regression check; suites on MSVC and g++ (release,
   ASan+UBSan, TSan) before any number is quoted. The `Pipe.h:118` contract is retired by it, so
   its pinning test (`PipeAllocatorContract.*`) turns into the opposite assertion.
3. **macOS + Linux** — before the merge, on the maintainer's macOS: the full suite (`release`,
   `sanitize`, `sanitize-thread`) and qb's own `dev/bench` gate against
   `baseline/macos-arm64.json`, which is the instrument for the axis-K fence on arm64; on the
   self-hosted `qb-vm-linux-arm64` runner: this repository's matrix, native, which is also the
   first non-hypervisor park floor. Neither exists yet as a run.
4. **Merge as 3.2.0, in lockstep** — qb + qbm-\* + qb-examples on one train, qev **5.1.0** with
   it (axis E needs `ev_active_count()`, which lives in the 22 shared files the identity guard
   checks); the root's citation sweep (`cite-digest.baseline`, `llm-guard.baseline`, `.cursor/`,
   the Factbook — already drifted by the branch, uncommitted) lands in the same commit as the
   pointer bump, or `verify.sh` is red in between.
5. **After the merge** — §9.12 (clang-cl on Windows, compiler vs OS on the dispatch gap); the
   SObjectizer spin-budget sweep (§4, adapter-side); the placement paragraph in `qb.llm.md`
   that closes 9.4 by design and the `send<>` sentence that closes 9.6; and 9.2, the 32-byte
   bucket, as a measured 4.0 experiment on top of the segmented pipe.


### The other 20 Savina benchmarks

Five of the suite's twenty-five are measured — the round trip, the fan-in, the ring, the fan-out
and the all-to-all. None of the five creates an actor after start-up, blocks on a rendezvous or
carries a pipeline. The ones that would change the picture most, roughly in order of what they
would teach:

| benchmark | what it adds that the five cannot show |
|---|---|
| `chameneos` | rendezvous through a shared broker — mailbox contention with state |
| `fib` / `nqueens` / `a-star` | dynamic actor creation and destruction, which none of the five exercises |
| `bank-transaction` | request/response with a reply promise |
| `philosophers` / `barber` / `smokers` | blocking-shaped coordination |
| `radixsort` / `sieve` / `trapezoid` | pipelines and data-parallel shapes |

Each needs one spec header in `benchmarks/specs/qvospec/savina/` and one implementation per
framework — `check-roster.py` refuses a framework missing from one. The per-framework support
headers (`frameworks/<fw>/*_support.h`) exist so that placement and spin/park do not have to be
re-decided twenty more times.

### Actor creation cost and memory footprint

Neither is measured at all. "How much does an actor cost to spawn, and how many bytes does it
occupy" is one of the first questions anyone asks of an actor framework, and this repository
currently cannot answer it. It needs a different measurement shape from the Savina timings —
`savina/fib` and `savina/nqueens` would give the spawn side; the footprint side needs an RSS probe
the harness does not have.

### The Linux axis

**WSL2 Debian 13 / g++ 14.2 is run** — 84 cells, 82 verified + 2 `n/a`, `results/wsl-debian-g++14/`,
measured in one quiet session on 2026-09-04; read with `docs/TUNING.md` §6, §8 and §9: the 2-core
park rows of the two benchmarks that cross a core per message measure the hypervisor's ~12 µs
futex wake, not the frameworks (the raw condition-variable floor is 25.47 µs per ping-pong round
trip and 13.01 µs per ring hop there). Two things remain:

- **Native Linux.** The same matrix on bare metal or a non-nested VM, where a futex wake is
  2–5 µs and the park row becomes a framework measurement. Nothing in the C++ path needs root.
- **The bistable cross-core park, traced.** `caf-detached` 2c-park, and the qb branch at idle
  floor 0 on Windows, each show two stable modes per repetition (Done, above, has the numbers).
  The fast mode is *consistent with* a phase lock where every message lands before its receiver
  reaches the futex / `WaitOnAddress` sleep; that is an inference from timings. A trace (ETW on
  Windows, `perf sched` on Linux) that counts actual sleeps per repetition is what would turn it
  into a finding, and it has not been taken.

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

### Guards this repository still does not have

- **`docs/TUNING.md` is not figure-checked.** `check-report.py` deliberately parses README.md
  only: TUNING's numbers come from side experiments (`caf-spin-sweep/`, the `ab-*` and
  `idlespin*` documents, `L-ba051409/`, `ab-axis-I/`, `ab-axis-IM/`) that are not cells of a
  published table, and a guard
  that pretended to verify them would verify nothing. Each subsection names the directory its
  numbers came from; a marker grammar for "this figure is `<document>.summary.work_p50 /
  work_units`" would close it and has not been written.
- **The qb-side findings in `docs/TUNING.md` §9 are not tied to a qb commit.** A finding that
  names `VirtualCore.cpp:199` is true of qb 3.1.0 and of the branch at `ba051409`, and already
  false of the branch at `f5c20eeb` (axis M moved `__flush_all__`; §9 says so in prose, which
  is all it can do); nothing here re-checks the citation when either moves. qb-dev's `llm-guard.py` does exactly that for its own
  docs and does not read this repository.
- **A footprint probe.** See "Actor creation cost and memory footprint".
