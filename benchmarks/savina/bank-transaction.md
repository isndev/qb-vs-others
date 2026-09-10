# savina/bank-transaction

Savina benchmark "Bank Transaction" (Imam & Sarkar, *Savina — An Actor Benchmark Suite*, AGERE
2014), the last of the "concurrency" group — one teller, a thousand accounts, fifty thousand
transfers, and **every transfer is a request that waits for a reply**. It is the first shape in
this repository that blocks on an answer: the seven before it push and forget.

## What it measures, and what it does not

It measures **what a framework's request-with-continuation costs** — `qb::ask` awaited from a
coroutine, CAF's `request().then()`, and for SObjectizer, which has no such primitive since 5.6
removed `request_value`, the same state kept by hand — on top of a thousand-writer fan-in of
acknowledgements into the teller. A transfer is four messages: the teller tells the SOURCE account
"move `amount` to `dst`"; the source debits itself and asks the destination to deposit; the
destination deposits and replies; the source acknowledges the teller. While a source waits for its
reply, further transfers addressed to it are held and started one at a time, in order, as each
reply arrives; a deposit is always served immediately. What each framework pays per transfer is
its request frame (or the absence of one), the routing of the reply back to the waiting
continuation, and the resume — on the same dispatch path `counting` and `big` measure alone.

It does **not** measure balancing: with `cores=2` the placing frameworks put the teller on core 0
and account *a* on core (1 + *a*) % cores, so about half the transfers, half the deposits and half
the acknowledgements cross a core and nothing rebalances that; the pools place freely and steal.
It does not measure contention on the balances either — an account is one actor and its balance
is touched by nobody else, which is the whole point of writing a bank as actors. And it does not
measure actor creation: the thousand accounts are built before the window opens (`fib` is the
creation shape).

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `accounts` | 1000 | 1000 | no deviation |
| `transactions` | 50 000 | 50 000 | no deviation; 200 000 messages per repetition inside the window, 2 000 more for the exit |
| `cores` | 2 | n/a | the teller on core 0; account *a* on core (1 + *a*) % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### The one deviation: integer amounts

Savina draws a `double` amount in [0, 1000). This suite draws an **integer in [1, 1000]** from the
same deterministic mix (`transfer(i, accounts)` in the spec header) so that a balance is exact
arithmetic and a checksum over it means something; a double sum's last bits depend on the order
the transfers complete in, which is the scheduler's business and differs run to run. The source
is drawn from the first 80 % of the accounts and the destination is strictly above it (Savina's
own `loopId == 0 → 1`), which is what keeps a chain of waiting accounts from ever closing on
itself.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | teller and every account on VirtualCore 0 | teller on VirtualCore 0, account *a* on core (1 + *a*) % 2 — fixed before start. The deposit is `co_await qb::ask<Deposit>(ctx, dst, 0, amount, txn)` from a coroutine the source account spawns: one live frame per account at most, which drains the account's deque of held transfers and returns when it is empty, so a busy account keeps one frame and an idle one holds none. The frame captures a `shared_ptr` to the account's state, never `this`. On qb 3.1.0, which has no emplace `ask`, the adapter selects the by-value `qb::ask(ctx, dst, Deposit{…}, 0)` at compile time — each version runs the ask it recommends |
| CAF | `max-threads=1` | `=2`, both pinned; the pool places the teller and the accounts and steals. The deposit is `mail(put_atom, amount, txn).request(dst, infinite).then(…)` — `then`, not `await`, so the account keeps serving deposits while its request is in flight, as the spec requires; the destination's handler RETURNS the value and CAF routes it back by response id. The continuation is a heap-allocated behavior per request |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads, `fifo_t::individual`; the dispatcher places. No request/reply primitive: the deposit is two plain messages on the accounts' direct mboxes (`msg_deposit` there, `msg_reply` straight back) and the continuation is the account's own `busy` flag and deque — exactly the state the other two primitives keep for the caller. SObjectizer pays its dispatch and saves the frame |
| floor | one thread, teller and accounts in one ring | two pinned threads, the same static placement as qb; accounts are vector slots with no mailbox, a deposit is one ring push and the reply one push back, the continuation a flag and a deque per slot. The per-account mailbox, the 1000-writer fan-in and the per-transfer frame are engineered out — that is what the floor is for |

## The verified answer

The teller folds `mix(i)` for every acknowledgement it receives — an acknowledgement carries the
index of the transfer it completes — and `account_key(a, balance)` for every balance reported at
exit, as wrapping sums. The final balance of every account is fixed by the transfer set alone,
whatever order the transfers complete in, so the expected total is plain arithmetic in the spec
header with no framework linked. A transfer acknowledged twice, never acknowledged, a deposit that
landed on the wrong account, a reply routed to the wrong continuation, or an account that never
reported changes the total. `expected_messages = 4n + 2a` — transfer, deposit, reply and
acknowledgement per transfer, exit and report per account — is counted at the receivers and
asserted alongside.

## The measured window

Opens when the teller sends the 50 000 transfers into an already-running system — every account
has reported ready, so every thread is up — and closes when the teller has received the 50 000th
acknowledgement. Every request frame, every reply routing and every resume is inside it. The
thousand accounts are created before the window and reported after it; the exit and the balance
reports are outside the window and inside the message count.

## What qb 3.1.0 measures here, and what the shape found

The published `savina-bank-transaction/` documents render **qb 3.1.0** at 14.71 / 14.58 / 9.21 /
9.31 ms per repetition on WSL2 g++-14 and 25.48 / 25.45 / 29.20 / 33.45 ms on Windows MSVC (1c
spin / 1c park / 2c spin / 2c park), against CAF's 41.5 / 43.0 / 36.6 / 36.6 and 57.8 / 58.0 /
57.5 / 58.1, SObjectizer's 19.5 / 20.5 / 25.5 / 27.7 and 29.6 / 31.5 / 38.0 / 44.4, and a floor of
1.10 / 2.59 / 6.30 / 4.63 and 3.47 / 3.92 / 32.2 / 7.25. Per transfer on one core that is 294 ns
for qb, 391 for SObjectizer, 830 for CAF and 22 for a ring push with no frame.

Two things about the 3.1.0 figure are known and recorded. It is **not** a logging figure this
time: a control built with `-DQB_WITH_LOGGING=OFF` from the same 3.1.0 tree measures 15.80 /
16.28 / 8.99 / 9.75 in the same session
(`results/wsl-debian-g++14/qb-branch-perf-coro-scope-local-refcount/grid-shipped-3.1.0-nolog/`) —
no faster, and `qb.1.log` is 1.5 KB against ~10 000 lines per repetition with logging on, all of
them the accounts' construction and teardown, outside the window. And on Windows three of the
`2c` cells that cross a core per transfer are WIDE rather than settled — the floor's 2c-spin
spans 20.0–62.2 ms over nine repetitions (IQR 29.5), shipped qb's 2c-spin 24.3–49.6 (IQR 13.0)
and its 2c-park 19.2–42.1 (IQR 18.5) — where every 1c cell and both hosts' other cells sit
inside a tenth of that. The report's bimodality rule
(an upper cluster starting at twice the lower's end) does not fire on them; they are quoted with
their spread, never as a median alone. The qb `develop` cells of the same session, below, do not
show it (7.57 / 7.56 ms, IQR 0.2).

The shape was written on 2026-09-06 against qb `develop` (`9d4aa94c`, every axis of
`docs/TUNING.md` §7–§11 already in), and its one-core `perf` profile read as a list of things that
should not be on a request path: a `shared_ptr` copied on every ask and every spawn (29 % of the
core, 24 % on one `lock xadd`), a `std::function` + vector push per ask for the cancellation
registration, a 64-byte request temporary moved three times and read back wide over narrow
stores, the same store-forwarding stall in `reply()`'s header writes, and an `alive = 0` byte
store into every arriving event before routing. Five defects, one commit (`fa1c5ce3`, Huly QB-42),
and removing the fifth exposed a sixth the store had been masking — a cross-core `forward()` of an
already-`reply()`ed event leaked a copy per relay. `docs/TUNING.md` §12 carries the chain. Measured
in ONE quiet session per host, three qb builds back to back beside the field
(`results/<host>/qb-branch-perf-coro-scope-local-refcount/`, 2026-09-07 02:21 UTC on WSL2, 02:24
on Windows), p50 ms as 1c spin / 1c park / 2c spin / 2c park:

| qb | WSL2 g++-14 (5 + 1) | Windows MSVC (9 + 2) |
|---|---|---|
| shipped **3.1.0** | 14.71 / 14.58 / 9.21 / 9.31 | 25.48 / 25.45 / 29.20 / 33.45 |
| `develop` `9d4aa94c`, the base | 9.40 / 9.44 / 5.09 / 5.02 | 13.65 / 13.86 / 7.97 / 7.78 |
| `develop` `fa1c5ce3`, the five fixes | **8.06 / 8.80 / 4.56 / 4.77** | **12.91 / 12.82 / 7.57 / 7.56** |

3.1.0 → `fa1c5ce3` is 1.8× / 2.0× on WSL2 and 2.0× / 3.9× on Windows (1c / 2c spin, the last
over the wide shipped cell); the five fixes alone are −14 % / −10 % on WSL2 and −5 % / −5 % on
Windows. Why the fixes buy g++ more than MSVC is not measured: the profile that found them was
taken on Linux and no Windows profile was. The 3.1.0 → base gap is the §7–§11 work landing on a request path for the
first time. The 1c-park candidate cell on WSL2 is the noisy one of the twelve (7.96–9.39, IQR
0.63); other launches of the same binary in the same session read 7.54–8.08. What remains on the
profile is the ask registry, a hash map keyed by correlation id, ≈10–12 % of the one-core cost —
recorded as the follow-up, not done in that commit.
