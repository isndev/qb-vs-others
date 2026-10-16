# Features — the comparison a nanosecond count cannot make

`REPORT.md` ranks three frameworks on message plumbing. This page is the other half of the
picture, and qb does not lead it everywhere: what each framework **has**, read from its source
at the pinned version, with source-file and symbol anchors that can be checked without relying
on line numbers from another checkout. Nothing here was
measured; every row is a fact about an API, not about speed.

**Source versions audited.** This is a historical source comparison: qb **v3.1.0**
(`830ea2442f8fbe5944290758148371edece2c10c`), CAF **1.1.0**
(`97e03500422f0ac48e35fee510d570e16b43a65e`), and SObjectizer **v5.8.5.1**
(`c5ec4485cf6e8b0d8290c309f9a10d306252e6ca`). A `file#symbol` reference uses the version named
for that framework throughout this page: qb paths are relative to `src/qb/` unless marked
repository-root, CAF paths to its repository root, SObjectizer paths to `dev/` unless marked
repository-root. These source versions do not identify every later candidate in `REPORT.md`;
use each campaign's source commit and provenance to identify the measured binary.

**How to read a "no".** A framework that lacks something in its core may ship it beside the core
(SObjectizer's request/reply lives in *so5extra*; qb's HTTP, PostgreSQL and Redis live in
`qbm/*`). The table says what the **pinned source version above** carries; the sections distinguish core
features from modules. It is not an assertion that every reported campaign used that version.

## Summary

| axis | qb 3.1.0 | CAF 1.1.0 | SObjectizer 5.8.5.1 |
|---|---|---|---|
| exception in a handler | **process termination if an exception escapes noexcept event dispatch** — no per-actor isolation | per-actor: exception handler, default terminates the actor with an error | per-agent policy: abort (default), shutdown, deregister coop, ignore |
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
| standard / license | C++20 (C++23 opt-in) / Apache-2.0 | minimum C++17 / BSD-3 | default C++17 / BSD-3 |
| mailbox model | one MPSC ring **per core**, one pipe per (core, core); no per-actor mailbox | one MPSC `lifo_inbox` per actor | one demand queue per agent or per coop; the mbox holds no queue |

## 1. Fault handling

**qb.** An exception escaping an `on(Event&)` handler crosses a `noexcept` route:
`VirtualCore` uses `memh<Event>`, whose resolver calls `semh::route`
(`system/event/router.h#EventResolver::resolve` and `system/event/router.h#semh::route`).
C++ therefore calls `std::terminate`; the catch in `core/Main.cpp#Main::start_thread` is not
per-actor containment and cannot catch an exception across that boundary. A throwing
`onInit()` is handled separately as an init failure (`core/VirtualCore.cpp#VirtualCore::__drive_init__`),
and spawned-coroutine exceptions are reported by `core/Actor.cpp#report_unhandled_coroutine_exception`. Supervision exists as a pattern — `qb::Supervisor`
(`core/patterns/supervisor.h#Supervisor`) with `restart_strategy::{one_for_one, one_for_all, rest_for_one}`,
restart intensity (`Supervisor::over_restart_limit`) and an `on_escalate()` hook in the same header —
but it is **cooperative**: the header says a child that dies without calling `stop()` "is not
auto-detected — supervision keys off the `ChildDown` notification" (`core/patterns/supervisor.h#Supervisor`), and the
documented `spawn_child` override uses `addRefActor`, keeping the shown supervisor and children on one core. There is no
link, no monitor, no exit reason.

**CAF.** `link_to(const actor_addr&)` (`libcaf_core/caf/abstract_actor.hpp#link_to`) and
`monitor(Handle, Fn) -> disposable` (`libcaf_core/caf/scheduled_actor.hpp#monitor`), with
`down_msg` / `exit_msg` / `node_down_msg` and `enum class exit_reason`
(`libcaf_core/caf/exit_reason.hpp#exit_reason`). Handlers are per actor: `set_down_handler` (`libcaf_core/caf/scheduled_actor.hpp#set_down_handler`),
`set_exit_handler` (`libcaf_core/caf/scheduled_actor.hpp#set_exit_handler`), `set_exception_handler` (`libcaf_core/caf/scheduled_actor.hpp#set_exception_handler`) — the default exception handler
turns the exception into an error and the actor terminates; other actors keep running. No
supervisor abstraction ships; a supervisor is written from links and exit handlers.

**SObjectizer.** Per-agent `exception_reaction_t` — `abort_on_exception`,
`shutdown_sobjectizer_on_exception`, `deregister_coop_on_exception`, `ignore_exception`,
`inherit_exception_reaction` (`so_5/agent.hpp#exception_reaction_t`); the environment default is
**abort** (`so_5/environment.cpp#abort_on_exception`), so an unconfigured SObjectizer program dies on the first
throw. The choice exists per agent and per environment; the default is process-wide, not per-agent isolation. The unit of lifetime is the
cooperation (`so_5/coop.hpp#coop_t`): parent/child coops (`so_5/environment.hpp#make_coop`), and
deregistration notificators (`so_5/coop.hpp#add_dereg_notificator`) are the death-watch. No restart strategies.

## 2. Typed interfaces

**qb.** Registering an event whose `on(E&)` overload is missing is a compile error — the router
calls `handler.on(event)` in a template (`system/event/router.h#invoke`). But the handler
*set* is not part of any type: `ActorId` is `{ServiceId, CoreId}` (`core/ActorId.h#ActorId`),
`Actor::push<E>(ActorId const&, ...)` (`core/Actor.h#Actor::push`) accepts any id for any `E`, and an
event the destination never registered falls into the router's `else` branch
(`system/event/router.h#route`) and is **logged and dropped** (`core/VirtualCore.cpp#VirtualCore::__receive_events__`).
`ActorHandle<T>` (`core/Actor.h#ActorHandle`) is a same-core reference, not an interface type.

**CAF.** `typed_actor<TraitOrSignature>` (`libcaf_core/caf/typed_actor.hpp#typed_actor`): the message
interface is the actor's type, `mail()` to a typed handle is checked at compile time, and the
`result<T>` of a handler is part of the signature. Untyped `actor` handles exist alongside.

**SObjectizer.** No typed interfaces: `abstract_message_box_t` (`so_5/mbox.hpp#abstract_message_box_t`) accepts
any message, subscriptions are per message type (`so_5/agent.hpp#so_subscribe`), and delivery is a
runtime `type_index` lookup in the mbox's subscriber table (`so_5/impl/local_mbox.hpp#type_index`).

## 3. Request / response

**qb.** `co_await qb::ask(ctx, target, req, timeout)` (`core/patterns/request.h#ask`) with
`qb::answer` on the responder (`core/patterns/request.h#answer`), `qb::Request<Resp>` (`core/patterns/request.h#Request`) and `qb::deadline` (`core/patterns/request.h#deadline`);
`Actor::reply` (`core/Actor.h#Actor::reply`), `Actor::forward` (`core/Actor.h#Actor::forward`). Beyond one ask: `ask_all` /
`ask_any` (`core/patterns/scatter.h#ask_all`, `core/patterns/scatter.h#ask_any`), `ask_retry` (`core/patterns/resilience.h#ask_retry`),
`ask_stream` (`core/patterns/streaming.h#ask_stream`), request de-duplication (`core/patterns/idempotency.h#dedup_map`),
circuit breaker / rate limiter / bulkhead (`core/patterns/resilience.h#CircuitBreaker`, `core/patterns/resilience.h#rate_limiter`, `core/patterns/resilience.h#bulkhead`),
saga (`core/patterns/saga.h#SagaScope`). The operations that suspend need a coroutine context;
`answer(Actor&, E&, Fn)`, `Actor::reply` / `forward`, `CircuitBreaker::allow(now_ns)` and
`rate_limiter::try_acquire(now_ns)` are synchronous. The admission probes take no
`ScopedCoroContext` (`core/patterns/resilience.h#CircuitBreaker::allow` and
`core/patterns/resilience.h#rate_limiter::try_acquire`).

**CAF.** `mail(...).request(receiver, timeout)` (`libcaf_core/caf/event_based_mail.hpp#request`,
blocking form `libcaf_core/caf/blocking_mail.hpp#request`) with `.then` / `.await`; `response_promise` /
`typed_response_promise` (`libcaf_core/caf/typed_response_promise.hpp#typed_response_promise`); delegation
(`libcaf_core/caf/local_actor.hpp#delegate`); fan-in policies `select_all` / `select_any`
(`libcaf_core/caf/policy/select_all.hpp#select_all`). Callback style, no coroutines.

**SObjectizer.** Not in the core library; `so_5/agent.hpp#so5extra` points at so5extra for
dispatchers and the request/reply helpers live there too. In core, request/reply is written
by hand with two messages, or synchronously through an `mchain`.

## 4. Networking and distribution

**qb.** No transparent distribution: `ActorId` has no node field (`core/ActorId.h#ActorId`), there is
no wire format for events and no `inspect`. Networking is explicit and wide: TCP
(`io/transport/tcp.h`), TLS (`io/transport/stcp.h`, `io/tcp/ssl/context.h`), UDP
(`io/transport/udp.h`), Unix domain sockets behind `QB_ENABLE_UDS` (`io/tcp/socket.h#QB_ENABLE_UDS`,
`io/udp/socket.h#QB_ENABLE_UDS`), QUIC with a pluggable backend (`io/quic/backend.h#backend`), async
acceptor/client/server scaffolding (`io/async/tcp/`). HTTP/1.1·2·3, WebSocket, PostgreSQL and
Redis are separate modules (`qbm/`), not part of what `REPORT.md` measured.

**CAF.** `caf::io::middleman` (`libcaf_io/caf/io/middleman.hpp#middleman`) and `libcaf_net/` give
location-transparent remote actors, `node_down_msg`, publish/remote_actor. **Not built in this
repository** (`CMakeLists.txt#CAF_ENABLE_IO_MODULE` and `CAF_ENABLE_NET_MODULE` in the CAF tree; the harness
builds the core only), so it is a feature of CAF and no part of any figure here.

**SObjectizer.** None in core. The project's networking answers are in so5extra and in
third-party packages.

## 5. Scheduling

**qb.** One worker thread per `VirtualCore`; every actor is created on a core before `start()`
(`core/Main.h#CoreInitializer::addActor`, `core/Main.h#Main::core`) or on the calling core at runtime (`core/VirtualCore.h#Actor::addRefActor`), and
"an actor never migrates between cores" (`core/Actor.h#Actor`). `setAffinity` (`core/Main.h#CoreInitializer::setAffinity`) and
`setLatency` (`core/Main.h#CoreInitializer::setLatency`) configure the worker in this version. `setIdleSpin` is absent from
qb 3.1.0. There is no work-stealing scheduler in this core implementation.

**CAF.** A work-stealing pool by default, work-sharing as the alternative
(`libcaf_core/caf/scheduler.hpp#make_work_stealing`, `libcaf_core/caf/scheduler.hpp#make_work_sharing`), `max-threads` (`libcaf_core/caf/actor_system_config.cpp#max-threads`),
and per-spawn options `detached`, `monitored`, `hidden`, `lazy_init`
(`libcaf_core/caf/spawn_options.hpp#spawn_options`). Actors are placed dynamically and rebalanced by
stealing; `frameworks/caf-detached/` measures the `detached` option.

**SObjectizer.** A dispatcher taxonomy: `one_thread`, `active_obj`, `active_group`,
`thread_pool`, `adv_thread_pool`, `prio_one_thread::{strictly_ordered, quoted_round_robin}`,
`prio_dedicated_threads::one_per_prio`, `nef_one_thread`, `nef_thread_pool` (directories under
`so_5/disp/`), with `fifo_t::{cooperation, individual}` choosing whether a coop shares one queue
(`so_5/disp/thread_pool/pub.hpp#fifo_t`). An agent is bound to a dispatcher at coop creation and
does not move between dispatchers; within a pool the thread is chosen per demand.

## 6. Priorities, filters, limits

**qb.** `EventQOS0` (`core/Event.h#EventQOS0`) marks an event droppable under cross-core backpressure
(`core/Event.h#EventQOS0`); `EventQOS1` and `EventQOS2` are both aliases of `Event` (`core/Event.h#EventQOS1`, `core/Event.h#EventQOS2`)
— "there is no middle QoS level to select" (`core/Event.h#EventQOS1`). No ordering by priority, no filters, no
per-actor limits. `push` is ordered, `send` is documented unordered (`core/Actor.h#Actor::send`).

**CAF.** `message_priority::{high, normal}` (`libcaf_core/caf/message_priority.hpp#message_priority`); a
`mail(...).urgent()` (`libcaf_core/caf/async_mail.hpp#urgent`) is pushed to the front of the
mailbox. No delivery filters, no message limits.

**SObjectizer.** Eight priorities `p0`–`p7` (`so_5/priority.hpp#p0`) set per agent through
`agent_tuning_options_t::priority` (`so_5/agent_tuning_options.hpp#priority`), honoured by the
`prio_*` dispatchers; delivery filters `so_set_delivery_filter` (`so_5/agent.hpp#so_set_delivery_filter`); message
limits `limit_then_drop` / `limit_then_abort` / `limit_then_redirect` / `limit_then_transform`
(`so_5/message_limit.hpp#limit_then_drop`, `so_5/message_limit.hpp#limit_then_abort`, `so_5/message_limit.hpp#limit_then_transform`).

## 7. Timers

**qb.** `with_timeout<Derived>` (`io/async/io.h#with_timeout`) with `setTimeout` (`io/async/io.h#setTimeout`); one-shot
`Timeout<Func>` (`io/async/io.h#Timeout`); `callback(f)` / `callback(f, duration)` (`io/async/io.h#callback`);
`co_await sleep(duration)` (`io/async/coroutine/utils.h#sleep`), a cancellable variant
(`io/async/coroutine/cancellation.h#cancellable_sleep`) and an actor-scoped `ctx.sleep` cancelled on kill
(`core/Actor.h#ScopedCoroContext::sleep`); periodic `interval(duration)` as an async stream
(`io/async/coroutine/stream.h#interval`).

**CAF.** `mail(...).delay(d)` / `.schedule(tp)` (`libcaf_core/caf/async_mail.hpp#delay`, `libcaf_core/caf/async_mail.hpp#schedule`),
`run_delayed` (`libcaf_core/caf/scheduled_actor.hpp#run_delayed`), `after()` in behaviours.

**SObjectizer.** `send_delayed` / `send_periodic` (`so_5/send_functions.hpp#send_delayed`), backed by a
timer thread whose mechanism is chosen at environment creation — wheel, heap or list
(`so_5/timers.hpp#timer_thread_t`). A `state_t::time_limit` moves an agent to another state after a delay
(`so_5/state.hpp#time_limit`).

## 8. Coroutines

**qb.** The asynchronous surface *is* C++20 coroutines: `task<T>` (`io/async/coroutine/task.h#task`),
`shared_task` (`io/async/coroutine/shared_task.h#shared_task`), `coroutine_scope` with joining / cancelling /
detaching exit policies (`io/async/coroutine/scope.h#coroutine_scope` and `coroutine_scope::cleanup_policy`), `parallel` (`io/async/coroutine/scope.h#parallel`),
`when_all` / `when_any` / timeouts (`io/async/coroutine/combinators.h#when_all`, `io/async/coroutine/combinators.h#when_any`, `io/async/coroutine/combinators.h#coro_with_timeout`),
`channel<T>` and `select` (`io/async/coroutine/channel.h#channel`, `io/async/coroutine/channel.h#select`), `generator` /
`async_generator` / `async_stream` (`io/async/coroutine/generator.h#generator`, `io/async/coroutine/generator.h#async_generator`;
`io/async/coroutine/stream.h#async_stream`), and six sync primitives — `semaphore`, `async_mutex`,
`async_rw_lock`, `barrier`, `async_event`, `async_latch` (`io/async/coroutine/sync.h#semaphore`, `io/async/coroutine/sync.h#async_mutex`,
`io/async/coroutine/sync.h#async_rw_lock`, `io/async/coroutine/sync.h#barrier`, `io/async/coroutine/sync.h#async_event`, `io/async/coroutine/sync.h#async_latch`) — plus `with_retry` (`io/async/coroutine/retry.h#with_retry`). An actor's
`onInit()` is itself a `task<bool>` (`core/Actor.h#onInit`) and the engine stashes events while it
is suspended (`core/VirtualCore.cpp#VirtualCore::__begin_activation__`). `Actor::spawn` binds a coroutine to the actor's
cancellation scope (`core/Actor.h#Actor::spawn`).

**CAF.** None: no `co_await` anywhere under `libcaf_core/caf/`. The asynchronous vocabulary is
callbacks, `caf::flow` observables (`libcaf_core/caf/flow/observable.hpp#observable`) and
`async::future`.

**SObjectizer.** None: no `co_await` anywhere under `so_5/`. Asynchrony is messages, `mchain`
(`so_5/mchain.hpp#overflow_reaction_t` names its overflow policies) and `select` over chains
(`so_5/mchain_select.hpp#select`).

## 9. Behaviour and state

**qb.** Dispatch is by event type through `registerEvent<E>` (`core/Actor.h#Actor::registerEvent`); there is no
behaviour stack and no state machine. `qb::ICallback` gives a per-tick hook
(`core/ICallback.h#ICallback`). Patterns shipped beside the core, one header each under
`core/patterns/`: `batcher`, `ping`/`require` discovery, `dedup_map`, `PubSub<Topic>`,
`ask`/`answer`, `ask_retry`/`CircuitBreaker`/`rate_limiter`/`bulkhead`, `WorkerPool`,
`SagaScope`, `ask_all`/`ask_any`, `ask_stream`, `Supervisor` (umbrella `core/patterns.h`).

**CAF.** `become` / `unbecome` behaviours (`libcaf_core/caf/event_based_actor.hpp#become`),
`stateful_actor` (`libcaf_core/caf/stateful_actor.hpp#stateful_actor`), `blocking_actor`
(`libcaf_core/caf/blocking_actor.hpp#blocking_actor`), `caf::flow` streams, an `actor_registry` of named
actors (`libcaf_core/caf/actor_registry.hpp#actor_registry`, `libcaf_core/caf/actor_registry.hpp#put`). Groups were **removed in 1.0**
(`CHANGELOG.md#1.0.0`).

**SObjectizer.** Hierarchical states with shallow and deep history (`so_5/state.hpp#history_t`,
`so_5/state.hpp#history_t`), `on_enter` / `on_exit`, `time_limit` (`so_5/state.hpp#time_limit`); named mboxes, MPMC and MPSC mbox kinds,
`unique_subscribers_mbox` (`so_5/unique_subscribers_mbox.hpp#make_unique_subscribers_mbox`), message sinks and bindings
(`so_5/single_sink_binding.hpp`, `so_5/multi_sink_binding.hpp`), `mutable_msg` / `immutable_msg`
(`so_5/message.hpp#mutable_msg`).

## 10. Runtime creation

**qb.** Before `start()`: `CoreInitializer::addActor` / `builder()` (`core/Main.h#CoreInitializer::addActor`, `core/Main.h#CoreInitializer::builder`),
`Main::addActor(CoreId, ...)` (`core/Main.h#Main::addActor`). At runtime: `Actor::addRefActor<T>` (`core/Actor.h#Actor::addRefActor`)
creates on the **calling core only** — `VirtualCore::_handler` is the thread-local current core
(`core/VirtualCore.h#Actor::addRefActor`) — and `Main::core(id)` is setup-phase only (`core/Main.h#Main::core`).
Creating an actor on another core at runtime is done by messaging a factory actor already there;
the framework has no call for it. Actor allocation is a customisation point (`core/VirtualCore.h#allocate_actor`).

**CAF.** `spawn` from the system or from any actor, on any worker, at any time; the pool
places it.

**SObjectizer.** `environment_t::make_coop` (`so_5/environment.hpp#make_coop`) / `introduce_coop` from
anywhere at any time; the dispatcher binds it.

## 11. Configuration, serialization, observability

**qb.** No configuration file and no CLI layer in core (`argv`, `getopt`, `.ini`, `toml`,
`yaml` do not occur in `core/`); `Main` / `CoreInitializer` are the configuration. `qb::json` is
nlohmann (`json.h#json`), `qb::crypto` (`io/crypto.h#crypto`), `qb::compression` (`io/compression.h#compression`);
**no reflection** — events are relocated by byte copy into buckets (64 bytes by default)
(`utility/prefix.h#EventBucket`), which is also why an event must use relocatable members such as
`qb::string<N>` (`core/patterns/request.h#Request`). Logging is nanolog through `QB_LOG_DEBUG` …
`QB_LOG_CRIT` (`io.h#QB_LOG_DEBUG`; `log::init` at `io.h#init`). No metrics.

**CAF.** `actor_system_config` (`libcaf_core/caf/actor_system_config.hpp#actor_system_config`) reads
`caf-application.conf` (`libcaf_core/caf/actor_system_config.cpp#default_config_file`) and command-line options;
`CAF_ADD_TYPE_ID` + `inspect()` (`libcaf_core/caf/type_id.hpp#CAF_ADD_TYPE_ID`) drive `binary_serializer`
(`libcaf_core/caf/binary_serializer.hpp#binary_serializer`) and `json_writer` (`libcaf_core/caf/json_writer.hpp#json_writer`);
`CAF_LOG_*` (`libcaf_core/caf/logger.hpp#CAF_LOG_INFO`) and telemetry with a Prometheus collector
(`libcaf_core/caf/telemetry/collector/prometheus.hpp`).

**SObjectizer.** Programmatic configuration through `environment_params_t`; single-threaded
infrastructures `simple_mtsafe` / `simple_not_mtsafe` (`so_5/env_infrastructures.hpp#simple_mtsafe`, `so_5/env_infrastructures.hpp#simple_not_mtsafe`);
no serialization in core; `error_logger_t` (`so_5/error_logger.hpp#error_logger_t`), message tracing
(`so_5/msg_tracing.hpp#tracer_t`), a stats controller (`so_5/stats/controller.hpp#controller_t`).

## 12. Standard, license, maturity

| | qb | CAF | SObjectizer |
|---|---|---|---|
| C++ source requirement / default | 20, 23 opt-in (`cmake/qbConfig.cmake#QB_CXX_STANDARD`, repository-root) | minimum 17 (`CMakeLists.txt#CAF_CXX_VERSION` and `target_compile_features`) | default 17 when the caller has not selected a standard (`dev/CMakeLists.txt#CMAKE_CXX_STANDARD`, repository-root) |
| license | Apache-2.0 (`LICENSE`, repository-root) | BSD-3 (`LICENSE`, copyright 2011–2024) | BSD-3 (`LICENSE`, repository-root: 2002–2013 JSC Intervale, 2013–2025 The SObjectizer Project) |
| lineage | 3.x, first public major 2.x | 1.1 after a decade of 0.x releases | SObjectizer-5 since 2010, the family since 2002 (`README.md`, repository-root, project history) |
| event loop | vendored, patched libev: epoll, kqueue, io_uring, wepoll on Windows (`ev/config.h.cmakein#EV_USE_EPOLL`, `EV_USE_KQUEUE`, `EV_USE_IOURING`) | own scheduler; `caf::net` for I/O | own dispatchers; no I/O loop in core |

The QVO CMake project selects C++20 before adding its dependencies. CAF's `cxx_std_17`
requirement is a minimum, and SObjectizer honors an already selected `CMAKE_CXX_STANDARD`.
Neither establishes that a QVO binary was compiled as C++17. Read the compile command of the
specific campaign for its effective standard; this source audit does not reconstruct old
binaries on other hosts.

Age is not quality, but it is exposure: CAF and SObjectizer have been run in places qb has not,
and a defect that only shows under a workload nobody here has written is a defect this page
cannot list.

## 13. Threading model — why the numbers look the way they do

This row is the one that explains `REPORT.md`, so it is stated in full.

**qb** has **no per-actor mailbox**. The inbound queue is one MPSC ring per **destination core**
(`core/Main.h#SharedCoreCommunication` and its `Mailbox`), written through one staging pipe per (source core,
destination core) pair (`core/Event.h#VirtualPipe`, `core/VirtualCore.h#_pipes`, flushed at `core/VirtualCore.h#__flush_all__`), and
the actor is resolved only after dequeue (`core/VirtualCore.cpp#VirtualCore::__receive_events__`). Every event occupies a
whole number of buckets (64 bytes by default) (`utility/prefix.h#QB_LOCKFREE_EVENT_BUCKET_BYTES`, `utility/prefix.h#EventBucket`). The core owns the transport path:
the 120 actors in `savina/big` share their cores' outgoing pipes; same-core delivery needs no
cross-core hop, and actors do not migrate away from a busy core.

**CAF** has one MPSC `lifo_inbox` per actor (`libcaf_core/caf/detail/default_mailbox.hpp#lifo_inbox`)
and a double-ended run queue per worker (`libcaf_core/caf/scheduler.cpp#work_stealing` in the CAF tree); a
message makes its receiver runnable on the sender's worker, and other workers steal.

**SObjectizer** keeps the queue on the dispatcher side — one demand queue per agent
(`fifo_t::individual`) or per cooperation (`fifo_t::cooperation`); the mbox itself holds no
queue and only routes (`so_5/impl/local_mbox.hpp#type_index`).

## What qb should be honest about

Read against the two others, qb's gaps are structural, not cosmetic, and `docs/ROADMAP.md` is
where any of them would become a plan:

1. **No exception isolation per actor** — an escaping handler exception crosses the
   `noexcept` dispatch boundary (`system/event/router.h#semh::route`). CAF handles exceptions per actor; SObjectizer provides configurable reactions but defaults to abort.
2. **No link / monitor / death-watch** — supervision is cooperative and same-core
   (`core/patterns/supervisor.h#Supervisor`).
3. **No typed actor interfaces** — an event nobody registered is a logged drop
   (`system/event/router.h#route`).
4. **No distribution** — no node id, no wire format, no reflection (`core/ActorId.h#ActorId`).
5. **No message priorities** — `EventQOS1 == EventQOS2 == Event` (`core/Event.h#EventQOS1` and `EventQOS2`).
6. **No migration, no rebalancing** — actors are pinned for life (`core/Actor.h#Actor`); a hot
   actor on a cold core stays there.
7. **No cross-core dynamic spawn** (`core/VirtualCore.h#Actor::addRefActor`, `core/Main.h#Main::core`).
8. **No configuration layer** in core.

What qb has that the others do not — a coroutine-first asynchronous model, a real I/O loop with
QUIC, and the shard-per-core dispatch whose cost `REPORT.md` measures — is the trade those gaps
buy. Whether it is the right trade depends on the application, which is the first sentence of
`FAIRNESS.md`.
