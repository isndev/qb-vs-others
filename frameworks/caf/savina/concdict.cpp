// @benchmark     savina/concdict
// @framework     caf 1.1.0
// @idiom-source  the bank-transaction and chameneos adapters beside this file (function-based
//                behaviors, stateful_actor, handles exchanged before the window, built-in atoms);
//                caf/response_promise.cpp `respond_to` + caf/detail/default_invoke_result_visitor.hpp
//                (a handler's RESULT is sent back to the sender of the message it handled -- for an
//                ordinary message as an ordinary message, `message_id::response_id()` of an async
//                id being async) and caf/event_based_mail.hpp for `mail(...).request(...).then(...)`.
// @idiom-note    The dictionary's two handlers RETURN the answer -- `(get_atom, key) -> value`,
//                `(put_atom, key, value) -> value` -- and CAF routes it to the sender, so the
//                dictionary never names a worker. form=0, the reference's own shape (Akka
//                `sender ! result`): a worker `mail(...).send(dictionary)`s its request and its
//                behavior takes the answer as an ordinary `(uint32)` message. form=1, CAF's own
//                request/response: `mail(...).request(dictionary, infinite).then(...)`, the
//                continuation a heap-allocated behavior keyed by the request id -- `then`, not
//                `await`, as in bank-transaction. Wiring `(put_atom, master)`, start `tick_atom`,
//                a worker's report `(ok_atom, worker, fold, received)`, the dictionary's exit
//                `close_atom` and report `(ok_atom, digest, received)`. Every actor is placed by
//                the work-stealing pool.

#include <qvospec/savina/concdict.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_concdict_caf {

using namespace qvospec::savina::concdict;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct dictionary_state {
    Store        *store{nullptr};
    caf::actor    master;
    std::uint64_t received{0};
};

struct worker_state {
    caf::actor    master;
    caf::actor    dictionary;
    Shape         shape{};
    std::uint32_t index{0};
    bool          ask{false};
    std::uint64_t next{0};  // the index of the request in flight
    std::uint64_t fold{0};
    std::uint64_t received{0};
};

struct master_state {
    caf::actor              dictionary;
    std::vector<caf::actor> workers;
    std::uint32_t           ready{0};
    std::uint32_t           done{0};
    std::uint64_t           received{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

using worker_actor = caf::stateful_actor<worker_state>;

void answered(worker_actor *self, std::uint32_t value);

// Request `next` of this worker, in the run's form.
void issue(worker_actor *self) {
    auto           &s = self->state();
    const Operation o = operation(s.shape, s.index, s.next);
    if (!s.ask) {
        if (o.write)
            self->mail(caf::put_atom_v, o.key, o.value).send(s.dictionary);
        else
            self->mail(caf::get_atom_v, o.key).send(s.dictionary);
        return;
    }
    auto then = [self](std::uint32_t value) { answered(self, value); };
    if (o.write)
        self->mail(caf::put_atom_v, o.key, o.value).request(s.dictionary, caf::infinite).then(then);
    else
        self->mail(caf::get_atom_v, o.key).request(s.dictionary, caf::infinite).then(then);
}

// The answer to request `next`: folded, then the next request -- or, after the last, the report.
void answered(worker_actor *self, std::uint32_t value) {
    auto &s = self->state();
    ++s.received;  // the answer: received by this worker like any other message
    if (s.next >= s.shape.messages) fail("a worker answered after its last request");
    s.fold += reply_key(s.index, s.next, value);
    if (++s.next == s.shape.messages) {
        self->mail(caf::ok_atom_v, s.index, s.fold, s.received).send(s.master);
        self->quit();
        return;
    }
    issue(self);
}

caf::behavior dictionary_fun(caf::stateful_actor<dictionary_state> *self, Store *store) {
    self->state().store = store;
    return {
        // Wiring handshake, once, outside the window.
        [self](caf::put_atom, caf::actor master) {
            auto &s  = self->state();
            s.master = std::move(master);
            self->mail(caf::ok_atom_v).send(s.master);
        },
        [self](caf::get_atom, std::uint32_t key) -> std::uint32_t {
            auto &s = self->state();
            ++s.received;
            return s.store->read(key);
        },
        [self](caf::put_atom, std::uint32_t key, std::uint32_t value) -> std::uint32_t {
            auto &s = self->state();
            ++s.received;
            return s.store->write(key, value);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.received;
            self->mail(caf::ok_atom_v, s.store->digest(), s.received).send(s.master);
            self->quit();
        },
    };
}

caf::behavior worker_fun(worker_actor *self, caf::actor dictionary, std::uint32_t index, Shape shape,
                         bool ask) {
    auto &s      = self->state();
    s.dictionary = std::move(dictionary);
    s.index      = index;
    s.shape      = shape;
    s.ask        = ask;
    return {
        [self](caf::put_atom, caf::actor master) {
            auto &st  = self->state();
            st.master = std::move(master);
            self->mail(caf::ok_atom_v).send(st.master);
        },
        [self](caf::tick_atom) {
            ++self->state().received;
            issue(self);
        },
        // form=0: the dictionary's handler result, sent back to this worker as an ordinary
        // message. With form=1 every answer is a response and goes to the request's `then`.
        [self](std::uint32_t value) { answered(self, value); },
    };
}

caf::behavior master_fun(caf::stateful_actor<master_state> *self, caf::actor dictionary,
                         std::vector<caf::actor> workers, qvo::Watch *watch, Sink *sink) {
    auto &st      = self->state();
    st.dictionary = std::move(dictionary);
    st.workers    = std::move(workers);
    st.watch      = watch;
    st.sink       = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    self->mail(caf::put_atom_v, me).send(st.dictionary);
    for (auto &w : st.workers) self->mail(caf::put_atom_v, me).send(w);

    return {
        // Every worker and the dictionary are wired: open the window and start the workers.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size() + 1) return;
            s.watch->start();
            for (auto &w : s.workers) self->mail(caf::tick_atom_v).send(w);
        },
        [self](caf::ok_atom, std::uint32_t worker, std::uint64_t fold, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += done_key(worker, fold);
            s.sink->messages += received;
            if (++s.done != s.workers.size()) return;
            s.watch->stop();  // every request answered and every worker done
            self->mail(caf::close_atom_v).send(s.dictionary);
        },
        [self](caf::ok_atom, std::uint64_t digest, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += digest_key(digest);
            s.sink->messages += received + s.received;
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape shape = qvospec::savina::concdict::shape(p);
    const bool  ask   = asks(p);
    const auto  cores = static_cast<std::size_t>(p.get("cores"));
    const bool  spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Store store(shape.keys);  // before the actor system, by this thread -- as in every adapter
    Sink  sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        auto dictionary = sys.spawn<qvocaf::kSpawnOptions>(dictionary_fun, &store);
        std::vector<caf::actor> workers;
        workers.reserve(shape.workers);
        for (std::uint32_t w = 0; w < shape.workers; ++w)
            workers.push_back(sys.spawn<qvocaf::kSpawnOptions>(worker_fun, dictionary, w, shape, ask));
        sys.spawn<qvocaf::kSpawnOptions>(master_fun, dictionary, workers, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_concdict_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

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
    spec.idiom_source      = "caf/response_promise.cpp respond_to (a handler's result goes back to "
                             "the sender) + caf/event_based_mail.hpp request().then() + the "
                             "bank-transaction adapter";
    spec.idiom_note        = "the dictionary's handlers return the answer; form=0: mail().send() and "
                             "the answer as an ordinary message to the worker's behavior; form=1: "
                             "mail().request(dictionary, infinite).then(); placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "every request and every answer is a new CAF message (a ref-counted, type-erased tuple): "
        "CAF messages are immutable and cannot be sent back the way qb's reply() and SObjectizer's "
        "mutable-message resend recycle one; with form=1 each request also allocates its "
        "continuation, keyed by the response id. The run's `form` is in its params");
    spec.caveats.emplace_back(
        "the master, the dictionary and the 20 workers are placed by the work-stealing pool; qb's "
        "cell pins the master and the dictionary on core 0 and worker w on core (1 + w) % cores "
        "-- see benchmarks/savina/concdict.md");
    spec.caveats.emplace_back(
        "the dictionary is the spec's Store (one std::unordered_map of `keys` entries, the same "
        "object code in every adapter), built before the actor system by the thread that runs the "
        "repetition; its lookups are part of every framework's figure");

    return qvo::run(argc, argv, std::move(spec), savina_concdict_caf::body);
}
