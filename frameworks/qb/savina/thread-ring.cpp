// @benchmark     savina/thread-ring
// @framework     qb
// @idiom-source  qb/src/qb/core/Actor.h `forward()` ("delegate an event to another actor without
//                creating a new event"), the ping-pong adapter's `reply()` being its sibling; ids
//                shared the way qb/llm/qb.llm.md shows (`addActor` returns an ActorId usable
//                before start()).
// @idiom-note    The token is ONE event for its whole life: each actor mutates it in place and
//                `forward()`s it to the next, exactly as ping-pong's `reply()` recycles the ball.
//                Actor i lives on VirtualCore i % cores, so with cores=2 every single hop crosses
//                a core -- the placement the spec asks for and the worst one for qb.

#include <qvospec/savina/thread-ring.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_thread_ring_qb {

using namespace qvospec::savina::thread_ring;

struct Token : qb::Event {
    std::uint64_t remaining{0};
    std::uint64_t acc{0};
    explicit Token(std::uint64_t hops) noexcept : remaining(hops) {}
};
struct Ready : qb::Event {};
struct Result : qb::Event {
    std::uint64_t acc{0};
    explicit Result(std::uint64_t a) noexcept : acc(a) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t results{0};  // the results the sink's handler ran
};

// The ring actors are created before start(), so every id is known to every actor from its
// constructor -- no discovery protocol on the hot path.
struct Ring {
    std::vector<qb::ActorId>   ids;
    qb::ActorId                sink;
    std::vector<std::uint64_t> received;  // slot i: the tokens actor i's handler ran, at teardown
};

class RingActor final : public qb::Actor {
    Ring               &_ring;
    const std::uint32_t _index;
    std::uint64_t       _received{0};

public:
    RingActor(Ring &ring, std::uint32_t index) noexcept : _ring(ring), _index(index) {}

    // Runs on this actor's VirtualCore when the engine tears it down, after the window; the body
    // reads the slot once the engine is gone.
    ~RingActor() noexcept override { _ring.received[_index] = _received; }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Token>(*this);
        push<Ready>(_ring.sink);
        co_return true;
    }

    void on(Token &event) {
        ++_received;
        event.acc += term(_index, event.remaining);
        if (--event.remaining == 0) {
            push<Result>(_ring.sink, event.acc);
        } else {
            const std::uint32_t next = _index + 1 == _ring.ids.size() ? 0 : _index + 1;
            forward(_ring.ids[next], event);
        }
    }
};

class SinkActor final : public qb::Actor {
    const Ring         &_ring;
    const std::uint64_t _hops;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::uint32_t       _ready{0};

public:
    SinkActor(const Ring &ring, std::uint64_t hops, qvo::Watch &watch, Sink &sink) noexcept
        : _ring(ring), _hops(hops), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Result>(*this);
        co_return true;
    }

    // The window opens once every ring actor has reported in: every core running and warm.
    void on(Ready const &) {
        if (++_ready == _ring.ids.size()) {
            _watch.start();
            push<Token>(_ring.ids[0], _hops);
        }
    }

    // The first result ends the run; one after it is counted, never measured.
    void on(Result const &event) {
        if (_sink.results++ != 0) return;
        _watch.stop();
        _sink.checksum = event.acc;
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto hops   = static_cast<std::uint64_t>(p.get("hops"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink sink;
    Ring ring;
    ring.received.assign(actors, 0);
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        ring.sink = engine.addActor<SinkActor>(0, std::cref(ring), hops, std::ref(watch),
                                               std::ref(sink));
        ring.ids.reserve(actors);
        for (std::uint32_t i = 0; i < actors; ++i)
            ring.ids.push_back(engine.addActor<RingActor>(
                static_cast<qb::CoreId>(i % static_cast<std::uint32_t>(ncores)), std::ref(ring),
                i));

        engine.start();
        engine.join();
    }
    // Every actor was destroyed with the engine: each count is in its slot.
    std::uint64_t messages = sink.results;
    for (const std::uint64_t n : ring.received) messages += n;
    return qvo::Answer{sink.checksum, messages};
}

}  // namespace savina_thread_ring_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::thread_ring::params();
    spec.expected          = qvospec::savina::thread_ring::expected;
    spec.expected_messages = qvospec::savina::thread_ring::expected_messages;
    spec.work_unit         = qvospec::savina::thread_ring::kWorkUnit;
    spec.work_units        = qvospec::savina::thread_ring::work_units;
    spec.idiom_source      = "qb/src/qb/core/Actor.h forward() + the ping-pong adapter";
    spec.idiom_note        = "one Token event forward()ed around the ring in place; actor i on "
                             "VirtualCore i % cores, so every hop crosses a core at cores=2";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "actor i lives on VirtualCore i % cores: with cores=2 EVERY hop is a cross-core hand-off. "
        "CAF and SObjectizer receive the same 2-thread budget but their schedulers decide where "
        "each hop runs, so the same cell measures qb's cross-core pipe against their intra-thread "
        "hand-off -- see benchmarks/savina/thread-ring.md");

    return qvo::run(argc, argv, std::move(spec), savina_thread_ring_qb::body);
}
