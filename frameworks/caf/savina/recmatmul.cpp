// @benchmark     savina/recmatmul
// @framework     caf 1.1.0
// @idiom-source  the nqueens adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms, `self->spawn` from the master's init for its pool, a tick/ok
//                handshake before the window, a fresh mail for every relay) and CAF's own message
//                ordering guarantee between one sender and one receiver, which the master's
//                done-count termination rests on.
// @idiom-note    A task is `(put_atom, uint64 corners_a, uint64 corners_b, uint64 corners_c,
//                uint64 blocks_dim)` -- the eight 32-bit fields packed into four words -- in both
//                directions; a done `(ok_atom)`; the ready handshake `(tick_atom)` / `(join_atom)`
//                before the window. The master relays each created task to the worker Savina's
//                routing names. Where a worker RUNS is the work-stealing pool's decision.

#include <qvospec/savina/recmatmul.h>

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

namespace savina_recmatmul_caf {

using namespace qvospec::savina::recmatmul;

// A Task as four built-in uint64 message elements: two corners per word.
struct Packed {
    std::uint64_t a{0}, b{0}, c{0}, n{0};
};
inline Packed pack(const Task &t) noexcept {
    auto two = [](std::uint32_t hi, std::uint32_t lo) {
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    };
    return {two(t.srA, t.scA), two(t.srB, t.scB), two(t.srC, t.scC), two(t.blocks, t.dim)};
}
inline Task unpack(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t n) noexcept {
    Task t;
    t.srA    = static_cast<std::uint32_t>(a >> 32);
    t.scA    = static_cast<std::uint32_t>(a);
    t.srB    = static_cast<std::uint32_t>(b >> 32);
    t.scB    = static_cast<std::uint32_t>(b);
    t.srC    = static_cast<std::uint32_t>(c >> 32);
    t.scC    = static_cast<std::uint32_t>(c);
    t.blocks = static_cast<std::uint32_t>(n >> 32);
    t.dim    = static_cast<std::uint32_t>(n);
    return t;
}

// The worker's counts go straight to its Tally, a 128-byte slot of its own: a CAF actor is not
// bound to a thread, so its state has no line that is its thread's alone.
caf::behavior worker_fun(caf::event_based_actor *self, caf::actor master, Config config,
                         Matrices *matrices, Tally *tally) {
    return {
        // Handshake, once, outside the window: the worker holds its behavior before the first
        // task is timed.
        [self, master](caf::tick_atom) { self->mail(caf::join_atom_v).send(master); },
        [self, master, config, matrices, tally](caf::put_atom, std::uint64_t a, std::uint64_t b,
                                                 std::uint64_t c, std::uint64_t n) {
            const Task task = unpack(a, b, c, n);
            ++tally->works;
            if (is_leaf(task, config)) {
                ++tally->leaves;
                multiply(*matrices, task);
            } else {
                ++tally->splits;
                split(task, [&](const Task &child) {
                    const Packed k = pack(child);
                    self->mail(caf::put_atom_v, k.a, k.b, k.c, k.n).send(master);
                });
            }
            self->mail(caf::ok_atom_v).send(master);
        },
        [self](caf::close_atom) { self->quit(); },
    };
}

struct master_state {
    std::vector<caf::actor> workers;
    Config                  config;
    std::size_t             ready{0};
    qvo::Watch             *watch{nullptr};
    MasterCounts           *counts{nullptr};
};

caf::behavior master_fun(caf::stateful_actor<master_state> *self, Config config, qvo::Watch *watch,
                         MasterCounts *counts, Matrices *matrices, std::vector<Tally> *tallies) {
    auto &st  = self->state();
    st.config = config;
    st.watch  = watch;
    st.counts = counts;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.workers.reserve(config.workers);
    for (std::uint32_t w = 0; w < config.workers; ++w) {
        st.workers.push_back(self->spawn<qvocaf::kSpawnOptions>(worker_fun, me, config, matrices,
                                                                &(*tallies)[w]));
        self->mail(caf::tick_atom_v).send(st.workers.back());
    }

    auto send_task = [self](const Task &task) {
        auto        &s = self->state();
        const Packed k = pack(task);
        ++s.counts->sent;
        self->mail(caf::put_atom_v, k.a, k.b, k.c, k.n).send(s.workers[route(task, s.config)]);
    };

    return {
        [self, send_task](caf::join_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size()) return;
            s.watch->start();
            send_task(root(s.config));
        },
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.counts->dones != s.counts->sent) return;
            s.watch->stop();
            for (auto &w : s.workers) self->mail(caf::close_atom_v).send(w);
            self->quit();
        },
        // A task a worker created: relayed to the worker Savina's routing names.
        [self, send_task](caf::put_atom, std::uint64_t a, std::uint64_t b, std::uint64_t c,
                          std::uint64_t n) {
            ++self->state().counts->created;
            send_task(unpack(a, b, c, n));
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<std::size_t>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Matrices           matrices(c.length);  // A, B and a zero C, built before the system
    MasterCounts       counts;
    std::vector<Tally> tallies(c.workers);
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(master_fun, c, &watch, &counts, &matrices, &tallies);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: C, the tallies and the master's counts are
    // final, and reduced here, outside the window.
    return qvo::Answer{checksum(matrices, tallies, counts), delivered(tallies, counts)};
}

}  // namespace savina_recmatmul_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::recmatmul::params();
    spec.expected          = qvospec::savina::recmatmul::expected;
    spec.expected_messages = qvospec::savina::recmatmul::expected_messages;
    spec.work_unit         = qvospec::savina::recmatmul::kWorkUnit;
    spec.work_units        = qvospec::savina::recmatmul::work_units;
    spec.idiom_source      = "the nqueens adapter (function-based behaviors, stateful_actor, "
                             "self->spawn from init, tick/join handshake, a fresh mail per relay)";
    spec.idiom_note        = "(put_atom, 4 x uint64) tasks both ways, (ok_atom) dones; the master "
                             "relays each created task to worker (srC + scC) % workers and the "
                             "work-stealing pool decides where each worker runs";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "Savina's routing sends every leaf of the defaults to five workers (0, 4, 8, 12, 16), but a "
        "CAF worker is not bound to a thread: an idle scheduler thread steals a runnable worker, "
        "so the leaves are balanced dynamically where qb's cell fixes worker w on core "
        "floor(w x cores / workers)");

    return qvo::run(argc, argv, std::move(spec), savina_recmatmul_caf::body);
}
