// @benchmark     savina/big
// @framework     caf 1.1.0
// @idiom-source  the ping-pong adapter beside this file (function-based behaviors,
//                stateful_actor, handles exchanged before the window) and caf/type_id.hpp's
//                built-in `ping_atom` / `pong_atom`, which exist for exactly this protocol.
// @idiom-note    A ping is `(ping_atom, uint64 pinger, uint64 k)` and a pong `(pong_atom, uint64
//                value)`; the two need an atom because one actor handles both. Every actor holds
//                the whole handle vector (a built-in CAF type, caf/type_id.hpp) from one wiring
//                message before the window, so a pong goes to `peers[pinger]` and no hot message
//                carries a handle.

#include <qvospec/savina/big.h>

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

namespace savina_big_caf {

using namespace qvospec::savina::big;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct big_state {
    std::vector<caf::actor> peers;
    caf::actor              sink;
    std::uint32_t           self{0};
    std::uint32_t           pings{0};
    TargetSequence          seq;
    std::uint64_t           acc{0};
    std::uint64_t           received{0};
    std::uint32_t           sent{0};

    big_state() : seq(0, 2) {}
};

struct sink_state {
    std::vector<caf::actor> actors;
    std::size_t             wired{0};
    std::size_t             done{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior big_fun(caf::stateful_actor<big_state> *self, std::uint32_t index,
                      std::uint32_t actors, std::uint32_t pings) {
    auto &st = self->state();
    st.self  = index;
    st.pings = pings;
    st.seq   = TargetSequence(index, actors);

    auto ping = [self] {
        auto &s              = self->state();
        const std::uint32_t k = s.sent++;
        self->mail(caf::ping_atom_v, std::uint64_t{s.self}, std::uint64_t{k})
            .send(s.peers[s.seq.next()]);
    };

    return {
        // Wiring handshake, once, outside the window.
        [self](caf::put_atom, std::vector<caf::actor> peers, caf::actor sink) {
            auto &s = self->state();
            s.peers = std::move(peers);
            s.sink  = sink;
            self->mail(caf::ok_atom_v).send(sink);
        },
        [self, ping](caf::tick_atom) {
            ++self->state().received;
            ping();
        },
        [self](caf::ping_atom, std::uint64_t pinger, std::uint64_t k) {
            auto &s = self->state();
            self->mail(caf::pong_atom_v, pong_value(static_cast<std::uint32_t>(pinger), s.self,
                                                    static_cast<std::uint32_t>(k)))
                .send(s.peers[pinger]);
        },
        // Two deliveries proven by one pong: the ping it answers and itself. Pings received
        // from peers are not counted: they may keep arriving after the done has been sent.
        [self, ping](caf::pong_atom, std::uint64_t value) {
            auto &s = self->state();
            s.received += 2;
            s.acc += value;
            if (s.sent < s.pings)
                ping();
            else
                self->mail(caf::ok_atom_v, s.acc, s.received).send(s.sink);
        },
    };
}

caf::behavior sink_fun(caf::stateful_actor<sink_state> *self, std::vector<caf::actor> actors,
                       qvo::Watch *watch, Sink *sink) {
    auto &st  = self->state();
    st.actors = std::move(actors);
    st.watch  = watch;
    st.sink   = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &a : st.actors) self->mail(caf::put_atom_v, st.actors, me).send(a);

    return {
        // Every actor has been scheduled at least once and holds the field: the window opens on
        // message passing, not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.actors.size()) return;
            s.watch->start();
            for (auto &a : s.actors) self->mail(caf::tick_atom_v).send(a);
        },
        [self](caf::ok_atom, std::uint64_t acc, std::uint64_t received) {
            auto &s = self->state();
            s.sink->checksum += acc;
            s.sink->messages += received + 1;
            if (++s.done != s.actors.size()) return;
            s.watch->stop();
            for (auto &a : s.actors) self->send_exit(a, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto pings  = static_cast<std::uint32_t>(p.get("pings"));
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
        field.reserve(actors);
        for (std::uint32_t a = 0; a < actors; ++a)
            field.push_back(sys.spawn<qvocaf::kSpawnOptions>(big_fun, a, actors, pings));
        sys.spawn<qvocaf::kSpawnOptions>(sink_fun, field, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_big_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::big::params();
    spec.expected          = qvospec::savina::big::expected;
    spec.expected_messages = qvospec::savina::big::expected_messages;
    spec.work_unit         = qvospec::savina::big::kWorkUnit;
    spec.work_units        = qvospec::savina::big::work_units;
    spec.idiom_source      = "the ping-pong adapter + caf/type_id.hpp built-in ping_atom/pong_atom";
    spec.idiom_note        = "(ping_atom, pinger, k) / (pong_atom, value) tuples; the handle "
                             "vector delivered once before the window; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the 120 actors are placed by the work-stealing pool, so how many pings cross a core is "
        "the scheduler's decision; qb's cell fixes actor a on core a % cores -- see "
        "benchmarks/savina/big.md");

    return qvo::run(argc, argv, std::move(spec), savina_big_caf::body);
}
