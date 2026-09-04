// @benchmark     savina/ping-pong
// @framework     caf 1.1.0
// @idiom-source  CAF's own examples, shipped in the 1.1.0 tree:
//                  examples/hello_world.cpp            -- function-based behaviors, sys.spawn(fun)
//                  examples/message_passing/dancing_kirby.cpp -- self->mail(...).send(dest), self->quit()
//                  caf/stateful_actor.hpp              -- the documented way to give an actor state
// @idiom-note    Messages are a bare `std::uint64_t` rather than an atom-tagged tuple. CAF's
//                examples use atoms mainly to disambiguate handlers; here the two actors have one
//                hot handler each, so the atom would be pure payload. This is the FASTER of the
//                two CAF spellings, and FAIRNESS.md 1.1 says the faster idiom is the one that
//                enters the table.
//
// A note on what is NOT done here, because it would flatter CAF's competitor: the peer handle is
// exchanged ONCE, in a handshake before the measured window, so the hot path carries no actor
// handle. Sending `caf::actor` on every hop would have been simpler to write and measurably
// slower, and it is exactly the sort of avoidable handicap this repository exists not to inflict.

#include <qvospec/savina/ping-pong.h>

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <cstdio>
#include <cstdlib>

namespace savina_ping_pong_caf {

using namespace qvospec::savina::ping_pong;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct pong_state {
    caf::actor peer;
};

struct ping_state {
    caf::actor    pong;
    std::uint64_t remaining{0};
    std::uint64_t acc{0};
    std::uint64_t delivered{0};
    qvo::Watch   *watch{nullptr};
    Sink         *sink{nullptr};
};

caf::behavior pong_fun(caf::stateful_actor<pong_state> *self) {
    return {
        // Handshake: learn the peer once, acknowledge, and never carry a handle again.
        [self](caf::actor peer) {
            self->state().peer = peer;
            self->mail(caf::ok_atom_v).send(peer);
        },
        // The hot path.
        [self](std::uint64_t seq) { self->mail(seq).send(self->state().peer); },
    };
}

caf::behavior ping_fun(caf::stateful_actor<ping_state> *self, caf::actor pong, std::uint64_t rounds,
                       qvo::Watch *watch, Sink *sink) {
    auto &st     = self->state();
    st.pong      = std::move(pong);
    st.remaining = rounds;
    st.watch     = watch;
    st.sink      = sink;

    self->mail(caf::actor_cast<caf::actor>(self)).send(st.pong);

    return {
        // The handshake round trip has completed: both actors have been scheduled at least once,
        // so the window that opens here contains message passing and not first-touch scheduling.
        // This mirrors what the qb implementation gets from its require<>() bootstrap.
        [self](caf::ok_atom) {
            auto &s = self->state();
            s.watch->start();
            self->mail(s.remaining - 1).send(s.pong);
        },
        [self](std::uint64_t seq) {
            auto &s = self->state();
            s.acc += qvo::mix(seq);
            s.delivered += 2;
            if (seq) {
                self->mail(seq - 1).send(s.pong);
            } else {
                s.watch->stop();
                s.sink->checksum = s.acc;
                s.sink->messages = s.delivered;
                self->send_exit(s.pong, caf::exit_reason::user_shutdown);
                self->quit();
            }
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores  = static_cast<std::size_t>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink sink;
    {
        caf::actor_system_config cfg;

        // The key is `caf.scheduler.max-threads`, read from the source of the pinned CAF
        // (libcaf_core/caf/scheduler.cpp: `get_or(cfg, "caf.scheduler.max-threads", ...)`), not
        // from memory or a web page.
        //
        // This matters more than it looks: CAF's default worker count comes from
        // hardware_concurrency, which on Windows ignores the process affinity mask. A silently
        // mistyped key would leave CAF spawning 24 workers onto the 2 CPUs the harness pinned --
        // a self-inflicted handicap that would look like a CAF result.
        cfg.set("caf.scheduler.max-threads", cores);

        // The spin/park lever, CAF's half.
        //
        // CAF's work-stealing workers poll aggressively, then moderately, then sleep. Keys read
        // from libcaf_core/caf/scheduler.cpp of the pinned CAF, lines 58-74. Raising the
        // aggressive budget high enough that a worker never leaves that phase is CAF's own way of
        // spelling "busy-spin", and it is what makes qb's setLatency(0) a comparable setting
        // rather than an unearned head start.
        // The exact spin profile is NOT guessed. `QVO_CAF_AGGRESSIVE_POLL` and
        // `QVO_CAF_STEAL_INTERVAL` exist so that docs/TUNING.md can sweep CAF's documented knobs
        // and record which profile is genuinely fastest, rather than this repository picking one
        // that happens to make its own framework look better. The defaults below are the winner
        // of that sweep; the sweep itself is reproducible and its table is published.
        if (spin) {
            auto env_or = [](const char *name, std::size_t fallback) -> std::size_t {
                if (const char *v = std::getenv(name)) return std::strtoull(v, nullptr, 10);
                return fallback;
            };
            cfg.set("caf.work-stealing.aggressive-poll-attempts",
                    env_or("QVO_CAF_AGGRESSIVE_POLL", 100));
            cfg.set("caf.work-stealing.aggressive-steal-interval",
                    env_or("QVO_CAF_STEAL_INTERVAL", 10));
        }

        caf::actor_system sys{cfg};

        // Read it back. A settings dictionary accepts any key, so the write proves nothing on its
        // own; this asserts the value is present under the exact name the scheduler reads.
        const auto effective = caf::get_or(sys.config(), "caf.scheduler.max-threads", std::size_t{0});
        if (effective != cores) {
            std::fprintf(stderr,
                         "qvo: CAF worker budget did not take effect (asked %zu, config reports "
                         "%zu). Refusing to report a number measured under a thread budget that "
                         "is not the one every other framework was given.\n",
                         cores, effective);
            std::exit(2);
        }

        auto pong = sys.spawn(pong_fun);
        sys.spawn(ping_fun, pong, rounds, &watch, &sink);
        sys.await_all_actors_done();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_ping_pong_caf

int main(int argc, char **argv) {
    // Must happen once, before any actor_system exists. The harness runs several repetitions in
    // one process, so this cannot live inside the body.
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::ping_pong::params();
    spec.expected          = qvospec::savina::ping_pong::expected;
    spec.expected_messages = qvospec::savina::ping_pong::expected_messages;
    spec.idiom_source      = "CAF 1.1.0 examples/hello_world.cpp + examples/message_passing/"
                             "dancing_kirby.cpp + caf/stateful_actor.hpp";
    spec.idiom_note        = "function-based behaviors, stateful_actor for the cached peer, bare "
                             "uint64_t on the hot path, one handshake outside the window";
    spec.caveats           = {
        "CAF's scheduler is a work-stealing thread pool; 'cores' is a thread BUDGET, not a "
        "placement. qb is given the same number of threads but also decides which actor runs on "
        "which one -- an architectural difference, not a tuning one",
        "wait=1 raises caf.work-stealing.aggressive-poll-attempts so workers never leave the "
        "aggressive polling phase (CAF's spelling of busy-spin); wait=0 leaves CAF's shipped "
        "defaults. Both values are always measured and always published",
        "CAF 1.1.0 builds itself at C++17 (its own CMake sets the standard); qb and the harness "
        "are C++20"};

    return qvo::run(argc, argv, std::move(spec), savina_ping_pong_caf::body);
}
