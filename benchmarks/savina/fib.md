# savina/fib

Savina benchmark 6 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014) — the recursive Fibonacci tree, where **every node of the recursion is an actor that
is created, asked once, answers once and dies**. It is the actor-creation benchmark of the suite,
the one `fork-join.md` names as not implemented before this page existed.

## What it measures, and what it does not

It measures **actor creation and destruction inside the window**, and nothing else is hot: a node
receives one request, spawns two children, sends them one request each, receives two responses,
sends one response up and terminates. For `n=23` that is 57 312 actors born and dead inside one
repetition — 2 × 57 312 requests and responses — with at most a few hundred alive at once on a
depth-first scheduler and tens of thousands on a breadth-first one. What a framework pays per
actor here is its registry, its mailbox allocation, its subscription and its teardown; message
dispatch is the same two pushes per actor for everyone and is measured elsewhere (`counting`,
`big`).

It does **not** measure balancing at `cores=2` for the frameworks that place: a qb child lives on
its parent's core (see the mapping table), so qb's two sub-trees are as unequal as `fib(n-1)` and
`fib(n-2)` — 62 / 38. The pools can steal; the cell records that asymmetry in the caveats rather
than hiding it. It does not measure deep recursion either: the tree is walked by messages, never by
the stack, so nothing here can overflow.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `n` | 23 | 25 | **deviation — see below**; 57 312 actors, 114 624 messages per repetition |
| `cores` | 2 | n/a | the root on core 0; the two seed sub-trees on cores 0 and 1 % cores for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's `n`, and why

Savina uses `n=25`: 150 049 actors per run. A qb `ActorId` carries a **16-bit** per-core index
(`ServiceIdPool`), so at most 65 534 actors can be alive on one VirtualCore at once — and a
breadth-first drain of the seed's pipe brings the whole sub-tree alive before the first leaf
answers, since a child is spawned in its parent's handler and asked before the parent returns.
`n=24` puts 92 735 nodes on the only core at `cores=1`, above the cap whenever the drain is
breadth-first; `n=23` puts 57 313 there and fits in every drain order, so it is the largest `n`
every framework can run in every configuration without depending on scheduling luck. The cap is recorded as a qb defect in the report (a 3.2
ActorId is 32 bits with a 16-bit index; widening it is an ABI change filed for 4.0 next to the
32-byte bucket) and the deviation is stated on every document.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the root and every node on VirtualCore 0; a spawn is `addRefActor` — same-core, synchronous `onInit`, id from the core's pool | root on VirtualCore 0, seed 0 on core 0, seed 1 on core 1; **a child always lives on its parent's core** (qb has no cross-core dynamic spawn), so core 0 runs `fib(22)` and core 1 `fib(21)` and nothing balances the 62 / 38 split |
| CAF | `max-threads=1` | `=2`, both pinned; every child is `self->spawn<>` and the work-stealing pool places it — a runnable child is stolen by the idle thread, which is the case stealing exists for |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; each node registers one **child coop per child** and deregisters its own when it answers (two siblings cannot share one: an agent ends by deregistering its coop, and the first response would take the other down — measured as SObjectizer error 185) — one coop registration and one deregistration per node is what the SObjectizer lifecycle costs |
| floor | one thread; a node is a heap slot in the thread's table | two pinned threads; a child is allocated in its parent's thread's table — the same static placement qb has, so the floor bounds the placing frameworks and not the pools |

## The verified answer

A leaf answers `value=1, chk=mix(1)`; a node answers `value = a + b` and `chk = mix(value) +
chk_a + chk_b`, as **wrapping sums**, and the root reports `chk(n-1) + chk(n-2)`. A response
delivered to the wrong parent, dropped, delivered twice or computed by a node that did not wait for
both children changes the total. Every response also carries the message count of its sub-tree, so
`expected_messages = 2 × (nodes(n) − 1)` — one request and one response per non-root node — is
asserted alongside.

## The measured window

Opens when the root sends the two seed requests into an already-running system — both seeds have
reported ready, so every thread is up and both are scheduled — and closes when the root has
received the second response. Every spawn, every `onInit` / `so_define_agent` / behavior
construction and every termination of the 57 312 nodes is inside the window: that is the point.
The floor's two seeds are allocated before the window, once, which is below resolution against
57 310 nodes allocated inside it.

## What the shipped qb cell measures, and why it is a logging figure

The published `savina-fib/` documents render **qb 3.1.0** at 159 / 205 ms per repetition on
WSL2 g++-14 and 459 / 558 ms on Windows MSVC (2c-spin / 1c-spin), against CAF's 39 / 69 and
53 / 85. That is not the actor registry. At its shipped default `QB_WITH_LOGGING=ON`, a release
build logs at INFO, and 3.1.0 logs **9 lines per actor lifetime** — `registerEvent` × 7 (the
five default subscriptions and this benchmark's two) plus `New` and `Delete` — which for 57 312
actors is **515 819 lines and 60.9 MB of `qb.1.log` per repetition**, measured by counting the
file. nanolog's producer side (format, timestamp, push to the writer's queue) runs on the actor's
core inside the window, and its writer thread is started before `main()` and shares the pinned
CPU set. The 3.2 line (`5665b6f8` on `perf/dense-table-growth`) logs those lines at VERBOSE, and
its fib cell in the same session is 7.6 / 11.9 ms (WSL2) and 10.5 / 17.2 ms (Windows) —
`results/<host>/qb-branch-perf-dense-table-growth/`.

The figure is kept and published as measured because it is what a 3.1.0 built at its defaults
does, which is the only qb a user of 3.1.0 has. Two things follow. The generic caveat every qb
document carried until 2026-09-06 — "its logger writes at startup, outside the measured window"
— was written for the five static shapes and is false here; the sixteen shipped documents keep
that sentence as their provenance and `frameworks/qb/qb_support.h` says the true thing for every
run after. And a 3.1.0 fib driven from a 9p mount (WSL2's `/mnt/<drive>`) takes minutes to EXIT
per repetition, draining that log line by line through `p9_client_rpc` — the shipped WSL2 cells
were driven from an ext4 cwd (`/tmp/qvo-cwd`) for that reason, and the delay is a 3.1.0
characteristic, not a runtime hang (gdb: `main` in `exit()` → `~NanoLogger` → `thread::join`).
