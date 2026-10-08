// @benchmark     savina/nqueens
// @framework     caf 1.1.0
// @idiom-source  the fib and big adapters beside this file (function-based behaviors,
//                stateful_actor, built-in atoms, `self->spawn` from the master's init for its
//                pool, a tick/ok handshake before the window) and CAF's own message ordering
//                guarantee between one sender and one receiver, which the master's done-count
//                termination rests on.
// @idiom-note    A work item is `(put_atom, uint64 w0, uint64 w1)` -- the packed board -- in both
//                directions; a result `(add_atom, uint64 hash)`; a done `(ok_atom, uint64 chk)`;
//                the ready handshake `(tick_atom)` / `(ok_atom)`. The master spawns the pool
//                itself, relays each child item to the next worker of its rotation with a fresh
//                mail, and quits after telling every worker to (`close_atom`). Where a worker
//                RUNS is the work-stealing pool's decision: an idle thread steals a runnable
//                worker.

#include <qvospec/savina/nqueens.h>

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

namespace savina_nqueens_caf {

using namespace qvospec::savina::nqueens;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

caf::behavior worker_fun(caf::event_based_actor *self, caf::actor master, int size, int threshold) {
    return {
        // Handshake, once, outside the window: the worker holds its behavior before the first
        // item is timed.
        [self, master](caf::tick_atom) { self->mail(caf::ok_atom_v).send(master); },
        [self, master, size, threshold](caf::put_atom, std::uint64_t w0, std::uint64_t w1) {
            const Board item{w0, w1};
            process(
                item, size, threshold,
                [&](const Board &child) {
                    self->mail(caf::put_atom_v, child.w0, child.w1).send(master);
                },
                [&](std::uint64_t hash) { self->mail(caf::add_atom_v, hash).send(master); });
            self->mail(caf::ok_atom_v, done_value(item)).send(master);
        },
        [self](caf::close_atom) { self->quit(); },
    };
}

struct master_state {
    std::vector<caf::actor> workers;
    std::size_t             ready{0};
    std::size_t             next{0};
    std::uint64_t           sent{0};
    std::uint64_t           completed{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior master_fun(caf::stateful_actor<master_state> *self, std::uint32_t workers, int size,
                         int threshold, qvo::Watch *watch, Sink *sink) {
    auto &st = self->state();
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.workers.reserve(workers);
    for (std::uint32_t w = 0; w < workers; ++w) {
        st.workers.push_back(self->spawn<qvocaf::kSpawnOptions>(worker_fun, me, size, threshold));
        self->mail(caf::tick_atom_v).send(st.workers.back());
    }

    auto hand_out = [self](std::uint64_t w0, std::uint64_t w1) {
        auto &s = self->state();
        self->mail(caf::put_atom_v, w0, w1).send(s.workers[s.next]);
        if (++s.next == s.workers.size()) s.next = 0;
        ++s.sent;
    };

    return {
        [self, hand_out](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size()) return;
            s.watch->start();
            const Board empty{};
            hand_out(empty.w0, empty.w1);
        },
        // A child item from a worker: relayed to the next worker of the rotation.
        [self, hand_out](caf::put_atom, std::uint64_t w0, std::uint64_t w1) {
            ++self->state().sink->messages;
            hand_out(w0, w1);
        },
        [self](caf::add_atom, std::uint64_t hash) {
            auto &s = self->state();
            ++s.sink->messages;
            s.sink->checksum += hash;
        },
        [self](caf::ok_atom, std::uint64_t chk) {
            auto &s = self->state();
            s.sink->messages += 2;  // this done, and the item its worker received
            s.sink->checksum += chk;
            if (++s.completed != s.sent) return;
            s.watch->stop();
            for (auto &w : s.workers) self->mail(caf::close_atom_v).send(w);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto size      = static_cast<int>(p.get("size"));
    const auto threshold = static_cast<int>(p.get("threshold"));
    const auto workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto cores     = static_cast<std::size_t>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(master_fun, workers, size, threshold, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_nqueens_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::nqueens::params();
    spec.expected          = qvospec::savina::nqueens::expected;
    spec.expected_messages = qvospec::savina::nqueens::expected_messages;
    spec.work_unit         = qvospec::savina::nqueens::kWorkUnit;
    spec.work_units        = qvospec::savina::nqueens::work_units;
    spec.idiom_source      = "the fib / big adapters (function-based behaviors, stateful_actor, "
                             "self->spawn from init, tick/ok handshake)";
    spec.idiom_note        = "(put_atom, w0, w1) items both ways, (add_atom, hash) results, "
                             "(ok_atom, chk) dones; the master relays round-robin and the "
                             "work-stealing pool decides where each worker runs";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the master hands items out round-robin like every other cell, but a CAF worker is not "
        "bound to a thread: an idle scheduler thread steals a runnable worker, so the search is "
        "balanced dynamically where qb's cell keeps worker w on core w % cores");

    return qvo::run(argc, argv, std::move(spec), savina_nqueens_caf::body);
}
