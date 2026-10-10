// @benchmark     savina/concsll
// @framework     qb
// @idiom-source  form=0: qb/tests/core/benchmark/messaging/ping-pong-latency.cpp -- qb's OWN
//                round-trip benchmark (a small trivially-destructible event, `reply()` recycling
//                the received event's bytes) -- and qb/llm/qb.llm.md "Invariants & gotchas" on
//                `reply` (a non-const `on(Event&)`, the event consumed) and on `send` ("ONE event
//                with nothing behind it to batch -- a request the sender then idles for");
//                form=1: qb/src/qb/core/patterns/request.h (`qb::ask`, `AskEvent`, `resolve_ask`)
//                and the bank-transaction adapter beside this file.
// @idiom-note    form=0, the reference's shape and concdict's: a worker's request is ONE event that
//                bounces between the worker and the list for the worker's whole run. The list
//                walks, writes the answer into the event and `reply()`s it; the worker folds the
//                answer, rewrites the same event into its next request and `reply()`s it back --
//                no event is constructed after the first, and `reply()` hands it to the peer at
//                once (VirtualCore::reply is a `send`, not the pass's batched flush), which is
//                what a request its sender then idles for wants. form=1: one coroutine per worker,
//                spawned at DoWork, awaits `qb::ask<ListAsk>(ctx, list, 0, id, kind, value)` once
//                per request and folds each answer; the worker's `on(ListAsk&)` routes it with
//                `resolve_ask`. The frame captures a shared_ptr to the worker's counters, never
//                `this` (qb.llm.md section 4). The list is ALONE on VirtualCore 0, the master and
//                the workers on core 1 (worker w on 1 + w % (cores-1) above two cores): the core
//                that walks the list does nothing else and keeps the list in its own cache, and
//                every request and answer crosses the cores.

#include <qvospec/savina/concsll.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/core/patterns/request.h>
#include <qb/main.h>

#include <algorithm>
#include <concepts>
#include <memory>
#include <vector>

namespace savina_concsll_qb {

using namespace qvospec::savina::concsll;

// form=0: a request and, once the list has written `value`, its answer -- one event for both
// directions, as `reply()` wants, and one per worker for its whole run. `value` is the value to
// insert or look for on the way in, the answer on the way back (the value inserted, 1 / 0 for a
// contains, the length for a size).
struct ListOp : qb::Event {
    std::uint64_t request;  // request_id(); not `id`, which qb::Event already names (the routing key)
    std::uint32_t kind;
    std::int32_t  value;
    ListOp(std::uint64_t r, std::uint32_t k, std::int32_t v) noexcept : request(r), kind(k), value(v) {}
};
// form=1: the same request as an ask. AskEvent carries the correlation id `reply()` preserves.
struct ListAsk : qb::AskEvent {
    std::uint64_t request;
    std::uint32_t kind;
    std::int32_t  value;
    ListAsk(std::uint64_t r, std::uint32_t k, std::int32_t v) noexcept : request(r), kind(k), value(v) {}
};
struct Ready : qb::Event {};  // handshake, outside the window
struct DoWork : qb::Event {};
// worker -> master, after its last answer: its sum of reply terms and the messages it received.
struct End : qb::Event {
    std::uint64_t acc;
    std::uint64_t received;
    End(std::uint64_t a, std::uint64_t r) noexcept : acc(a), received(r) {}
};
struct Finish : qb::Event {};
// list -> master, after the window: its list_term (the final contents and the requests it
// received) and what it counted.
struct Report : qb::Event {
    std::uint64_t term;
    std::uint64_t received;
    ListStats     stats;
    Report(std::uint64_t t, std::uint64_t r, const ListStats &s) noexcept
        : term(t), received(r), stats(s) {}
};

// Where the verified answer is left for the harness: written by the master on its core, read after
// `join()`.
struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    ListStats     stats{};
};

struct Field {
    qb::ActorId              list;
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
};

// The emplace `ask` (qb 3.2, request.h), detected by callability as in the bank-transaction and
// concdict adapters: a qb without it gets the by-value `qb::ask(ctx, target, E{...}, timeout)`,
// chosen at compile time, so each version runs the ask its own request.h recommends.
template <typename Ctx>
concept HasEmplaceAsk = requires(const Ctx &ctx, qb::ActorId id, std::uint64_t i, std::uint32_t k,
                                 std::int32_t v) {
    qb::ask<ListAsk>(ctx, id, qb::duration::zero(), i, k, v);
};

template <typename Ctx>
auto ask_list(const Ctx &ctx, qb::ActorId list, std::uint64_t id, const Request &r) {
    if constexpr (HasEmplaceAsk<Ctx>)
        return qb::ask<ListAsk>(ctx, list, qb::duration::zero(), id, r.kind, r.value);
    else
        return qb::ask(ctx, list, ListAsk{id, r.kind, r.value}, qb::duration::zero());
}

class ListActor final : public qb::Actor {
    const Field  &_field;
    SortedList    _list;
    std::uint64_t _received{0};
    std::uint64_t _requests{0};  // the request terms, as the requests arrived

    // The reference SortedList.process: walk, write the answer into the request, answer the sender.
    template <typename E>
    void serve(E &op) {
        ++_received;
        _requests += request_term(op.request, op.kind, op.value);
        switch (op.kind) {
        case kWrite: _list.add(op.value); break;  // the answer is the value inserted: already there
        case kContains: op.value = _list.contains(op.value) ? 1 : 0; break;
        default: op.value = _list.size(); break;
        }
        reply(op);
    }

public:
    explicit ListActor(const Field &field) : _field(field) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<ListOp>(*this);
        registerEvent<ListAsk>(*this);
        registerEvent<Finish>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    // Non-const: `reply()` consumes the event and sends its bytes back to the worker.
    void on(ListOp &op) { serve(op); }
    void on(ListAsk &op) { serve(op); }

    void on(Finish const &) {
        push<Report>(_field.master, list_term(_list, _requests), _received, _list.stats());
    }
};

// What the form=1 coroutine and the worker share: the worker's handler counts every answer it
// receives, the coroutine reads the count once, after its last answer, to report it.
struct AskState {
    std::uint64_t received{0};
};

class Worker final : public qb::Actor {
    const Field                     &_field;
    const std::vector<std::uint8_t> &_written;
    const std::uint32_t              _index;
    const Config                     _config;
    const bool                       _ask;
    Script                           _script;
    Request                          _asked{};  // form=0: what the request in flight asks
    std::uint64_t                    _seq{0};   // form=0: its index in this worker's sequence
    std::uint64_t                    _acc{0};
    std::uint64_t                    _received{0};
    std::shared_ptr<AskState>        _st;

    void finish() { push<End>(_field.master, _acc, _received); }

    // form=1: the worker's whole sequence in one coroutine. Everything it needs is captured by
    // value before its first suspension; `written` outlives the engine.
    void start_asking() {
        spawn([st = _st, list = _field.list, master = _field.master, written = &_written,
               index = _index, config = _config](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            Script        script(index, config);
            std::uint64_t acc = 0;
            for (std::uint64_t j = 0; j < config.messages; ++j) {
                const Request       asked  = script.next();
                const std::uint64_t id     = request_id(index, j);
                const ListAsk       answer = co_await ask_list(ctx, list, id, asked);
                acc += answer_term(id, asked, answer.request, answer.kind, answer.value, *written);
            }
            ctx.push_to<End>(master, acc, st->received);
        });
    }

public:
    Worker(const Field &field, const std::vector<std::uint8_t> &written, std::uint32_t index,
           Config config, bool ask) noexcept
        : _field(field)
        , _written(written)
        , _index(index)
        , _config(config)
        , _ask(ask)
        , _script(index, config) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<DoWork>(*this);
        if (_ask) {
            _st = std::make_shared<AskState>();
            registerEvent<ListAsk>(*this);
        } else {
            registerEvent<ListOp>(*this);
        }
        push<Ready>(_field.master);
        co_return true;
    }

    void on(DoWork const &) {
        if (_ask) {
            ++_st->received;
            if (_config.messages == 0)
                push<End>(_field.master, std::uint64_t{0}, _st->received);
            else
                start_asking();
            return;
        }
        ++_received;
        if (_config.messages == 0) return finish();
        // The worker's one event for the whole run, sent as the request it then idles for.
        _asked = _script.next();
        send<ListOp>(_field.list, request_id(_index, 0), _asked.kind, _asked.value);
    }

    // form=0: the answer to request `_seq`, checked against what was asked (answer_term); then the
    // same event goes back as the next request -- the reference's Worker.process -- or, after the
    // last, the worker reports.
    void on(ListOp &answer) {
        ++_received;
        if (_seq >= _config.messages) fail("a worker answered after its last request");
        _acc += answer_term(request_id(_index, _seq), _asked, answer.request, answer.kind, answer.value,
                            _written);
        if (++_seq == _config.messages) return finish();
        _asked         = _script.next();
        answer.request = request_id(_index, _seq);
        answer.kind    = _asked.kind;
        answer.value   = _asked.value;
        reply(answer);
    }

    // form=1: every answer belongs to the coroutine's pending ask.
    void on(ListAsk &answer) {
        ++_st->received;
        if (!resolve_ask(answer)) fail("an answer no ask of this worker waits for");
    }
};

class Master final : public qb::Actor {
    const Field  &_field;
    qvo::Watch   &_watch;
    Sink         &_sink;
    std::size_t   _ready{0};
    std::size_t   _ended{0};
    std::uint64_t _received{0};

    void close_window() {
        _watch.stop();  // every request answered and every worker done
        _sink.messages += _received;
        push<Finish>(_field.list);
    }

public:
    Master(const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<End>(*this);
        registerEvent<Report>(*this);
        co_return true;
    }

    // The window opens once the list and every worker have run their onInit: every core is up.
    void on(Ready const &) {
        if (++_ready != _field.workers.size() + 1) return;
        _watch.start();
        if (_field.workers.empty()) return close_window();
        for (const auto &w : _field.workers) push<DoWork>(w);
    }

    void on(End const &end) {
        ++_received;
        _sink.checksum += end.acc;
        _sink.messages += end.received;
        if (++_ended == _field.workers.size()) close_window();
    }

    void on(Report const &report) {
        _sink.checksum += report.term;
        _sink.messages += report.received;
        _sink.stats = report.stats;
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config        c      = Config::of(p);
    const bool          ask    = asks(p);
    const auto          cores  = static_cast<std::uint32_t>(p.get("cores"));
    const bool          spin   = p.get("wait") != 0;
    const std::uint32_t ncores = cores < 1 ? 1u : cores;

    // Which contains answers are fixed: framework-free, before the window (setup).
    const std::vector<std::uint8_t> written = written_values(c);

    // Configure only the cores that will hold an actor.
    std::vector<bool> populated(ncores, false);
    populated[kListCore]           = true;
    populated[master_core(ncores)] = true;
    for (std::uint32_t w = 0; w < c.workers; ++w) populated[worker_core(w, ncores)] = true;

    Sink  sink;
    Field field;
    {
        qb::Main engine;
        for (std::uint32_t k = 0; k < ncores; ++k)
            if (populated[k]) qvoqb::configure_core(engine, static_cast<int>(k), spin);

        field.list   = engine.addActor<ListActor>(static_cast<qb::CoreId>(kListCore), std::cref(field));
        field.master = engine.addActor<Master>(static_cast<qb::CoreId>(master_core(ncores)),
                                               std::cref(field), std::ref(watch), std::ref(sink));
        field.workers.reserve(c.workers);
        for (std::uint32_t w = 0; w < c.workers; ++w)
            field.workers.push_back(engine.addActor<Worker>(static_cast<qb::CoreId>(worker_core(w, ncores)),
                                                            std::cref(field), std::cref(written), w, c,
                                                            ask));

        engine.start();
        engine.join();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed = observations(sink.stats);
    return answer;
}

}  // namespace savina_concsll_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::concsll::params();
    // The reply path this adapter's table cell runs: the reference's own (form=0) until the
    // quiet-host measurement of both forms names the faster one (benchmarks/savina/concsll.md).
    spec.params["form"]    = 0;
    spec.expected          = qvospec::savina::concsll::expected;
    spec.expected_messages = qvospec::savina::concsll::expected_messages;
    spec.observed_at_least = qvospec::savina::concsll::observed_at_least();
    spec.work_unit         = qvospec::savina::concsll::kWorkUnit;
    spec.work_units        = qvospec::savina::concsll::work_units;
    spec.idiom_source      = "form=0: qb.llm.md reply(e) (reuses the received event) + the ping-pong "
                             "adapter; form=1: qb/src/qb/core/patterns/request.h qb::ask + the "
                             "bank-transaction adapter";
    spec.idiom_note        = "form=0: one ListOp event per worker bounces worker <-> list through "
                             "reply() for the whole run; form=1: one coroutine per worker awaits "
                             "qb::ask per request; list alone on VirtualCore 0, master and workers on "
                             "core 1";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "form=0 recycles ONE event per worker for all its requests: reply() swaps destination and "
        "source and hands the same bytes back (qb's documented reply idiom), so no request or answer "
        "is constructed after the first -- CAF builds a message per request and per answer; "
        "SObjectizer's form=0 resends one mutable message the same way. The run's `form` is in its "
        "params; form=1 is qb::ask, a pending-ask slot and a coroutine resume per request");
    spec.caveats.emplace_back(
        "placement is fixed before start: the list actor alone on VirtualCore 0, the master and every "
        "worker on core 1 (1 + w % (cores - 1) above two cores), so with cores=2 every request and "
        "every answer crosses a core and the list's core runs nothing but the list -- the pools place "
        "freely, and may move the list between their threads");
    spec.caveats.emplace_back(qvospec::savina::concsll::kSharedListCaveat);

    return qvo::run(argc, argv, std::move(spec), savina_concsll_qb::body);
}
