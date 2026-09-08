# qb branch `perf/request-deadlines` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-189** (a request timeout is a deadline in the core's
own clock, not a libev timer), measured on 2026-09-08 at 06:21 UTC in one quiet session (load 2,
no build, no leftover probe — an earlier attempt at 06:19 ran beside a probe a truncated
pipeline had left behind and read every figure doubled; it was discarded and the session
re-run, the WSL2 side idle). Control = `build/ab188-cand` (the working tree at `develop`
`6c3a9e0b` with its ev copy), candidate = `build/ab189-cand` against `D:\repo\qb-189-dev`
(the branch, final code). Same flags as the published directories (`/O2 /Ob2 /DNDEBUG`), CPU 0,
candidate and control alternated five times, 1.5 s per point.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-ask-cost` `ask` and one- and 64-chunk `ask_stream` **with a 500 ms timeout**, the untimed `ask`, `push`; `qvoprobe-pass-cost` k = 1 — cand / ctl × 5. |

None of it is merged into the published tables.

## The probes (ns per round trip, one core, medians of five)

| probe | control (`6c3a9e0b`) | **branch** | Δ |
|---|---:|---:|---:|
| **ask with a 500 ms timeout** | 157.1 | **90.1** | **−43 %** |
| stream, 1 chunk, with a 500 ms timeout | 354.8 | **301.9** | −15 % |
| stream, 64 chunks, with a 500 ms timeout (per chunk) | 73.4 | 73.6 | level |
| ask (no timeout) | 66.8 | 65.0 | level |
| push | 31.2 | 31.2 | level |
| `pass-cost` k = 1 | 15.5 | 15.8 | level |

The timed ask is back under the 124 ns it read before the Windows clock became precise
(QB-193) — the acceptance the roadmap wrote for this phase — and pays **25 ns** over the
untimed ask (the one `QueryPerformanceCounter` read at the arm, ~16 ns, plus the list) where it
paid 90 (the arm's `ev_now_update`, the heap, three passes of a 31 ns `ev_run`). The coarse
pre-check here is `GetTickCount64`, a ~3 ns memory read of the system tick.

The thread_local shape measured on the way (`push` 31.6 → 33.6, +1 ns a pass: MSVC's TLS access
is four dependent loads) is what made the list a `VirtualCore` member; with it push reads
level.
