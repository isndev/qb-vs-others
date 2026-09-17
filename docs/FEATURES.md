# Features — the comparison a nanosecond count cannot make

`REPORT.md` ranks three frameworks on message plumbing. This page is the other half of the
picture, and qb does not lead it everywhere: what each framework **has**, read from its source
at the pinned version, with the file and line where the claim can be checked. Nothing here was
measured; every row is a fact about an API, not about speed.

**What was read.** qb **3.1.0** (`qb/src/`, the submodule of this superproject), CAF **1.1.0**
and SObjectizer **5.8.5.1** — the trees `tools/run.py` builds against, which CMake fetches
under `build/<preset>/_deps/caf-src/` and `build/<preset>/_deps/sobjectizer-src/`. Citations
are `path:line` relative to `qb/src/qb/` for qb, to the CAF checkout for CAF, and to `dev/` of the
SObjectizer checkout for SObjectizer. A line number is a pointer into a pinned tree: it is
right for these versions and for no other.

**How to read a "no".** A framework that lacks something in its core may ship it beside the core
(SObjectizer's request/reply lives in *so5extra*; qb's HTTP, PostgreSQL and Redis live in
`qbm/*`). The table says what the **library measured in `REPORT.md`** carries, because that is
the thing whose cost was measured; the sections say where the rest lives.

## Summary

| axis | qb 3.1.0 | CAF 1.1.0 | SObjectizer 5.8.5.1 |
|---|---|---|---|
| exception in a handler | **kills the whole core thread** — no per-actor isolation | per-actor: exception handler, default terminates the actor with an error | per-agent policy: abort (default), shutdown, deregister coop, ignore |
| links / monitors / death-watch | none — cooperative `ChildDown` only | `link_to`, `monitor`, `down_msg`/`exit_msg`, exit reasons | coop deregistration notificators; parent/child coops |
| supervisor strategies | one_for_one / one_for_all / rest_for_one + restart intensity + escalation | none built in (links + exit handlers are the building blocks) | none built in (coops are the unit) |
| typed actor interfaces | none — `ActorId` is untyped, an unhandled event is a runtime drop | `typed_actor<...>`, statically checked `mail()` | none — untyped `mbox_t`, runtime `type_index` dispatch |
| request / response | `qb::ask` / `answer`, plus `ask_all`, `ask_any`, `ask_retry`, `ask_stream` | `mail().request().then/.await`, `response_promise`, `delegate` | not in core (so5extra) |
| networking / distribution | explicit qb-io: TCP, TLS, UDP, UDS, QUIC; **no** remote actors | `caf::io` / `caf::net` modules: transparent remote actors (not built here) | none in core |
| scheduler | one thread per core, actors pinned for life, no stealing | work-stealing (default) or work-sharing pool, `max-threads` | dispatchers: one_thread, active_obj, active_group, thread_pool, adv_thread_pool, prio_*, nef_* |
| message priorities | none (a binary drop-on-backpressure QoS) | 2 levels, `.urgent()` prepends | 8 levels p0–p7 on prio dispatchers |
| delivery filters / message limits | none | none | `so_set_delivery_filter`; `limit_then_drop/abort/redirect/transform` |
| timers | `with_timeout`, `callback(f, d)`, `co_await sleep`, `interval` | `mail().delay/.schedule`, `run_delayed`, `after()` | `send_delayed`, `send_periodic`, timer wheel/heap/list |
| C++20 coroutines | first-class: `task`, scopes, channels, generators, sync primitives, `onInit` itself | none | none |
| state machines | none (dispatch is by event type) | `become`/`unbecome` behaviours | hierarchical `state_t` with history, `time_limit`, on_enter/on_exit |
| dynamic spawn at runtime | same core only (`addRefActor`) | anywhere, `spawn` from any actor | anywhere, `make_coop` / `introduce_coop` |
| configuration file / CLI | none | `caf-application.conf` + CLI options | none (programmatic) |
| serialization | JSON (nlohmann), crypto, compression; **no** reflection | `inspect()` reflection, binary + JSON serializers | none in core |
| logging / metrics | nanolog-based `QB_LOG_*` | `CAF_LOG_*` + telemetry with a Prometheus collector | `error_logger`, `msg_tracing`, stats controller |
| standard / license | C++20 (C++23 opt-in) / Apache-2.0 | C++17 / BSD-3 | C++17 / BSD-3 |
| mailbox model | one MPSC ring **per core**, one pipe per (core, core); no per-actor mailbox | one MPSC `lifo_inbox` per actor | one demand queue per agent or per coop; the mbox holds no queue |

## 1. Fault handling

**qb.** A `throw` out of `on(Event&)` is not caught at the actor: dispatch is
`_router.route(*event, ...)` with no `try` around it (`core/VirtualCore.cpp:199`), and the only
handler is at the thread body, which logs and records `VirtualCore::Error::ExceptionThrown`
(`core/Main.cpp:348`, `:369`, `:373`; the enum at `core/VirtualCore.h:130`). **One throwing
handler terminates that VirtualCore thread and every actor pinned to it.** Two places do contain
an exception: a throwing `onInit()` is an init failure the engine reports (`core/VirtualCore.cpp:502`),
and an unhandled coroutine exception is logged by `report_unhandled_coroutine_exception`
(`core/Actor.cpp:356`). Supervision exists as a pattern — `qb::Supervisor`
(`core/patterns/supervisor.h:128`) with `restart_strategy::{one_for_one, one_for_all, rest_for_one}`
(`:45`), restart intensity over a window (`:138`, `:229`) and an `on_escalate()` hook (`:216`) —
but it is **cooperative**: the header says a child that dies without calling `stop()` "is not
auto-detected — supervision keys off the `ChildDown` notification" (`:125`), and its
`spawn_child` uses `addRefActor`, so a supervisor and its children share a core. There is no
link, no monitor, no exit reason.

**CAF.** `link_to(const actor_addr&)` (`libcaf_core/caf/abstract_actor.hpp:87`) and
`monitor(Handle, Fn) -> disposable` (`libcaf_core/caf/scheduled_actor.hpp:652`), with
`down_msg` / `exit_msg` / `node_down_msg` and `enum class exit_reason`
(`libcaf_core/caf/exit_reason.hpp:25`). Handlers are per actor: `set_down_handler` (`:300`),
`set_exit_handler` (`:337`), `set_exception_handler` (`:355`) — the default exception handler
turns the exception into an error and the actor terminates; other actors keep running. No
supervisor abstraction ships; a supervisor is written from links and exit handlers.

**SObjectizer.** Per-agent `exception_reaction_t` — `abort_on_exception`,
`shutdown_sobjectizer_on_exception`, `deregister_coop_on_exception`, `ignore_exception`,
`inherit_exception_reaction` (`so_5/agent.hpp:64`–`81`); the environment default is
**abort** (`so_5/environment.cpp:37`), so an unconfigured SObjectizer program dies on the first
throw, like qb, but the choice exists per agent and per environment. The unit of lifetime is the
cooperation (`so_5/coop.hpp:388`): parent/child coops (`so_5/environment.hpp:1332`), and
deregistration notificators (`so_5/coop.hpp:296`) are the death-watch. No restart strategies.

## 2. Typed interfaces

**qb.** Registering an event whose `on(E&)` overload is missing is a compile error — the router
calls `handler.on(event)` in a template (`system/event/router.h:84`, `:286`). But the handler
*set* is not part of any type: `ActorId` is `{ServiceId, CoreId}` (`core/ActorId.h:384`),
`Actor::push<E>(ActorId const&, ...)` (`core/Actor.h:929`) accepts any id for any `E`, and an
event the destination never registered falls into the router's `else` branch
(`system/event/router.h:870`) and is **logged and dropped** (`core/VirtualCore.cpp:206`).
`ActorHandle<T>` (`core/Actor.h:1902`) is a same-core reference, not an interface type.

**CAF.** `typed_actor<TraitOrSignature>` (`libcaf_core/caf/typed_actor.hpp:31`): the message
interface is the actor's type, `mail()` to a typed handle is checked at compile time, and the
`result<T>` of a handler is part of the signature. Untyped `actor` handles exist alongside.

**SObjectizer.** No typed interfaces: `abstract_message_box_t` (`so_5/mbox.hpp:185`) accepts
any message, subscriptions are per message type (`so_5/agent.hpp:119`), and delivery is a
runtime `type_index` lookup in the mbox's subscriber table (`so_5/impl/local_mbox.hpp:625`).

## 3. Request / response

**qb.** `co_await qb::ask(ctx, target, req, timeout)` (`core/patterns/request.h:100`) with
`qb::answer` on the responder (`:194`), `qb::Request<Resp>` (`:71`) and `qb::deadline` (`:115`);
`Actor::reply` (`core/Actor.h:1089`), `Actor::forward` (`:1112`). Beyond one ask: `ask_all` /
`ask_any` (`core/patterns/scatter.h:59`, `:140`), `ask_retry` (`core/patterns/resilience.h:427`),
`ask_stream` (`core/patterns/streaming.h:324`), request de-duplication (`core/patterns/idempotency.h:65`),
circuit breaker / rate limiter / bulkhead (`core/patterns/resilience.h:120`, `:239`, `:331`),
saga (`core/patterns/saga.h:44`). Every one of these needs the coroutine context.

**CAF.** `mail(...).request(receiver, timeout)` (`libcaf_core/caf/event_based_mail.hpp:48`,
blocking form `blocking_mail.hpp:47`) with `.then` / `.await`; `response_promise` /
`typed_response_promise` (`libcaf_core/caf/typed_response_promise.hpp:20`); delegation
(`libcaf_core/caf/local_actor.hpp:352`); fan-in policies `select_all` / `select_any`
(`libcaf_core/caf/policy/select_all.hpp:27`). Callback style, no coroutines.

**SObjectizer.** Not in the core library; `so_5/agent.hpp:2621` points at so5extra for
dispatchers and the request/reply helpers live there too. In core, request/reply is written
by hand with two messages, or synchronously through an `mchain`.

## 4. Networking and distribution

**qb.** No transparent distribution: `ActorId` has no node field (`core/ActorId.h:401`), there is
no wire format for events and no `inspect`. Networking is explicit and wide: TCP
(`io/transport/tcp.h`), TLS (`io/transport/stcp.h`, `io/tcp/ssl/context.h`), UDP
(`io/transport/udp.h`), Unix domain sockets behind `QB_ENABLE_UDS` (`io/tcp/socket.h:192`,
`io/udp/socket.h:159`), QUIC with a pluggable backend (`io/quic/backend.h:53`, `:84`), async
acceptor/client/server scaffolding (`io/async/tcp/`). HTTP/1.1·2·3, WebSocket, PostgreSQL and
Redis are separate modules (`qbm/`), not part of what `REPORT.md` measured.

**CAF.** `caf::io::middleman` (`libcaf_io/caf/io/middleman.hpp:35`) and `libcaf_net/` give
location-transparent remote actors, `node_down_msg`, publish/remote_actor. **Not built in this
repository** (`CMakeLists.txt:68`–`69` of the CAF tree are the module switches; the harness
builds the core only), so it is a feature of CAF and no part of any figure here.

**SObjectizer.** None in core. The project's networking answers are in so5extra and in
third-party packages.

## 5. Scheduling

**qb.** One worker thread per `VirtualCore`; every actor is created on a core before `start()`
(`core/Main.h:243`, `:679`) or on the calling core at runtime (`core/VirtualCore.h:1021`), and
"an actor never migrates between cores" (`core/Actor.h:215`). `setAffinity` (`core/Main.h:271`),
`setLatency` (`:286`) and `setIdleSpin` (`:303`) are the only knobs. No work stealing — the word
does not occur in `core/`.

**CAF.** A work-stealing pool by default, work-sharing as the alternative
(`libcaf_core/caf/scheduler.hpp:22`, `:24`), `max-threads` (`libcaf_core/caf/actor_system_config.cpp:114`),
and per-spawn options `detached`, `monitored`, `hidden`, `lazy_init`
(`libcaf_core/caf/spawn_options.hpp:34`–`49`). Actors are placed dynamically and rebalanced by
stealing; `frameworks/caf-detached/` measures the `detached` option.

**SObjectizer.** A dispatcher taxonomy: `one_thread`, `active_obj`, `active_group`,
`thread_pool`, `adv_thread_pool`, `prio_one_thread::{strictly_ordered, quoted_round_robin}`,
`prio_dedicated_threads::one_per_prio`, `nef_one_thread`, `nef_thread_pool` (directories under
`so_5/disp/`), with `fifo_t::{cooperation, individual}` choosing whether a coop shares one queue
(`so_5/disp/thread_pool/pub.hpp:155`). An agent is bound to a dispatcher at coop creation and
does not move between dispatchers; within a pool the thread is chosen per demand.

## 6. Priorities, filters, limits

**qb.** `EventQOS0` (`core/Event.h:516`) marks an event droppable under cross-core backpressure
(`core/Event.h:494`); `EventQOS1` and `EventQOS2` are both aliases of `Event` (`:499`, `:509`)
— "there is no middle QoS level to select" (`:503`). No ordering by priority, no filters, no
per-actor limits. `push` is ordered, `send` is documented unordered (`core/Actor.h:932`).

**CAF.** `message_priority::{high, normal}` (`libcaf_core/caf/message_priority.hpp:15`); a
`mail(...).urgent()` (`libcaf_core/caf/async_mail.hpp:175`) is pushed to the front of the
mailbox. No delivery filters, no message limits.

**SObjectizer.** Eight priorities `p0`–`p7` (`so_5/priority.hpp:27`) set per agent through
`agent_tuning_options_t::priority` (`so_5/agent_tuning_options.hpp:321`), honoured by the
`prio_*` dispatchers; delivery filters `so_set_delivery_filter` (`so_5/agent.hpp:2471`); message
limits `limit_then_drop` / `limit_then_abort` / `limit_then_redirect` / `limit_then_transform`
(`so_5/message_limit.hpp:629`, `:643`, `:695`).

## 7. Timers

**qb.** `with_timeout<Derived>` (`io/async/io.h:111`) with `setTimeout` (`:149`); one-shot
`Timeout<Func>` (`:211`); `callback(f)` / `callback(f, duration)` (`:368`, `:374`);
`co_await sleep(duration)` (`io/async/coroutine/utils.h:101`), a cancellable variant
(`io/async/coroutine/cancellation.h:765`) and an actor-scoped `ctx.sleep` cancelled on kill
(`core/Actor.h:1283`); periodic `interval(duration)` as an async stream
(`io/async/coroutine/stream.h:833`).

**CAF.** `mail(...).delay(d)` / `.schedule(tp)` (`libcaf_core/caf/async_mail.hpp:187`, `:181`),
`run_delayed` (`libcaf_core/caf/scheduled_actor.hpp:608`), `after()` in behaviours.

**SObjectizer.** `send_delayed` / `send_periodic` (`so_5/send_functions.hpp:67`), backed by a
timer thread whose mechanism is chosen at environment creation — wheel, heap or list
(`so_5/timers.hpp:252`). A `state_t::time_limit` moves an agent to another state after a delay
(`so_5/state.hpp:124`).

## 8. Coroutines

**qb.** The asynchronous surface *is* C++20 coroutines: `task<T>` (`io/async/coroutine/task.h:435`),
`shared_task` (`io/async/coroutine/shared_task.h:55`), `coroutine_scope` with joining / cancelling /
detaching exit policies (`io/async/coroutine/scope.h:76`, `:617`–`:635`), `parallel` (`:689`),
`when_all` / `when_any` / timeouts (`io/async/coroutine/combinators.h:76`, `:207`, `:689`),
`channel<T>` and `select` (`io/async/coroutine/channel.h:125`, `:1267`), `generator` /
`async_generator` / `async_stream` (`io/async/coroutine/generator.h:77`, `:289`;
`io/async/coroutine/stream.h:63`), and six sync primitives — `semaphore`, `async_mutex`,
`async_rw_lock`, `barrier`, `async_event`, `async_latch` (`io/async/coroutine/sync.h:64`, `:437`,
`:671`, `:960`, `:1098`, `:1275`) — plus `with_retry` (`io/async/coroutine/retry.h:219`). An actor's
`onInit()` is itself a `task<bool>` (`core/Actor.h:176`) and the engine stashes events while it
is suspended (`core/VirtualCore.cpp:488`). `Actor::spawn` binds a coroutine to the actor's
cancellation scope (`core/Actor.h:1291`).

**CAF.** None: no `co_await` anywhere under `libcaf_core/caf/`. The asynchronous vocabulary is
callbacks, `caf::flow` observables (`libcaf_core/caf/flow/observable.hpp:189`) and
`async::future`.

**SObjectizer.** None: no `co_await` anywhere under `so_5/`. Asynchrony is messages, `mchain`
(`so_5/mchain.hpp:198` names its overflow policies) and `select` over chains
(`so_5/mchain_select.hpp:154`).

## 9. Behaviour and state

**qb.** Dispatch is by event type through `registerEvent<E>` (`core/Actor.h:823`); there is no
behaviour stack and no state machine. `qb::ICallback` gives a per-tick hook
(`core/ICallback.h:146`). Patterns shipped beside the core, one header each under
`core/patterns/`: `batcher`, `ping`/`require` discovery, `dedup_map`, `PubSub<Topic>`,
`ask`/`answer`, `ask_retry`/`CircuitBreaker`/`rate_limiter`/`bulkhead`, `WorkerPool`,
`SagaScope`, `ask_all`/`ask_any`, `ask_stream`, `Supervisor` (umbrella `core/patterns.h`).

**CAF.** `become` / `unbecome` behaviours (`libcaf_core/caf/event_based_actor.hpp:66`),
`stateful_actor` (`libcaf_core/caf/stateful_actor.hpp:20`), `blocking_actor`
(`libcaf_core/caf/blocking_actor.hpp:42`), `caf::flow` streams, an `actor_registry` of named
actors (`libcaf_core/caf/actor_registry.hpp:32`, `:77`). Groups were **removed in 1.0**
(`CHANGELOG.md:200`).

**SObjectizer.** Hierarchical states with shallow and deep history (`so_5/state.hpp:404`,
`:405`), `on_enter` / `on_exit`, `time_limit` (`:124`); named mboxes, MPMC and MPSC mbox kinds,
`unique_subscribers_mbox` (`so_5/unique_subscribers_mbox.hpp:27`), message sinks and bindings
(`so_5/single_sink_binding.hpp`, `so_5/multi_sink_binding.hpp`), `mutable_msg` / `immutable_msg`
(`so_5/message.hpp:366`).

## 10. Runtime creation

**qb.** Before `start()`: `CoreInitializer::addActor` / `builder()` (`core/Main.h:243`, `:257`),
`Main::addActor(CoreId, ...)` (`:725`). At runtime: `Actor::addRefActor<T>` (`core/Actor.h:1174`)
creates on the **calling core only** — `VirtualCore::_handler` is the thread-local current core
(`core/VirtualCore.h:1021`) — and `Main::core(id)` is setup-phase only (`core/Main.h:679`).
Creating an actor on another core at runtime is done by messaging a factory actor already there;
the framework has no call for it. Actor allocation is a customisation point (`core/VirtualCore.h:777`).

**CAF.** `spawn` from the system or from any actor, on any worker, at any time; the pool
places it.

**SObjectizer.** `environment_t::make_coop` (`so_5/environment.hpp:1332`) / `introduce_coop` from
anywhere at any time; the dispatcher binds it.

## 11. Configuration, serialization, observability

**qb.** No configuration file and no CLI layer in core (`argv`, `getopt`, `.ini`, `toml`,
`yaml` do not occur in `core/`); `Main` / `CoreInitializer` are the configuration. `qb::json` is
nlohmann (`json.h:47`), `qb::crypto` (`io/crypto.h:95`), `qb::compression` (`io/compression.h:45`);
**no reflection** — events are relocated by byte copy into 64-byte buckets
(`utility/prefix.h:138`), which is also why an event must use relocatable members such as
`qb::string<N>` (`core/patterns/request.h:59`). Logging is nanolog through `QB_LOG_DEBUG` …
`QB_LOG_CRIT` (`io.h:272`; `log::init` at `io.h:84`). No metrics.

**CAF.** `actor_system_config` (`libcaf_core/caf/actor_system_config.hpp:28`) reads
`caf-application.conf` (`libcaf_core/caf/actor_system_config.cpp:80`) and command-line options;
`CAF_ADD_TYPE_ID` + `inspect()` (`libcaf_core/caf/type_id.hpp:151`) drive `binary_serializer`
(`libcaf_core/caf/binary_serializer.hpp:25`) and `json_writer` (`libcaf_core/caf/json_writer.hpp:15`);
`CAF_LOG_*` (`libcaf_core/caf/logger.hpp:384`) and telemetry with a Prometheus collector
(`libcaf_core/caf/telemetry/collector/prometheus.hpp`).

**SObjectizer.** Programmatic configuration through `environment_params_t`; single-threaded
infrastructures `simple_mtsafe` / `simple_not_mtsafe` (`so_5/env_infrastructures.hpp:52`, `:62`);
no serialization in core; `error_logger_t` (`so_5/error_logger.hpp:25`), message tracing
(`so_5/msg_tracing.hpp:29`), a stats controller (`so_5/stats/controller.hpp:33`).

## 12. Standard, license, maturity

| | qb | CAF | SObjectizer |
|---|---|---|---|
| C++ standard | 20, 23 opt-in (`cmake/qbConfig.cmake:246`–`250` of the qb tree) | 17 (`CMakeLists.txt:79`) | 17 (`dev/CMakeLists.txt:53`) |
| license | Apache-2.0 (`LICENSE:1` of the qb tree) | BSD-3 (`LICENSE:1`, copyright 2011–2024) | BSD-3 (`LICENSE:5`–`6`: 2002–2013 JSC Intervale, 2013–2025 The SObjectizer Project) |
| lineage | 3.x, first public major 2.x | 1.1 after a decade of 0.x releases | SObjectizer-5 since 2010, the family since 2002 (`README.md:46`) |
| event loop | vendored, patched libev: epoll, kqueue, io_uring, wepoll on Windows (`ev/config.h.cmakein:10`–`15`) | own scheduler; `caf::net` for I/O | own dispatchers; no I/O loop in core |

Age is not quality, but it is exposure: CAF and SObjectizer have been run in places qb has not,
and a defect that only shows under a workload nobody here has written is a defect this page
cannot list.

## 13. Threading model — why the numbers look the way they do

This row is the one that explains `REPORT.md`, so it is stated in full.

**qb** has **no per-actor mailbox**. The inbound queue is one MPSC ring per **destination core**
(`core/Main.h:380`, owned at `:495`), written through one staging pipe per (source core,
destination core) pair (`core/Event.h:689`, `core/VirtualCore.h:465`, flushed at `:521`), and
the actor is resolved only after dequeue (`core/VirtualCore.cpp:199`). Every event occupies a
whole number of 64-byte buckets (`utility/prefix.h:68`, `:138`). Consequences, all visible in the
tables: the 120-writer contention of `savina/big` collapses to a 2-writer pipe; a same-core hop
never touches a shared cache line; and an actor can never leave a busy core.

**CAF** has one MPSC `lifo_inbox` per actor (`libcaf_core/caf/detail/default_mailbox.hpp:74`)
and a double-ended run queue per worker (`libcaf_core/scheduler.cpp:88` in the CAF tree); a
message makes its receiver runnable on the sender's worker, and other workers steal.

**SObjectizer** keeps the queue on the dispatcher side — one demand queue per agent
(`fifo_t::individual`) or per cooperation (`fifo_t::cooperation`); the mbox itself holds no
queue and only routes (`so_5/impl/local_mbox.hpp:625`).

## What qb should be honest about

Read against the two others, qb's gaps are structural, not cosmetic, and `docs/ROADMAP.md` is
where any of them would become a plan:

1. **No exception isolation per actor** — a throw takes the core down (`core/VirtualCore.cpp:199`,
   `core/Main.cpp:369`). CAF and SObjectizer both isolate at the actor.
2. **No link / monitor / death-watch** — supervision is cooperative and same-core
   (`core/patterns/supervisor.h:125`).
3. **No typed actor interfaces** — an event nobody registered is a logged drop
   (`system/event/router.h:870`).
4. **No distribution** — no node id, no wire format, no reflection (`core/ActorId.h:401`).
5. **No message priorities** — `EventQOS1 == EventQOS2 == Event` (`core/Event.h:499`–`509`).
6. **No migration, no rebalancing** — actors are pinned for life (`core/Actor.h:215`); a hot
   actor on a cold core stays there.
7. **No cross-core dynamic spawn** (`core/VirtualCore.h:1021`, `core/Main.h:679`).
8. **No configuration layer** in core.

What qb has that the others do not — a coroutine-first asynchronous model, a real I/O loop with
QUIC, and the shard-per-core dispatch whose cost `REPORT.md` measures — is the trade those gaps
buy. Whether it is the right trade depends on the application, which is the first sentence of
`FAIRNESS.md`.
