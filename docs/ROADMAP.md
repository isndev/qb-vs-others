# ROADMAP.md — what is not done

Stated plainly, because a benchmark repository that lets its coverage be inferred from its
ambition has already misled the reader.

## Done

- The harness: verified checksums, verified CPU pinning, distribution reporting, JSON results.
- Build discipline: all frameworks from source, one flag set, pinned refs matching the qb development tree's own
  vcpkg baseline.
- Worker placement through each framework's own public API.
- **Five Savina benchmarks** — `savina/ping-pong`, `counting`, `thread-ring`, `fork-join`,
  `big` — for qb 3.1.0, CAF 1.1.0, SObjectizer 5.8.5.1 and the floor, plus the `caf-detached`
  variant on ping-pong (CAF's only cross-core placement; the other four declare it omitted with
  the reason in `frameworks/caf-detached/CMakeLists.txt`): **84 cells per platform, 82 verified
  + 2 `n/a`**, both platforms measured in one quiet session each on 2026-09-04, with the
  candidate qb branch measured through the same adapters minutes after the shipped build (the
  20-cell grids under `results/*/qb-branch-perf-core-hot-path/M-230c5035/`, with the
  same-session shipped control beside each; `L-32b28130/` is the previous candidate, kept).
  Every qb-side cost the four new shapes exposed is in `docs/TUNING.md` §9, and the one
  the candidate itself exposed — a dispatch made fast enough to flip the cross-core pipe
  into a per-event publish regime — is 9.10, fixed on the same branch.
- **Savina wave A, ten more shapes** — `savina/fork-join-create`, `nqueens`, `a-star`,
  `philosophers`, `barber`, `bndbuffer`, `cigsmok`, `concdict`, `concsll`, `logmap` — for qb,
  CAF, SObjectizer and the floor (`caf-detached` declared omitted): **160 cells per host on
  Windows and WSL2, all verified**, one quiet session per host on 2026-10-11 with **shipped qb
  3.2.1** as the qb column and the 3.3.0 candidate (`develop` `73018675`) beside it in the same
  session, plus a 528-launch interleaved census per host (`results/<host>/savina-<shape>/`,
  `results/<host>/qb-branch-develop/grid-73018675/`, `docs/TUNING.md` §21) — 292 cells per host,
  eighteen shapes. The first shapes whose cells carry OBSERVATIONS beside the number (FAIRNESS.md
  §0: refused requests, wake-ups, producer waits, work messages). The one shape the candidate lost
  on (fork-join-create at one core, WSL2) was traced to qb's arena and fixed, and the fix measured
  level with 3.2.1 in a second session (§21.6). Not yet on macOS or the arm64 guest.
- **The two CAF coherence defects, closed** (`docs/TUNING.md` §1.1 and §8). The spin-knob sweep
  was run on both axes — poll budget 100 → 10⁶ at fixed steal interval, steal interval 1 → 10⁶ at
  fixed budget; ten documents in `results/desktop-win11-msvc19/caf-spin-sweep/` — and no
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
- `tools/negative-control.py` — the last complete base census is **28 CAUGHT / 11 CONFIRMED /
  0 MISSED**, recorded on Windows MSVC and WSL2 g++-14 on 2026-10-12. This historical result covers
  A–K. The merged B/C battery and actual-adapter plants passed their macOS arm64 census
  (151 CAUGHT / 43 CONFIRMED / 0 MISSED, 194 cases); Windows/WSL validation remains;
  branch sums are not measured floors. [FAIRNESS.md](../FAIRNESS.md) describes the export and
  the required case-by-case reconciliation.
- **The document guards**, and their battery. `tools/check-roster.py`: every spec has an adapter
  in every framework or a declared omission with a reason, every adapter binds to exactly its
  spec, every spec has its page, and — given a results directory — every roster cell has a
  document and every document is a roster cell. `tools/check-report.py`: every `REPORT.md` is
  byte-identical to `report.py`'s render of its directory, and every figure in a marked README
  table (`<!-- check-report: <dir> ... -->`, including the one-framework `framework=qb` grids
  of the candidate branch) equals the JSON it summarises, to `report.py`'s own formatting and
  its own ratio. `tools/guards-negative-control.py` plants a defect at a time in a sandbox copy
  and hashes the real checkout before and after: **34 CAUGHT / 3 CONFIRMED / 0 MISSED** in the last recorded base census. Its
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
**measured as one grid on 2026-09-07**: `develop` `43f62afe` (29 commits over v3.1.0) on all
eight shapes against shipped 3.1.0 in one quiet session per host
(`results/<host>/qb-branch-develop/`, README.md's candidate grids, `docs/TUNING.md` §13). What
is left of the pipeline is the train itself.

1. **Record and re-read** — done at each step: `docs/TUNING.md` §7 and §9 carry every axis with
   its A/B, the shipped control of the same session beside every candidate grid, and the burst
   sweep (§9.11) that says what the one-core cell measures.
2. **`perf/event-pipe-segmented`** — done, local (`b34fbc23` + `279e6cd4` over
   `perf/core-hot-path`): the pipe's growth (§9.11) taken by a segmented pipe over a process-wide
   slab cache, A/B'd against `230c5035` on both hosts with the burst sweep as the instrument
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
   without measurable effect on `dmb ish`. **Both arm64 hosts ran the final candidate on 2026-09-19**
   (`174e515a`, all eight shapes, the whole field and two censuses per host, TUNING §13.9): macOS
   again, and the `qb-vm-linux-arm64` machine — which is a native-arm64 Linux, and a GUEST: its park
   floor (20.8 µs) is its hypervisor's. Bare-metal Linux is the run that does not exist yet.
4. **Merge as 3.2.0, in lockstep** — qb + qbm-\* + qb-examples on one train, qev **5.1.0** with
   it (axis E needs `ev_active_count()`, which lives in the 22 shared files the identity guard
   checks). **The merge to `develop` is done** (2026-09-06, the citation sweep in the same
   commits as the pointer bumps); the train (QB-45, `release-gate.sh` in the development tree) is not,
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
   its own for the rest (the qev performance roadmap of the development tree, QB-186: the pass at
   its floor, request timeouts without a libev timer, the embedder's clock, the io pass with
   io_uring, the wake). Phase 1 landed (§17.4, QB-188): the pass 51 → 22 ns on g++, the timed
   ask 174 → 115 — and it found that Windows had been judging every libev timer on the 15.6 ms
   system tick (QB-193, fixed). Phase 2 landed (§17.5, QB-189): the request path has no libev
   timer at all, the timed ask **115 → 68** on g++ and **166 → 90** on MSVC (under its
   pre-QB-193 124). Phase 3 landed (§17.6, QB-191): a core polls a quiet socket on a cadence
   (`EVRUN_NOPOLL` between, hot after a delivery), the pass with a socket **124 → 48.5** on g++
   and **275 → 83** on MSVC — and it found qev's wepoll suite had never measured wepoll
   (QB-194, fixed). Phase 3 landed (§17.7, QB-190): a pass with nothing for the loop to do does
   not run it (the loop asked inline, a timer's reach estimated on the CPU's counter, the
   reading handed to the loop with `ev_now_set` when it fires), a busy pass with a far timer
   **36.9 → 26.2** on g++ and **47.7 → 29.5** on MSVC, a cold io pass 48.6 → 28.5 / 82.9 → 49.5
   — and it found that the QPC clock of QB-193 had never been compiled in on Windows (QB-195,
   fixed). Left on the core side: the receive side per event, and the idle loop's
   shape (§16.4, QB-181), measured against §16 as the base.
6. **After the merge** — the SObjectizer spin-budget sweep (§4, adapter-side); the placement
   paragraph in `qb.llm.md` that closes 9.4 by design and the `send<>` sentence that closes
   9.6; and 9.2, the 32-byte bucket, as a measured 4.0 experiment on top of the segmented pipe.
   §9.12 (the MSVC dispatch gap) closed with item 2: warm, MSVC dispatches at 6.6–10.4 ns against
   g++'s 5.9–9.3 from 2 k to 4 M, so the clang-cl A/B has no premise left.


### Savina B/C: source integrated, combined proof pending

The source roster contains thirty specifications, thirty pages and thirty adapters in each of
qb, CAF, SObjectizer and the floor. Waves B and C supply the twelve shapes below. This source
inventory does not establish validated or published thirty-shape coverage. Published datasets
still cover the earlier eighteen shapes, and the complete-results roster/report controls remain
red until new complete datasets exist.

| wave | benchmarks | combined acceptance work still needed |
|---|---|---|
| B1 | `radixsort`, `filterbank`, `sieve` | Revalidate the shared Mesh dispatch change and wave-A callers; complete applicable sanitizers and supported sieve placement cases. |
| B2 | `trapezoid`, `piprecision`, `recmatmul` | Complete four-framework native matrices, controls, document guards and sanitizers. The handoff reports Windows roster/report/runner checks, but supplies no complete matrix or sanitizer evidence and no battery census. |
| B3 | `quicksort`, `bitonicsort` | Complete native matrices, document guards, runner checks and sanitizers. The handoff reports a standalone MSVC battery of 43 CAUGHT / 13 CONFIRMED / 0 MISSED; it does not certify the merged candidate. Quicksort's `n < threshold` leaf-root is refused before the body runs; retain that domain. |
| C1 | `apsp`, `sor` | Run the new compile-only plants in the actual qb/floor adapter sources, including blind checksum witnesses, protocol rejection and lost-message deadlines; revalidate the final combined source. |
| C2 | `uct`, `facloc` | Prove the SObjectizer dispatcher-binder ownership repair through complete Windows return and repeated teardown, then revalidate matrices and sanitizers on the combined source; include sieve in the lifetime audit. |

The five standalone batteries share the same base. Their additions are merged into unique
sections L–W with A–K preserved, including positive shuffled arrivals and checksum-only
witnesses. Actual APSP/SOR qb/floor plants are compiled into `qvoctl-*` control executables from
the real adapter sources, with no plant path in ordinary benchmark binaries. A separate Mesh
owner-dispatch control emits functional verdicts without timing fields and compares a compile-only
reentrant mutant with the fixed dispatcher. Run the complete combined battery, retain stdout and
exit code, and reconcile every identity in its `--census-out` export with the source inventory
before deriving another floor. The macOS arm64 merged census is recorded in FAIRNESS.md;
Windows/WSL validation and the remaining architecture campaign are still required.

Workload decisions remain visible: UCT's **common `nodes=60000` variant** applies to all four
frameworks; Savina's **canonical `nodes=200000`** is a separate case, with qb's declared pool
failure retained until the Actor64 design has a proven capacity solution. Actor64 is design work
only here. Sieve keeps **canonical `local=1000`**; **`local=1`** is separately labelled
one-filter-per-prime stress. Facility location's omission of sibling notifications and
radixsort's 16-bit generator remain documented deviations applied consistently across the field.

Publication needs one quiet-host campaign: shipped qb and the candidate in the same session,
all four frameworks, one and two cores, spin and park, complete documents and launch censuses
where required. It must include the supported sieve placement sweep and APSP qb push/send forms.
Windows and WSL2 run sequentially on the Windows host; macOS runs unpinned with its launch census.
The existing historical eighteen-shape figures cannot supply B/C results. Do not weaken a guard,
replace a timing output or relabel old data to fill that gap.

Wave A also left the one-time `concsll` placement experiment, the form sweep for every newly
published multi-form shape, and the first `perf` / ETW profile of each new shape. These remain
publication obligations; the source merge does not complete them.

### Actor creation cost and memory footprint

The spawn side is measured now — `savina/fib` is 57 312 spawn-and-die cycles per repetition, and
at `001be013` qb pays **~200 ns per actor lifetime** on one core (11.4 ms / 57 312 on WSL2 — the
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
without the user's password. Needs a `docs/SEASTAR.md` (not written yet) and a provisioning step before it can be a
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
  `idlespin*` documents, `L-32b28130/`, `ab-axis-I/`, `ab-axis-IM/`) that are not cells of a
  published table, and a guard
  that pretended to verify them would verify nothing. Each subsection names the directory its
  numbers came from; a marker grammar for "this figure is `<document>.summary.work_p50 /
  work_units`" would close it and has not been written.
- **The qb-side findings in `docs/TUNING.md` §9 are not tied to a qb commit.** A finding that
  names `VirtualCore.cpp:209` is true of qb 3.1.0 and of the branch at `32b28130`, and already
  false of the branch at `230c5035` (axis M moved `__flush_all__`; §9 says so in prose, which
  is all it can do); nothing here re-checks the citation when either moves. the qb development tree's `llm-guard.py` does exactly that for its own
  docs and does not read this repository.
- **A footprint probe.** See "Actor creation cost and memory footprint".
