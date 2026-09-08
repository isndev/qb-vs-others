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
user-reachable defect, and the branch changes no observable behaviour). **Merged on qb
`develop` since 2026-09-06** — axes A–N, QB-43, the ask/coroutine work, QB-171..178 — and
**measured as one grid on 2026-09-07**: `develop` `f8eba11d` (29 commits over v3.1.0) on all
eight shapes against shipped 3.1.0 in one quiet session per host
(`results/<host>/qb-branch-develop/`, README.md's candidate grids, `docs/TUNING.md` §13). What
is left of the pipeline is the train itself.

1. **Record and re-read** — done at each step: `docs/TUNING.md` §7 and §9 carry every axis with
   its A/B, the shipped control of the same session beside every candidate grid, and the burst
   sweep (§9.11) that says what the one-core cell measures.
2. **`perf/event-pipe-segmented`** — done, local (`518d956e` + `a017b8a5` over
   `perf/core-hot-path`): the pipe's growth (§9.11) taken by a segmented pipe over a process-wide
   slab cache, A/B'd against `f5c20eeb` on both hosts with the burst sweep as the instrument
   (g++ 1 M: 35.0 → 9.2 ns, MSVC 25.8 → 9.5; page faults per 1 M process 230 942 → 291) and the
   five-benchmark grid plus a launch census as the regression check — no cell slower beyond the
   instrument's spread, the two cross-core `2c-spin` cells on Windows read through the census
   (§9.11). Suites on MSVC and g++ (release, ASan+UBSan, TSan) run before the numbers were
   quoted. The `Pipe.h:118` contract is retired by it; `PushReferenceStability.*` asserts the
   opposite.
3. **macOS + Linux** — before the merge, on the maintainer's macOS: the full suite (`release`,
   `sanitize`, `sanitize-thread`) and qb's own `dev/bench` gate against
   `baseline/macos-arm64.json`, which is the instrument for the axis-K fence on arm64; on the
   self-hosted `qb-vm-linux-arm64` runner: this repository's matrix, native, which is also the
   first non-hypervisor park floor. **macOS ran on 2026-09-05** (`results/macbook-m4pro-macos-clang21/`,
   TUNING §9.13): every superproject preset at its floor, `dev/bench` PASS with the one gated
   engine metric +94.9 %, the grids and censuses in the candidate's favour, one residual
   (ping-pong 2c-park, +12 % across three instruments, distributions overlapping), the axis-K fence
   without measurable effect on `dmb ish`. Native Linux is the run that does not exist yet.
4. **Merge as 3.2.0, in lockstep** — qb + qbm-\* + qb-examples on one train, qev **5.1.0** with
   it (axis E needs `ev_active_count()`, which lives in the 22 shared files the identity guard
   checks). **The merge to `develop` is done** (2026-09-06, the citation sweep in the same
   commits as the pointer bumps); the train (Huly QB-45, `dev/agent/release-gate.sh`) is not,
   and the 3.2.0 grid above is the figure it ships with.
5. **The residuals of §13.3, on `develop` before the train** — residual 1 (the per-pass cost of a
   core with one event in flight) took its first cut on 2026-09-07: `perf/loop-clock-on-demand`
   (Huly QB-180, §14 — the tick phase's `LoopEvent` read the wall clock on every pass of a
   callback-free core; ping-pong 1c **65.8 → 28.3 ns** on g++, **80.7 → 41.6** on MSVC, the
   cross-core cells inside their spread once the idle pass kept a paced clock read), measured on
   both hosts in one session each, A/B'd through the censuses. What §14 leaves: the pass itself
   (`has_work()`, the empty `__receive__` walk, the resolver) with `perf` as the instrument, and a
   hardware wait on the peer line (`umonitor`/`umwait`, `tpause`, arm64 `wfe`) as an axis nobody
   has tried. The second cut landed the same day (QB-182, §15): the pass itself and its enqueue
   path, measured with the `pass-cost` probe — a one-event pass 14.6 → 12.9 ns and the marginal
   event 8.9 → 4.2 on g++, 20.4 → 15.6 on MSVC; ping-pong 1c **−21 / −26 %**, fork-join and big
   −10 to −25 %; one cell the other way (thread-ring 2c, +1–5 %, bisected to a shorter idle pass,
   handed to QB-181 with the ring as its instrument). The pass ORDER question of §15.3 was
   measured and parked (QB-183, §16.1: no cell beyond its spread; a two-actor ping-pong
   phase-locks and only phase-averaged probes compare across builds) and led to the third cut
   (QB-184, §16): the SPSC ring's producer re-read the line it publishes on every enqueue — a
   cross-core miss per hop — and with the working indices on private lines the two-core
   ping-pong reads **−22 %**, the cross-core ring **−31 %** per hop, `Multi_PingPong` −20 % on g++,
   on MSVC ping-pong 2c −22 %, the ring −29 %, `Multi_PingPong` −23 %. The fourth cut (QB-185,
   §17.1) resumed an `ask` reply inline from the handler that routes it: ask 54 → 46.7 ns on
   g++, 82 → 72 on MSVC, bank 2c −9 %. And §17.2 is where the audit turned to **qev**: a timed
   ask cost 798 / 1108 ns because libev read its clocks through the raw syscall and polled the
   backend over a loop with no fd — 172 / 124 after the two fixes (QB-187), and a programme of
   its own for the rest (`dev/plans/roadmaps/QEV_PERFORMANCE_ROADMAP.md`, QB-186: the pass at
   its floor, request timeouts without a libev timer, the embedder's clock, the io pass with
   io_uring, the wake). Left on the core side: the receive side per event, and the idle loop's
   shape (§16.4, QB-181), measured against §16 as the base.
6. **After the merge** — the SObjectizer spin-budget sweep (§4, adapter-side); the placement
   paragraph in `qb.llm.md` that closes 9.4 by design and the `send<>` sentence that closes
   9.6; and 9.2, the 32-byte bucket, as a measured 4.0 experiment on top of the segmented pipe.
   §9.12 (the MSVC dispatch gap) closed with item 2: warm, MSVC dispatches at 6.6–10.4 ns against
   g++'s 5.9–9.3 from 2 k to 4 M, so the clang-cl A/B has no premise left.


### The other 17 Savina benchmarks

Eight of the suite's twenty-five are written — the round trip, the fan-in, the ring, the fan-out,
the all-to-all, since 2026-09-06 the two that the first five could not show: `savina/fib`
(dynamic actor creation and destruction — 57 312 actors born and dead inside the window) and
`savina/chameneos` (rendezvous through a shared broker), and since 2026-09-07
`savina/bank-transaction`, the first that WAITS for a reply (one `qb::ask` per transfer, 50 000
of them, `benchmarks/savina/bank-transaction.md`). All three are published on Windows and WSL2
with shipped 3.1.0 like the five before them (`results/<host>/savina-fib/`,
`savina-chameneos/`, `savina-bank-transaction/`; macOS not yet), and were written against the
qb work they produced (`results/<host>/qb-branch-perf-dense-table-growth/`,
`qb-branch-perf-coro-scope-local-refcount/`; `docs/TUNING.md` §11 and §12), and joined the
tables with the 3.2.0 grid on 2026-09-07 (`qb-branch-develop/`, §13). fib alone found a 43 s defect in unreleased `develop` on its first
run, and then found that shipped 3.1.0 logs nine INFO lines per actor lifetime inside the window
— 159 / 459 ms against the branch's 7.6 / 10.5; bank-transaction put a `perf` profile on the
coroutine request path for the first time and found five defects on it in one afternoon (qb
`9814c2a1`) — the argument for writing the rest. None of the eight carries a pipeline. The ones
that would change the picture most, roughly in order of what they would teach:

| benchmark | what it adds that the eight cannot show |
|---|---|
| `nqueens` / `a-star` | creation with WORK per actor — fib's nodes compute nothing, so it isolates the registry; these two would show whether the registry still matters once a node does something |
| `philosophers` / `barber` / `smokers` | blocking-shaped coordination |
| `radixsort` / `sieve` / `trapezoid` | pipelines and data-parallel shapes |

Each needs one spec header in `benchmarks/specs/qvospec/savina/` and one implementation per
framework — `check-roster.py` refuses a framework missing from one. The per-framework support
headers (`frameworks/<fw>/*_support.h`) exist so that placement and spin/park do not have to be
re-decided seventeen more times; fib, chameneos and bank-transaction cost one afternoon each on
that basis.

### Actor creation cost and memory footprint

The spawn side is measured now — `savina/fib` is 57 312 spawn-and-die cycles per repetition, and
at `8362a4b8` qb pays **~200 ns per actor lifetime** on one core (11.4 ms / 57 312 on WSL2 — the
floor's malloc-and-free node is 30 ns) against CAF's 1.2 µs and SObjectizer's 2.6 µs. The
footprint side is not: "how many bytes does an actor occupy" needs an RSS probe
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
