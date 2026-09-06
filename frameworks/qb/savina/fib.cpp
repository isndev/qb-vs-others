// @benchmark     savina/fib
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("Dynamic actor creation": `addRefActor<T>()` from inside an
//                actor creates a child ON THE SAME VirtualCore, its `onInit` run synchronously,
//                and the handle's `id()` is valid before the call returns) and `kill()`.
// @idiom-note    A FibActor asked for n > 2 creates two children with `addRefActor<FibActor>(id())`,
//                pushes fib(n-1) / fib(n-2) to them, and dies with `kill()` right after pushing
//                its own response. qb has NO cross-core spawn primitive, so a whole sub-tree
//                lives on its seed's core: `cores=2` is two independent trees on two cores, not
//                one tree balanced across them. A child id that comes back invalid is the
//                per-core 16-bit slot pool being exhausted -- reported as a hard failure, never
//                folded into a checksum that could happen to match.

#include <qvospec/savina/fib.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <cstdio>
#include <cstdlib>

namespace savina_fib_qb {

using namespace qvospec::savina::fib;

struct Request : qb::Event {
    std::uint32_t n{0};
    explicit Request(std::uint32_t nn) noexcept : n(nn) {}
};
struct Response : qb::Event {
    std::uint64_t value{0};
    std::uint64_t chk{0};
    std::uint64_t messages{0};
    Response(std::uint64_t v, std::uint64_t c, std::uint64_t m) noexcept
        : value(v), chk(c), messages(m) {}
};
struct Ready : qb::Event {};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

// The two seeds' ids, filled before the engine starts (ids are assigned at addActor time, the
// actors themselves are constructed at start) and read-only from then on.
struct Field {
    qb::ActorId seeds[2];
};

class FibActor final : public qb::Actor {
    const qb::ActorId _parent;
    const bool        _seed;
    std::uint64_t     _value{0};
    std::uint64_t     _chk{0};
    std::uint64_t     _messages{0};
    std::uint32_t     _pending{0};

    void respond(std::uint64_t value, std::uint64_t chk) {
        push<Response>(_parent, value, chk, _messages);
        kill();
    }

    qb::ActorId spawn_child() {
        const auto child = addRefActor<FibActor>(id(), false);
        if (!child.valid()) {
            // The per-VirtualCore ServiceIdPool is 16-bit: 65 534 live actors. Exhausting it
            // is a qb limit this benchmark exists to expose, and a limit is a FAILED cell, not
            // a checksum that happens to still add up.
            std::fprintf(stderr,
                         "savina/fib qb: addRefActor returned an invalid handle -- the "
                         "VirtualCore's 16-bit actor id pool is exhausted (n too large for one "
                         "core; see benchmarks/savina/fib.md)\n");
            std::abort();
        }
        return child.id();
    }

public:
    FibActor(qb::ActorId parent, bool seed) noexcept : _parent(parent), _seed(seed) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Request>(*this);
        registerEvent<Response>(*this);
        if (_seed) push<Ready>(_parent);
        co_return true;
    }

    void on(Request const &event) {
        ++_messages;
        if (event.n <= 2) {
            respond(1, qvo::mix(1));
            return;
        }
        const auto a = spawn_child();
        const auto b = spawn_child();
        push<Request>(a, event.n - 1);
        push<Request>(b, event.n - 2);
        _pending = 2;
    }

    void on(Response const &event) {
        _messages += 1 + event.messages;
        _value += event.value;
        _chk += event.chk;
        if (--_pending == 0) respond(_value, qvo::mix(_value) + _chk);
    }
};

// The root of the tree: asks the two seeds and closes the window on the second answer.
class SinkActor final : public qb::Actor {
    const std::uint32_t _n;
    const Field        &_field;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::size_t         _ready{0};
    std::size_t         _done{0};

public:
    SinkActor(std::uint32_t n, const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _n(n), _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Response>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != 2) return;
        _watch.start();
        push<Request>(_field.seeds[0], _n - 1);
        push<Request>(_field.seeds[1], _n - 2);
    }

    void on(Response const &event) {
        _sink.checksum += event.chk;
        _sink.messages += 1 + event.messages;
        if (++_done == 2) {
            _watch.stop();
            broadcast<qb::KillEvent>();
        }
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n     = static_cast<std::uint32_t>(p.get("n"));
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        const auto sink_id =
            engine.addActor<SinkActor>(0, n, std::cref(field), std::ref(watch), std::ref(sink));
        for (std::size_t s = 0; s < 2; ++s)
            field.seeds[s] = engine.addActor<FibActor>(
                static_cast<qb::CoreId>(s % static_cast<std::size_t>(ncores)), sink_id, true);

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fib_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fib::params();
    spec.expected          = qvospec::savina::fib::expected;
    spec.expected_messages = qvospec::savina::fib::expected_messages;
    spec.work_unit         = qvospec::savina::fib::kWorkUnit;
    spec.work_units        = qvospec::savina::fib::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: addRefActor<T>() (same-core child, onInit run "
                             "synchronously) + kill()";
    spec.idiom_note        = "every node is a FibActor created by its parent with addRefActor, "
                             "answered with one push<Response>, and killed right after; seed s "
                             "on VirtualCore s % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb has no cross-core spawn: a child is created on its parent's VirtualCore, so with "
        "cores=2 the tree is two independent sub-trees (fib(n-1) on core 0, fib(n-2) on core 1) "
        "that never balance -- core 0 does ~62% of the work and the window closes when it does");
    spec.caveats.emplace_back(
        "qb's actor id is a 16-bit slot per VirtualCore (65 534 live actors); the tree is "
        "expanded breadth-first by the FIFO mailbox, so n=23 keeps 57 313 alive on one core at "
        "the cores=1 peak and n=24 (92 735) would not fit -- the reason the parameter deviates "
        "from Savina's 25");

    return qvo::run(argc, argv, std::move(spec), savina_fib_qb::body);
}
