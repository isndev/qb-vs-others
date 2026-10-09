# savina/philosophers

Savina's Dining Philosophers, from the "concurrency" group (Imam & Sarkar, *Savina — An Actor
Benchmark Suite*, AGERE 2014), in the form its actor variants write it
(`edu.rice.habanero.benchmarks.philosopher.PhilosopherAkkaActorBenchmark` and siblings):
`philosophers` philosophers around as many forks, ONE arbitrator that owns every fork, and each
philosopher eating `rounds` meals. A philosopher asks the arbitrator for its two forks; the
arbitrator grants them if both are free and refuses otherwise; a refused philosopher asks again at
once; a fed one releases its forks and asks for the next meal.

## What it measures, and what it does not

It measures **coordination through a single arbiter under contention**: every request of every
philosopher lands in the arbitrator's mailbox and is answered, and a large share of the answers
are refusals that come to nothing but another request. That is the shape chameneos has (one hot
mailbox written by everyone, answering everyone) with a refusal path added: what a framework
pays per request when most requests lose a race. A meal costs one Start, one granted request, one
Eat, one Done — and however many refused request/answer pairs the interleaving produced first.

Those refusals are the benchmark's one non-deterministic quantity: how often a philosopher's
request arrives while a neighbour holds a fork depends on how the framework schedules the
arbitrator against the philosophers. Savina reports it as "Num retries" and asserts nothing about
it, and neither does this repository (see *The verified answer*): a framework whose scheduling
makes the philosophers collide more often does more work in the same cell, and that is the
benchmark as Savina defines it. What this repository adds is that the amount is never hidden:
every adapter REPORTS it (see *What is observed, not asserted*), and the table prints it in a
sub-row under the cell, so two cells that refused very different numbers of requests are never
compared silently. It does not measure fairness either — the arbitrator is a
first-come rule, not a queue — nor real blocking: nobody waits, a refused philosopher retries.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `philosophers` | 20 | 20 (`N`) | no deviation |
| `rounds` | 10 000 | 10 000 (`M`) | no deviation; 200 000 meals, 800 020 asserted messages per repetition |
| `cores` | 2 | n/a | arbitrator on core 0; philosopher i on core `1 + i % (cores-1)` for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Savina's third parameter, `C` (channels, default 1), is read only by its Habanero **selector**
variants; the actor variants this page follows ignore it, and so does this repository.

### The one deviation in the protocol

Savina's arbitrator keeps a boolean per fork. Here it keeps the **owner** of each fork — the same
one comparison per fork on a request, one store per fork on a grant and on a release — so that a
release arriving for forks the sender does not hold (two philosophers eating with a shared fork,
which only a duplicated or misdelivered message can cause) is detected and makes the run fail
verification. No message, no branch on the request path and no answer is added.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the arbitrator and all 20 philosophers on VirtualCore 0; ONE `Request` event per philosopher carries the conversation, mutated Hungry → Eat/Denied at the arbitrator and Denied → Hungry / Eat → Done at the philosopher, each time `reply()`ed — Start (to itself) and Exit are `push<>` | arbitrator alone on VirtualCore 0, every philosopher on core 1: **every request and every answer crosses a core**, and the arbitrator's mailbox is one cross-core pipe with 20 writers on the far side |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places the arbitrator and the philosophers — whether the arbitrator's mailbox is written cross-core is the scheduler's decision, and it can change from run to run |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; same remark — the pool decides which thread runs the arbitrator |
| floor | one thread with the arbitrator and the philosophers in its own ring | two pinned threads; the arbitrator is thread 0 and every philosopher thread 1 — the same placement qb gets (with more cores, philosopher i on thread `1 + i % (cores-1)`, as qb), but the 20 philosophers share ONE SPSC ring into the arbitrator, so the 20-writer fan-in is engineered out |

**The floor bounds the one-core cell only.** At `cores=2` it hands every message to the other
thread with one ring push of its own, while qb publishes a pass's events to the peer core in one
batched flush: qb's two-core cell runs UNDER this floor, and the floor there is a reference for the
per-message cost of a hand-off, not a bound on what a framework can do.

## The verified answer

Every message the protocol FIXES is folded once, by its receiver, as `term(kind, philosopher,
round)` — a wrapping sum of `mix(mix(kind << 56 | philosopher) + round)`, where `round` is the
receiver's own 1-based count of that kind of message for that philosopher:

- a philosopher folds its r-th **Start** and its r-th **Eat**, and carries the sum and its message
  count in its Exit;
- the arbitrator folds its r-th **granted** Hungry and its r-th **Done** from each philosopher,
  and each **Exit** with `round` = the number of Dones it has received from that philosopher then.

So `expected = Σ_i [ Σ_{r=1..M} (start + grant + eat + done)(i, r) + exit(i, M) ]`, independent of
who was refused how often. A grant answered twice, a meal delivered twice, a Done lost or
duplicated, an Exit that overtakes its philosopher's last Done, a philosopher that stops early or
eats once too often, or two philosophers fed from one fork changes the total.
`expected_messages = 4·N·M + N` — N·M Starts, N·M granted requests, N·M Eats, N·M Dones and N
Exits — is counted at the receivers and asserted alongside. The refused pairs (a refused request
and its Denied, or a Denied and the retry it triggers) are in neither: their number is the
scheduler's, and it is reported instead (next section).

## What is observed, not asserted

FAIRNESS.md section 0: work whose amount depends on the interleaving is reported beside the cell,
never silently compared. Every adapter — the three frameworks and the floor — counts at the
arbitrator the Hungry requests it REFUSED and returns the count as the observation `refused`
(`qvo::Answer::observed`, the name is `kObservedRefused` in the spec). It is Savina's "Num
retries": each refusal is answered with Denied and re-sent at once, so the refused count is also
the retry count. Every measured repetition's value is written into the result document
(`"observed": {"refused": {samples, min, p50, max}}`), and `tools/report.py` prints the median and
the range in a sub-row under the cell. No lower bound is declared (`observed_at_least`): a run in
which no request ever loses a race is a correct run, so zero is a legal value and nothing is
asserted about it.

Every message here is needed by a later step — a philosopher has one message of its cycle in
flight, or two right after a meal (its Done to the arbitrator and its Start to itself) — so a
message lost in the middle of a run does not produce a wrong answer, it stalls the run: a lost
Hungry, Denied, Eat or Start stops its philosopher; a lost Done leaves two forks taken for good,
so that philosopher and both neighbours are refused from then on; a lost Exit leaves the
arbitrator one short. A stalled run emits no timing (`tools/run.py`'s timeout). Only a loss that
stops nobody — a philosopher's last Done once its neighbours have finished — can complete, and
the checksum rejects it.

Termination does not depend on luck: a fork is taken only by a granted philosopher whose Done is
on its way, so whenever no Done is pending every fork is free and the next request is granted;
and each philosopher eats exactly `rounds` meals, so one refused for as long as its neighbours eat
is granted at the latest once they have finished.

## The measured window

Opens when the arbitrator, having seen all `N` philosophers report ready, sends the `N` first
Starts — every thread is running and every philosopher has been scheduled once — and closes when
the arbitrator has received the `N`-th Exit, and every Done is in by then: each philosopher sends
its Exit after the Done of its last meal, and the Exit cannot overtake it. In CAF, SObjectizer and
the floor both are messages from the same sender to the same mailbox, delivered in sending order.
In qb they take two transports — the Done is the Eat `reply()`ed, the Exit a `push<>` — and the
order holds on both: `reply()` either publishes the Done into the arbitrator's ring at once,
ahead of anything the Exit's later flush appends there, or, when that ring is full or the
arbitrator shares the core, appends it to the same outbound pipe the Exit is appended to after
it. An Exit that did overtake would be folded with one Done too few and fail verification. The
floor's second thread is created just before the window; its start-up, once, is below resolution
at 800 020 messages.
