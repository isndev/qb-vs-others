# qb branch `perf/loop-listener-ref` — WSL2 Debian 13 / g++ 14.2

The WSL2 half of the A/B for Huly **QB-199** (`__workflow__` reaches `listener::current` once per loop
instead of three times a pass — it is an inline thread_local with a non-trivial constructor, so g++
routes every access through its TLS wrapper, the init guard `__tls_init`, 2.2 % of savina/ping-pong
1c under `perf`), measured 2026-09-08 **22:29–22:32 UTC** in one quiet session: 60 s of quiet after
the last build, no build during the points, the Windows side idle, Docker Desktop quit. Control =
`~/qvo-cand-batch` (`develop` `d20417f9`), candidate = `~/qvo-198-C` (`bd0858a2`, QB-199 alone — the
code the branch ships, before the test and the changelog joined it), both from `git archive` into
ext4 against this harness at `1f88232`, same flags (`-O3 -DNDEBUG`), CPUs 0,2, asserted by marker
(`listener::current` 5 / 7 occurrences in `VirtualCore.cpp`).

| file | what |
|---|---|
| `census/` | eight Savina cells × cand/ctl × **12** interleaved rounds (3 repetitions + 1 warm-up, JSON), `census-summary.txt` |
| `probe.txt` | `qvoprobe-ask-cost` `push` / `ask`, `qvoprobe-pass-cost` k = 1 — five alternations, 2 s, CPU 2 |

None of it is merged into the published tables. The same change was measured three more times
inside the QB-198 sessions (`../qb-branch-perf-dispatch-prefetch/`: `ABC` against `AB`, `C` at
eight rounds, and inside `A4`) with the same reading.

## The census (`work_p50 / work_units`, ns, medians of twelve interleaved rounds)

| cell | control `d20417f9` | **candidate `bd0858a2`** | Δ | quartiles ctl / cand |
|---|---:|---:|---:|---|
| bank-transaction 1c | 145.0 | 144.6 | level | 142.8–147.1 / 141.3–146.9 |
| bank-transaction 2c | 81.8 | 80.3 | −1.8 %, spreads overlap | 78.7–82.2 / 78.8–84.0 |
| big 1c | 18.1 | 18.0 | level | 17.9–18.2 / 17.9–18.4 |
| counting 1c | 7.5 | 7.5 | level | 7.4–7.5 / 7.4–7.5 |
| fib 1c | 126.7 | 127.8 | +0.9 %, spreads overlap | 124.3–127.6 / 125.1–129.5 |
| **ping-pong 1c** | 23.1 | **22.8** | **−1.4 %, quartiles separated** | 22.8–23.1 / 22.5–22.9 |
| ping-pong 2c | 154.1 | 153.9 | level | 151.7–154.9 / 148.9–155.1 |
| thread-ring 2c | 73.4 | 73.5 | level | 73.2–73.5 / 72.9–74.0 |

## The probes (one core, medians of five)

| probe | control | **candidate** | Δ |
|---|---:|---:|---:|
| `ask` (ns per trip) | 46.49 | 46.23 | −0.6 % |
| `push` (ns per trip) | 24.03 | 24.04 | level |
| `pass-cost` k = 1 (ns per pass) | 12.28 | 12.31 | level |

One cell moves and it is the one the profile named: a ping-pong at one core is two passes per round
trip and nothing else, so the three wrapper calls a pass were 2.2 % of it and the reference takes
them out — 0.3 ns a round trip, quartiles separated at twelve rounds. Every other cell is level, on
both hosts (the Windows half: `../../desktop-b67osn6-win-msvc/qb-branch-perf-loop-listener-ref/`).
