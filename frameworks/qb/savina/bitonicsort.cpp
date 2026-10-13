// @benchmark     savina/bitonicsort
// @framework     qb
// @idiom-source  the thread-ring adapter beside this file (ids shared before start() the way
//                qb/llm/qb.llm.md shows -- `addActor` returns an ActorId usable before the engine
//                runs -- tallies kept in the actor and handed to their slot by its destructor) and
//                qb/src/qb/core/Actor.h `push_forward()`: "Forward a received event to a new
//                destination with push's ordering, reusing the event object".
// @idiom-note    One actor class per kind of the reference's network (the spec's topology(), built
//                before the window, 44 772 actors at the defaults), each a few lines around the
//                spec's Splitter / Comparator / Joiner. A value that passes through unchanged -- a
//                stage, a step, a partition, a splitter, a DataValueAdapter -- is the received event
//                `push_forward`ed in place; a compare-exchange, a joiner and a ValueDataAdapter push a
//                new one. Every hop is ordered (push or push_forward, never the unordered forward):
//                a joiner's lanes and the Exit wave depend on per-sender FIFO. With cores=2 the steps
//                are cut into two contiguous halves of equal message weight (bitonicsort.h,
//                placement()): a value crosses a core once, never inside a step.

#include <qvospec/savina/bitonicsort.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_bitonicsort_qb {

using namespace qvospec::savina::bitonicsort;

struct Value : qb::Event {
    std::uint64_t value;
    explicit Value(std::uint64_t v) noexcept : value(v) {}
};
struct Data : qb::Event {
    std::uint32_t order;
    std::uint64_t value;
    Data(std::uint32_t o, std::uint64_t v) noexcept : order(o), value(v) {}
};
struct Exit : qb::Event {};
struct Start : qb::Event {};
struct Ready : qb::Event {};

// Filled before start() -- every id is assigned at addActor time -- and read-only from then on.
struct Net {
    const Topology          *topology{nullptr};
    const Values            *input{nullptr};
    std::vector<qb::ActorId> ids;
    std::vector<Tally>      *tallies{nullptr};
    Values                  *out{nullptr};
    qvo::Watch              *watch{nullptr};
};

// What every node shares: its place in the network, its counts -- kept in the actor, on the line
// its handlers already touch (an actor lives in its core's thread arena, so no actor of another core
// shares it: thread-ring.h, Counts) -- and the hand-over of those counts to its slot when the engine
// destroys it, on its core, after the window.
class NodeActor : public qb::Actor {
protected:
    const Net          &_net;
    const Node         &_node;
    const std::uint32_t _index;
    const qb::ActorId   _next;
    Counts              _counts;

    NodeActor(const Net &net, std::uint32_t index) noexcept
        : _net(net)
        , _node(net.topology->nodes[index])
        , _index(index)
        , _next(_node.next == kNone ? qb::ActorId{} : net.ids[_node.next]) {}

    void ready() { push<Ready>(_net.ids[Topology::kSink]); }

public:
    ~NodeActor() noexcept override { static_cast<Counts &>((*_net.tallies)[_index]) = _counts; }
};

class SourceActor final : public NodeActor {
public:
    SourceActor(const Net &net, std::uint32_t index) noexcept : NodeActor(net, index) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Start>(*this);
        ready();
        co_return true;
    }
    void on(Start const &) {
        _counts.signal();
        for (const std::uint64_t v : *_net.input) push<Value>(_next, v);
        push<Exit>(_next);
    }
};

// A stage, a step, a partition: what it receives goes on to one actor, unchanged.
class ForwardActor final : public NodeActor {
public:
    ForwardActor(const Net &net, std::uint32_t index) noexcept : NodeActor(net, index) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        ready();
        co_return true;
    }
    void on(Value &event) {
        _counts.value(event.value);
        push_forward(_next, event);
    }
    void on(Exit &event) {
        _counts.signal();
        push_forward(_next, event);
    }
};

class SplitterActor final : public NodeActor {
    std::vector<qb::ActorId> _receivers;
    Splitter                 _splitter;

public:
    SplitterActor(const Net &net, std::uint32_t index) : NodeActor(net, index) {
        _receivers.reserve(_node.lanes);
        for (std::uint32_t k = 0; k < _node.lanes; ++k)
            _receivers.push_back(net.ids[net.topology->receivers[_node.first + k]]);
    }
    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        ready();
        co_return true;
    }
    void on(Value &event) {
        _counts.value(event.value);
        push_forward(_receivers[_splitter.next(_node)], event);
    }
    void on(Exit const &) {
        _counts.signal();
        for (const qb::ActorId r : _receivers) push<Exit>(r);
    }
};

class CompareActor final : public NodeActor {
    Comparator _cmp;

public:
    CompareActor(const Net &net, std::uint32_t index) noexcept : NodeActor(net, index) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        ready();
        co_return true;
    }
    void on(Value const &event) {
        _counts.value(event.value);
        std::uint64_t a = 0, b = 0;
        if (_cmp.accept(event.value, _node.up, a, b)) {
            push<Data>(_next, _node.order, a);
            push<Data>(_next, _node.order, b);
        }
    }
    void on(Exit &event) {
        _counts.signal();
        push_forward(_next, event);
    }
};

class JoinerActor final : public NodeActor {
    Joiner _joiner;

public:
    JoinerActor(const Net &net, std::uint32_t index) : NodeActor(net, index), _joiner(_node) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Data>(*this);
        registerEvent<Exit>(*this);
        ready();
        co_return true;
    }
    void on(Data const &event) {
        _counts.value(event.value);
        _joiner.accept(event.order, event.value, [this](std::uint64_t v) { push<Value>(_next, v); });
    }
    void on(Exit &event) {
        _counts.signal();
        if (_joiner.close()) push_forward(_next, event);
    }
};

class ValueDataActor final : public NodeActor {
public:
    ValueDataActor(const Net &net, std::uint32_t index) noexcept : NodeActor(net, index) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Data>(*this);
        registerEvent<Exit>(*this);
        ready();
        co_return true;
    }
    void on(Value const &event) {
        _counts.value(event.value);
        push<Data>(_next, _node.order, event.value);
    }
    void on(Data &event) {
        _counts.value(event.value);
        push_forward(_next, event);
    }
    void on(Exit &event) {
        _counts.signal();
        push_forward(_next, event);
    }
};

class DataValueActor final : public NodeActor {
public:
    DataValueActor(const Net &net, std::uint32_t index) noexcept : NodeActor(net, index) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Data>(*this);
        registerEvent<Exit>(*this);
        ready();
        co_return true;
    }
    void on(Value &event) {
        _counts.value(event.value);
        push_forward(_next, event);
    }
    void on(Data const &event) {
        _counts.value(event.value);
        push<Value>(_next, event.value);
    }
    void on(Exit &event) {
        _counts.signal();
        push_forward(_next, event);
    }
};

// The sink: opens the window once every other actor has reported in -- every core running, every
// actor constructed and initialised -- records the values, and closes the window on the first Exit.
class SinkActor final : public NodeActor {
    std::size_t _ready{0};
    bool        _done{false};

public:
    SinkActor(const Net &net, std::uint32_t index) noexcept : NodeActor(net, index) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        co_return true;
    }
    void on(Ready const &) {
        if (++_ready != _net.topology->nodes.size() - 1) return;
        _net.watch->start();
        push<Start>(_net.ids[_net.topology->source()]);
    }
    void on(Value const &event) {
        _counts.value(event.value);
        _net.out->push_back(event.value);
    }
    void on(Exit const &) {
        _counts.signal();
        if (_done) return;
        _done = true;
        _net.watch->stop();
        broadcast<qb::KillEvent>();
    }
};

template <typename A>
qb::ActorId add(qb::Main &engine, qb::CoreId core, const Net &net, std::uint32_t index) {
    const qb::ActorId id = engine.addActor<A>(core, std::cref(net), index);
    if (!id.is_valid())
        qvo::fail("savina/bitonicsort qb: addActor returned an invalid id -- the VirtualCore's "
                  "16-bit actor id pool is exhausted");
    return id;
}

// qb's actor id is a 16-bit slot per VirtualCore: 65 534 actors, the services' slots aside.
inline constexpr std::size_t kActorsPerCore = 65534;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores  = static_cast<std::uint32_t>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;
    const auto ncores = cores < 1 ? 1u : cores;

    const Reference                 &ref   = reference(p);
    const Topology                  &net_t = ref.net;
    const std::vector<std::uint32_t> core  = placement(ref, ncores);
    std::vector<std::size_t>         per_core(ncores, 0);
    for (const std::uint32_t c : core) ++per_core[c];
    {
        for (std::uint32_t c = 0; c < ncores; ++c)
            if (per_core[c] > kActorsPerCore)
                qvo::not_applicable(
                    "the network of n=" + std::to_string(net_t.n) + " places " + std::to_string(per_core[c])
                    + " actors on VirtualCore " + std::to_string(c) + "; qb's actor id is a 16-bit slot "
                    "per VirtualCore (65 534 actors) -- see benchmarks/savina/bitonicsort.md");
    }

    const Values       in = input(p);
    std::vector<Tally> tallies(net_t.nodes.size());
    Values             out;
    out.reserve(in.size() + 1);
    Net net;
    net.topology = &net_t;
    net.input    = &in;
    net.tallies  = &tallies;
    net.out      = &out;
    net.watch    = &watch;
    {
        qb::Main engine;
        // Only the cores the placement populates: qb refuses to start a VirtualCore that holds no
        // actor ("Core Init Failed"), and a network of one step (n=2) is all on core 0.
        for (std::uint32_t c = 0; c < ncores; ++c)
            if (per_core[c] != 0) qvoqb::configure_core(engine, static_cast<int>(c), spin);

        net.ids.reserve(net_t.nodes.size());
        for (std::uint32_t i = 0; i < net_t.nodes.size(); ++i) {
            const auto c = static_cast<qb::CoreId>(core[i]);
            switch (net_t.nodes[i].kind) {
            case Kind::kSource: net.ids.push_back(add<SourceActor>(engine, c, net, i)); break;
            case Kind::kForward: net.ids.push_back(add<ForwardActor>(engine, c, net, i)); break;
            case Kind::kSplitter: net.ids.push_back(add<SplitterActor>(engine, c, net, i)); break;
            case Kind::kCompare: net.ids.push_back(add<CompareActor>(engine, c, net, i)); break;
            case Kind::kJoiner: net.ids.push_back(add<JoinerActor>(engine, c, net, i)); break;
            case Kind::kValueData: net.ids.push_back(add<ValueDataActor>(engine, c, net, i)); break;
            case Kind::kDataValue: net.ids.push_back(add<DataValueActor>(engine, c, net, i)); break;
            case Kind::kSink: net.ids.push_back(add<SinkActor>(engine, c, net, i)); break;
            }
        }
        engine.start();
        engine.join();
        if (engine.hasError())
            qvo::fail("savina/bitonicsort qb: the engine reported an error (a core failed to start "
                      "or a handler threw)");
    }
    // The engine is gone: every actor handed its counts to its slot as it was destroyed, and the
    // tallies and the sink's sequence are reduced here, outside the window.
    explain(tallies, out, p);
    return qvo::Answer{checksum(tallies, out), delivered(tallies)};
}

}  // namespace savina_bitonicsort_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bitonicsort::params();
    spec.expected          = qvospec::savina::bitonicsort::expected;
    spec.expected_messages = qvospec::savina::bitonicsort::expected_messages;
    spec.work_unit         = qvospec::savina::bitonicsort::kWorkUnit;
    spec.work_units        = qvospec::savina::bitonicsort::work_units;
    spec.idiom_source      = "the thread-ring adapter (ids shared before start, tallies in the "
                             "actor) + qb/src/qb/core/Actor.h push_forward()";
    spec.idiom_note        = "one actor class per kind of the reference network; a value passing "
                             "through unchanged is push_forward()ed in place, a compare-exchange, "
                             "a joiner and a ValueDataAdapter push a new event; every hop ordered";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "with cores=2 the network's steps are cut into two contiguous halves of equal message "
        "weight (bitonicsort.h placement(): the cut before step 27 of 55 at the defaults), each "
        "step whole on one VirtualCore, so a value crosses a core once; CAF and SObjectizer receive "
        "the same budget and their schedulers place every actor -- see "
        "benchmarks/savina/bitonicsort.md");
    spec.caveats.emplace_back(
        "qb's actor id is a 16-bit slot per VirtualCore (65 534 actors): the reference network is "
        "44 772 actors at n=1024, 104 720 at 2048 and 241 984 at Savina's 4096, which neither one "
        "core nor two can hold -- the reason the parameter deviates from Savina's 4096; a cell whose "
        "network does not fit is declared not applicable");

    return qvo::run(argc, argv, std::move(spec), savina_bitonicsort_qb::body);
}
