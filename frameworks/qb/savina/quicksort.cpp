// @benchmark     savina/quicksort
// @framework     qb
// @idiom-source  the fib adapter beside this file (`addRefActor<T>()` from inside an actor creates a
//                child on the SAME VirtualCore, `kill()` right after the answer) and qb/llm/qb.llm.md
//                "Events": bulk data behind a smart-pointer member -- the receiving core runs a
//                delivered event's destructor exactly once (qb/src/qb/core/Event.h's width contract),
//                so an event may own heap; `push` for every message.
// @idiom-note    A Sort or a Result carries its values as a `std::unique_ptr<std::vector<uint64_t>>`:
//                eight bytes cross the pipe, the elements never move, and the receiver takes
//                ownership out of the event. qb's width rule leaves no other choice for the large
//                ones: an event wider than the cross-core ring (~64 KiB) is a compile error, an
//                `allocated_push` tail must fit that ring with its event (DeadLetterReason::oversize),
//                and the first split hands each seed ~4 MB. An actor of `threshold` or more values
//                creates its children with `addRefActor` (the root's two are the seeds, created
//                before the window: seed s on VirtualCore s % cores), so with cores=2 each half of
//                the input is sorted on its own core and never rebalanced -- qb has no cross-core
//                spawn.

#include <qvospec/savina/quicksort.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <memory>
#include <utility>

namespace savina_quicksort_qb {

using namespace qvospec::savina::quicksort;
using Payload = std::unique_ptr<Values>;

struct Sort : qb::Event {
    Payload data;
    explicit Sort(Payload d) noexcept : data(std::move(d)) {}
};
struct Result : qb::Event {
    Payload  data;
    Counts   counts;
    Position position;
    Result(Payload d, Counts c, Position p) noexcept : data(std::move(d)), counts(c), position(p) {}
};
struct Ready : qb::Event {};

struct Sink {
    Values output;
    Counts counts;
};

// Filled before the engine starts (ids are assigned at addActor time) and read-only from then on.
struct Field {
    qb::ActorId driver;
    qb::ActorId root;
    qb::ActorId seeds[2];
    std::size_t threshold{0};
};

class SortActor final : public qb::Actor {
    const Field   &_field;
    const qb::ActorId _parent;
    const Position _position;
    const bool     _announce;  // the root and the seeds report to the driver before the window
    Values         _held;
    Counts         _counts;
    std::uint32_t  _fragments{0};

    void finish(Payload data) {
        push<Result>(_parent, std::move(data), _counts, _position);
        kill();
    }

    qb::ActorId child(Position position) {
        const auto handle = addRefActor<SortActor>(_field, id(), position, false);
        if (!handle.valid())
            qvo::fail("savina/quicksort qb: addRefActor returned an invalid handle -- the "
                      "VirtualCore's 16-bit actor id pool is exhausted");
        return handle.id();
    }

public:
    SortActor(const Field &field, qb::ActorId parent, Position position, bool announce) noexcept
        : _field(field), _parent(parent), _position(position), _announce(announce) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Sort>(*this);
        registerEvent<Result>(*this);
        if (_announce) push<Ready>(_field.driver);
        co_return true;
    }

    void on(Sort &event) {
        ++_counts.sorts;
        Values &data = *event.data;
        if (data.size() < _field.threshold) {
            sort_leaf(data);
            finish(std::move(event.data));
            return;
        }
        Split               s     = split(data);
        const bool          root  = _position == Position::kInitial;
        const qb::ActorId   left  = root ? _field.seeds[0] : child(Position::kLeft);
        const qb::ActorId   right = root ? _field.seeds[1] : child(Position::kRight);
        push<Sort>(left, std::make_unique<Values>(std::move(s.less)));
        push<Sort>(right, std::make_unique<Values>(std::move(s.greater)));
        _held      = std::move(s.equal);
        _fragments = 1;
    }

    void on(Result &event) {
        _counts.add_result(event.counts);
        join(_held, *event.data, event.position);
        if (++_fragments == 3) finish(std::make_unique<Values>(std::move(_held)));
    }
};

// The driver: hands the input to the root once the root and the seeds are up, and closes the
// window on the root's Result.
// The input is the body's, built before the engine (qb hands an actor's constructor arguments to
// it as lvalues, so a move-only one cannot travel through addActor), and moved out at the window.
class DriverActor final : public qb::Actor {
    const Field &_field;
    Payload     &_input;
    qvo::Watch  &_watch;
    Sink        &_sink;
    std::size_t  _ready{0};

public:
    DriverActor(const Field &field, Payload &input, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _input(input), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Result>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != 3) return;
        _watch.start();
        push<Sort>(_field.root, std::move(_input));
    }

    void on(Result &event) {
        _watch.stop();
        _sink.counts.add_result(event.counts);
        _sink.output = std::move(*event.data);
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink    sink;
    Field   field;
    Payload values = std::make_unique<Values>(input(p));
    field.threshold = static_cast<std::size_t>(p.get("threshold"));
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.driver = engine.addActor<DriverActor>(0, std::cref(field), std::ref(values),
                                                    std::ref(watch), std::ref(sink));
        field.root   = engine.addActor<SortActor>(0, std::cref(field), field.driver,
                                                Position::kInitial, true);
        for (std::size_t s = 0; s < 2; ++s)
            field.seeds[s] = engine.addActor<SortActor>(
                static_cast<qb::CoreId>(s % static_cast<std::size_t>(ncores)), std::cref(field),
                field.root, s == 0 ? Position::kLeft : Position::kRight, true);

        engine.start();
        engine.join();
        if (engine.hasError())
            qvo::fail("savina/quicksort qb: the engine reported an error (a core failed to start or "
                      "a handler threw)");
    }
    // The engine is gone; the sequence and the counts are reduced here, outside the window.
    const Output o = measure(sink.output);
    explain(o, sink.counts, p);
    return qvo::Answer{answer(o, sink.counts), sink.counts.sorts + sink.counts.results};
}

}  // namespace savina_quicksort_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::quicksort::params();
    spec.expected          = qvospec::savina::quicksort::expected;
    spec.expected_messages = qvospec::savina::quicksort::expected_messages;
    spec.work_unit         = qvospec::savina::quicksort::kWorkUnit;
    spec.work_units        = qvospec::savina::quicksort::work_units;
    spec.idiom_source      = "the fib adapter (addRefActor same-core child + kill()) + qb.llm.md "
                             "Events: bulk data behind a smart-pointer member";
    spec.idiom_note        = "Sort and Result carry a unique_ptr<vector<uint64_t>>; a splitting "
                             "actor creates its children with addRefActor, the root's two are "
                             "seeds created before the window on VirtualCore s % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb has no cross-core spawn: a child is created on its parent's VirtualCore, so with "
        "cores=2 each half of the first split is sorted on its seed's core and never rebalanced -- "
        "the window closes when the larger half and the root's last join are done");
    spec.caveats.emplace_back(
        "the values travel behind a std::unique_ptr in the event (8 bytes through the pipe); an "
        "allocated_push tail would have to fit the ~64 KiB cross-core ring with its event, and the "
        "first split hands each seed ~4 MB -- see benchmarks/savina/quicksort.md");

    return qvo::run(argc, argv, std::move(spec), savina_quicksort_qb::body);
}
