// @benchmark     savina/counting
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("push = ordered, any event; send = unordered") and the
//                ping-pong adapter beside this file for the bootstrap and teardown.
// @idiom-note    `push<>` on the hot path, not `send<>`: the protocol requires the retrieve to
//                arrive AFTER every increment, and `send<>` gives no such guarantee -- a cross-core
//                `send<>` may deliver ahead of events still sitting in the pipe. `push<>` is also
//                the shape this workload wants: a million events batched into one growable pipe
//                and flushed in bulk, rather than a million attempted eager writes.

#include <qvospec/savina/counting.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

namespace savina_counting_qb {

using namespace qvospec::savina::counting;

struct Increment : qb::Event {
    std::uint64_t index{0};
    explicit Increment(std::uint64_t i) noexcept : index(i) {}
};
struct Retrieve : qb::Event {};
struct Result : qb::Event {
    std::uint64_t acc{0};
    std::uint64_t count{0};
    Result(std::uint64_t a, std::uint64_t c) noexcept : acc(a), count(c) {}
};

// Written by an actor on a worker thread, read after join() -- the same happens-before edge the
// ping-pong adapter relies on.
struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

class CounterActor final : public qb::Actor {
    std::uint64_t _acc{0};
    std::uint64_t _count{0};

public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<Increment>(*this);
        registerEvent<Retrieve>(*this);
        co_return true;
    }

    void on(Increment const &event) {
        _acc += qvo::mix(event.index);
        ++_count;
    }

    void on(Retrieve const &event) { push<Result>(event.getSource(), _acc, _count); }
};

class ProducerActor final : public qb::Actor {
    const std::uint64_t _n;
    qvo::Watch         &_watch;
    Sink               &_sink;

public:
    ProducerActor(std::uint64_t n, qvo::Watch &watch, Sink &sink) noexcept
        : _n(n), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<Result>(*this);
        require<CounterActor>();
        co_return true;
    }

    // The window opens once the counter has been discovered -- every core running and warm, so
    // the measured span is message passing and not thread creation (see ping-pong.cpp).
    void on(qb::RequireEvent const &event) {
        const qb::ActorId counter = event.getSource();
        _watch.start();
        for (std::uint64_t i = 0; i < _n; ++i) push<Increment>(counter, i);
        push<Retrieve>(counter);
    }

    void on(Result const &event) {
        _watch.stop();
        _sink.checksum = event.acc;
        _sink.messages = event.count + 2;  // the increments the counter saw, retrieve, result
        kill();
        push<qb::KillEvent>(event.getSource());
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n     = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink sink;
    {
        qb::Main engine;

        // Producer on core 0, counter on core 1 when two cores are budgeted: every increment
        // crosses a core and the counter's mailbox is a real cross-core queue.
        qvoqb::configure_core(engine, 0, spin);
        engine.addActor<ProducerActor>(0, n, std::ref(watch), std::ref(sink));

        const int counter_core = (cores >= 2) ? 1 : 0;
        if (counter_core != 0) qvoqb::configure_core(engine, counter_core, spin);
        engine.addActor<CounterActor>(counter_core);

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_counting_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::counting::params();
    spec.expected          = qvospec::savina::counting::expected;
    spec.expected_messages = qvospec::savina::counting::expected_messages;
    spec.work_unit         = qvospec::savina::counting::kWorkUnit;
    spec.work_units        = qvospec::savina::counting::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md (push = ordered) + the ping-pong adapter";
    spec.idiom_note        = "push<> for the ordered increment stream (send<> may reorder across "
                             "cores), require<>() bootstrap, one VirtualCore per actor";
    spec.caveats           = qvoqb::caveats();

    return qvo::run(argc, argv, std::move(spec), savina_counting_qb::body);
}
