// @benchmark     savina/ping-pong
// @framework     qb
// @idiom-source  qb/tests/core/benchmark/messaging/ping-pong-latency.cpp -- qb's OWN ping-pong
//                benchmark, written by the framework's maintainer. The event shape (a small
//                trivially-destructible event carrying a countdown), the `require<T>()` +
//                `on(RequireEvent)` bootstrap, `send<>` for the hot path, and the
//                `kill(); send<qb::KillEvent>(peer)` teardown are all taken from it.
// @idiom-note    `send<>` is used rather than `push<>`: the event is trivially destructible and
//                the workload is a strict single-message ping-pong, so the ordering guarantee
//                `push<>` buys is not needed here. This is the faster of the two qb idioms and is
//                what qb's own benchmark uses.

#include <qvospec/savina/ping-pong.h>

#include <cstdlib>

#include <qb/actor.h>
#include <qb/main.h>

namespace savina_ping_pong_qb {

using namespace qvospec::savina::ping_pong;

// Trivially destructible, and small: a `send<>` requirement, and the shape qb's own benchmarks use.
struct Ball : qb::Event {
    std::uint64_t seq{0};
    explicit Ball(std::uint64_t s) noexcept : seq(s) {}
};

// Where the verified answer is left for the harness. It is written by an actor on a worker thread
// and read after `join()`, which is a happens-before edge qb's shutdown already establishes.
struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

class PongActor final : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<Ball>(*this);
        co_return true;
    }

    // Non-const handler: `reply()` recycles the event's own bytes, which is qb's cheapest
    // round-trip and the one its benchmark uses.
    void on(Ball &event) { reply(event); }
};

class PingActor final : public qb::Actor {
    const std::uint64_t _rounds;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::uint64_t       _acc{0};
    std::uint64_t       _delivered{0};

public:
    PingActor(std::uint64_t rounds, qvo::Watch &watch, Sink &sink) noexcept
        : _rounds(rounds), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<Ball>(*this);
        require<PongActor>();
        co_return true;
    }

    // The workload window opens HERE, not at engine.start(): by the time the peer has been
    // discovered every core is running and warm, so the measured span is message passing and not
    // thread creation. Framework startup is still measured -- as `outside_window_ns`.
    void on(qb::RequireEvent const &event) {
        _watch.start();
        send<Ball>(event.getSource(), _rounds - 1);
    }

    void on(Ball const &event) {
        _acc += qvo::mix(event.seq);
        _delivered += 2;  // the ping that went out and the pong that came back

        if (event.seq) {
            send<Ball>(event.getSource(), event.seq - 1);
        } else {
            _watch.stop();
            _sink.checksum = _acc;
            _sink.messages = _delivered;
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    // The spin/park lever, applied identically in every framework's adapter.
    //
    // `setLatency(0)` makes a VirtualCore busy-spin; a non-zero value makes it park on a
    // condition variable for AT MOST that long -- Main.h's Mailbox::notify() signals the cv on
    // every enqueue, so a message still wakes the core immediately. The interval is therefore an
    // idle cap, not an added per-message latency, which is what makes it the honest counterpart
    // of CAF's parking scheduler and SObjectizer's simple_lock_factory rather than a handicap.
    // The park interval is NOT a guess either. `QVO_QB_PARK_US` lets docs/TUNING.md sweep it the
    // same way CAF's work-stealing knobs are swept, and the default below is the winner of that
    // sweep. Symmetry is the point: a repository that tunes its competitor's park setting while
    // leaving its own at an arbitrary value has rigged the axis in the other direction.
    std::chrono::microseconds park{1000};
    if (const char *v = std::getenv("QVO_QB_PARK_US"))
        park = std::chrono::microseconds{std::strtoull(v, nullptr, 10)};

    const qb::duration latency = spin ? qb::duration::zero() : qb::duration{park};

    Sink sink;
    {
        qb::Main engine;

        engine.core(0).setLatency(latency);
        engine.addActor<PongActor>(0);

        const int ping_core = (cores >= 2) ? 1 : 0;
        if (ping_core != 0) engine.core(ping_core).setLatency(latency);
        engine.addActor<PingActor>(ping_core, rounds, std::ref(watch), std::ref(sink));

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_ping_pong_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::ping_pong::params();
    spec.expected          = qvospec::savina::ping_pong::expected;
    spec.expected_messages = qvospec::savina::ping_pong::expected_messages;
    spec.idiom_source      = "qb/tests/core/benchmark/messaging/ping-pong-latency.cpp";
    spec.idiom_note        = "send<> hot path, require<>() bootstrap, busy-spin cores -- qb's own "
                             "benchmark idiom";
    spec.caveats           = {
        "qb requires an explicit core per actor; 'cores' maps to that placement directly, whereas "
        "pool-based frameworks are only given a thread budget. This is an architectural "
        "difference in qb's favour on this workload, not a tuning advantage",
        "wait=1 maps to setLatency(0) (busy-spin, 100% CPU per core); wait=0 maps to a 1 ms park "
        "cap on a cv that is signalled on every enqueue"};

    return qvo::run(argc, argv, std::move(spec), savina_ping_pong_qb::body);
}
