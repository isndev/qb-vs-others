// @benchmark     savina/cigsmok
// @framework     caf 1.1.0
// @idiom-source  the barber adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms, `self->spawn(...)` from an actor's own initialisation for the
//                actors it owns, `self->quit()` for an actor's end) -- CAF's own idioms from
//                libcaf_core/caf/scheduled_actor.hpp.
// @idiom-note    The reference's actors one for one: the arbiter spawns its smokers while it
//                initialises, as CigaretteSmokerAkkaActorBenchmark's ArbiterActor builds them in
//                its constructor, and each smoker says it is up before the window. Messages are
//                built-in atoms plus arguments, typed per receiver: StartSmoking `(get_atom,
//                uint64 round, uint64 period)`, StartedSmoking `(put_atom, uint64 round, uint64
//                smoker)`, Exit `(close_atom)`, Report `(ok_atom, uint64 partial, uint64
//                messages)`, the arbiter's Start `(tick_atom)` and a smoker's handshake
//                `(ok_atom)`. The smoker mails StartedSmoking before it smokes; whether the
//                arbiter runs its next decision while the smoke runs is the work-stealing pool's
//                decision, which places every actor.

#include <qvospec/savina/cigsmok.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <cstdio>
#include <utility>
#include <vector>

namespace savina_cigsmok_caf {

using namespace qvospec::savina::cigsmok;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t stale{0};
};

struct smoker_state {
    caf::actor    arbiter;
    std::uint32_t number{0};
    std::uint64_t acc{0};  // the smoker's terms of the checksum (cigsmok.h)
    std::uint64_t received{0};
};

struct arbiter_state {
    std::vector<caf::actor> smokers;
    std::uint64_t           rounds{0};
    std::uint64_t           smoke{0};
    std::size_t             ready{0};
    std::uint64_t           outstanding{0};  // the round whose StartedSmoking is awaited
    std::uint64_t           played{0};       // rounds acknowledged
    bool                    exiting{false};
    std::size_t             reports{0};
    std::uint64_t           acc{0};  // the arbiter's terms of the checksum (cigsmok.h)
    std::uint64_t           messages{0};  // reported by the smokers
    std::uint64_t           received{0};
    std::uint64_t           stale{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior smoker_fun(caf::stateful_actor<smoker_state> *self, caf::actor arbiter,
                         std::uint32_t number) {
    auto &st   = self->state();
    st.arbiter = std::move(arbiter);
    st.number  = number;
    self->mail(caf::ok_atom_v).send(st.arbiter);  // up -- the handshake, outside the window
    return {
        // StartSmoking: acknowledge first -- the ingredients are off the table -- then smoke.
        [self](caf::get_atom, std::uint64_t round, std::uint64_t period) {
            auto &s = self->state();
            ++s.received;
            self->mail(caf::put_atom_v, round, static_cast<std::uint64_t>(s.number))
                .send(s.arbiter);
            s.acc += smoke_term(s.number, round, static_cast<std::uint32_t>(period));
        },
        [self](caf::close_atom) {  // Exit
            auto &s = self->state();
            ++s.received;
            s.acc += exit_term(s.number);
            self->mail(caf::ok_atom_v, s.acc, s.received).send(s.arbiter);
            self->quit();
        },
    };
}

caf::behavior arbiter_fun(caf::stateful_actor<arbiter_state> *self, std::uint64_t rounds,
                          std::uint64_t smokers, std::uint64_t smoke, qvo::Watch *watch,
                          Sink *sink) {
    auto &st  = self->state();
    st.rounds = rounds;
    st.smoke  = smoke;
    st.watch  = watch;
    st.sink   = sink;

    // The arbiter builds its smokers, as the reference's does in its constructor.
    const auto me = caf::actor_cast<caf::actor>(self);
    st.smokers.reserve(static_cast<std::size_t>(smokers));
    for (std::uint64_t j = 0; j < smokers; ++j)
        st.smokers.push_back(
            self->spawn<qvocaf::kSpawnOptions>(smoker_fun, me, static_cast<std::uint32_t>(j)));

    // Put the ingredients on the table for round `round`: the draw names the smoker and the period.
    auto choose = [self](std::uint64_t round) {
        auto               &s      = self->state();
        const std::uint32_t smoker = smoker_of(round, s.smokers.size());
        s.outstanding              = round;
        self->mail(caf::get_atom_v, round, static_cast<std::uint64_t>(period_of(round, s.smoke)))
            .send(s.smokers[smoker]);
    };

    return {
        // Every smoker is up and scheduled: open the window with the reference's StartMessage.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.smokers.size()) return;
            s.watch->start();
            self->mail(caf::tick_atom_v).send(caf::actor_cast<caf::actor>(self));
        },
        [self, choose](caf::tick_atom) {  // Start
            ++self->state().received;
            choose(0);
        },
        // StartedSmoking. Every one is folded into the sum; only the one naming the outstanding
        // round plays the next -- a duplicate completes the run with a wrong sum.
        [self, choose](caf::put_atom, std::uint64_t round, std::uint64_t smoker) {
            auto &s = self->state();
            ++s.received;
            s.acc += ack_term(static_cast<std::uint32_t>(smoker), round);
            if (s.exiting || round != s.outstanding) {
                ++s.stale;
                return;
            }
            if (++s.played < s.rounds) {
                choose(s.played);
                return;
            }
            s.exiting = true;
            for (auto &smk : s.smokers) self->mail(caf::close_atom_v).send(smk);
        },
        [self](caf::ok_atom, std::uint64_t partial, std::uint64_t messages) {  // Report
            auto &s = self->state();
            ++s.received;
            s.acc += partial;
            s.messages += messages;
            if (++s.reports != s.smokers.size()) return;
            s.sink->checksum = s.acc;
            s.sink->messages = s.messages + s.received;
            s.sink->stale    = s.stale;
            s.watch->stop();
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds  = at_least_one(p.get("rounds"), "rounds");
    const auto smokers = at_least_one(p.get("smokers"), "smokers");
    const auto smoke   = at_least_one(p.get("smoke"), "smoke");
    const auto cores   = static_cast<std::size_t>(p.get("cores"));
    const bool spin    = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(arbiter_fun, rounds, smokers, smoke, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    if (sink.stale != 0)
        std::fprintf(stderr, "savina/cigsmok caf: %llu StartedSmoking named no outstanding round\n",
                     static_cast<unsigned long long>(sink.stale));
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_cigsmok_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::cigsmok::params();
    spec.expected          = qvospec::savina::cigsmok::expected;
    spec.expected_messages = qvospec::savina::cigsmok::expected_messages;
    spec.work_unit         = qvospec::savina::cigsmok::kWorkUnit;
    spec.work_units        = qvospec::savina::cigsmok::work_units;
    spec.idiom_source      = "the barber adapter + self->spawn() from the arbiter's "
                             "initialisation (scheduled_actor.hpp)";
    spec.idiom_note        = "the reference's actors one for one, the smokers spawned by the "
                             "arbiter; built-in atoms typed per receiver; StartedSmoking mailed "
                             "before the smoke; placement left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the smoker mails StartedSmoking before it smokes, which makes the arbiter runnable; in "
        "the pool a receiver made ready from a worker is prepended to THAT worker's queue "
        "(worker::delay), so the arbiter's next decision runs after the smoke on the same "
        "thread unless the other worker steals it -- whether a decision overlaps a smoke is the "
        "scheduler's, and is not reported (benchmarks/savina/cigsmok.md)");

    return qvo::run(argc, argv, std::move(spec), savina_cigsmok_caf::body);
}
