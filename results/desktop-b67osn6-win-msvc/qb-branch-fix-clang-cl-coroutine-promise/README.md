# qb branch `fix/clang-cl-coroutine-promise` — Windows 11 / MSVC 19.51 (neutrality)

The MSVC half of the neutrality measurement for Huly **QB-200** / **QB-201** (see the WSL2 half for
what the branch does), measured 2026-09-09 **00:03–00:05 UTC** in one quiet session (no build during
the points, Docker Desktop quit, the WSL2 side idle at 0.23). Control = `build/ab200-ctl` against
`D:\repo\qb-dev\qb` (`develop` `381e4995`), candidate = `build/ab200-cand` against the worktree
`D:\repo\qb-200` at `ce39584a`, both asserted by SHA and by marker (`promise_access.h` 0 / 1,
`promise_of` 0 / 8 in `task.h`); `/O2 /Ob2 /DNDEBUG`, CPUs 0,2, eight interleaved rounds, five
alternations of 1.5 s per probe. `cl` takes the standard calls here — clang-cl's path is
`../qb-46-clang-cl/`.

| file | what |
|---|---|
| `census/` | bank-transaction 1c, ping-pong 1c / 2c, fib 1c × cand/ctl × 8 rounds |
| `probe.txt` | `ask-cost` `push` / `ask` (500 ms timeout), `pass-cost` k = 1 — cand / ctl × 5 |

None of it is merged into the published tables.

| cell / probe | control | candidate | Δ | quartiles ctl / cand |
|---|---:|---:|---:|---|
| bank-transaction 1c | 231.6 | 231.3 | level | 229.7–234.5 / 228.1–231.8 |
| fib 1c | 180.7 | 181.2 | level | 180.2–181.2 / 180.0–183.5 |
| ping-pong 1c | 31.1 | 31.3 | level | 30.7–31.6 / 30.9–32.0 |
| ping-pong 2c | 194.0 | 193.0 | level | 187.0–195.4 / 188.3–194.1 |
| `ask` with a 500 ms timeout | 89.85 | 90.64 | +0.9 % | |
| `push` | 31.34 | 31.41 | level | |
| `pass-cost` k = 1 | 15.35 | 15.38 | level | |
