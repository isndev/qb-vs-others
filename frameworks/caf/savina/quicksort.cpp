// @benchmark     savina/quicksort
// @framework     caf 1.1.0
// @idiom-source  the fib adapter beside this file (function-based behaviors, stateful_actor,
//                `self->spawn(...)` from inside a behavior for every child, `self->quit()` for the
//                actor's own end) and examples/custom_type/custom_types_1.cpp of CAF 1.1.0 for the
//                type-id block a message element needs (`CAF_BEGIN_TYPE_ID_BLOCK` +
//                `CAF_ADD_TYPE_ID`, `caf::init_global_meta_objects<...>()` before the system).
// @idiom-note    The values travel as a `std::vector<uint64_t>` moved into the message
//                (`self->mail(std::move(v))`) and taken by MUTABLE reference in the handler: a
//                message delivered to one actor is not shared, so CAF's copy-on-write hands the
//                handler the message's own vector, which it moves out -- the elements are never
//                copied in transit, as in qb's unique_ptr and SObjectizer's mutable message. A Sort
//                is `(vector)`, a Result `(vector, uint32 position, uint64 sorts, uint64 results)`.
//                The root learns its two seeds -- spawned before the window -- in a wiring message;
//                every other child is spawned by its parent into the work-stealing pool, which
//                places and balances the tree itself.

#include <qvospec/savina/quicksort.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>
#include <caf/type_id.hpp>

#include <cstdint>
#include <utility>
#include <vector>

CAF_BEGIN_TYPE_ID_BLOCK(qvo_quicksort, caf::first_custom_type_id)
    CAF_ADD_TYPE_ID(qvo_quicksort, (std::vector<uint64_t>))
CAF_END_TYPE_ID_BLOCK(qvo_quicksort)

namespace savina_quicksort_caf {

using namespace qvospec::savina::quicksort;

struct Sink {
    Values output;
    Counts counts;
};

struct sort_state {
    caf::actor    parent;
    Position      position{Position::kInitial};
    std::size_t   threshold{0};
    caf::actor    seeds[2];  // the root's, from the wiring message
    Values        held;
    Counts        counts;
    std::uint32_t fragments{0};
};

struct driver_state {
    caf::actor  root;
    caf::actor  seeds[2];
    std::size_t ready{0};
    Values     *input{nullptr};
    qvo::Watch *watch{nullptr};
    Sink       *sink{nullptr};
};

caf::behavior sort_fun(caf::stateful_actor<sort_state> *self, caf::actor parent, std::uint32_t position,
                       std::size_t threshold) {
    auto &st     = self->state();
    st.parent    = std::move(parent);
    st.position  = static_cast<Position>(position);
    st.threshold = threshold;

    auto finish = [self](Values data) {
        auto &s = self->state();
        self->mail(std::move(data), static_cast<std::uint32_t>(s.position), s.counts.sorts,
                   s.counts.results)
            .send(s.parent);
        self->quit();
    };
    auto child = [self](Position position) {
        auto &s = self->state();
        return self->spawn<qvocaf::kSpawnOptions>(sort_fun, caf::actor_cast<caf::actor>(self),
                                                  static_cast<std::uint32_t>(position), s.threshold);
    };

    return {
        // Wiring, once, outside the window: the root learns its seeds; every announced actor
        // answers the driver, so all three have been scheduled before the window opens.
        [self](caf::put_atom, caf::actor left, caf::actor right, caf::actor driver) {
            auto &s    = self->state();
            s.seeds[0] = std::move(left);
            s.seeds[1] = std::move(right);
            self->mail(caf::ok_atom_v).send(driver);
        },
        [self, finish, child](Values &data) {
            auto &s = self->state();
            ++s.counts.sorts;
            if (data.size() < s.threshold) {
                sort_leaf(data);
                finish(std::move(data));
                return;
            }
            Split            sp    = split(data);
            const bool       root  = s.position == Position::kInitial;
            const caf::actor left  = root ? s.seeds[0] : child(Position::kLeft);
            const caf::actor right = root ? s.seeds[1] : child(Position::kRight);
            self->mail(std::move(sp.less)).send(left);
            self->mail(std::move(sp.greater)).send(right);
            s.held      = std::move(sp.equal);
            s.fragments = 1;
        },
        [self, finish](Values &data, std::uint32_t position, std::uint64_t sorts,
                       std::uint64_t results) {
            auto &s = self->state();
            s.counts.add_result(Counts{sorts, results});
            join(s.held, data, static_cast<Position>(position));
            if (++s.fragments == 3) finish(std::move(s.held));
        },
    };
}

caf::behavior driver_fun(caf::stateful_actor<driver_state> *self, std::size_t threshold, Values *input,
                         qvo::Watch *watch, Sink *sink) {
    auto &st = self->state();
    st.input = input;
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.root       = self->spawn<qvocaf::kSpawnOptions>(sort_fun, me,
                                                 static_cast<std::uint32_t>(Position::kInitial),
                                                 threshold);
    st.seeds[0]   = self->spawn<qvocaf::kSpawnOptions>(sort_fun, st.root,
                                                     static_cast<std::uint32_t>(Position::kLeft),
                                                     threshold);
    st.seeds[1]   = self->spawn<qvocaf::kSpawnOptions>(sort_fun, st.root,
                                                     static_cast<std::uint32_t>(Position::kRight),
                                                     threshold);
    self->mail(caf::put_atom_v, st.seeds[0], st.seeds[1], me).send(st.root);
    for (const auto &seed : st.seeds) self->mail(caf::put_atom_v, caf::actor{}, caf::actor{}, me).send(seed);

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != 3) return;
            s.watch->start();
            self->mail(std::move(*s.input)).send(s.root);
        },
        [self](Values &data, std::uint32_t, std::uint64_t sorts, std::uint64_t results) {
            auto &s = self->state();
            s.watch->stop();
            s.sink->counts.add_result(Counts{sorts, results});
            s.sink->output = std::move(data);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores     = static_cast<std::size_t>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;
    const auto threshold = static_cast<std::size_t>(p.get("threshold"));
    qvocaf::refuse_spin_if_detached(spin);

    Sink   sink;
    Values values = input(p);
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(driver_fun, threshold, &values, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers; the sequence and the counts are reduced here,
    // outside the window.
    const Output o = measure(sink.output);
    explain(o, sink.counts, p);
    return qvo::Answer{answer(o, sink.counts), sink.counts.sorts + sink.counts.results};
}

}  // namespace savina_quicksort_caf

int main(int argc, char **argv) {
    caf::init_global_meta_objects<caf::id_block::qvo_quicksort>();
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::quicksort::params();
    spec.expected          = qvospec::savina::quicksort::expected;
    spec.expected_messages = qvospec::savina::quicksort::expected_messages;
    spec.work_unit         = qvospec::savina::quicksort::kWorkUnit;
    spec.work_units        = qvospec::savina::quicksort::work_units;
    spec.idiom_source      = "the fib adapter + self->spawn() from a behavior + "
                             "examples/custom_type (CAF_ADD_TYPE_ID for std::vector<uint64_t>)";
    spec.idiom_note        = "(vector) / (vector, position, sorts, results), the vector moved into "
                             "the mail and out of a mutable-reference handler; children spawned "
                             "into the work-stealing pool, the root's seeds before the window";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "a child spawned from a worker is enqueued on THAT worker and stolen from there, so with "
        "cores=2 the tree is balanced dynamically; qb's cell keeps each half of the first split on "
        "its seed's core because qb has no cross-core spawn -- see benchmarks/savina/quicksort.md");

    return qvo::run(argc, argv, std::move(spec), savina_quicksort_caf::body);
}
