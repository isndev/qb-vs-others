// @benchmark     savina/apsp
// @framework     qb
// @idiom-source  the thread-ring adapter beside this file (ids from `addActor` known before
//                start(), a Ready handshake before the window, `broadcast<qb::KillEvent>()` at
//                the end) and qb/src/qb/core/Actor.h `push()` (ordered, published by the core's
//                next flush) and `send()` (handed to the peer's ring at once) -- the spec's
//                `form` axis.
// @idiom-note    One actor per block, block-row bi on VirtualCore bi * cores / grid. A tile
//                travels as a POINTER in a small event (generation, block id, tile): the tiles
//                are the spec's double buffers in memory the body owns (apsp.h, Storage), so no
//                event carries a block^2 payload and nothing is allocated in the window. form=0
//                publishes a generation with `push`, form=1 with `send`.

#include <qvospec/savina/apsp.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_apsp_qb {

using namespace qvospec::savina::apsp;

struct Ready : qb::Event {};    // handshake, outside the window
struct Initial : qb::Event {};  // Savina's ApspInitialMessage
struct TileEvent : qb::Event {  // Savina's ApspResultMessage
    std::uint32_t       generation{0};
    std::uint32_t       from{0};
    const std::int64_t *tile{nullptr};
    TileEvent(std::uint32_t g, std::uint32_t f, const std::int64_t *t) noexcept
        : generation(g), from(f), tile(t) {}
};
struct Done : qb::Event {};

struct Sink {
    std::uint64_t dones{0};  // the Dones the sink's handler ran
};

// Every id is known to every actor from its constructor: the blocks are created before start().
struct Grid {
    std::vector<qb::ActorId> ids;
    qb::ActorId              sink;
};

class BlockActor final : public qb::Actor {
    const Grid &_grid;
    Block       _block;
    const bool  _send;

    void publish(std::uint32_t g, const std::int64_t *tile) {
        _block.for_each_neighbour([&](std::uint32_t to) {
            if (_send)
                send<TileEvent>(_grid.ids[to], g, _block.id(), tile);
            else
                push<TileEvent>(_grid.ids[to], g, _block.id(), tile);
        });
    }

public:
    BlockActor(const Grid &grid, Storage &storage, std::uint32_t id, bool send_form) noexcept
        : _grid(grid), _block(storage, id), _send(send_form) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Initial>(*this);
        registerEvent<TileEvent>(*this);
        push<Ready>(_grid.sink);
        co_return true;
    }

    void on(Initial const &) { publish(0, _block.on_initial()); }

    void on(TileEvent const &e) {
        if (!_block.accept(e.generation, e.from, e.tile)) return;
        do {
            const std::int64_t *tile = _block.compute();
            if (_block.publishes()) publish(_block.generation(), tile);
        } while (_block.ready());
        if (_block.finished()) push<Done>(_grid.sink);
    }
};

class SinkActor final : public qb::Actor {
    const Grid &_grid;
    qvo::Watch &_watch;
    Sink       &_sink;
    std::size_t _ready{0};

public:
    SinkActor(const Grid &grid, qvo::Watch &watch, Sink &sink) noexcept
        : _grid(grid), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    // The window opens once every block has reported in: every core running and warm.
    void on(Ready const &) {
        if (++_ready != _grid.ids.size()) return;
        _watch.start();
        for (const qb::ActorId id : _grid.ids) push<Initial>(id);
    }

    void on(Done const &) {
        if (++_sink.dones != _grid.ids.size()) return;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape s    = shape(p);
    const bool  spin = p.get("wait") != 0;

    Storage storage(s);
    Sink    sink;
    Grid    grid;
    {
        qb::Main engine;
        for (std::uint32_t c = 0; c < s.cores; ++c)
            qvoqb::configure_core(engine, static_cast<int>(c), spin);

        grid.sink = engine.addActor<SinkActor>(0, std::cref(grid), std::ref(watch), std::ref(sink));
        grid.ids.reserve(s.blocks);
        for (std::uint32_t b = 0; b < s.blocks; ++b)
            grid.ids.push_back(engine.addActor<BlockActor>(static_cast<qb::CoreId>(core_of(s, b)),
                                                           std::cref(grid), std::ref(storage), b,
                                                           s.form == 1));
        engine.start();
        engine.join();
        if (engine.hasError()) qvo::fail("savina/apsp: a qb core stopped on an error");
    }
    // The engine is gone: every block's state is final in the Storage, reduced here, outside the
    // window.
    qvo::Answer answer{checksum(storage), delivered(storage, sink.dones)};
    answer.observed[kObservedAhead] = ahead(storage);
    return answer;
}

}  // namespace savina_apsp_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::apsp::params();
    // The publication this adapter's table cell runs: push (form=0), qb's default, until the
    // quiet-host measurement of both forms names the faster one (benchmarks/savina/apsp.md).
    spec.params["form"]    = 0;
    spec.expected          = qvospec::savina::apsp::expected;
    spec.expected_messages = qvospec::savina::apsp::expected_messages;
    spec.work_unit         = qvospec::savina::apsp::kWorkUnit;
    spec.work_units        = qvospec::savina::apsp::work_units;
    spec.idiom_source      = "the thread-ring adapter + qb/src/qb/core/Actor.h push() / send()";
    spec.idiom_note        = "one actor per block, block-row bi on VirtualCore bi * cores / grid; a "
                             "tile travels as a pointer into the spec's double buffers; form=0 "
                             "publishes with push, form=1 with send";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "block-row bi lives on VirtualCore bi * cores / grid: at cores=2 the top half of the grid on "
        "core 0 and the bottom half on core 1, so each block's row neighbours share its core and "
        "half its column neighbours do not -- the static placement the floor has; the pools place "
        "by themselves");
    spec.caveats.emplace_back(
        "form=0 publishes each generation with push: an event to another core leaves at the core's "
        "next flush, after every handler of the pass that produced it -- several blocks' phases on "
        "a busy core; form=1 hands it to the peer's ring at once with send. The run's `form` is in "
        "its params (benchmarks/savina/apsp.md, \"The publication\")");

    return qvo::run(argc, argv, std::move(spec), savina_apsp_qb::body);
}
