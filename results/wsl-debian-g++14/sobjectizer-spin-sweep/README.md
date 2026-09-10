# sobjectizer-spin-sweep — SObjectizer's `combined_lock` spin budget, swept (Huly QB-47)

**A side experiment, not a table.** `tools/report.py` leaves this directory out (its documents
declare `savina/ping-pong` and `savina/counting`, which this directory is not named for) and
every document but the two profile controls carries `SWEEP DOCUMENT, NOT A TABLE CELL` as its
first caveat, written by the adapter itself. The reading is in `docs/TUNING.md` §1.2; this
directory is the evidence for it, the symmetric of `caf-spin-sweep/`.

WSL2 Debian 13 / g++ 14.2 (Linux 6.6), i9-12900K, its own quiet session (no Windows bench), 2026-09-09 13:17–13:19 UTC. Binaries: `~/qvo/linux/bin/qvo-sobjectizer-savina-{ping-pong,counting}` (SObjectizer 5.8.5.1, the harness at qb-vs-others `b693a21` plus the override). Driver: `tools/so-sweep.sh`, which sets `QVO_SO_SPIN_WAIT_US` (the combined lock's
waiting time in microseconds, 0 = `simple_lock_factory`) and runs the same harness command —
`--repetitions 7 --warmup 2 --cpus 0,2 --param messages=1000000 --param cores=2 --param wait=1` —
once per budget, the adapter's own profile (unset, 10 s) first and last; `sweep.log` is its
stdout, one line per run (p50 / min / max per message, and whether the document carries the
sweep caveat).

| file | `QVO_SO_SPIN_WAIT_US` | what |
|---|---:|---|
| `<shape>-profile-10s.json` | unset | the adapter's profile, 10 s — the published `2c-spin` configuration, measured first (ping-pong 631 [574, 693] ns) |
| `<shape>-simple-lock.json` | 0 | `simple_lock_factory` under wait=1: the `2c-park` configuration in the spin column, the lower bound |
| `<shape>-wait1us.json` … `wait100ms.json` | 1, 10, 100, 1 000, 10 000, 100 000 | the budget in microseconds; 1 000 is SObjectizer's own default |
| `<shape>-wait10s-b.json` | 10 000 000 | the profile's value through the override, a check that the override path costs nothing |
| `<shape>-profile-10s-b.json` | unset | the profile again, last (ping-pong 608 [561, 655] ns): the drift control |

The finding, both hosts: no budget beats the profile — everything from 100 µs up (10 µs up here on
Linux) reads inside the profile's own launch spread, SObjectizer's 1 ms default included; a budget
under the hop time is the park cell in disguise; and `counting` never exercises the knob (a one-way
flood keeps the queue non-empty, so its lock never waits).
