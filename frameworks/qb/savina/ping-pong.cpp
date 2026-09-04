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

#include "../qb_support.h"

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

    Sink sink;
    {
        qb::Main engine;

        // Placement and spin/park both come from qvoqb::configure_core, shared by every qb
        // benchmark here. It pins each VirtualCore to one CPU of the harness's set -- the
        // mechanism qb is built on, and the one an earlier revision of this file accidentally
        // switched off by pinning only the process.
        qvoqb::configure_core(engine, 0, spin);
        engine.addActor<PongActor>(0);

        const int ping_core = (cores >= 2) ? 1 : 0;
        if (ping_core != 0) qvoqb::configure_core(engine, ping_core, spin);
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
    spec.caveats           = qvoqb::caveats();

    return qvo::run(argc, argv, std::move(spec), savina_ping_pong_qb::body);
}
