# qb branch `fix/clang-cl-coroutine-promise` — WSL2 Debian 13 / g++ 14.2 (neutrality)

The g++ half of the neutrality measurement for Huly **QB-200** (`promise_access.h`: every
`from_promise` / `promise()` in qb goes through `detail::handle_from_promise` / `promise_of`, which
hand clang-cl the promise's real alignment and ARE the standard calls everywhere else) and
**QB-201** (the clang-cl toolchain profile — CMake only, inert here), measured 2026-09-09
**00:00–00:02 UTC** in one quiet session (60 s of quiet after the builds, no build during the points,
the Windows side idle). Control `develop` `9366384b`, candidate `ce39584a`, both from `git archive`
into ext4 against this harness at `93a9c0a`, same flags, CPUs 0,2, asserted by marker
(`promise_access.h` absent / present).

| file | what |
|---|---|
| `census/` | bank-transaction 1c, ping-pong 1c / 2c, fib 1c × ctl/cand × 8 interleaved rounds (3 repetitions + 1 warm-up) |
| `probe.txt` | `ask-cost` `ask` / `push`, `pass-cost` k = 1 — five alternations, 2 s, CPU 2 |

None of it is merged into the published tables.

| cell / probe | control | candidate | Δ | quartiles ctl / cand |
|---|---:|---:|---:|---|
| bank-transaction 1c (ns per transfer) | 146.2 | 145.7 | level | 145.2–146.5 / 141.9–147.1 |
| fib 1c (ns per actor) | 126.9 | 127.6 | level | 125.6–128.3 / 126.4–127.8 |
| ping-pong 1c (ns per round trip) | 22.7 | 22.7 | level | 22.3–22.7 / 22.6–22.8 |
| ping-pong 2c | 153.9 | 155.9 | +1.3 %, overlapping | 151.1–157.5 / 154.4–159.9 |
| `ask` (ns per trip) | 46.41 | 46.39 | level | |
| `push` | 23.94 | 23.98 | level | |
| `pass-cost` k = 1 | 12.40 | 12.32 | level | |

Level, as the code says: on g++ the two helpers compile to `std::coroutine_handle<P>::from_promise`
and `h.promise()` exactly as before. The Windows half is
`../../desktop-b67osn6-win-msvc/qb-branch-fix-clang-cl-coroutine-promise/`; the clang-cl figures are
in `../../desktop-b67osn6-win-msvc/qb-46-clang-cl/`.
