// @benchmark     savina/concdict
// @framework     qb
// @idiom-source  form=0: qb/llm/qb.llm.md "reply vs forward" (`reply(e)` swaps dest and source and
//                reuses the received event, the same idiom qb's own ping-pong benchmark and the
//                ping-pong adapter beside this file use). form=1: qb/src/qb/core/patterns/request.h
//                (`qb::ask`, `AskEvent`, `resolve_ask`) and the bank-transaction adapter beside this
//                file (the spawned coroutine, the emplace ask detected by callability).
// @idiom-note    form=0, the reference's own shape: a worker's request is ONE event that bounces
//                between the worker and the dictionary for the worker's whole run. The dictionary
//                does the lookup, writes the answer into the event and `reply()`s it; the worker
//                folds the answer, rewrites the same event into its next request and `reply()`s it
//                back -- no event is constructed after the first, and `reply()` hands it to the
//                peer at once (VirtualCore::reply is a `send`, not the pass's batched flush), which
//                is what a request its sender then idles for wants (qb.llm.md, send vs push).
//                form=1: one coroutine per worker, spawned at Start, awaits
//                `qb::ask<OpAsk>(ctx, dictionary, 0, key, value, write)` once per request and folds
//                each answer; the worker's `on(OpAsk&)` routes it with `resolve_ask`. The frame
//                captures a shared_ptr to the worker's counter, never `this` (qb.llm.md section 4).
//                Master and dictionary on VirtualCore 0, worker w on core (1 + w) % cores.

#include <qvospec/savina/concdict.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/core/patterns/request.h>
#include <qb/main.h>

#include <concepts>
#include <memory>
#include <vector>

namespace savina_concdict_qb {

using namespace qvospec::savina::concdict;

// A request and, once the dictionary has written `value`, its answer: one event for both
// directions, as `reply()` wants. `value` is the value a write stores on the way in, and the value
// stored (a write) or read (a read) on the way back.
struct Op : qb::Event {
    std::uint32_t key;
    std::uint32_t value;
    bool          write;
    Op(std::uint32_t k, std::uint32_t v, bool w) noexcept : key(k), value(v), write(w) {}
};
// The same request as an ask (form=1): AskEvent carries the correlation id `reply()` preserves.
struct OpAsk : qb::AskEvent {
    std::uint32_t key;
    std::uint32_t value;
    bool          write;
    OpAsk(std::uint32_t k, std::uint32_t v, bool w) noexcept : key(k), value(v), write(w) {}
};
struct Ready : qb::Event {};  // handshake, outside the window
struct Start : qb::Event {};
struct Done : qb::Event {
    std::uint32_t worker;
    std::uint64_t fold;
    std::uint64_t received;
    Done(std::uint32_t w, std::uint64_t f, std::uint64_t r) noexcept : worker(w), fold(f), received(r) {}
};
struct Exit : qb::Event {};
struct Report : qb::Event {
    std::uint64_t digest;
    std::uint64_t received;
    Report(std::uint64_t d, std::uint64_t r) noexcept : digest(d), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct Field {
    qb::ActorId              master;
    qb::ActorId              dictionary;
    std::vector<qb::ActorId> workers;
};

// The emplace `ask` (qb 3.2, request.h), detected by callability as in the bank-transaction
// adapter: a qb without it gets the by-value `qb::ask(ctx, target, E{...}, timeout)`, chosen at
// compile time, so each version runs the ask its own request.h recommends.
template <typename Ctx>
concept HasEmplaceAsk = requires(const Ctx &ctx, qb::ActorId id, std::uint32_t v, bool w) {
    qb::ask<OpAsk>(ctx, id, qb::duration::zero(), v, v, w);
};

template <typename Ctx>
auto ask_dictionary(const Ctx &ctx, qb::ActorId dictionary, const Operation &o) {
    if constexpr (HasEmplaceAsk<Ctx>)
        return qb::ask<OpAsk>(ctx, dictionary, qb::duration::zero(), o.key, o.value, o.write);
    else
        return qb::ask(ctx, dictionary, OpAsk{o.key, o.value, o.write}, qb::duration::zero());
}

class Dictionary final : public qb::Actor {
    const Field  &_field;
    Store        &_store;
    std::uint64_t _received{0};

    // Savina's Dictionary.process: the lookup or the put, answered to the sender.
    template <typename E>
    void serve(E &event) {
        ++_received;
        event.value = event.write ? _store.write(event.key, event.value) : _store.read(event.key);
        reply(event);
    }

public:
    Dictionary(const Field &field, Store &store) noexcept : _field(field), _store(store) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Op>(*this);
        registerEvent<OpAsk>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Op &event) { serve(event); }
    void on(OpAsk &event) { serve(event); }

    void on(Exit const &) {
        ++_received;
        push<Report>(_field.master, _store.digest(), _received);
    }
};

// What the form=1 coroutine and the worker share: the worker counts what it receives, the
// coroutine reads the count once, after its last answer, to report it.
struct AskState {
    std::uint64_t received{0};
};

class Worker final : public qb::Actor {
    const Field              &_field;
    const std::uint32_t       _index;
    const Shape               _shape;
    const bool                _ask;
    std::uint64_t             _next{0};  // form=0: the index of the request in flight
    std::uint64_t             _fold{0};
    std::uint64_t             _received{0};
    std::shared_ptr<AskState> _st;

    void start_asking() {
        spawn([st = _st, dictionary = _field.dictionary, master = _field.master, shape = _shape,
               index = _index](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            std::uint64_t fold = 0;
            for (std::uint64_t j = 0; j < shape.messages; ++j) {
                const Operation o      = operation(shape, index, j);
                const auto      answer = co_await ask_dictionary(ctx, dictionary, o);
                fold += reply_key(index, j, answer.value);
            }
            ctx.push_to<Done>(master, index, fold, st->received);
        });
    }

public:
    Worker(const Field &field, std::uint32_t index, Shape shape, bool ask) noexcept
        : _field(field), _index(index), _shape(shape), _ask(ask) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Start>(*this);
        if (_ask) {
            _st = std::make_shared<AskState>();
            registerEvent<OpAsk>(*this);
        } else {
            registerEvent<Op>(*this);
        }
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Start const &) {
        if (_ask) {
            ++_st->received;
            start_asking();
            return;
        }
        ++_received;
        const Operation o = operation(_shape, _index, 0);
        push<Op>(_field.dictionary, o.key, o.value, o.write);
    }

    // form=0: the answer to request `_next`. Folded, then the same event goes back as the next
    // request -- or, after the last, the worker reports.
    void on(Op &event) {
        ++_received;
        if (_next >= _shape.messages) fail("a worker answered after its last request");
        _fold += reply_key(_index, _next, event.value);
        if (++_next == _shape.messages) {
            push<Done>(_field.master, _index, _fold, _received);
            return;
        }
        const Operation o = operation(_shape, _index, _next);
        event.key         = o.key;
        event.value       = o.value;
        event.write       = o.write;
        reply(event);
    }

    // form=1: every answer belongs to the coroutine's pending ask.
    void on(OpAsk &event) {
        ++_st->received;
        if (!resolve_ask(event)) fail("an answer no ask of this worker waits for");
    }
};

class Master final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _workers;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::uint32_t       _ready{0};
    std::uint32_t       _done{0};
    std::uint64_t       _received{0};

public:
    Master(const Field &field, std::uint32_t workers, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _workers(workers), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Done>(*this);
        registerEvent<Report>(*this);
        co_return true;
    }

    // Every worker and the dictionary are up: open the window and start the workers.
    void on(Ready const &) {
        if (++_ready != _workers + 1) return;
        _watch.start();
        for (const auto &w : _field.workers) push<Start>(w);
    }

    void on(Done const &event) {
        ++_received;
        _sink.checksum += done_key(event.worker, event.fold);
        _sink.messages += event.received;
        if (++_done != _workers) return;
        _watch.stop();  // every request answered and every worker done
        push<Exit>(_field.dictionary);
    }

    void on(Report const &event) {
        ++_received;
        _sink.checksum += digest_key(event.digest);
        _sink.messages += event.received + _received;
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape shape = qvospec::savina::concdict::shape(p);
    const bool  ask   = asks(p);
    const auto  cores = static_cast<int>(p.get("cores"));
    const bool  spin  = p.get("wait") != 0;

    Store store(shape.keys);  // before the engine, by this thread -- as in every adapter
    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.master     = engine.addActor<Master>(0, std::cref(field), shape.workers, std::ref(watch),
                                                   std::ref(sink));
        field.dictionary = engine.addActor<Dictionary>(0, std::cref(field), std::ref(store));
        field.workers.reserve(shape.workers);
        for (std::uint32_t w = 0; w < shape.workers; ++w) {
            // The dictionary is on core 0; worker w on core (1 + w) % cores, so the dictionary
            // shares its core with half the workers and the other half is one pipe away.
            const int core = (1 + static_cast<int>(w)) % ncores;
            field.workers.push_back(engine.addActor<Worker>(static_cast<qb::CoreId>(core),
                                                            std::cref(field), w, shape, ask));
        }

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_concdict_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::concdict::params();
    // The reply path this adapter's table cell runs: the reference's own (form=0) until the
    // quiet-host measurement of both forms names the faster one (benchmarks/savina/concdict.md).
    spec.params["form"]    = 0;
    spec.expected          = qvospec::savina::concdict::expected;
    spec.expected_messages = qvospec::savina::concdict::expected_messages;
    spec.work_unit         = qvospec::savina::concdict::kWorkUnit;
    spec.work_units        = qvospec::savina::concdict::work_units;
    spec.idiom_source      = "form=0: qb.llm.md reply(e) (reuses the received event) + the ping-pong "
                             "adapter; form=1: qb/src/qb/core/patterns/request.h qb::ask + the "
                             "bank-transaction adapter";
    spec.idiom_note        = "form=0: one Op event per worker bounces worker <-> dictionary through "
                             "reply() for the whole run; form=1: one coroutine per worker awaits "
                             "qb::ask per request; master and dictionary on VirtualCore 0, worker w "
                             "on core (1 + w) % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "form=0 recycles ONE event per worker for all its requests: reply() swaps destination and "
        "source and hands the same bytes back (qb's documented reply idiom), so no request or answer "
        "is constructed after the first -- CAF builds a message per request and per answer; "
        "SObjectizer's form=0 resends one mutable message the same way. The run's `form` is in its "
        "params; form=1 is qb::ask, a pending-ask slot and a coroutine resume per request");
    spec.caveats.emplace_back(
        "placement is fixed before start: the master and the dictionary on VirtualCore 0, worker w "
        "on core (1 + w) % cores, so with cores=2 half the round trips cross a core and the "
        "dictionary's core also runs half the workers -- the pools place freely");
    spec.caveats.emplace_back(
        "the dictionary is the spec's Store (one std::unordered_map of `keys` entries, the same "
        "object code in every adapter), built before the engine by the thread that runs the "
        "repetition; its lookups are part of every framework's figure");

    return qvo::run(argc, argv, std::move(spec), savina_concdict_qb::body);
}
