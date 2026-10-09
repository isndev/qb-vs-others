// @benchmark     savina/concsll
// @framework     caf 1.1.0
// @idiom-source  CAF's own examples, shipped in the 1.1.0 tree:
//                  examples/message_passing/calculator.cpp -- a server whose handlers RETURN the
//                    answer (`[](add_atom, int32_t a, int32_t b) { return a + b; }`);
//                  libcaf_core/caf/response_promise.cpp (`respond_to` / `deliver_impl`) -- a returned
//                    value goes back to the SENDER, as an ordinary message when the input was an
//                    ordinary message (its response id is the default async id);
//                and the chameneos / bank-transaction adapters beside this file (function-based
//                behaviors, stateful_actor, built-in atoms, handles exchanged before the window).
// @idiom-note    The list is CAF's calculator: each request handler RETURNS the answer and CAF
//                routes it to the worker that sent the request. form=0, the reference's shape and
//                concdict's: the worker sends with a plain `mail(...).send(list)`, so the answer
//                comes back as an ordinary message and the worker's behavior takes it and sends
//                the next request -- Savina's worker has no continuation. form=1: the worker sends
//                with `mail(...).request(list, infinite).then(...)`, CAF's request/response
//                primitive, a response handler and a request id per request. Every request and
//                every answer is a new CAF message either way: CAF messages are immutable and
//                cannot be sent back. The request kind is the message signature, built-in atoms
//                only: a write is `(add_atom, id, value)`, a contains `(get_atom, id, value)`, a
//                size `(get_atom, id)`; the answer is `(id, kind, value)`. Every actor is placed
//                by the work-stealing pool -- including the list, which the pool may move between
//                its threads.

#include <qvospec/savina/concsll.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/result.hpp>
#include <caf/stateful_actor.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace savina_concsll_caf {

using namespace qvospec::savina::concsll;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    ListStats     stats{};
};

// The answer as the list's handlers return it; CAF sends it back to the asking worker.
using answer_t = caf::result<std::uint64_t, std::uint32_t, std::int32_t>;

struct list_state {
    caf::actor    master;
    SortedList    list;
    std::uint64_t received{0};
};

struct worker_state {
    caf::actor                       list;
    caf::actor                       master;
    const std::vector<std::uint8_t> *written{nullptr};
    std::uint32_t                    index{0};
    std::uint64_t                    messages{0};
    bool                             ask{false};  // form=1
    std::optional<Script>            script;      // built in worker_fun: a Script has no default
    Request                          asked{};
    std::uint64_t                    seq{0};
    std::uint64_t                    acc{0};
    std::uint64_t                    received{0};
};

struct master_state {
    caf::actor              list;
    std::vector<caf::actor> workers;
    std::size_t             ready{0};
    std::size_t             ended{0};
    std::uint64_t           received{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior list_fun(caf::stateful_actor<list_state> *self) {
    return {
        // Wiring handshake, once, outside the window.
        [self](caf::put_atom, caf::actor master) {
            auto &s  = self->state();
            s.master = std::move(master);
            self->mail(caf::ok_atom_v).send(s.master);
        },
        // The hot path: walk, then answer -- the returned value goes back to the sender.
        [self](caf::add_atom, std::uint64_t id, std::int32_t value) -> answer_t {
            auto &s = self->state();
            ++s.received;
            s.list.add(value);
            return {id, static_cast<std::uint32_t>(kWrite), value};
        },
        [self](caf::get_atom, std::uint64_t id, std::int32_t value) -> answer_t {
            auto &s = self->state();
            ++s.received;
            return {id, static_cast<std::uint32_t>(kContains),
                    static_cast<std::int32_t>(s.list.contains(value) ? 1 : 0)};
        },
        [self](caf::get_atom, std::uint64_t id) -> answer_t {
            auto &s = self->state();
            ++s.received;
            return {id, static_cast<std::uint32_t>(kSize), s.list.size()};
        },
        // After the window: the final contents and the counts, then done.
        [self](caf::close_atom) {
            auto            &s = self->state();
            const ListStats &t = s.list.stats();
            self->mail(caf::ok_atom_v, s.list.fold(), s.received, t.contains_walk, t.write_walk, t.size_walk,
                       t.contains_found)
                .send(s.master);
            self->quit();
        },
    };
}

using worker_actor = caf::stateful_actor<worker_state>;

void answered(worker_actor *self, std::uint64_t id, std::uint32_t kind, std::int32_t value);

// The next request of the worker's sequence: a plain send (form=0) or a request whose response
// goes to a `then` continuation (form=1).
void issue(worker_actor *self) {
    auto &s                = self->state();
    s.asked                = s.script->next();
    const std::uint64_t id = request_id(s.index, s.seq);
    if (!s.ask) {
        switch (s.asked.kind) {
        case kWrite: self->mail(caf::add_atom_v, id, s.asked.value).send(s.list); break;
        case kContains: self->mail(caf::get_atom_v, id, s.asked.value).send(s.list); break;
        default: self->mail(caf::get_atom_v, id).send(s.list); break;
        }
        return;
    }
    auto on_answer = [self](std::uint64_t i, std::uint32_t k, std::int32_t v) { answered(self, i, k, v); };
    switch (s.asked.kind) {
    case kWrite:
        self->mail(caf::add_atom_v, id, s.asked.value).request(s.list, caf::infinite).then(on_answer);
        break;
    case kContains:
        self->mail(caf::get_atom_v, id, s.asked.value).request(s.list, caf::infinite).then(on_answer);
        break;
    default: self->mail(caf::get_atom_v, id).request(s.list, caf::infinite).then(on_answer); break;
    }
}

void finish(worker_actor *self) {
    auto &s = self->state();
    self->mail(caf::close_atom_v, s.acc, s.received).send(s.master);
    self->quit();
}

// The answer to the request in flight, checked against what was asked (reply_term); the next
// request leaves from here -- the reference's Worker.process.
void answered(worker_actor *self, std::uint64_t id, std::uint32_t kind, std::int32_t value) {
    auto &s = self->state();
    ++s.received;
    if (s.seq >= s.messages) fail("a worker answered after its last request");
    s.acc += reply_term(request_id(s.index, s.seq), s.asked.kind, id, kind,
                        asserted_result(s.asked, value, *s.written));
    if (++s.seq == s.messages)
        finish(self);
    else
        issue(self);
}

caf::behavior worker_fun(worker_actor *self, caf::actor list, std::uint32_t index, Config config,
                         bool ask, const std::vector<std::uint8_t> *written) {
    auto &st    = self->state();
    st.list     = std::move(list);
    st.written  = written;
    st.index    = index;
    st.messages = config.messages;
    st.ask      = ask;
    st.script.emplace(index, config);

    return {
        [self](caf::put_atom, caf::actor master) {
            auto &s  = self->state();
            s.master = std::move(master);
            self->mail(caf::ok_atom_v).send(s.master);
        },
        // DoWork: the window is open.
        [self](caf::open_atom) {
            auto &s = self->state();
            ++s.received;
            if (s.messages == 0)
                finish(self);
            else
                issue(self);
        },
        // form=0: the answer, an ordinary message (form=1 answers go to the `then` in issue()).
        [self](std::uint64_t id, std::uint32_t kind, std::int32_t value) {
            answered(self, id, kind, value);
        },
    };
}

void close_window(caf::stateful_actor<master_state> *self) {
    auto &s = self->state();
    s.watch->stop();
    s.sink->messages += s.received;
    self->mail(caf::close_atom_v).send(s.list);
}

caf::behavior master_fun(caf::stateful_actor<master_state> *self, caf::actor list,
                         std::vector<caf::actor> workers, qvo::Watch *watch, Sink *sink) {
    auto &st   = self->state();
    st.list    = std::move(list);
    st.workers = std::move(workers);
    st.watch   = watch;
    st.sink    = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    self->mail(caf::put_atom_v, me).send(st.list);
    for (auto &w : st.workers) self->mail(caf::put_atom_v, me).send(w);

    return {
        // The list and every worker have been scheduled at least once: the window opens.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size() + 1) return;
            s.watch->start();
            if (s.workers.empty()) return close_window(self);
            for (auto &w : s.workers) self->mail(caf::open_atom_v).send(w);
        },
        [self](caf::close_atom, std::uint64_t acc, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += acc;
            s.sink->messages += received;
            if (++s.ended == s.workers.size()) close_window(self);
        },
        [self](caf::ok_atom, std::uint64_t fold, std::uint64_t received, std::uint64_t contains_walk,
               std::uint64_t write_walk, std::uint64_t size_walk, std::uint64_t contains_found) {
            auto &s = self->state();
            s.sink->checksum += fold;
            s.sink->messages += received;
            s.sink->stats = ListStats{contains_walk, write_walk, size_walk, contains_found};
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = Config::of(p);
    const bool   ask   = asks(p);
    const auto   cores = static_cast<std::size_t>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    // Which contains answers are fixed: framework-free, before the window (setup).
    const std::vector<std::uint8_t> written = written_values(c);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        auto                    list = sys.spawn<qvocaf::kSpawnOptions>(list_fun);
        std::vector<caf::actor> workers;
        workers.reserve(c.workers);
        for (std::uint32_t w = 0; w < c.workers; ++w)
            workers.push_back(sys.spawn<qvocaf::kSpawnOptions>(worker_fun, list, w, c, ask, &written));
        sys.spawn<qvocaf::kSpawnOptions>(master_fun, list, workers, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed = observations(sink.stats);
    return answer;
}

}  // namespace savina_concsll_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

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
    spec.idiom_source      = "CAF 1.1.0 examples/message_passing/calculator.cpp (handlers return the answer) "
                             "+ libcaf_core/caf/response_promise.cpp + the chameneos adapter";
    spec.idiom_note        = "the list handler returns the answer and CAF routes it to the sender; "
                             "form=0: plain mail(...).send(list), the answer an ordinary message; "
                             "form=1: mail(...).request(list, infinite).then(); placement left to the "
                             "pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the list actor is placed by the work-stealing pool like every other actor: which of the cores "
        "walks the list, and whether it changes thread between two requests (and so whether its list "
        "stays in one core's cache), is the scheduler's decision -- qb's cell pins the list alone on "
        "VirtualCore 0, see benchmarks/savina/concsll.md");
    spec.caveats.emplace_back(qvospec::savina::concsll::kSharedListCaveat);

    return qvo::run(argc, argv, std::move(spec), savina_concsll_caf::body);
}
