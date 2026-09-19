# QB-46 — the same qb tree under MSVC 19.51 and clang-cl 22.1.7, Windows 11

The discriminating experiment Huly **QB-46** asked for: qb `develop` `381e4995` built twice from
`D:\repo\qb-dev\qb` by `cl` 19.51.36256 (`build/ab46-msvc`) and by `clang-cl` 22.1.7 (`build/ab46-clang`
— LLVM's Clang behind MSVC's command line, ABI, CRT and STL, same allocator, same vcpkg), measured
2026-09-08 **23:01–23:05 UTC** in one quiet session (no build during the points, Docker Desktop quit,
the WSL2 side idle at 0.00): eight Savina cells × eight interleaved rounds (3 repetitions + 1
warm-up, CPUs 0,2), six probes × five alternations (1.5 s, CPU 0), `dispatch-population` × three,
then the burst sweep (`counting` 1c-spin, 2 k … 4 M, 7 repetitions + 2 warm-up). The flags of the
two builds differ only by what the compilers accept of the same CMake lines (`/O2 /Ob2` from
CMake's Release set on both; qb's GCC/Clang branch added `-O3 -march=x86-64 …` under clang-cl, see
QB-201); the clang-cl build carried 17 919 warnings (`-Wall` read as `/Wall` = `-Weverything`,
fixed since) and **crashed on every `qb::ask`** (QB-200, fixed since) — which is why two cells and
one probe below read CRASH.

| file | what |
|---|---|
| `census/` | the eight cells × msvc/clang × 8 rounds (JSON) |
| `probe.txt` | `ask-cost` `push` / `ask`, `pass-cost` k = 1 / 2, `io-pass` `timer` / `pass` — msvc / clang × 5 |
| `dispatch-population.txt` | N = 16 … 16 384 actors, batches of 256, msvc / clang × 3 |
| `burst-sweep/` | `counting` 1c-spin, burst 2 k … 4 M, 7 repetitions, both binaries (`tools/burst-sweep.py`) |

None of it is merged into the published tables.

## The cells (`work_p50 / work_units`, ns, medians of eight interleaved rounds)

| cell | `cl` 19.51 | **`clang-cl` 22** | Δ | quartiles cl / clang-cl |
|---|---:|---:|---:|---|
| ping-pong 1c (round trip) | 31.6 | **27.4** | **−13.2 %** | 30.6–31.6 / 27.2–28.1 |
| ping-pong 2c | 192.1 | 182.9 | −4.8 % | 185.5–195.1 / 175.5–184.5 |
| big 1c | 17.1 | **14.8** | **−13.6 %** | 17.0–17.2 / 14.6–15.0 |
| counting 1c | 12.1 | 11.7 | bimodal on both (8.2–12.1 / 8.4–12.0) | — |
| fib 1c (actor) | 179.1 | **161.0** | **−10.1 %** | 174.7–180.4 / 160.1–162.6 |
| thread-ring 2c | 94.7 | 98.7 | +4.3 %, spreads overlap | 91.5–99.6 / 97.2–99.1 |
| bank-transaction 1c | 232.3 | CRASH | — | the `ask` path, QB-200 |
| bank-transaction 2c | 142.7 | CRASH | — | idem |

## The probes (one core, medians of five)

| probe | `cl` | **`clang-cl`** | Δ |
|---|---:|---:|---:|
| `push` (ns per trip) | 31.56 | **28.73** | −9.0 % |
| `ask` (500 ms timeout) | 90.6 | CRASH | QB-200 |
| `pass-cost` k = 1 (ns per pass) | 15.58 | **14.06** | −9.8 % |
| `pass-cost` k = 2 | 22.54 | **19.52** | −13.4 % |
| `io-pass` `timer` (a busy pass with one far timer) | 29.28 | 27.91 | −4.7 % |
| `io-pass` `pass` (a quiet socket, 1 µs cadence) | 48.87 | **43.72** | −10.5 % |

`dispatch-population` (ns per event, medians of three): 6.12 / **5.15** at 16 actors (−15.8 %),
6.64 / 5.45 at 256, 6.97 / 6.22 at 1 024, 11.26 / 10.45 at 4 096, 16.77 / 15.57 at 16 384.

## The burst sweep (`counting` 1c-spin, ns per message, p50 of seven)

| burst | `cl` | `clang-cl` | Δ |
|---:|---:|---:|---:|
| 2 000 | 5.75 | 5.80 | +0.9 % |
| 10 000 | 5.68 | 5.76 | +1.4 % |
| 30 000 | 5.73 | 6.09 | +6.3 % |
| 100 000 | 6.02 | 6.03 | level |
| 300 000 | 6.87 | 7.33 | +6.8 % |
| 1 000 000 | 8.20 | **12.26** | **+49 %** (min 8.05 / 12.14 — not noise) |
| 4 000 000 | 8.23 | 8.66 | +5.3 % |

## Reading — codegen or platform?

**Both, in halves.** On the dispatch-bound shapes — two actors and a reply (ping-pong), many actors
and a fan-out (big), an actor's whole life (fib), the pass itself (`pass-cost`), the io pass — the
same source compiled by clang-cl runs **10–18 % faster** than by MSVC on the same silicon, CRT and
allocator: that part of §9.12's gap is MSVC's codegen (its inlining and register allocation of
`__receive_events__` → `route` → the trampoline → the handler → `push`). Against g++-14 on WSL2 the
same tree reads ping-pong 1c **22.8 ns** (`../../wsl-debian-g++14/qb-branch-perf-loop-listener-ref/`):
MSVC 31.6 is +39 %, clang-cl 27.4 is +20 % — so the other half is not the compiler: the platform's
clock, allocator, page and scheduling behaviour, which a hypervisor's guest and a native Windows
share differently, and which the `counting` burst sweep isolates — on ONE actor whose dispatch is
cache-resident and perfectly predictable, `cl` and `clang-cl` are level (5.7–6.0 ns a message from
2 k to 100 k), and §9.12's original 3× (20.5 → 28.3 ns on MSVC against 6.5 → 12.5 on g++) is gone
with the segmented pipe (QB-43) and the pass floor (QB-182/188): at 2 k the cell reads 5.75 here
against g++'s ~5 on the same shape. One anomaly is recorded, not explained: at a burst of exactly
1 M, the clang-cl binary reads +49 % over MSVC with a minimum as high as its median, and at 4 M
the two are +5 % apart again — a layout effect of that burst size on that binary, the same class
as the non-monotonic MSVC segment §9.12 recorded, and a thing to re-measure once QB-200/201 have
landed (the binary that produced it carried the broken flag set).

**What it changed.** Two defects found on the way are the actionable part: under clang-cl every
`task<qb::Event>` crashed on its first `co_await` — the MSVC STL's `from_promise()` / `promise()`
computed with an alignment of zero, wrong for an over-aligned promise (QB-200, fixed in qb with
`promise_access.h`, reproduced in thirty lines) — and qb's CMake compiled clang-cl through its
GCC/Clang branch (QB-201, fixed: clang-cl is an MSVC-frontend toolchain now, 0 warnings, the
`clang-cl` preset, the suite 194/194). §9.12 closes with a recommendation rather than a defect: on
Windows, build with clang-cl.
