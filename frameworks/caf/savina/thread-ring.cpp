// @benchmark     savina/thread-ring
// @framework     caf 1.1.0
// @idiom-source  the ping-pong adapter beside this file (function-based behaviors,
//                stateful_actor, handles exchanged before the window) and caf/type_id.hpp's
//                built-in `put_atom` / `ok_atom` for the wiring handshake.
// @idiom-note    The token is a bare `(uint64 remaining, uint64 acc)` pair -- the one hot handler
//                per actor needs no atom to disambiguate it, the same reasoning as ping-pong.
//                Each actor learns its successor ONCE, from the sink, before the window opens:
//                a ring cannot be spawned with its `next` as a constructor argument because the
//                last actor's successor is the first, which does not exist yet.

#include <qvospec/savina/thread-ring.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_thread_ring_caf {

using namespace qvospec::savina::thread_ring;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct ring_state {
    caf::actor next;
    caf::actor sink;
};

struct sink_state {
    std::vector<caf::actor> ring;
    std::uint64_t           hops{0};
    std::size_t             wired{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior ring_fun(caf::stateful_actor<ring_state> *self) {
    return {
        // Wiring handshake, once per actor, outside the window.
        [self](caf::put_atom, caf::actor next, caf::actor sink) {
            auto &s = self->state();
            s.next  = std::move(next);
            s.sink  = sink;
            self->mail(caf::ok_atom_v).send(sink);
        },
        // The hot path: one token, mutated and passed on.
        [self](std::uint64_t remaining, std::uint64_t acc) {
            auto &s = self->state();
            acc += qvo::mix(remaining);
            if (--remaining == 0)
                self->mail(caf::ok_atom_v, acc).send(s.sink);
            else
                self->mail(remaining, acc).send(s.next);
        },
    };
}

caf::behavior sink_fun(caf::stateful_actor<sink_state> *self, std::vector<caf::actor> ring,
                       std::uint64_t hops, qvo::Watch *watch, Sink *sink) {
    auto &st = self->state();
    st.ring  = std::move(ring);
    st.hops  = hops;
    st.watch = watch;
    st.sink  = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (std::size_t i = 0; i < st.ring.size(); ++i) {
        const std::size_t next = i + 1 == st.ring.size() ? 0 : i + 1;
        self->mail(caf::put_atom_v, st.ring[next], me).send(st.ring[i]);
    }

    return {
        // Every actor has been scheduled at least once and knows its successor: the window that
        // opens here contains message passing and not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.ring.size()) return;
            s.watch->start();
            self->mail(s.hops, std::uint64_t{0}).send(s.ring[0]);
        },
        [self](caf::ok_atom, std::uint64_t acc) {
            auto &s = self->state();
            s.watch->stop();
            s.sink->checksum = acc;
            s.sink->messages = s.hops + 1;
            for (auto &a : s.ring) self->send_exit(a, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::size_t>(p.get("actors"));
    const auto hops   = static_cast<std::uint64_t>(p.get("hops"));
    const auto cores  = static_cast<std::size_t>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> ring;
        ring.reserve(actors);
        for (std::size_t i = 0; i < actors; ++i)
            ring.push_back(sys.spawn<qvocaf::kSpawnOptions>(ring_fun));
        sys.spawn<qvocaf::kSpawnOptions>(sink_fun, ring, hops, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_thread_ring_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::thread_ring::params();
    spec.expected          = qvospec::savina::thread_ring::expected;
    spec.expected_messages = qvospec::savina::thread_ring::expected_messages;
    spec.work_unit         = qvospec::savina::thread_ring::kWorkUnit;
    spec.work_units        = qvospec::savina::thread_ring::work_units;
    spec.idiom_source      = "the ping-pong adapter + caf/type_id.hpp built-in put_atom/ok_atom";
    spec.idiom_note        = "bare (uint64, uint64) token; successor learnt once in a wiring "
                             "handshake outside the window; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the ring's actors are placed by the work-stealing pool: a hop stays on the sender's "
        "worker (worker::delay) unless the idle worker steals it, so with cores=2 CAF measures "
        "mostly same-thread hand-offs where qb measures a cross-core pipe on EVERY hop -- see "
        "benchmarks/savina/thread-ring.md");

    return qvo::run(argc, argv, std::move(spec), savina_thread_ring_caf::body);
}
