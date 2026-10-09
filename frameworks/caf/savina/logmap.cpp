// @benchmark     savina/logmap
// @framework     caf 1.1.0
// @idiom-source  CAF's own examples, shipped in the 1.1.0 tree: examples/message_passing/
//                calculator.cpp (a handler that RETURNS its result, which CAF sends back to the
//                sender) and the ping-pong adapter beside this file (function-based behaviors,
//                stateful_actor, bare payloads instead of atom tuples on the hot path, handles
//                wired once before the window); caf/response_promise.cpp (`respond_to`: a value
//                returned for an ASYNCHRONOUS message is delivered to its sender as an ordinary
//                message, through a stack-held promise -- no allocation beyond the message).
// @idiom-note    The ask is CAF's own reply-to-sender: the worker sends its term as a bare
//                `double` with `mail(term).send(computer)`, the computer's handler RETURNS the
//                next term, and CAF routes it to the worker -- Savina's `sender() ! result`, with
//                no handle in the message. Plain messages, not `request().then()` / `await()`:
//                those allocate a continuation per request and buy nothing for a worker with one
//                request in flight (bank-transaction is the shape that prices CAF's request), and
//                `await()` would hold the mailbox by suspending the behavior, where the held
//                NextTerm requests -- which carry nothing -- are a count here as in every
//                adapter. NextTerm is `update_atom`, GetTerm `get_atom`, the answer
//                `(ok_atom, uint32 index, uint64 chain, uint64 received, uint64 held)`, the stop
//                `close_atom` and the computer's report `(ok_atom, uint32 index, uint64 served,
//                uint64 received)`. Every actor is placed by the work-stealing pool.

#include <qvospec/savina/logmap.h>

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

namespace savina_logmap_caf {

using namespace qvospec::savina::logmap;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t held{0};
};

struct computer_state {
    caf::actor    master;
    std::uint32_t index{0};
    double        rate{0};
    std::uint64_t served{0};
    std::uint64_t received{0};
};

struct worker_state {
    caf::actor    master;
    caf::actor    computer;
    std::uint32_t index{0};
    double        term{0};
    std::uint64_t chain{0};
    std::uint64_t owed{0};  // NextTerm requests held while an answer is awaited
    bool          awaiting{false};
    bool          get_pending{false};
    std::uint64_t received{0};
    std::uint64_t held{0};
};

struct master_state {
    std::vector<caf::actor> workers;
    std::vector<caf::actor> computers;
    std::uint64_t           terms{0};
    std::size_t             wired{0};
    std::size_t             results{0};
    std::size_t             reports{0};
    std::uint64_t           received{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior computer_fun(caf::stateful_actor<computer_state> *self, std::uint32_t index) {
    auto &st = self->state();
    st.index = index;
    st.rate  = rate_of(index);
    return {
        // Wiring, once, outside the window.
        [self](caf::put_atom, caf::actor master) {
            auto &s  = self->state();
            s.master = std::move(master);
            self->mail(caf::ok_atom_v).send(s.master);
        },
        // Savina's RateComputer: the next term, RETURNED -- CAF sends it to the sender.
        [self](double term) -> double {
            auto &s = self->state();
            ++s.received;
            ++s.served;
            return next_term(term, s.rate);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.received;
            self->mail(caf::ok_atom_v, s.index, s.served, s.received).send(s.master);
            self->quit();
        },
    };
}

caf::behavior worker_fun(caf::stateful_actor<worker_state> *self, std::uint32_t index) {
    auto &st = self->state();
    st.index = index;
    st.term  = start_of(index);
    st.chain = chain_seed(index);

    auto answer = [self] {
        auto &s = self->state();
        self->mail(caf::ok_atom_v, s.index, s.chain, s.received, s.held).send(s.master);
    };

    return {
        // Wiring, once, outside the window.
        [self](caf::put_atom, caf::actor master, caf::actor computer) {
            auto &s    = self->state();
            s.master   = std::move(master);
            s.computer = std::move(computer);
            self->mail(caf::ok_atom_v).send(s.master);
        },
        [self](caf::update_atom) {
            auto &s = self->state();
            ++s.received;
            if (s.awaiting) {
                ++s.owed;
                ++s.held;
                return;
            }
            s.awaiting = true;
            self->mail(s.term).send(s.computer);
        },
        // The computer's answer. Still owing a term: ask for the next one at once.
        [self, answer](double next) {
            auto &s = self->state();
            ++s.received;
            s.term  = next;
            s.chain = chain_step(s.chain, next);
            if (s.owed) {
                --s.owed;
                self->mail(s.term).send(s.computer);
                return;
            }
            s.awaiting = false;
            if (s.get_pending) answer();
        },
        [self, answer](caf::get_atom) {
            auto &s = self->state();
            ++s.received;
            if (s.awaiting)
                s.get_pending = true;  // answered when the last answer lands
            else
                answer();
        },
        // Teardown, after the window: not counted.
        [self](caf::close_atom) { self->quit(); },
    };
}

caf::behavior master_fun(caf::stateful_actor<master_state> *self, std::vector<caf::actor> workers,
                         std::vector<caf::actor> computers, std::uint64_t terms, qvo::Watch *watch,
                         Sink *sink) {
    auto &st     = self->state();
    st.workers   = std::move(workers);
    st.computers = std::move(computers);
    st.terms     = terms;
    st.watch     = watch;
    st.sink      = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (std::size_t i = 0; i < st.workers.size(); ++i)
        self->mail(caf::put_atom_v, me, st.computers[i]).send(st.workers[i]);
    for (auto &c : st.computers) self->mail(caf::put_atom_v, me).send(c);

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.workers.size() + s.computers.size()) return;
            s.watch->start();
            // The reference's loop: term by term, worker by worker, then one GetTerm each.
            for (std::uint64_t k = 0; k < s.terms; ++k)
                for (auto &w : s.workers) self->mail(caf::update_atom_v).send(w);
            for (auto &w : s.workers) self->mail(caf::get_atom_v).send(w);
        },
        // A worker's answer.
        [self](caf::ok_atom, std::uint32_t index, std::uint64_t chain, std::uint64_t received,
               std::uint64_t held) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += series_key(index, chain);
            s.sink->messages += received;
            s.sink->held += held;
            if (++s.results != s.workers.size()) return;
            for (auto &c : s.computers) self->mail(caf::close_atom_v).send(c);
        },
        // A computer's report.
        [self](caf::ok_atom, std::uint32_t index, std::uint64_t served, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += computer_key(index, served);
            s.sink->messages += received;
            if (++s.reports != s.computers.size()) return;
            s.sink->messages += s.received;
            s.watch->stop();
            for (auto &w : s.workers) self->mail(caf::close_atom_v).send(w);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto terms  = static_cast<std::uint64_t>(p.get("terms"));
    const auto series = static_cast<std::uint32_t>(p.get("series"));
    const auto cores  = static_cast<std::size_t>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> workers;
        std::vector<caf::actor> computers;
        workers.reserve(series);
        computers.reserve(series);
        for (std::uint32_t i = 0; i < series; ++i) {
            computers.push_back(sys.spawn<qvocaf::kSpawnOptions>(computer_fun, i));
            workers.push_back(sys.spawn<qvocaf::kSpawnOptions>(worker_fun, i));
        }
        sys.spawn<qvocaf::kSpawnOptions>(master_fun, workers, computers, terms, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kHeld] = sink.held;
    return answer;
}

}  // namespace savina_logmap_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::logmap::params();
    spec.expected          = qvospec::savina::logmap::expected;
    spec.expected_messages = qvospec::savina::logmap::expected_messages;
    spec.work_unit         = qvospec::savina::logmap::kWorkUnit;
    spec.work_units        = qvospec::savina::logmap::work_units;
    spec.idiom_source      = "examples/message_passing/calculator.cpp (a returned value goes "
                             "back to the sender) + the ping-pong adapter";
    spec.idiom_note        = "mail(term).send(computer), the computer RETURNS the next term and "
                             "CAF routes it to the sender; held NextTerms are a count; placement "
                             "left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "a worker and its computer are placed by the work-stealing pool: a computer made ready "
        "by its worker's message is prepended to the worker's own queue, so a pair usually runs "
        "on one thread, but which pairs share a thread and whether a round trip crosses a core is "
        "the scheduler's decision; qb's cell pins series i (worker + computer) on core "
        "(1 + i) % cores -- see benchmarks/savina/logmap.md");
    spec.caveats.emplace_back(
        "the computer's answer is a value RETURNED from its handler, which CAF sends to the "
        "sender of an asynchronous message as an ordinary message (response_promise::respond_to, "
        "no allocation beyond the message itself); every hop allocates its message, which is "
        "CAF's model -- qb and SObjectizer recycle one event per chain");

    return qvo::run(argc, argv, std::move(spec), savina_logmap_caf::body);
}
