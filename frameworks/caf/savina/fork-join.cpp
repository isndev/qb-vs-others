// @benchmark     savina/fork-join
// @framework     caf 1.1.0
// @idiom-source  the ping-pong and counting adapters beside this file (function-based
//                behaviors, stateful_actor, handles exchanged before the window), the master's
//                loop being counting's producer loop over sixty destinations.
// @idiom-note    A job is a bare `uint64` index -- the worker's one hot handler needs no atom --
//                and a worker's answer is `(ok_atom, acc, received)`. Each worker learns the
//                master once in a handshake; the master is what CAF calls a stateful_actor and
//                the workers are scheduled wherever the pool puts them, which on a fan-out is
//                the case work stealing is FOR.

#include <qvospec/savina/fork-join.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_fork_join_caf {

using namespace qvospec::savina::fork_join;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct worker_state {
    caf::actor    master;
    std::uint64_t self{0};
    std::uint64_t messages{0};
    int           work{0};
    std::uint64_t acc{0};
    std::uint64_t received{0};
};

struct master_state {
    std::vector<caf::actor> workers;
    std::uint64_t           messages{0};
    std::size_t             ready{0};
    std::size_t             done{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior worker_fun(caf::stateful_actor<worker_state> *self, std::uint64_t index,
                         std::uint64_t messages, int work) {
    auto &st    = self->state();
    st.self     = index;
    st.messages = messages;
    st.work     = work;
    return {
        // Handshake: learn the master once, acknowledge.
        [self](caf::actor master) {
            self->state().master = master;
            self->mail(caf::ok_atom_v).send(master);
        },
        // The hot path.
        [self](std::uint64_t index) {
            auto &s = self->state();
            s.acc += job_value(s.self, index, s.work);
            if (++s.received == s.messages)
                self->mail(caf::ok_atom_v, s.acc, s.received).send(s.master);
        },
    };
}

caf::behavior master_fun(caf::stateful_actor<master_state> *self, std::vector<caf::actor> workers,
                         std::uint64_t messages, qvo::Watch *watch, Sink *sink) {
    auto &st    = self->state();
    st.workers  = std::move(workers);
    st.messages = messages;
    st.watch    = watch;
    st.sink     = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &w : st.workers) self->mail(me).send(w);

    return {
        // Every worker has been scheduled once and knows the master: the window opens on message
        // passing, not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size()) return;
            s.watch->start();
            for (std::uint64_t i = 0; i < s.messages; ++i)
                for (auto &w : s.workers) self->mail(i).send(w);
        },
        [self](caf::ok_atom, std::uint64_t acc, std::uint64_t received) {
            auto &s = self->state();
            s.sink->checksum += acc;
            s.sink->messages += received + 1;
            if (++s.done != s.workers.size()) return;
            s.watch->stop();
            for (auto &w : s.workers) self->send_exit(w, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors   = static_cast<std::size_t>(p.get("actors"));
    const auto messages = static_cast<std::uint64_t>(p.get("messages"));
    const auto work     = static_cast<int>(p.get("work"));
    const auto cores    = static_cast<std::size_t>(p.get("cores"));
    const bool spin     = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> workers;
        workers.reserve(actors);
        for (std::size_t w = 0; w < actors; ++w)
            workers.push_back(sys.spawn<qvocaf::kSpawnOptions>(
                worker_fun, static_cast<std::uint64_t>(w), messages, work));
        sys.spawn<qvocaf::kSpawnOptions>(master_fun, workers, messages, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fork_join_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join::params();
    spec.expected          = qvospec::savina::fork_join::expected;
    spec.expected_messages = qvospec::savina::fork_join::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join::work_units;
    spec.idiom_source      = "the ping-pong + counting adapters (stateful_actor, handshake outside "
                             "the window)";
    spec.idiom_note        = "bare uint64 job index, (ok_atom, acc, received) answer; workers "
                             "placed by the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the sixty workers are placed by the work-stealing pool: whichever worker thread is idle "
        "steals the next runnable actor, which is the balancing qb's static placement does not "
        "do -- see benchmarks/savina/fork-join.md");

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_caf::body);
}
