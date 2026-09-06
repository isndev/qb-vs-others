// @benchmark     savina/fib
// @framework     caf 1.1.0
// @idiom-source  the big adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms) and CAF's own `self->spawn(...)` from inside a behavior, which
//                is how every CAF example creates a child (libcaf_core/caf/scheduled_actor.hpp),
//                plus `self->quit()` for the actor's own end.
// @idiom-note    A request is `(get_atom, uint32 n)` and a response `(ok_atom, uint64 value,
//                uint64 chk, uint64 messages)`. A node spawns its two children with
//                `self->spawn(fib_fun, self_handle)`, which hands the new actor to the
//                work-stealing pool -- CAF places dynamically, so with cores=2 the tree is
//                balanced by stealing, the thing qb's same-core addRefActor cannot do. A node
//                quits right after mailing its response.

#include <qvospec/savina/fib.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>

namespace savina_fib_caf {

using namespace qvospec::savina::fib;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct fib_state {
    caf::actor    parent;
    std::uint64_t value{0};
    std::uint64_t chk{0};
    std::uint64_t messages{0};
    std::uint32_t pending{0};
};

struct sink_state {
    caf::actor    seeds[2];
    std::uint32_t n{0};
    std::size_t   ready{0};
    std::size_t   done{0};
    qvo::Watch   *watch{nullptr};
    Sink         *sink{nullptr};
};

caf::behavior fib_fun(caf::stateful_actor<fib_state> *self, caf::actor parent) {
    self->state().parent = std::move(parent);

    auto respond = [self](std::uint64_t value, std::uint64_t chk) {
        auto &s = self->state();
        self->mail(caf::ok_atom_v, value, chk, s.messages).send(s.parent);
        self->quit();
    };

    return {
        // Seed handshake, once, outside the window: the seed has been scheduled and holds its
        // behavior before the root's request is timed.
        [self](caf::tick_atom) { self->mail(caf::ok_atom_v).send(self->state().parent); },
        [self, respond](caf::get_atom, std::uint32_t n) {
            auto &s = self->state();
            ++s.messages;
            if (n <= 2) {
                respond(1, qvo::mix(1));
                return;
            }
            const auto me = caf::actor_cast<caf::actor>(self);
            auto       a  = self->spawn<qvocaf::kSpawnOptions>(fib_fun, me);
            auto       b  = self->spawn<qvocaf::kSpawnOptions>(fib_fun, me);
            self->mail(caf::get_atom_v, n - 1).send(a);
            self->mail(caf::get_atom_v, n - 2).send(b);
            s.pending = 2;
        },
        [self, respond](caf::ok_atom, std::uint64_t value, std::uint64_t chk,
                        std::uint64_t messages) {
            auto &s = self->state();
            s.messages += 1 + messages;
            s.value += value;
            s.chk += chk;
            if (--s.pending == 0) respond(s.value, qvo::mix(s.value) + s.chk);
        },
    };
}

caf::behavior sink_fun(caf::stateful_actor<sink_state> *self, std::uint32_t n, qvo::Watch *watch,
                       Sink *sink) {
    auto &st = self->state();
    st.n     = n;
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &seed : st.seeds) {
        seed = self->spawn<qvocaf::kSpawnOptions>(fib_fun, me);
        self->mail(caf::tick_atom_v).send(seed);
    }

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != 2) return;
            s.watch->start();
            self->mail(caf::get_atom_v, s.n - 1).send(s.seeds[0]);
            self->mail(caf::get_atom_v, s.n - 2).send(s.seeds[1]);
        },
        [self](caf::ok_atom, std::uint64_t, std::uint64_t chk, std::uint64_t messages) {
            auto &s = self->state();
            s.sink->checksum += chk;
            s.sink->messages += 1 + messages;
            if (++s.done != 2) return;
            s.watch->stop();
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n     = static_cast<std::uint32_t>(p.get("n"));
    const auto cores = static_cast<std::size_t>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(sink_fun, n, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fib_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fib::params();
    spec.expected          = qvospec::savina::fib::expected;
    spec.expected_messages = qvospec::savina::fib::expected_messages;
    spec.work_unit         = qvospec::savina::fib::kWorkUnit;
    spec.work_units        = qvospec::savina::fib::work_units;
    spec.idiom_source      = "the big adapter + self->spawn() from a behavior (scheduled_actor.hpp)";
    spec.idiom_note        = "(get_atom, n) / (ok_atom, value, chk, messages); a node spawns its "
                             "two children with self->spawn and quits after mailing its response; "
                             "placement left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "a child spawned from a worker is enqueued on THAT worker and stolen from there, so with "
        "cores=2 the tree is balanced dynamically; qb's cell keeps each sub-tree on its seed's "
        "core because qb has no cross-core spawn -- see benchmarks/savina/fib.md");

    return qvo::run(argc, argv, std::move(spec), savina_fib_caf::body);
}
