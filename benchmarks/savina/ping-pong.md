# savina/ping-pong

Savina benchmark 1 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014). Two actors exchange a message N times.

## What it measures, and what it does not

It measures **one round trip through a framework's message path with nothing else happening**: no
contention, no parallelism, no actor creation, no fan-in. That makes it the cleanest possible
measurement of per-message cost and the **least representative workload in the suite** — a
framework that wins here has shown one thing about itself, not a ranking.

The single most instructive result from it in this repository is that every framework measured is
**faster on one core than on two**. There is no parallelism to exploit, so the second core only
adds a cache-line crossing per hop.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `messages` | 1 000 000 | 40 000 | **deviation — see below** |
| `cores` | 2 | n/a | Savina does not parameterise this; the frameworks differ too much not to |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation from Savina's message count, and why

Savina's default is 40 000. Measured here, 40 000 round trips complete in about 10 ms with a
run-to-run IQR near 20 % — wider than any framework difference this benchmark could report. The
measurement would have been noise with a ranking printed on top of it.

Savina's figure is calibrated for a JVM, where 40 000 iterations is roughly what it takes to reach
steady-state JIT compilation. For a native binary that reasoning does not apply and the number is
simply too small. 1 000 000 is used instead — which is also what qb's own ping-pong benchmark uses.

Pass `--param messages=40000` to reproduce Savina's figure exactly.

## How `cores` maps to each framework

`cores` is a **CPU budget**, identical for every framework. How each spends it differs, and the
difference is architectural rather than a tuning choice:

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | both actors on VirtualCore 0 | one actor per VirtualCore, each pinned to one CPU |
| CAF | `caf.scheduler.max-threads=1` | `=2`, workers pinned via `thread_hook`; CAF still steals between them |
| SObjectizer | `one_thread` dispatcher | `active_obj` dispatcher, work threads pinned via a custom factory |
| floor | one thread, message through a real queue in both directions | two pinned threads, two SPSC rings |

qb is the only one that *places* an actor. CAF and SObjectizer are given the same CPUs and the
same worker count, but their schedulers decide what runs where. That is a genuine architectural
difference in qb's favour on this workload, and it is recorded as a caveat on every qb result
rather than presented as a tuning win.

## The verified answer

Each round trip carries a sequence number counting down from `messages - 1` to 0. The ping actor
accumulates `mix(seq)` on every reply, as a **wrapping sum**.

A sum rather than an XOR, deliberately: XOR is self-cancelling, so a message delivered twice would
leave the checksum unchanged. A sum is sensitive to drops *and* duplicates while staying
insensitive to arrival order, which is the property the fan-in benchmarks in this suite will need
from the same reduction.

The expected value is computed by plain arithmetic in
`benchmarks/specs/qvospec/savina/ping-pong.h`, with no framework linked, and
`expected_messages = 2 x messages` is asserted alongside it so that a framework cannot reach the
right checksum by doing a different amount of work.

## The measured window

Opens when the ping actor injects the first ball into an already-running system, closes when it
observes the last reply. Framework construction, actor creation and shutdown are outside it and
reported separately as `outside_window_ns`.

Every implementation performs one **handshake round trip before the window opens**, so that both
actors have been scheduled at least once and their worker threads are warm. qb gets this from its
`require<PongActor>()` bootstrap; CAF and SObjectizer do it explicitly with a hello/ack pair. The
window therefore contains message passing rather than first-touch scheduling, in every framework.
