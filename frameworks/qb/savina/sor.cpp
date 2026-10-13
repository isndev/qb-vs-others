// @benchmark     savina/sor
// @framework     qb
// @idiom-source  the thread-ring adapter beside this file (ids from `addActor` known before
//                start(), a Ready handshake before the window, `broadcast<qb::KillEvent>()` at
//                the end) and qb/src/qb/core/Actor.h `push()` -- ordered, the default.
// @idiom-note    One actor per cell, Savina's runner and peer as the two owners. A cell's value
//                is one small event per neighbour (generation, cell index, value); its report one
//                event to its owner. Placement follows the grid's split: the runner's half of the
//                columns on the first half of the cores, the peer's on the rest -- at cores=2 only
//                the values across the middle column cross a core.

#include <qvospec/savina/sor.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_sor_qb {

using namespace qvospec::savina::sor;

struct Ready : qb::Event {};  // handshake, outside the window
struct Start : qb::Event {};  // to a cell, and from the runner to the peer
struct Value : qb::Event {    // Savina's SorValueMessage, with the sender's generation and index
    std::uint32_t generation{0};
    std::uint32_t from{0};
    std::int64_t  value{0};
    Value(std::uint32_t g, std::uint32_t f, std::int64_t v) noexcept : generation(g), from(f), value(v) {}
};
struct Report : qb::Event {  // Savina's SorResultMessage: what the cell's owner keeps
    std::uint32_t cell{0};
    std::uint32_t updates{0};
    std::int64_t  value{0};
    std::uint64_t received{0};
    std::uint64_t work{0};
    Report(std::uint32_t c, std::uint32_t u, std::int64_t v, std::uint64_t r, std::uint64_t w) noexcept
        : cell(c), updates(u), value(v), received(r), work(w) {}
};
struct Half : qb::Event {};  // the peer's half complete, to the runner

// Every id is known to every actor from its constructor: the grid is created before start().
struct Wiring {
    std::vector<qb::ActorId> cells;
    qb::ActorId              runner;
    qb::ActorId              peer;
};

class CellActor final : public qb::Actor {
    const Wiring &_wiring;
    Cell          _cell;
    const bool    _runner_owned;
    qb::ActorId   _owner;

    void publish() {
        _cell.for_each_neighbour([&](std::uint32_t to) {
            push<Value>(_wiring.cells[to], _cell.generation(), _cell.index(), _cell.value());
        });
    }
    void report_if_due() {
        if (_cell.report_due())
            push<Report>(_owner, _cell.index(), _cell.updates(), _cell.value(), _cell.received(),
                         _cell.work());
    }

public:
    CellActor(const Wiring &wiring, const Shape &s, std::uint32_t index, std::int64_t value) noexcept
        : _wiring(wiring), _cell(s, index, value), _runner_owned(runner_owns(s, index)) {}

    qb::io::async::task<bool> onInit() final {
        _owner = _runner_owned ? _wiring.runner : _wiring.peer;
        registerEvent<Start>(*this);
        registerEvent<Value>(*this);
        push<Ready>(_wiring.runner);
        co_return true;
    }

    void on(Start const &) {
        if (_cell.start()) publish();
        report_if_due();
    }

    void on(Value const &e) {
        if (_cell.accept(e.generation, e.from, e.value)) {
            _cell.update();
            if (_cell.publishes()) publish();
        }
        report_if_due();
    }
};

class PeerActor final : public qb::Actor {
    const Wiring &_wiring;
    const Shape   _shape;
    Owner        &_owner;

public:
    PeerActor(const Wiring &wiring, const Shape &s, Owner &owner) noexcept
        : _wiring(wiring), _shape(s), _owner(owner) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Start>(*this);
        registerEvent<Report>(*this);
        push<Ready>(_wiring.runner);
        co_return true;
    }

    // Savina's SorPeer boots its half and starts it.
    void on(Start const &) {
        _owner.count();
        for (std::uint32_t c = 0; c < _shape.cells; ++c)
            if (!runner_owns(_shape, c)) push<Start>(_wiring.cells[c]);
    }

    void on(Report const &r) {
        if (_owner.record(r.cell, r.value, r.updates, r.received, r.work)) push<Half>(_wiring.runner);
    }
};

class RunnerActor final : public qb::Actor {
    const Wiring &_wiring;
    const Shape   _shape;
    Owner        &_owner;
    qvo::Watch   &_watch;
    std::size_t   _ready{0};
    bool          _half{false};

    void finish_if_done() {
        if (!_half || !_owner.complete()) return;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }

public:
    RunnerActor(const Wiring &wiring, const Shape &s, Owner &owner, qvo::Watch &watch) noexcept
        : _wiring(wiring), _shape(s), _owner(owner), _watch(watch) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Report>(*this);
        registerEvent<Half>(*this);
        co_return true;
    }

    // The window opens once every cell and the peer have reported in: every core running and
    // warm. The runner starts its half and the peer, which starts the other.
    void on(Ready const &) {
        if (++_ready != _wiring.cells.size() + 1) return;
        _watch.start();
        for (std::uint32_t c = 0; c < _shape.cells; ++c)
            if (runner_owns(_shape, c)) push<Start>(_wiring.cells[c]);
        push<Start>(_wiring.peer);
    }

    void on(Report const &r) {
        _owner.record(r.cell, r.value, r.updates, r.received, r.work);
        finish_if_done();
    }

    void on(Half const &) {
        _owner.count();
        _half = true;
        finish_if_done();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape s    = shape(p);
    const bool  spin = p.get("wait") != 0;

    // A VirtualCore's actors are numbered by a 16-bit ServiceId (qb/src/qb/core/ActorId.h): a
    // core cannot hold 65 535 of them. A grid that would put more on one core has no qb cell.
    std::vector<std::uint32_t> per_core(s.cores, 0);
    for (std::uint32_t c = 0; c < s.cells; ++c) ++per_core[core_of(s, c)];
    for (std::uint32_t k = 0; k < s.cores; ++k)
        if (per_core[k] + 2 > 65000)
            qvo::fail("savina/sor: qb cannot place " + std::to_string(per_core[k])
                      + " cell actors on one VirtualCore -- its actor ids are 16-bit");

    const std::vector<std::int64_t> start = initial(s);
    Owner                           runner(s, true);
    Owner                           peer(s, false);
    Wiring                          wiring;
    {
        qb::Main engine;
        for (std::uint32_t c = 0; c < s.cores; ++c)
            qvoqb::configure_core(engine, static_cast<int>(c), spin);

        wiring.runner = engine.addActor<RunnerActor>(static_cast<qb::CoreId>(runner_core(s)),
                                                     std::cref(wiring), std::cref(s),
                                                     std::ref(runner), std::ref(watch));
        wiring.peer   = engine.addActor<PeerActor>(static_cast<qb::CoreId>(peer_core(s)),
                                                   std::cref(wiring), std::cref(s), std::ref(peer));
        wiring.cells.reserve(s.cells);
        for (std::uint32_t c = 0; c < s.cells; ++c)
            wiring.cells.push_back(engine.addActor<CellActor>(static_cast<qb::CoreId>(core_of(s, c)),
                                                              std::cref(wiring), std::cref(s), c,
                                                              start[c]));
        engine.start();
        engine.join();
        if (engine.hasError()) qvo::fail("savina/sor: a qb core stopped on an error");
    }
    // The engine is gone: the owners' reports are final, reduced here in cell order, outside the
    // window.
    return qvo::Answer{checksum(s, runner, peer), delivered(s, runner, peer)};
}

}  // namespace savina_sor_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sor::params();
    spec.expected          = qvospec::savina::sor::expected;
    spec.expected_messages = qvospec::savina::sor::expected_messages;
    spec.work_unit         = qvospec::savina::sor::kWorkUnit;
    spec.work_units        = qvospec::savina::sor::work_units;
    spec.idiom_source      = "the thread-ring adapter + qb/src/qb/core/Actor.h push()";
    spec.idiom_note        = "one actor per cell, Savina's runner and peer as owners; one Value "
                             "event per neighbour and update; the runner's columns on the first "
                             "half of the cores, the peer's on the rest";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "the runner and its half of the columns live on VirtualCore 0 and the peer and its half on "
        "VirtualCore 1 at cores=2, so only the values across the middle column cross a core -- the "
        "static placement the floor has; the pools place by themselves");

    return qvo::run(argc, argv, std::move(spec), savina_sor_qb::body);
}
