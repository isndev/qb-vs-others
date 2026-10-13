// @benchmark     savina/radixsort
// @framework     caf 1.1.0
// @idiom-source  the thread-ring adapter beside this file (function-based behaviors,
//                stateful_actor, a bare `uint64` as the hot message, built-in atoms for the
//                handshake and the end) -- CAF's own idioms from libcaf_core/caf/type_id.hpp and
//                scheduled_actor.hpp.
// @idiom-note    The reference's actors one for one, spawned as RadixSortAkkaActorBenchmark spawns
//                them: the validation first, then the stages from the last to the first, each
//                handed its successor, then the source. A value is a bare `uint64`; a stage mails
//                a value whose bit is 0 on at once and the held ones from the handler of its last
//                arrival. Messages from one sender to one receiver keep their order in CAF, which
//                the sort needs. Before the window the source asks every stage and the
//                validation to report in (`get_atom` -> `ok_atom`); the validation, on the last
//                value, stops the stages (`send_exit`) and quits. The pool places everything.

#include <qvospec/savina/radixsort.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_radixsort_caf {

using namespace qvospec::savina::radixsort;

struct Sink {
    std::uint64_t starts{0};  // the Starts the source's handler ran
};

struct source_state {
    std::vector<caf::actor> stages;
    std::uint64_t           values{0};
    long long               seed{0};
    long long               bits{0};
    std::size_t             ready{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

struct stage_state {
    Stage                      stage;
    caf::actor                 next;
    std::vector<std::uint64_t> held;
};

struct validation_state {
    Validation             *validation{nullptr};  // a line of its own the body owns
    std::vector<caf::actor> stages;               // stopped at the end of the run
    qvo::Watch             *watch{nullptr};
};

// A stage: its Counts are its tally slot's (a line of its own; CAF's actor objects come from the
// heap and two of them may share one).
caf::behavior stage_fun(caf::stateful_actor<stage_state> *self, std::uint32_t index,
                        std::uint64_t values, caf::actor next, Tally *tally) {
    auto &st = self->state();
    st.stage = Stage(index, values, *tally);
    st.next  = std::move(next);
    st.held.reserve(static_cast<std::size_t>(values));
    return {
        // Handshake, once, outside the window.
        [self](caf::get_atom, caf::actor source) { self->mail(caf::ok_atom_v).send(source); },
        [self](std::uint64_t value) {
            auto &s = self->state();
            if (s.stage.take(value)) self->mail(value).send(s.next);
            else s.held.push_back(value);
            if (s.stage.complete())
                for (const std::uint64_t h : s.held) self->mail(h).send(s.next);
        },
    };
}

caf::behavior validation_fun(caf::stateful_actor<validation_state> *self, Validation *validation,
                             qvo::Watch *watch) {
    auto &st      = self->state();
    st.validation = validation;
    st.watch      = watch;
    return {
        // Handshake, once, outside the window: the validation learns whom to stop.
        [self](caf::put_atom, std::vector<caf::actor> stages, caf::actor source) {
            self->state().stages = std::move(stages);
            self->mail(caf::ok_atom_v).send(source);
        },
        // The last value ends the run; one after it fails it (radixsort::overflow).
        [self](std::uint64_t value) {
            auto &s = self->state();
            if (!s.validation->take(value)) return;
            s.watch->stop();
            for (auto &a : s.stages) self->send_exit(a, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

caf::behavior source_fun(caf::stateful_actor<source_state> *self, std::vector<caf::actor> stages,
                         caf::actor validation, std::uint64_t values, long long seed,
                         long long bits, qvo::Watch *watch, Sink *sink) {
    auto &st  = self->state();
    st.stages = std::move(stages);
    st.values = values;
    st.seed   = seed;
    st.bits   = bits;
    st.watch  = watch;
    st.sink   = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &s : st.stages) self->mail(caf::get_atom_v, me).send(s);
    self->mail(caf::put_atom_v, st.stages, me).send(validation);

    return {
        // Every stage and the validation have been scheduled once: the window that opens here
        // contains the sort and not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.stages.size() + 1) return;
            s.watch->start();
            self->mail(caf::tick_atom_v).send(caf::actor_cast<caf::actor>(self));
        },
        // Savina's IntSourceActor: every value from this one handler, then the source ends.
        [self](caf::tick_atom) {
            auto &s = self->state();
            ++s.sink->starts;
            Source            source(s.seed, s.bits);
            const caf::actor &first = s.stages.front();
            for (std::uint64_t i = 0; i < s.values; ++i) self->mail(source.next()).send(first);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto values = static_cast<std::uint64_t>(p.get("values"));
    const auto bits   = static_cast<std::uint32_t>(p.get("bits"));
    const auto cores  = static_cast<std::size_t>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink                       sink;
    std::vector<Tally>         tallies(bits);
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(values));
    ValidationSlot             validator{Validation(sequence.data(), values)};
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        // As the reference: the validation, then the stages from the last to the first.
        caf::actor validation =
            sys.spawn<qvocaf::kSpawnOptions>(validation_fun, &validator.validation, &watch);
        std::vector<caf::actor> stages(bits);
        caf::actor              next = validation;
        for (std::uint32_t k = bits; k-- > 0;) {
            stages[k] = sys.spawn<qvocaf::kSpawnOptions>(stage_fun, k, values, next, &tallies[k]);
            next      = stages[k];
        }
        sys.spawn<qvocaf::kSpawnOptions>(source_fun, std::move(stages), validation, values,
                                         p.get("seed"), p.get("bits"), &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: every tally is final, and the tallies and the
    // sequence are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence),
                       delivered(tallies, validator.validation.count(), sink.starts)};
}

}  // namespace savina_radixsort_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::radixsort::params();
    spec.expected          = qvospec::savina::radixsort::expected;
    spec.expected_messages = qvospec::savina::radixsort::expected_messages;
    spec.work_unit         = qvospec::savina::radixsort::kWorkUnit;
    spec.work_units        = qvospec::savina::radixsort::work_units;
    spec.idiom_source      = "the thread-ring adapter + caf/type_id.hpp built-in atoms";
    spec.idiom_note        = "the reference's actors one for one, spawned in its order (validation, "
                             "stages last to first, source); a bare uint64 per value; placement "
                             "left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the chain's actors are placed by the work-stealing pool: an actor made ready by a mail is "
        "prepended to the sender's worker (worker::delay) unless the idle worker steals it, so how "
        "much of the chain runs on each worker is the scheduler's decision, where qb and the floor "
        "cut it into two contiguous halves -- see benchmarks/savina/radixsort.md");

    return qvo::run(argc, argv, std::move(spec), savina_radixsort_caf::body);
}
