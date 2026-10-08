// @benchmark     savina/nqueens
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("push is ordered" -- the per-sender FIFO the master's
//                done-count termination rests on) and the big adapter beside this file (a sink
//                that opens the window once every actor reported ready, actor a on VirtualCore
//                a % cores, broadcast<qb::KillEvent>() to end the run).
// @idiom-note    The master and the `workers` workers are created before start(); worker w lives
//                on VirtualCore w % cores, the master on core 0. A worker answers a Work event by
//                running the shared kernel and push<>ing each child item and each result to the
//                master, then one Done -- all on one ordered channel, which is what makes "done
//                == forwarded" a sound end. The master relays a child item with a fresh push<>
//                to the next worker of its rotation. qb places statically: an item goes where the
//                rotation says, never to an idle core -- the cell measures that, it is not hidden.

#include <qvospec/savina/nqueens.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_nqueens_qb {

using namespace qvospec::savina::nqueens;

struct Work : qb::Event {
    Board board;
    explicit Work(Board b) noexcept : board(b) {}
};
struct Result : qb::Event {
    std::uint64_t hash{0};
    explicit Result(std::uint64_t h) noexcept : hash(h) {}
};
struct Done : qb::Event {
    std::uint64_t chk{0};
    explicit Done(std::uint64_t c) noexcept : chk(c) {}
};
struct Ready : qb::Event {};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

// Filled before the engine starts (addActor assigns ids before the actors are constructed) and
// read-only from then on.
struct Field {
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
};

class WorkerActor final : public qb::Actor {
    const Field &_field;
    const int    _size;
    const int    _threshold;

public:
    WorkerActor(const Field &field, int size, int threshold) noexcept
        : _field(field), _size(size), _threshold(threshold) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Work>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Work const &event) {
        const qb::ActorId master = _field.master;
        process(
            event.board, _size, _threshold, [&](const Board &child) { push<Work>(master, child); },
            [&](std::uint64_t hash) { push<Result>(master, hash); });
        push<Done>(master, done_value(event.board));
    }
};

class MasterActor final : public qb::Actor {
    const Field &_field;
    qvo::Watch  &_watch;
    Sink        &_sink;
    std::size_t  _ready{0};
    std::size_t  _next{0};
    std::uint64_t _sent{0};
    std::uint64_t _completed{0};

    void hand_out(const Board &board) {
        push<Work>(_field.workers[_next], board);
        if (++_next == _field.workers.size()) _next = 0;
        ++_sent;
    }

public:
    MasterActor(const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Work>(*this);
        registerEvent<Result>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.workers.size()) return;
        _watch.start();
        hand_out(Board{});  // the empty board, depth 0
    }

    // A child item from a worker: relayed to the next worker of the rotation.
    void on(Work const &event) {
        ++_sink.messages;
        hand_out(event.board);
    }

    void on(Result const &event) {
        ++_sink.messages;
        _sink.checksum += event.hash;
    }

    void on(Done const &event) {
        _sink.messages += 2;  // this done, and the item its worker received
        _sink.checksum += event.chk;
        if (++_completed == _sent) {
            _watch.stop();
            broadcast<qb::KillEvent>();
        }
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto size      = static_cast<int>(p.get("size"));
    const auto threshold = static_cast<int>(p.get("threshold"));
    const auto workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto cores     = static_cast<int>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.master =
            engine.addActor<MasterActor>(0, std::cref(field), std::ref(watch), std::ref(sink));
        field.workers.reserve(workers);
        for (std::uint32_t w = 0; w < workers; ++w)
            field.workers.push_back(engine.addActor<WorkerActor>(
                static_cast<qb::CoreId>(w % static_cast<std::uint32_t>(ncores)), std::cref(field),
                size, threshold));

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_nqueens_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::nqueens::params();
    spec.expected          = qvospec::savina::nqueens::expected;
    spec.expected_messages = qvospec::savina::nqueens::expected_messages;
    spec.work_unit         = qvospec::savina::nqueens::kWorkUnit;
    spec.work_units        = qvospec::savina::nqueens::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md (push<> ordered per sender) + the big adapter "
                             "(ready handshake, actor a on VirtualCore a % cores)";
    spec.idiom_note        = "master on core 0 relays every child item round-robin with push<>; "
                             "worker w on VirtualCore w % cores runs the shared kernel and pushes "
                             "items, results and one Done to the master";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "worker w lives on VirtualCore w % cores, fixed before start, and the master hands items "
        "out round-robin: qb has no work stealing, so an item lands on the core the rotation "
        "names even when the other core is idle -- the balance of the search across cores is the "
        "rotation's, and this cell measures it");

    return qvo::run(argc, argv, std::move(spec), savina_nqueens_qb::body);
}
