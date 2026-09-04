// @benchmark     savina/counting
// @framework     caf 1.1.0
// @idiom-source  the ping-pong adapter beside this file (function-based behaviors,
//                stateful_actor, one handshake outside the window) and CAF's built-in atoms in
//                caf/type_id.hpp: `(add_atom, ...)` is the tag CAF's own calculator example
//                dispatches on (examples/message_passing/calculator.cpp:37, `self->mail(add_atom_v,
//                x, y)` at :82); `get_atom` / `ok_atom` come from the same built-in list.
// @idiom-note    Increments are `(add_atom, uint64)`, the retrieve is `(get_atom)`, the answer
//                `(ok_atom, uint64, uint64)`. The atom is the handler selector CAF is built
//                around; a bare uint64 would need a second message type for the retrieve anyway.
//                The counter learns the producer once in a handshake and never receives a handle
//                on the hot path.

#include <qvospec/savina/counting.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

namespace savina_counting_caf {

using namespace qvospec::savina::counting;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct counter_state {
    caf::actor    producer;
    std::uint64_t acc{0};
    std::uint64_t count{0};
};

struct producer_state {
    caf::actor    counter;
    std::uint64_t n{0};
    qvo::Watch   *watch{nullptr};
    Sink         *sink{nullptr};
};

caf::behavior counter_fun(caf::stateful_actor<counter_state> *self) {
    return {
        // Handshake: learn the producer once, acknowledge, and never carry a handle again.
        [self](caf::actor producer) {
            self->state().producer = producer;
            self->mail(caf::ok_atom_v).send(producer);
        },
        // The hot path.
        [self](caf::add_atom, std::uint64_t index) {
            auto &s = self->state();
            s.acc += qvo::mix(index);
            ++s.count;
        },
        [self](caf::get_atom) {
            auto &s = self->state();
            self->mail(caf::ok_atom_v, s.acc, s.count).send(s.producer);
        },
    };
}

caf::behavior producer_fun(caf::stateful_actor<producer_state> *self, caf::actor counter,
                           std::uint64_t n, qvo::Watch *watch, Sink *sink) {
    auto &st   = self->state();
    st.counter = std::move(counter);
    st.n       = n;
    st.watch   = watch;
    st.sink    = sink;

    self->mail(caf::actor_cast<caf::actor>(self)).send(st.counter);

    return {
        // The handshake round trip has completed: both actors have been scheduled at least once,
        // so the window that opens here contains message passing and not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            s.watch->start();
            for (std::uint64_t i = 0; i < s.n; ++i) self->mail(caf::add_atom_v, i).send(s.counter);
            self->mail(caf::get_atom_v).send(s.counter);
        },
        [self](caf::ok_atom, std::uint64_t acc, std::uint64_t count) {
            auto &s = self->state();
            s.watch->stop();
            s.sink->checksum = acc;
            s.sink->messages = count + 2;  // the increments the counter saw, retrieve, result
            self->send_exit(s.counter, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n     = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores = static_cast<std::size_t>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        auto counter = sys.spawn<qvocaf::kSpawnOptions>(counter_fun);
        sys.spawn<qvocaf::kSpawnOptions>(producer_fun, counter, n, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_counting_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::counting::params();
    spec.expected          = qvospec::savina::counting::expected;
    spec.expected_messages = qvospec::savina::counting::expected_messages;
    spec.work_unit         = qvospec::savina::counting::kWorkUnit;
    spec.work_units        = qvospec::savina::counting::work_units;
    spec.idiom_source      = "the ping-pong adapter + caf/type_id.hpp built-in atoms as in "
                             "examples/message_passing/calculator.cpp";
    spec.idiom_note        = "(add_atom, uint64) increments, (get_atom) retrieve, (ok_atom, acc, "
                             "count) answer; one handshake outside the window";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);

    return qvo::run(argc, argv, std::move(spec), savina_counting_caf::body);
}
