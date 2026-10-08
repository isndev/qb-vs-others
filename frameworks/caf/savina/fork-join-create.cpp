// @benchmark     savina/fork-join-create
// @framework     caf 1.1.0
// @idiom-source  the fib adapter beside this file (function-based behaviors, `self->spawn(...)`
//                from inside a behavior, which is how every CAF example creates an actor --
//                libcaf_core/caf/scheduled_actor.hpp -- and `self->quit()` for the actor's own
//                end).
// @idiom-note    A forked actor is a STATELESS function-based actor
//                (`caf::event_based_actor`): its creator and index are captured by its one
//                handler, so no state object is allocated. A creator forks its share in one loop
//                and the work-stealing pool places every forked actor; with cores=2 the two
//                creators are placed by the pool too. CAF's `caf::lazy_init` spawn option
//                (spawn_options.hpp: "delay its initialization until a message arrives") fits
//                this spawn-then-send shape and was tried: an interleaved check showed no gain
//                over the default spawn (equal or slower minimums at both core counts), so the
//                default -- what CAF's documentation leads with -- is what runs.

#include <qvospec/savina/fork-join-create.h>

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

namespace savina_fork_join_create_caf {

using namespace qvospec::savina::fork_join_create;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct creator_state {
    caf::actor    driver;
    std::uint64_t first{0};
    std::uint64_t stride{1};
    std::uint64_t actors{0};
    std::uint64_t share{0};
    int           work{0};
    std::uint64_t acc{0};
    std::uint64_t messages{0};
    std::uint64_t done{0};
};

struct driver_state {
    std::vector<caf::actor> creators;
    std::size_t             ready{0};
    std::size_t             done{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

// One forked actor: answers its creator once and quits.
caf::behavior fork_fun(caf::event_based_actor *self, caf::actor creator, std::uint64_t index,
                       int work) {
    return {
        [self, creator = std::move(creator), index, work](caf::get_atom, std::uint64_t job) {
            self->mail(caf::ok_atom_v, job_value(index, job, work), std::uint64_t{1}).send(creator);
            self->quit();
        },
    };
}

caf::behavior creator_fun(caf::stateful_actor<creator_state> *self, caf::actor driver,
                          std::uint64_t first, std::uint64_t stride, std::uint64_t actors,
                          int work) {
    auto &st  = self->state();
    st.driver = std::move(driver);
    st.first  = first;
    st.stride = stride;
    st.actors = actors;
    st.share  = share(actors, stride, first);
    st.work   = work;

    return {
        // Handshake, once, outside the window: the creator has been scheduled and holds its
        // behavior before the driver's fork order is timed.
        [self](caf::tick_atom) { self->mail(caf::ok_atom_v).send(self->state().driver); },
        [self](caf::spawn_atom) {
            auto &s = self->state();
            ++s.messages;
            const auto me = caf::actor_cast<caf::actor>(self);
            for (std::uint64_t i = s.first; i < s.actors; i += s.stride) {
                auto child = self->spawn<qvocaf::kSpawnOptions>(fork_fun, me, i,
                                                                                 s.work);
                self->mail(caf::get_atom_v, i).send(child);
            }
            if (s.share == 0) {  // more creators than actors: nothing to join
                self->mail(caf::put_atom_v, s.acc, s.messages).send(s.driver);
                self->quit();
            }
        },
        [self](caf::ok_atom, std::uint64_t value, std::uint64_t messages) {
            auto &s = self->state();
            s.acc += value;
            s.messages += 1 + messages;
            if (++s.done != s.share) return;
            self->mail(caf::put_atom_v, s.acc, s.messages).send(s.driver);
            self->quit();
        },
    };
}

caf::behavior driver_fun(caf::stateful_actor<driver_state> *self, std::uint64_t ncreat,
                         std::uint64_t actors, int work, qvo::Watch *watch, Sink *sink) {
    auto &st = self->state();
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (std::uint64_t s = 0; s < ncreat; ++s) {
        st.creators.push_back(
            self->spawn<qvocaf::kSpawnOptions>(creator_fun, me, s, ncreat, actors, work));
        self->mail(caf::tick_atom_v).send(st.creators.back());
    }

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.creators.size()) return;
            s.watch->start();
            for (const auto &c : s.creators) self->mail(caf::spawn_atom_v).send(c);
        },
        [self](caf::put_atom, std::uint64_t chk, std::uint64_t messages) {
            auto &s = self->state();
            s.sink->checksum += chk;
            s.sink->messages += 1 + messages;
            if (++s.done != s.creators.size()) return;
            s.watch->stop();
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint64_t>(p.get("actors"));
    const auto work   = static_cast<int>(p.get("work"));
    const auto ncreat = creators(p);
    const bool spin   = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, static_cast<std::size_t>(ncreat), spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, static_cast<std::size_t>(ncreat));

        sys.spawn<qvocaf::kSpawnOptions>(driver_fun, ncreat, actors, work, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fork_join_create_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join_create::params();
    spec.expected          = qvospec::savina::fork_join_create::expected;
    spec.expected_messages = qvospec::savina::fork_join_create::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join_create::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join_create::work_units;
    spec.idiom_source      = "the fib adapter + self->spawn() from a behavior (scheduled_actor.hpp)";
    spec.idiom_note        = "creator s forks its share with self->spawn(fork_fun) and one "
                             "(get_atom, i) each; a forked actor is a stateless event_based_actor "
                             "that mails (ok_atom, value, 1) to its creator and quits; placement "
                             "left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "forked actors use CAF's default spawn; caf::lazy_init (no scheduling until the first "
        "message) fits the spawn-then-send shape and was tried, and showed no gain over the default "
        "-- see benchmarks/savina/fork-join-create.md");
    spec.caveats.emplace_back(
        "a forked actor spawned from a worker is enqueued on THAT worker and stolen from there, so "
        "with cores=2 the forked actors are balanced dynamically; qb's cell keeps each creator's "
        "actors on its core because qb has no cross-core spawn -- see "
        "benchmarks/savina/fork-join-create.md");

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_create_caf::body);
}
