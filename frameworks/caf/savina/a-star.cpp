// @benchmark     savina/a-star
// @framework     caf 1.1.0
// @idiom-source  the fib and big adapters beside this file (function-based behaviors,
//                stateful_actor, built-in atoms, a parent spawning its field with `self->spawn` and
//                handshaking it before the window) and CAF's mail API
//                (`self->mail(...).send(dest)`), which libcaf_core/caf/local_actor.hpp names as the
//                replacement of the deprecated `delegate`.
// @idiom-note    Work to a worker is `(get_atom, uint32 node)`, a node handed back `(put_atom, uint32
//                node)`, an acknowledgement `(ok_atom, uint64 chk, uint64 nodes)`; the master spawns
//                its workers with `self->spawn` and the work-stealing pool places them. A worker
//                mails its frontier and then its acknowledgement to the master from one handler,
//                and CAF delivers one sender's messages to one receiver in order, so the frontier
//                always lands first. A relay is a fresh message: CAF has no forward of the current
//                message outside the request/response path.

#include <qvospec/savina/a-star.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <vector>

namespace savina_a_star_caf {

using namespace qvospec::savina::a_star;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t work_messages{0};  // observed, and asserted >= min_work_messages
};

// What every worker shares: the graph, the claim slots and the two knobs of the search.
struct Shared {
    const Grid   *grid{nullptr};
    Claims       *claims{nullptr};
    std::uint32_t threshold{0};
    int           work{0};
};

struct worker_state {
    caf::actor                 master;
    Shared                     shared;
    std::vector<std::uint32_t> queue;
};

struct master_state {
    std::vector<caf::actor> workers;
    std::size_t             ready{0};
    std::uint64_t           sent{0};
    std::uint64_t           completed{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior worker_fun(caf::stateful_actor<worker_state> *self, caf::actor master,
                         Shared shared) {
    self->state().master = std::move(master);
    self->state().shared = shared;

    return {
        // Readiness handshake, once, outside the window.
        [self](caf::tick_atom) { self->mail(caf::ok_atom_v).send(self->state().master); },
        [self](caf::get_atom, std::uint32_t root) {
            auto       &s = self->state();
            const Chunk c = search(*s.shared.grid, *s.shared.claims, root, s.shared.threshold,
                                   s.shared.work, s.queue, [self, &s](std::uint32_t node) {
                                       self->mail(caf::put_atom_v, node).send(s.master);
                                   });
            self->mail(caf::ok_atom_v, c.chk, c.nodes).send(s.master);
        },
    };
}

caf::behavior master_fun(caf::stateful_actor<master_state> *self, std::uint32_t workers,
                         Shared shared, qvo::Watch *watch, Sink *sink) {
    auto &st = self->state();
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.workers.reserve(workers);
    for (std::uint32_t w = 0; w < workers; ++w) {
        st.workers.push_back(self->spawn<qvocaf::kSpawnOptions>(worker_fun, me, shared));
        self->mail(caf::tick_atom_v).send(st.workers.back());
    }

    auto dispatch = [self](std::uint32_t node) {
        auto &s = self->state();
        self->mail(caf::get_atom_v, node).send(s.workers[s.sent++ % s.workers.size()]);
    };

    return {
        [self, dispatch](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size()) return;
            s.watch->start();
            dispatch(Grid::kOrigin);
        },
        // A node a worker handed back goes on to the next worker.
        [self, dispatch](caf::put_atom, std::uint32_t node) {
            ++self->state().sink->messages;
            dispatch(node);
        },
        [self](caf::ok_atom, std::uint64_t chk, std::uint64_t /*nodes*/) {
            auto &s = self->state();
            // The acknowledgement and the work message it proves delivered.
            s.sink->messages += 2;
            s.sink->checksum += chk;
            if (++s.completed != s.sent) return;
            s.watch->stop();
            s.sink->work_messages = s.sent;
            for (auto &w : s.workers) self->send_exit(w, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto workers = static_cast<std::uint32_t>(p.get("workers"));
    const auto cores   = static_cast<std::size_t>(p.get("cores"));
    const bool spin    = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    const Grid grid(static_cast<std::uint32_t>(p.get("grid")));
    Claims     claims(grid.nodes());
    const Shared shared{&grid, &claims, static_cast<std::uint32_t>(p.get("threshold")),
                        static_cast<int>(p.get("work"))};

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(master_fun, workers, shared, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedWorkMessages] = sink.work_messages;
    return answer;
}

}  // namespace savina_a_star_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::a_star::params();
    spec.expected          = qvospec::savina::a_star::expected;
    spec.work_unit         = qvospec::savina::a_star::kWorkUnit;
    spec.work_units        = qvospec::savina::a_star::work_units;
    spec.observed_at_least[qvospec::savina::a_star::kObservedWorkMessages] =
        qvospec::savina::a_star::min_work_messages;
    spec.idiom_source      = "the fib/big adapters + self->spawn from a behavior + the mail API";
    spec.idiom_note        = "(get_atom, node) / (put_atom, node) / (ok_atom, chk, nodes); the "
                             "master spawns its workers and relays every handed-back node with a "
                             "fresh mail round-robin; placement left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the workers are placed by the work-stealing pool, so how many hand-backs cross a core is "
        "the scheduler's decision; qb's cell fixes actor a on core a % cores -- see "
        "benchmarks/savina/a-star.md");
    spec.caveats.emplace_back(
        "the claim slots are shared memory every worker CASes, as in Savina's own implementation "
        "-- the one input not passed by message, identical for every framework (a-star.h, Claims)");

    return qvo::run(argc, argv, std::move(spec), savina_a_star_caf::body);
}
