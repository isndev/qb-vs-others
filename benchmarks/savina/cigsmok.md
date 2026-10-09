# savina/cigsmok

Savina's Cigarette Smokers, one of the "concurrency" benchmarks (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014; `CigaretteSmokerConfig.java` and
`CigaretteSmokerAkkaActorBenchmark.scala`). In the reference, an arbiter repeatedly chooses one of
`smokers` smokers and tells it to start smoking. The chosen smoker acknowledges at once, because the
ingredients are off the table, and then smokes, which is busy work. Only then does the arbiter
choose the next smoker. So exactly one round is outstanding at a time, and the smokes of earlier
rounds may still be running.

## What it measures, and what it does not

It measures **an arbiter's round trip — one decision, one request, one acknowledgement — with real
work running behind it**. Each round sends one `StartSmoking` to the chosen smoker. The chosen
smoker answers `StartedSmoking` *before* it smokes, which is the reference's order of statements and
the reason the shape is interesting. A runtime that delivers the acknowledgement while the smoke
runs lets the arbiter's next decision, and the next smoke, run beside it. A runtime that delivers it
after the smoke serialises the two.

The busy work is the harness's `qvo::spin_work`, the same object code for every framework, so what
differs between the frameworks is:

- the cost of a round trip;
- where the chosen smoker runs relative to the arbiter;
- when the acknowledgement becomes visible.

Two smokes on one thread always run one after the other. Two on two threads may overlap. How often
they do is decided by placement and scheduling, and that is part of what the cell measures.

It does not measure fan-out. The arbiter sends to one smoker per round, never to all of them. The
only all-smokers step is the `Exit` at the end, 200 messages per repetition against two per round.
It does not measure contention either. Exactly one round is outstanding, so the arbiter's mailbox
never holds more than one acknowledgement in a correct run.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `rounds` | 1 000 | 1 000 (`R`) | no deviation; see "The window at Savina's rounds" |
| `smokers` | 200 | 200 (`S`) | no deviation; every smoker is created with the arbiter, before the window |
| `smoke` | 1 000 | hard-coded 1 000 | no deviation; a smoke is `uniform[0, smoke) + 10` busy-work iterations. Declared so `smoke=1` runs the coordination alone |
| `cores` | 2 | n/a | the arbiter on core 0 and smoker `j` on core `(j + 1) % cores`, for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

### Deviation: the arbiter's choices are drawn from `qvo::mix`, not from Savina's generator

**How Savina draws them.** Savina's arbiter already draws deterministically: `PseudoRandom` is a
16-bit linear congruential generator, `value = (value × 1309 + 13849) & 65535`, seeded with
`R × S = 200 000`. The arbiter calls it twice per round: once for the smoker,
`|nextInt()| % S`, and once for the period, `nextInt(1000) + 10`.

**The defect.** The generator's low bit alternates, and the two draws of a round are consecutive
outputs, so every smoker draw has the same parity. Replaying the generator for the default
parameters (`R = 1 000`, `S = 200`) shows that:

- every smoker draw is odd, so **only the 100 odd-numbered smokers of 200 are ever chosen**;
- every period draw is even, `10..1008`, with a mean of 494.

With this repository's static placement (smoker `j` on core `(j + 1) % 2`), the odd-numbered smokers
are exactly the ones on core 0, the arbiter's core. So the reference's choices would put every
smoke of the two-core cell on the arbiter's own core. The cell would then measure a quirk of a 16-bit
generator, not the shape.

**What this repository does instead.** The smoker and the period of round `r` are drawn from
`qvo::mix` over `r`, as uniform over `[0, smokers)` and `[10, smoke + 10)` as the reference meant
them to be. The arbiter draws them and carries them in `StartSmoking`, as Savina's arbiter carries
`busyWaitPeriod`. At the defaults:

- 199 of the 200 smokers are chosen;
- 499 rounds go to core 1 and 501 to core 0;
- the smokes total 520 076 iterations, against the reference's 494 000.

The draws depend on the round alone, so the sequence is the same in every framework and every
interleaving, which the checksum needs.

### The run ends with reports, not with termination detection

Savina's smokers exit on `ExitMessage`, the arbiter exits after sending them, and the benchmark
ends when the actor system observes that every actor has terminated. Here each smoker answers
`Exit` with a `Report` carrying its share of the checksum, and the window closes on the arbiter's
`smokers`-th report. These are the same `S` messages in, plus `S` messages back instead of a
runtime's termination protocol. They are counted, and they are how every smoke's result reaches the
checksum.

### The window at Savina's rounds

Savina's 1 000 rounds are 520 076 busy-work iterations in all, a short window for a native binary.
`ping-pong.md` records what that does: Savina's 40 000 round trips, about 10 ms, had a run-to-run
IQR near 20 %, wider than any difference between frameworks, so that page raised the count.

`rounds` stays at Savina's value here until the quiet-host measurement shows the window and its
spread. If that spread hides the differences between frameworks, `rounds` is raised the same way,
and this section will carry the measured figures and the new default. Pass `--param rounds=1000` to
reproduce Savina's count either way.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | everything on VirtualCore 0 | the arbiter on VirtualCore 0 and smoker `j` on core `(j + 1) % 2`, all placed before start with `Main::addActor`: half the smokes run beside the arbiter and overlap its next decision, and half share its core and hold it until they end |
| CAF | `max-threads=1` | `=2`, both pinned. The arbiter spawns its smokers while it initialises, as the reference does, and the work-stealing pool places every actor |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`. The arbiter and its smokers are one coop, registered before the window |
| floor | one thread | two pinned threads: the arbiter on thread 0 and smoker `j` on thread `(j + 1) % 2`. This is qb's placement, so the floor bounds the placing frameworks and not the pools |

In each framework the acknowledgement leaves the smoker before its smoke:

- **qb**: the smoker `reply()`s the `StartSmoking` it received, which goes through `send<>` and is
  published into the arbiter's core at once. A `push<>` would be published by the pass's flush,
  after the smoke.
- **CAF**: a mail makes the arbiter runnable. The pool prepends it to the smoker's own worker
  (`worker::delay`), so the arbiter runs after the smoke on that thread unless the other worker
  steals it.
- **SObjectizer**: the send is queued at the arbiter at once. The pool decides when the arbiter
  runs.
- **The floor**: the smoker pushes the acknowledgement into the arbiter's ring before it smokes.

## The verified answer

Every round `r` has one designated smoker `s(r)` and one period `p(r)`. The checksum names who
smoked which round:

- a smoker adds, for every `StartSmoking` it receives, its **own** identity times a key of the round,
  plus the smoke itself: `spin_work` seeded by the round, run for the period the message carried;
- the arbiter adds, for every `StartedSmoking` it receives, the identity it names times a second key
  of the round;
- every smoker adds its identity times an exit key for every `Exit`, and reports its share once.
  This makes every report a non-zero share named by its smoker. A smoker the draw never chose
  smokes nothing, and without the exit key a doubled report from it would add zero.

The expected value is that sum over `r` of the designated smoker's terms, plus the exit terms of all
smokers. It is the same for every interleaving.

How each kind of fault moves the sum:

- **A misrouted `StartSmoking`** is credited to the smoker that received it, and its identity is not
  the expected one.
- **A doubled `StartSmoking`** is smoked twice.
- **A doubled `StartedSmoking`** adds an acknowledgement no round expects. The arbiter folds every
  acknowledgement into the sum, but plays the next round only on the one that names the outstanding
  round. A duplicate therefore completes the run with a wrong sum (exit 1). It does not start a
  second round in parallel, and it does not hang.
- **A doubled `Report`** adds one smoker's share twice. The arbiter ends on the `smokers`-th report,
  so another share is missing as well.
- **A doubled `Exit`** to a smoker that is still running makes it report twice, with the same effect
  as a doubled `Report`. A smoker that has ended (qb, CAF) receives nothing, as an exited actor
  receives nothing in the reference.

The message count is asserted too: `2 × rounds + 2 × smokers + 1`. That is the arbiter's `Start`,
the two messages of every round, and an `Exit` and a `Report` per smoker, counted at the receivers.
Each smoker's handshake before the window is not counted.

**A lost message cannot be shown as a wrong sum here.** The protocol waits for every
acknowledgement, every `Exit` and every `Report`, as the reference waits for its acknowledgements
and its termination, so losing one of them stalls the run. So does an `Exit` delivered to the wrong
smoker, which leaves one smoker without its own. A drop is therefore proved on the smoke:
a smoker that receives a `StartSmoking`, acknowledges it and skips the smoke and its term fails the
run.

Nothing in this shape depends on the interleaving except time. The round's smoker and period are
the draw's and every count is fixed, so the documents carry no `observed` field and the spec
declares no `observed_at_least`.

## The measured window

**When it opens.** Every smoker tells the arbiter it is up, outside the window. On the last of those
messages the arbiter starts the watch and sends itself the reference's `StartMessage`.

**When it closes.** On the arbiter's `smokers`-th `Report`. That report comes after the last round's
acknowledgement and after every smoker has finished every smoke it was given, because a smoker
answers `Exit` only after the smoke in front of it.

**What is inside and outside.** Every round, every smoke and the `Exit`/`Report` exchange are inside
the window. The creation of the arbiter and of the 200 smokers is outside it, as in every shape here
but `fib` and `barber`. Savina times its whole iteration, actor system included, and this repository
times from the first workload message (`harness.h`). The reference creates the smokers in the
arbiter's constructor, before its `StartMessage`, so they fall on the same side of that line here.
The floor's second thread is created just before the window.
