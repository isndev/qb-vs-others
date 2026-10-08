// @benchmark     savina/philosophers
// @framework     caf 1.1.0
// @idiom-source  the chameneos adapter beside this file (function-based behaviors, stateful_actor,
//                handles exchanged before the window, built-in atoms from caf/type_id.hpp) and
//                CAF's own examples/dynamic_behavior/dining_philosophers.cpp for an actor mailing
//                itself (`self->mail(...).send(self)`).
// @idiom-note    Hungry is `(get_atom, uint32 philosopher)`, the answer `(ok_atom, bool granted)`,
//                Done `(put_atom, uint32 philosopher)`, Start `(tick_atom)` mailed to itself and
//                Exit `(close_atom, uint32, uint64 fold, uint64 messages)`. Plain one-way mails,
//                as Savina's Akka actors `!`: CAF's own sample models each chopstick as a typed
//                actor answered through request/then, a different protocol (no arbitrator) and a
//                slower primitive -- the benchmark's shape is Savina's arbitrator, so the
//                arbitrator holds the philosopher handles from one wiring message before the
//                window and answers `philosophers[i]`. Placement is left to the work-stealing
//                pool.

#include <qvospec/savina/philosophers.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <limits>
#include <utility>
#include <vector>

namespace savina_philosophers_caf {

using namespace qvospec::savina::philosophers;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct philosopher_state {
    caf::actor    arbitrator;
    std::uint32_t index{0};
    std::uint64_t rounds{0};
    std::uint64_t starts{0};
    std::uint64_t eats{0};
    std::uint64_t chk{0};
    std::uint64_t received{0};
};

struct arbitrator_state {
    static constexpr std::uint32_t kFree = std::numeric_limits<std::uint32_t>::max();

    std::vector<caf::actor>    philosophers;
    std::vector<std::uint32_t> owner;
    std::vector<std::uint64_t> grants;
    std::vector<std::uint64_t> dones;
    std::uint64_t              chk{0};
    std::uint64_t              received{0};
    std::size_t                wired{0};
    std::size_t                exited{0};
    bool                       violation{false};
    qvo::Watch                *watch{nullptr};
    Sink                      *sink{nullptr};
};

caf::behavior philosopher_fun(caf::stateful_actor<philosopher_state> *self, std::uint32_t index,
                              std::uint64_t rounds) {
    auto &st  = self->state();
    st.index  = index;
    st.rounds = rounds;

    return {
        // Wiring handshake, once, outside the window.
        [self](caf::put_atom, caf::actor arbitrator) {
            self->state().arbitrator = std::move(arbitrator);
            self->mail(caf::ok_atom_v).send(self->state().arbitrator);
        },
        [self](caf::tick_atom) {
            auto &s = self->state();
            ++s.received;
            s.chk += term(kStart, s.index, ++s.starts);
            self->mail(caf::get_atom_v, s.index).send(s.arbitrator);
        },
        [self](caf::ok_atom, bool granted) {
            auto &s = self->state();
            if (!granted) {
                self->mail(caf::get_atom_v, s.index).send(s.arbitrator);
                return;
            }
            ++s.received;
            s.chk += term(kEat, s.index, ++s.eats);
            self->mail(caf::put_atom_v, s.index).send(s.arbitrator);
            if (s.eats < s.rounds) {
                self->mail(caf::tick_atom_v).send(self);
                return;
            }
            self->mail(caf::close_atom_v, s.index, s.chk, s.received).send(s.arbitrator);
            self->quit();
        },
    };
}

caf::behavior arbitrator_fun(caf::stateful_actor<arbitrator_state> *self,
                             std::vector<caf::actor> philosophers, qvo::Watch *watch, Sink *sink) {
    auto       &st = self->state();
    const auto  n  = philosophers.size();
    st.philosophers = std::move(philosophers);
    st.owner.assign(n, arbitrator_state::kFree);
    st.grants.assign(n, 0);
    st.dones.assign(n, 0);
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &p : st.philosophers) self->mail(caf::put_atom_v, me).send(p);

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.philosophers.size()) return;
            s.watch->start();
            for (auto &p : s.philosophers) self->mail(caf::tick_atom_v).send(p);
        },
        [self](caf::get_atom, std::uint32_t i) {
            auto               &s     = self->state();
            const std::uint32_t left  = i;
            const std::uint32_t right = (i + 1) % static_cast<std::uint32_t>(s.philosophers.size());
            if (s.owner[left] != arbitrator_state::kFree ||
                s.owner[right] != arbitrator_state::kFree) {
                self->mail(caf::ok_atom_v, false).send(s.philosophers[i]);
                return;
            }
            s.owner[left] = s.owner[right] = i;
            ++s.received;
            s.chk += term(kGrant, i, ++s.grants[i]);
            self->mail(caf::ok_atom_v, true).send(s.philosophers[i]);
        },
        [self](caf::put_atom, std::uint32_t i) {
            auto               &s     = self->state();
            const std::uint32_t left  = i;
            const std::uint32_t right = (i + 1) % static_cast<std::uint32_t>(s.philosophers.size());
            ++s.received;
            s.chk += term(kDone, i, ++s.dones[i]);
            if (s.owner[left] != i || s.owner[right] != i) s.violation = true;
            s.owner[left] = s.owner[right] = arbitrator_state::kFree;
        },
        [self](caf::close_atom, std::uint32_t i, std::uint64_t chk, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.chk += term(kExit, i, s.dones[i]) + chk;
            s.received += received;
            if (++s.exited != s.philosophers.size()) return;
            s.watch->stop();
            s.sink->checksum = s.chk + (s.violation ? kForkViolation : 0);
            s.sink->messages = s.received;
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n      = static_cast<std::uint32_t>(p.get("philosophers"));
    const auto rounds = static_cast<std::uint64_t>(p.get("rounds"));
    const auto cores  = static_cast<std::size_t>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> field;
        field.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            field.push_back(sys.spawn<qvocaf::kSpawnOptions>(philosopher_fun, i, rounds));
        sys.spawn<qvocaf::kSpawnOptions>(arbitrator_fun, field, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_philosophers_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::philosophers::params();
    spec.expected          = qvospec::savina::philosophers::expected;
    spec.expected_messages = qvospec::savina::philosophers::expected_messages;
    spec.work_unit         = qvospec::savina::philosophers::kWorkUnit;
    spec.work_units        = qvospec::savina::philosophers::work_units;
    spec.idiom_source      = "the chameneos adapter + caf/type_id.hpp built-in atoms + "
                             "examples/dynamic_behavior/dining_philosophers.cpp (self-mail)";
    spec.idiom_note        = "(get_atom, i) / (ok_atom, granted) / (put_atom, i) / tick_atom to "
                             "itself / (close_atom, i, fold, messages); philosopher handles "
                             "delivered once before the window; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the arbitrator and the 20 philosophers are placed by the work-stealing pool, so whether "
        "the arbitrator's mailbox is a cross-core queue is the scheduler's decision; qb's cell "
        "pins the arbitrator alone on core 0 and every philosopher on the far side -- see "
        "benchmarks/savina/philosophers.md");
    spec.caveats.emplace_back(
        "how many requests the arbitrator refuses depends on the interleaving and is neither "
        "asserted nor reported; every refused request and its retry are delivered and timed");

    return qvo::run(argc, argv, std::move(spec), savina_philosophers_caf::body);
}
