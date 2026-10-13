// @benchmark     savina/trapezoid
// @framework     caf 1.1.0
// @idiom-source  the nqueens adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms, `self->spawn` from the master's init for its pool, a tick/ok
//                handshake before the window).
// @idiom-note    A work message is `(put_atom, uint64 begin, uint64 end)`, a result
//                `(add_atom, double area, uint32 worker)`, the ready handshake `(tick_atom)` /
//                `(ok_atom)`. The master spawns its workers itself, sends the whole fan-out from one
//                handler, and quits after telling every worker to (`close_atom`). Where a worker
//                RUNS is the work-stealing pool's decision.

#include <qvospec/savina/trapezoid.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_trapezoid_caf {

using namespace qvospec::savina::trapezoid;

struct Sink {
    std::vector<Record> records;
    double              total{0.0};
};

// The worker's count goes straight to its Tally, a 128-byte slot of its own: a CAF actor is not
// bound to a thread, so its state has no line that is its thread's alone.
caf::behavior worker_fun(caf::event_based_actor *self, caf::actor master, std::uint32_t index,
                         double left, double h, Tally *tally) {
    return {
        // Handshake, once, outside the window: the worker holds its behavior before the first
        // work message is timed.
        [self, master](caf::tick_atom) { self->mail(caf::ok_atom_v).send(master); },
        [self, master, index, left, h, tally](caf::put_atom, std::uint64_t begin, std::uint64_t end) {
            ++tally->works;
            self->mail(caf::add_atom_v, integrate(begin, end, left, h), index).send(master);
        },
        [self](caf::close_atom) { self->quit(); },
    };
}

struct master_state {
    std::vector<caf::actor> workers;
    Config                  config;
    std::size_t             ready{0};
    std::uint64_t           received{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior master_fun(caf::stateful_actor<master_state> *self, Config config, qvo::Watch *watch,
                         Sink *sink, std::vector<Tally> *tallies) {
    auto &st  = self->state();
    st.config = config;
    st.watch  = watch;
    st.sink   = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.workers.reserve(config.workers);
    for (std::uint32_t w = 0; w < config.workers; ++w) {
        st.workers.push_back(self->spawn<qvocaf::kSpawnOptions>(worker_fun, me, w, config.left,
                                                                config.h, &(*tallies)[w]));
        self->mail(caf::tick_atom_v).send(st.workers.back());
    }

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size()) return;
            s.watch->start();
            for (std::uint32_t w = 0; w < s.config.workers; ++w)
                self->mail(caf::put_atom_v, first_piece(w, s.config), first_piece(w + 1, s.config))
                    .send(s.workers[w]);
        },
        [self](caf::add_atom, double area, std::uint32_t worker) {
            auto &s = self->state();
            if (worker >= s.sink->records.size())
                qvo::fail("savina/trapezoid: a result names worker " + std::to_string(worker)
                          + ", which does not exist");
            s.sink->records[worker].add(area);
            if (++s.received != s.config.workers) return;
            s.sink->total = fold(s.sink->records);
            s.watch->stop();
            for (auto &w : s.workers) self->mail(caf::close_atom_v).send(w);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<std::size_t>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    sink.records.assign(c.workers, Record{});
    std::vector<Tally> tallies(c.workers);
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(master_fun, c, &watch, &sink, &tallies);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: every record and tally is final, and reduced
    // here, outside the window.
    return qvo::Answer{checksum(sink.records, sink.total, tallies), delivered(sink.records, tallies)};
}

}  // namespace savina_trapezoid_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::trapezoid::params();
    spec.expected          = qvospec::savina::trapezoid::expected;
    spec.expected_messages = qvospec::savina::trapezoid::expected_messages;
    spec.work_unit         = qvospec::savina::trapezoid::kWorkUnit;
    spec.work_units        = qvospec::savina::trapezoid::work_units;
    spec.idiom_source      = "the nqueens adapter (function-based behaviors, stateful_actor, "
                             "self->spawn from init, tick/ok handshake)";
    spec.idiom_note        = "(put_atom, begin, end) work messages from one master handler, "
                             "(add_atom, area, worker) results; the work-stealing pool decides where "
                             "each worker runs";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "a CAF worker is not bound to a thread: each worker's one work message runs on whichever "
        "scheduler thread takes it, so the integration is balanced dynamically where qb's cell "
        "keeps worker w on core w % cores");

    return qvo::run(argc, argv, std::move(spec), savina_trapezoid_caf::body);
}
