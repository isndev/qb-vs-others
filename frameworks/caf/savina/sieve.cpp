// @benchmark     savina/sieve
// @framework     caf 1.1.0
// @idiom-source  the fib adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms, `self->spawn(...)` from inside a behavior for an actor created at
//                run time, `self->quit()` for an actor's own end) -- CAF's own idioms from
//                libcaf_core/caf/scheduled_actor.hpp.
// @idiom-note    The reference's actors one for one: a producer that sends every candidate, a bare
//                `uint64`, from one handler and then Exit (`close_atom`), and filters that each
//                spawn their successor with `self->spawn` when a new prime finds no room -- the
//                reference's `context.system.actorOf`, inside the window. Messages from one sender
//                to one receiver keep their order in CAF, which the sieve needs. A filter quits
//                behind its Exit, as the reference's does. The pool places every filter, so the
//                spec's `place` has no counterpart here: place=1 is declared not applicable.

#include <qvospec/savina/sieve.h>

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

namespace savina_sieve_caf {

using namespace qvospec::savina::sieve;

// The run's shared, read-only wiring: the slots and the primes' vectors the body owns, one per
// filter, each written only by its filter (a line of its own: CAF's actor objects come from the
// heap and two of them may share one).
struct Field {
    std::vector<Tally>                      *tallies{nullptr};
    std::vector<std::vector<std::uint64_t>> *primes{nullptr};
    std::uint64_t                            local{1};
    qvo::Watch                              *watch{nullptr};
};

struct filter_state {
    const Field  *field{nullptr};
    std::uint32_t index{0};
    Filter        filter;
    caf::actor    next;
};

struct producer_state {
    caf::actor     first;
    std::uint64_t  limit{0};
    std::uint64_t *starts{nullptr};
    qvo::Watch    *watch{nullptr};
};

caf::behavior filter_fun(caf::stateful_actor<filter_state> *self, const Field *field,
                         std::uint32_t index, std::uint64_t prime) {
    auto &st  = self->state();
    st.field  = field;
    st.index  = index;
    st.filter = Filter(prime, field->local, (*field->tallies)[index], (*field->primes)[index]);
    return {
        // Handshake, once, outside the window (the first filter only).
        [self](caf::get_atom, caf::actor producer) { self->mail(caf::ok_atom_v).send(producer); },
        [self](std::uint64_t candidate) {
            auto &s = self->state();
            switch (s.filter.take(candidate)) {
            case Filter::Verdict::pass: self->mail(candidate).send(s.next); break;
            case Filter::Verdict::grow:
                if (s.index + 1 >= s.field->tallies->size()) too_many_filters();
                s.next = self->spawn<qvocaf::kSpawnOptions>(filter_fun, s.field, s.index + 1, candidate);
                break;
            default: break;  // dropped, or kept here
            }
        },
        // The Exit follows the last candidate down the chain; the last filter's ends the run.
        [self](caf::close_atom) {
            auto &s = self->state();
            ++(*s.field->tallies)[s.index].exits;
            if (s.filter.linked()) self->mail(caf::close_atom_v).send(s.next);
            else s.field->watch->stop();
            self->quit();
        },
    };
}

caf::behavior producer_fun(caf::stateful_actor<producer_state> *self, caf::actor first,
                           std::uint64_t limit, std::uint64_t *starts, qvo::Watch *watch) {
    auto &st  = self->state();
    st.first  = std::move(first);
    st.limit  = limit;
    st.starts = starts;
    st.watch  = watch;
    self->mail(caf::get_atom_v, caf::actor_cast<caf::actor>(self)).send(st.first);
    return {
        // The first filter has been scheduled once: the window opens.
        [self](caf::ok_atom) {
            self->state().watch->start();
            self->mail(caf::tick_atom_v).send(caf::actor_cast<caf::actor>(self));
        },
        // Savina's NumberProducerActor: every candidate from this one handler, then the Exit.
        [self](caf::tick_atom) {
            auto &s = self->state();
            ++*s.starts;
            for (std::uint64_t n = 3; n < s.limit; n += 2) self->mail(n).send(s.first);
            self->mail(caf::close_atom_v).send(s.first);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto limit = static_cast<std::uint64_t>(p.get("limit"));
    const auto cores = static_cast<std::size_t>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);
    if (p.get("place") != 0)
        qvo::not_applicable("CAF's work-stealing pool places every actor itself; the spec's static "
                            "filter placement (place=1) has no counterpart -- read the place=0 cell");

    const auto                              nfilt = static_cast<std::size_t>(filters(p));
    std::vector<Tally>                      tallies(nfilt);
    std::vector<std::vector<std::uint64_t>> primes(nfilt);
    std::uint64_t                           starts = 0;
    Field                                   field{&tallies, &primes,
                                                  static_cast<std::uint64_t>(p.get("local")), &watch};
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        auto first = sys.spawn<qvocaf::kSpawnOptions>(filter_fun, &field, std::uint32_t{0},
                                                      std::uint64_t{2});
        sys.spawn<qvocaf::kSpawnOptions>(producer_fun, first, limit, &starts, &watch);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: every tally is final, and the tallies and the
    // primes are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, primes), delivered(tallies, starts)};
}

}  // namespace savina_sieve_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sieve::params();
    spec.expected          = qvospec::savina::sieve::expected;
    spec.expected_messages = qvospec::savina::sieve::expected_messages;
    spec.work_unit         = qvospec::savina::sieve::kWorkUnit;
    spec.work_units        = qvospec::savina::sieve::work_units;
    spec.idiom_source      = "the fib adapter + self->spawn() from a behavior (scheduled_actor.hpp)";
    spec.idiom_note        = "the reference's actors one for one; a filter spawns its successor "
                             "with self->spawn when a new prime finds no room; a bare uint64 per "
                             "candidate, close_atom for the Exit; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "every filter is spawned into the work-stealing pool, which places it and steals it: the "
        "spec's `place` axis (where qb and the floor put the filters) has no counterpart, and "
        "place=1 is reported not applicable -- see benchmarks/savina/sieve.md");

    return qvo::run(argc, argv, std::move(spec), savina_sieve_caf::body);
}
