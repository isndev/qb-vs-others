// @benchmark     savina/trapezoid
// @framework     qb
// @idiom-source  the nqueens adapter beside this file (a master that opens the window once every
//                worker reported ready, worker w on VirtualCore w % cores, broadcast<qb::KillEvent>()
//                to end the run) and qb/llm/qb.llm.md for push<> (ordered, the default send).
// @idiom-note    The master and the `workers` workers are created before start(); worker w lives on
//                VirtualCore w % cores, the master on core 0. The master's one handler push<>es the
//                whole fan-out -- one Work per worker, published to the other core by the pass's
//                flush once the handler returns -- and each worker answers one Result after the
//                shared kernel has integrated its range. qb places statically: at cores=2 each core
//                integrates the ranges of the workers it holds, half of them.

#include <qvospec/savina/trapezoid.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_trapezoid_qb {

using namespace qvospec::savina::trapezoid;

struct Work : qb::Event {
    std::uint64_t begin{0};
    std::uint64_t end{0};
    Work(std::uint64_t b, std::uint64_t e) noexcept : begin(b), end(e) {}
};
struct Result : qb::Event {
    double        area{0.0};
    std::uint32_t worker{0};
    Result(double a, std::uint32_t w) noexcept : area(a), worker(w) {}
};
struct Ready : qb::Event {};

// Filled before the engine starts (addActor assigns ids before the actors are constructed) and
// read-only from then on.
struct Field {
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
};

// What the master leaves behind: one Record per worker and the fold, written by the master's
// handlers alone and read once the engine is gone.
struct Sink {
    std::vector<Record> records;
    double              total{0.0};
};

// The worker's count is the actor's own state, on the line its handler already touches: an actor
// object lives in its core's thread arena, so no actor of another core shares its line
// (thread_ring::Counts says why). The slot, a line of its own in the body's vector, receives it
// when the engine tears the actor down -- on its VirtualCore, after the window.
class WorkerActor final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _index;
    const double        _left;
    const double        _h;
    std::uint64_t       _works{0};
    Tally              &_slot;

public:
    WorkerActor(const Field &field, std::uint32_t index, double left, double h, Tally &slot) noexcept
        : _field(field), _index(index), _left(left), _h(h), _slot(slot) {}

    ~WorkerActor() noexcept override { _slot.works = _works; }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Work>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Work const &event) {
        ++_works;
        push<Result>(_field.master, integrate(event.begin, event.end, _left, _h), _index);
    }
};

class MasterActor final : public qb::Actor {
    const Field   &_field;
    const Config   _config;
    qvo::Watch    &_watch;
    Sink          &_sink;
    std::size_t    _ready{0};
    std::uint64_t  _received{0};

public:
    MasterActor(const Field &field, const Config &config, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _config(config), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Result>(*this);
        co_return true;
    }

    // The window opens once every worker has reported in: every core running and warm.
    void on(Ready const &) {
        if (++_ready != _field.workers.size()) return;
        _watch.start();
        for (std::uint32_t w = 0; w < _config.workers; ++w)
            push<Work>(_field.workers[w], first_piece(w, _config), first_piece(w + 1, _config));
    }

    void on(Result const &event) {
        if (event.worker >= _sink.records.size())
            qvo::fail("savina/trapezoid: a result names worker " + std::to_string(event.worker)
                      + ", which does not exist");
        _sink.records[event.worker].add(event.area);
        if (++_received != _config.workers) return;
        _sink.total = fold(_sink.records);
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<int>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;

    Sink sink;
    sink.records.assign(c.workers, Record{});
    std::vector<Tally> tallies(c.workers);
    Field              field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int k = 0; k < ncores; ++k) qvoqb::configure_core(engine, k, spin);

        field.master = engine.addActor<MasterActor>(0, std::cref(field), c, std::ref(watch),
                                                    std::ref(sink));
        field.workers.reserve(c.workers);
        for (std::uint32_t w = 0; w < c.workers; ++w)
            field.workers.push_back(engine.addActor<WorkerActor>(
                static_cast<qb::CoreId>(w % static_cast<std::uint32_t>(ncores)), std::cref(field), w,
                c.left, c.h, std::ref(tallies[w])));

        engine.start();
        engine.join();
    }
    // The engine is gone: every worker handed its count to its slot as it was destroyed, and the
    // records and tallies are reduced here, outside the window.
    return qvo::Answer{checksum(sink.records, sink.total, tallies), delivered(sink.records, tallies)};
}

}  // namespace savina_trapezoid_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::trapezoid::params();
    spec.expected          = qvospec::savina::trapezoid::expected;
    spec.expected_messages = qvospec::savina::trapezoid::expected_messages;
    spec.work_unit         = qvospec::savina::trapezoid::kWorkUnit;
    spec.work_units        = qvospec::savina::trapezoid::work_units;
    spec.idiom_source      = "the nqueens adapter (ready handshake, actor a on VirtualCore a % cores, "
                             "broadcast<qb::KillEvent>()) + qb/llm/qb.llm.md (push<>)";
    spec.idiom_note        = "the master on core 0 push<>es one Work per worker from one handler; "
                             "worker w on VirtualCore w % cores runs the shared kernel and push<>es "
                             "one Result";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "worker w lives on VirtualCore w % cores, fixed before start: at cores=2 each core "
        "integrates exactly the ranges of the workers it holds -- half the trapezoids, within one "
        "per worker -- and never takes over the other core's; the pools of CAF and SObjectizer "
        "place each worker's one demand dynamically");

    return qvo::run(argc, argv, std::move(spec), savina_trapezoid_qb::body);
}
