// @benchmark     savina/big
// @framework     qb
// @idiom-source  the ping-pong adapter beside this file (one request in flight per actor, the
//                pong recycled with `reply()`) and qb/llm/qb.llm.md for `push<>`.
// @idiom-note    A Ping is answered by mutating it into its own Pong and `reply()`ing it -- one
//                event object per round trip, qb's cheapest exchange. The next Ping is a fresh
//                `push<>`. Actor a lives on VirtualCore a % cores, so with cores=2 about half of
//                all pings and pongs cross a core and every core's inbound queue has ~60 writers
//                on the other side.

#include <qvospec/savina/big.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_big_qb {

using namespace qvospec::savina::big;

// One event for both halves of a round trip: it goes out as a ping (pinger, k) and comes back as
// a pong (value), the way ping-pong's Ball comes back with its own bytes.
struct Ping : qb::Event {
    std::uint32_t pinger{0};
    std::uint32_t k{0};
    std::uint64_t value{0};
    Ping(std::uint32_t p, std::uint32_t kk) noexcept : pinger(p), k(kk) {}
};
struct Start : qb::Event {};
struct Ready : qb::Event {};
struct Done : qb::Event {
    std::uint64_t acc{0};
    std::uint64_t received{0};
    Done(std::uint64_t a, std::uint64_t r) noexcept : acc(a), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct Field {
    std::vector<qb::ActorId> actors;
    qb::ActorId              sink;
};

class BigActor final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _self;
    const std::uint32_t _pings;
    TargetSequence      _seq;
    std::uint64_t       _acc{0};
    std::uint64_t       _received{0};
    std::uint32_t       _sent{0};

    void ping() {
        const std::uint32_t k = _sent++;
        // push<>, not send<>: the eager cross-core variant was measured and is NOT faster here
        // (WSL2 g++ 14, 3-rep min, ns per round trip: send 26.1 / 25.9 / 26.2 against push
        // 24.9 / 25.8 / 25.1 for 1c-spin / 2c-spin / 2c-park). With one request in flight per
        // actor and 120 actors, the batch a pipe flush carries is what send<> gives up.
        push<Ping>(_field.actors[_seq.next()], _self, k);
    }

public:
    BigActor(const Field &field, std::uint32_t self, std::uint32_t actors,
             std::uint32_t pings) noexcept
        : _field(field), _self(self), _pings(pings), _seq(self, actors) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Start>(*this);
        registerEvent<Ping>(*this);
        push<Ready>(_field.sink);
        co_return true;
    }

    void on(Start const &) {
        ++_received;
        ping();
    }

    // Both halves land here: a ping from a peer (answer it in place) or our own ping coming back
    // as a pong (value set, and getSource() is the peer that answered).
    void on(Ping &event) {
        if (event.pinger != _self) {
            event.value = pong_value(event.pinger, _self, event.k);
            reply(event);
            return;
        }
        // Two deliveries proven by one pong: the ping it answers and itself. Pings received
        // from peers are not counted: they may keep arriving after Done has been sent.
        _received += 2;
        _acc += event.value;
        if (_sent < _pings)
            ping();
        else
            push<Done>(_field.sink, _acc, _received);
    }
};

class SinkActor final : public qb::Actor {
    const Field &_field;
    qvo::Watch  &_watch;
    Sink        &_sink;
    std::size_t  _ready{0};
    std::size_t  _done{0};

public:
    SinkActor(const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.actors.size()) return;
        _watch.start();
        for (const auto &a : _field.actors) push<Start>(a);
    }

    void on(Done const &event) {
        _sink.checksum += event.acc;
        _sink.messages += event.received + 1;
        if (++_done == _field.actors.size()) {
            _watch.stop();
            broadcast<qb::KillEvent>();
        }
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto pings  = static_cast<std::uint32_t>(p.get("pings"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.sink = engine.addActor<SinkActor>(0, std::cref(field), std::ref(watch),
                                                std::ref(sink));
        field.actors.reserve(actors);
        for (std::uint32_t a = 0; a < actors; ++a)
            field.actors.push_back(engine.addActor<BigActor>(
                static_cast<qb::CoreId>(a % static_cast<std::uint32_t>(ncores)),
                std::cref(field), a, actors, pings));

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_big_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::big::params();
    spec.expected          = qvospec::savina::big::expected;
    spec.expected_messages = qvospec::savina::big::expected_messages;
    spec.work_unit         = qvospec::savina::big::kWorkUnit;
    spec.work_units        = qvospec::savina::big::work_units;
    spec.idiom_source      = "the ping-pong adapter (reply() recycling) + qb/llm/qb.llm.md";
    spec.idiom_note        = "one Ping event per round trip, answered in place with reply(); "
                             "actor a on VirtualCore a % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "actor a lives on VirtualCore a % cores, fixed before start: no load balancing, and with "
        "cores=2 each core's inbound queue is written by the ~60 actors of the other core");

    return qvo::run(argc, argv, std::move(spec), savina_big_qb::body);
}
