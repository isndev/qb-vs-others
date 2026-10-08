# savina/fork-join-create

Savina benchmark 4 of the "micro" group (Imam & Sarkar, *Savina — An Actor Benchmark Suite*,
AGERE 2014) — `fjcreate` in the suite's sources, the **actor-creation** fork-join: a creator
forks a burst of actors in one loop, **every one of which is created, sent one message, does one
job and terminates**. It is the twin of benchmark 5 ([`fork-join.md`](fork-join.md)), which
streams jobs at sixty workers that live for the whole run, where this one creates a fresh actor
per job; [`fib.md`](fib.md) is the suite's other creation benchmark, from a recursion rather than
a loop.

## What it measures, and what it does not

It measures **actor creation and destruction from a flat loop**: the creator's spawn rate, one
inbound message per actor, one answer, and the termination — `actors` times, 40 000 by default,
all inside the window. Where `savina/fib` creates its actors from a recursion (a node spawns two
children, waits, answers and dies, so a depth-first scheduler keeps a few hundred alive), this
one creates them from ONE loop per creator before any of them has run on a single-threaded
runtime, so the burst — tens of thousands of actors alive at once — is part of the shape. What a
framework pays per actor is its registry, its mailbox allocation, its subscription, the scheduling
of a new actor and its teardown; the per-actor message traffic is the same one job and one answer
for everyone.

It does **not** measure balancing at `cores=2` for the frameworks that place: a qb actor lives on
its creator's core (see the mapping table), so each core forks and runs exactly its own half. The
pools can steal; the cells record that in their caveats. It does not measure per-actor
computation either: at the default `work=0` an actor's job is one `mix()`.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `actors` | 40 000 | 40 000 (`N`) | no deviation; 40 000 actors, 80 000 messages + 2 per creator per repetition |
| `work` | 0 | one `sin(37.2)²` | **deviation — see below** |
| `cores` | 2 | n/a | worker budget, and the number of creators — **deviation, see below** |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

Savina's `C` ("num channels") is parsed by its configuration and read by no actor implementation
of the benchmark; it has no counterpart here.

### Deviation from Savina's per-actor work, and why

Savina's actor computes `sin(37.2)²` and throws if the result is not positive — a few nanoseconds
whose purpose in a JVM benchmark is to keep the JIT from deleting an actor whose result nobody
reads. Here every actor's job is observable through the checksum, so nothing can be deleted, and
the default is `work=0`: the cell reports what an actor costs to *create, deliver to and end*,
not that plus a computation. `--param work=N` puts a deterministic computation back
(`qvo::spin_work`, folded into the checksum so that a framework which skipped it would fail
verification). This is the deviation `fork-join.md` records for benchmark 5, for the same reason.

### Deviation from Savina's single creator, and why

Savina's creator is the JVM's **main thread**, outside the actor system: it calls `actorOf` and
sends the message `N` times, and the pool runs the actors. Here creation has to happen inside the
measured system, from actors — the harness times a running actor system, and a framework whose
only creation primitive is callable from an actor (qb's `addRefActor`) has no main-thread
equivalent once the engine is running. So the creators are actors, and there is **one per core of
the cell**: creator `s` forks the actors whose index `i` satisfies `i % cores == s`. With
`cores=1` that is one creator forking all 40 000 — Savina's shape exactly; with `cores=2` two
creators fork 20 000 each, in parallel, in every framework. One creator at `cores=2` was the
alternative, and it would have measured a framework's placement rather than its creation: qb
cannot create on a core other than the creator's, so its second core would have done nothing,
while the pools would have spread the actors' one message and kept the spawns serial.

### Deviation from Savina's end condition, and why

Savina's actors answer nobody and its driver waits for the actor system to terminate. Here every
actor answers its creator — the answer must be asserted (FAIRNESS.md §0) — and each creator, once
it has every answer of its share, sends one summary to the driver. That answer is the *join* the
benchmark's name promises, and it costs every framework the same one message per actor.

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the driver and the one creator on VirtualCore 0; a fork is `addRefActor<ForkActor>(id(), i, work)` — same-core, synchronous `onInit`, id from the core's pool — then `push<Job>` to the new id; the actor answers with one `push<Done>` and `kill()`s itself | creator 0 on VirtualCore 0 (with the driver), creator 1 on VirtualCore 1; **a forked actor always lives on its creator's core** (qb has no cross-core dynamic spawn), so each core forks and runs exactly 20 000 actors and nothing balances them |
| CAF | `max-threads=1` | `=2`, both pinned; the creators and every forked actor are placed by the work-stealing pool. A forked actor is a stateless `event_based_actor` (no state object) spawned with CAF's default options. `caf::lazy_init` — CAF's own option for an actor messaged right after its spawn, not scheduled until its first message arrives (`spawn_options.hpp`) — was tried as the possibly faster idiom (FAIRNESS.md §1.1) and showed no gain in an interleaved check at either core count, so the default runs |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; each forked actor is **its own child coop** of its creator and deregisters it after its one message — the same lifecycle `fib.md` measures. One coop holding a creator's whole share would register it in one transaction, but no agent in it could end after its message without ending all the others, so it is not this benchmark's actor |
| floor | one thread; a forked actor is a heap node in the thread's slot table | two pinned threads; creator `s` on thread `s`, its actors in that thread's table — the same static placement qb has, so the floor bounds the placing frameworks and not the pools |

## The verified answer

Actor `i` is created with its index `i` (a constructor argument) and is sent a job carrying `i`;
it answers its creator with `job_value(self, job, work) = mix((self << 32) | job)` — plus
`spin_work` of the same key when `work > 0` — so an actor that received another actor's job folds
a different key. Each creator folds its answers and the driver adds the summaries, as **wrapping
sums**: a job dropped, delivered twice or delivered to the wrong actor, an answer lost or
repeated, a skipped computation, or a summary lost or repeated all change the total.
`expected_messages = 2 × actors + 2 × creators` — one job and one answer per actor, one fork order
and one summary per creator — is asserted alongside: every answer carries the job it answers, and
every summary the count of its share.

What the checksum does **not** prove is that an actor was created: a creator that computed
`job_value(i, i, work)` itself and sent the answers to itself would fold the same total. That each
forked actor is a real actor of its framework — created by that framework's creation primitive,
sent its job through that framework's messaging and ended by that framework's termination — is
established by reading the four adapters (their header blocks name the primitive and the idiom
source), not by the checksum. The same is true of every benchmark here that creates actors.

## The measured window

Opens when the driver sends the fork orders into an already-running system — every creator has
reported ready, so every thread is up and every creator is scheduled — and closes when the driver
has received the last creator's summary. Every spawn, every `onInit` / `so_define_agent` /
behavior construction and every one of the 40 000 terminations is requested inside the window.
The creators and the driver are created before the window, once.

How much of an actor's END lands inside the window differs by framework, and one framework does
part of it outside its worker budget. In qb, CAF and the floor, the reclamation of an actor runs
on the thread that ran it, inside the budget, and only the last few actors to answer can finish
being reclaimed after the window closes — below resolution against 40 000 actor lives.
SObjectizer's multi-threaded environment runs the **final deregistration** of every coop —
unbinding its agent from the dispatcher, releasing the coop and with it the agent, the
repository's counters under its lock — on a dedicated thread the environment starts for itself
(`coop_repo_t::start()`, `dev/so_5/impl/mt_env_infrastructure.cpp`), with the work threads handing
each finished coop over under a mutex they share with it. That thread is not one of the `cores`
work threads: it runs inside the process's pinned CPU set, competing with them, and the coops
still in its chain when the driver receives the last summary are released after the window
closes. Here that is 40 000 coops per repetition, one per forked actor. It is a plausible
contributor to SObjectizer's `cores=2` cell coming out slower than its `cores=1` one on the
unpinned, unmeasured correctness runs: at `cores=2` two work threads, not one, contend with that
thread on the hand-over mutex and on the repository's lock, which every registration (on a work
thread) and every final deregistration (on that thread) takes, and in a pinned run the three
threads share the set's two CPUs. That is a
hypothesis for the quiet host, not a measured attribution. The cell's caveats say so.

For qb, a creator's whole share is alive at once — its actors run only once the forking loop has
returned — so 40 000 live actors sit on one core at `cores=1`. qb's actor id is a 16-bit slot per
VirtualCore (65 534 live actors): the default fits, and a share above the cap aborts the cell
with a message rather than reporting a checksum.
