# qb branch `perf/request-deadlines` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-189** (a request timeout is a deadline in the core's own clock, not a
libev timer) against the `develop` it forks from (qb `639d1708`, qev `3c89e5a`: the QB-185 /
187 / 188 / 193 batch), measured on 2026-09-08 in quiet windows (no build, the Windows side
idle, load < 1 at the start of each). The candidate is `~/qvo/cand-189` built against `~/qb-189`
(a clean LF clone at the branch), the control `~/qvo/cand-188`, the same clone at `639d1708`;
same flags (`-O3 -DNDEBUG`), CPU 0 for the probes and CPUs 0,2 for the cells, candidate and
control alternated.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-ask-cost`: `ask` and one- and 64-chunk `ask_stream` **with a 500 ms timeout**, the untimed `ask`, `push`; `qvoprobe-pass-cost` k = 1 — cand / ctl × 5, 1.5 s each. The last of three runs, on the final code (the list as a `VirtualCore` member, the disarm fast path inline). |
| `census/`, `census-summary.txt` | bank-transaction 1c/2c-spin and ping-pong 1c-spin, cand / ctl × 5 launches (3 reps + 1 warmup each). |
| `census2/`, `census2-summary.txt` | bank-transaction 1c/2c-spin and ping-pong 2c-spin, **× 10 launches** — the witness that the per-pass gate is invisible on the cells that never arm a deadline. |

None of it is merged into the published tables.

## The probes (ns per round trip, one core, medians of five)

| probe | control (`639d1708`) | **branch** | Δ |
|---|---:|---:|---:|
| **ask with a 500 ms timeout** | 111.6 | **67.8** | **−39 %** |
| stream, 1 chunk, with a 500 ms timeout | 173.6 | **131.3** | −24 % |
| stream, 64 chunks, with a 500 ms timeout (per chunk) | 26.1 | 25.1 | −4 % |
| ask (no timeout) | 45.3 | 46.0 | level (ten launches: 45.5 → 45.9, inside the spread) |
| push | 23.6 | 23.7 | level |
| `pass-cost` k = 1 (ns per pass, no watcher) | 12.6 | 12.3 | level |

What the timed ask pays over the untimed one now: **22 ns** (67.8 − 46.0) — the one precise
clock read at the arm (~17 ns) and the list's insert and unlink — where it paid 66 before (the
arm's `ev_now_update`, the heap, and three `ev_run` passes at 22 ns each). The loop is not run
at all for a pending request: `has_work()` is false, the pass's gate is a member load, and a
busy core with a deadline armed reads the coarse clock (~5 ns) on each pass and the precise one
only within a scheduler tick of the deadline.

## The cells (ns per message / transfer, medians)

| cell | control | **branch** |
|---|---:|---:|
| bank-transaction 2c-spin (× 10) | 83.0 | 80.9 |
| bank-transaction 1c-spin (× 10) | 142.9 | 144.8 |
| ping-pong 2c-spin (× 10) | 159.5 | 160.3 |
| ping-pong 1c-spin (× 5) | 23.1 | 23.1 |

bank asks with `duration::zero()` and ping-pong never asks: these cells see only the per-pass
gate and the awaiter's new shape, and read level inside their spreads (bank 1c is bimodal
between ~127 and ~143 on this host; both trees show both regimes).

## The two shapes that were measured and replaced

The list was first a `thread_local` behind an out-of-line `deadlines_armed()`: **push 23.3 →
24.4 ns** (+0.5 per pass, on every pass of every core). Read inline (`extern constinit
thread_local`, one `%fs` load after the linker's GD→LE relaxation) the g++ cost vanished, but
MSVC's TLS access is four dependent loads and read **push 31.6 → 33.6**. The list is a
`VirtualCore` member now — the gate is a load off the object the pass already holds — and both
hosts read level on push and on the one-core pass.

Suites at the branch: WSL2 release / ASan+UBSan / TSan 193/193 ×3 (the eight new cases in
`ask-deadlines`), the superproject presets and the Windows gate: figures in the Huly comment.
