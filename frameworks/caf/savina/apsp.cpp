// @benchmark     savina/apsp
// @framework     caf 1.1.0
// @idiom-source  the thread-ring and nqueens adapters beside this file (function-based behaviors,
//                stateful_actor, handles wired before the window, built-in atoms) and CAF's own
//                custom-type registration (examples/custom_type/custom_types_1.cpp:
//                CAF_BEGIN_TYPE_ID_BLOCK / CAF_ADD_TYPE_ID, init_global_meta_objects) with
//                CAF_ALLOW_UNSAFE_MESSAGE_TYPE (caf/allowed_unsafe_message_type.hpp) for a message
//                that carries a pointer and is never serialized.
// @idiom-note    A block learns its neighbour list once, from the sink, before the window --
//                Savina's ApspNeighborMessage, the actor handles in Savina's order. A generation is
//                ONE `caf::message` holding an `apsp_tile` (generation, block id, pointer to the
//                tile in the spec's double buffers), built once and mailed to every neighbour: the
//                message is shared, as Savina's Akka actor shares one ApspResultMessage. Initial is
//                `tick_atom`, a block's Done `close_atom`. Where a block RUNS is the
//                work-stealing pool's decision.

#include <qvospec/savina/apsp.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/allowed_unsafe_message_type.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/message.hpp>
#include <caf/stateful_actor.hpp>
#include <caf/type_id.hpp>

#include <utility>
#include <vector>

namespace savina_apsp_caf {

// Savina's ApspResultMessage: a generation of one block's tile, by pointer.
struct apsp_tile {
    std::uint32_t       generation{0};
    std::uint32_t       from{0};
    const std::int64_t *tile{nullptr};
};

}  // namespace savina_apsp_caf

CAF_BEGIN_TYPE_ID_BLOCK(qvo_savina_apsp, caf::first_custom_type_id)
    CAF_ADD_TYPE_ID(qvo_savina_apsp, (savina_apsp_caf::apsp_tile))
CAF_END_TYPE_ID_BLOCK(qvo_savina_apsp)

// The message carries a raw pointer into the body's memory: it is only ever delivered inside this
// process, and CAF's serialization is never asked for it.
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(savina_apsp_caf::apsp_tile)

namespace savina_apsp_caf {

using namespace qvospec::savina::apsp;

struct Sink {
    std::uint64_t dones{0};  // the Dones the sink's handler ran
};

struct block_state {
    Block                   block;
    std::vector<caf::actor> neighbours;  // in for_each_neighbour's order
    caf::actor              sink;
};

void publish(caf::stateful_actor<block_state> *self, std::uint32_t g, const std::int64_t *tile) {
    auto       &st  = self->state();
    const auto  msg = caf::make_message(apsp_tile{g, st.block.id(), tile});
    for (const auto &n : st.neighbours) self->mail(msg).send(n);
}

caf::behavior block_fun(caf::stateful_actor<block_state> *self, Storage *storage, std::uint32_t id) {
    self->state().block = Block(*storage, id);
    return {
        // Savina's ApspNeighborMessage, once per block, outside the window.
        [self](caf::put_atom, std::vector<caf::actor> neighbours, caf::actor sink) {
            auto &st      = self->state();
            st.neighbours = std::move(neighbours);
            st.sink       = std::move(sink);
            self->mail(caf::ok_atom_v).send(st.sink);
        },
        [self](caf::tick_atom) { publish(self, 0, self->state().block.on_initial()); },
        [self](const apsp_tile &t) {
            Block &b = self->state().block;
            if (!b.accept(t.generation, t.from, t.tile)) return;
            do {
                const std::int64_t *tile = b.compute();
                if (b.publishes()) publish(self, b.generation(), tile);
            } while (b.ready());
            if (b.finished()) self->mail(caf::close_atom_v).send(self->state().sink);
        },
    };
}

struct sink_state {
    std::vector<caf::actor> blocks;
    std::size_t             wired{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior sink_fun(caf::stateful_actor<sink_state> *self, std::vector<caf::actor> blocks,
                       const Shape *shape, qvo::Watch *watch, Sink *sink) {
    auto &st  = self->state();
    st.blocks = std::move(blocks);
    st.watch  = watch;
    st.sink   = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (std::uint32_t b = 0; b < st.blocks.size(); ++b) {
        // The neighbour list in Savina's order -- the one Block::for_each_neighbour walks.
        std::vector<caf::actor> neighbours;
        neighbours.reserve(shape->neighbours);
        const std::uint32_t bi = b / shape->grid, bj = b % shape->grid;
        for (std::uint32_t r = 0; r < shape->grid; ++r)
            if (r != bi) neighbours.push_back(st.blocks[r * shape->grid + bj]);
        for (std::uint32_t c = 0; c < shape->grid; ++c)
            if (c != bj) neighbours.push_back(st.blocks[bi * shape->grid + c]);
        self->mail(caf::put_atom_v, std::move(neighbours), me).send(st.blocks[b]);
    }

    return {
        // Every block has been scheduled at least once and holds its neighbours: the window that
        // opens here contains the phases and not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.blocks.size()) return;
            s.watch->start();
            for (const auto &b : s.blocks) self->mail(caf::tick_atom_v).send(b);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            if (++s.sink->dones != s.blocks.size()) return;
            s.watch->stop();
            for (const auto &b : s.blocks) self->send_exit(b, caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape s     = shape(p);
    const auto  cores = static_cast<std::size_t>(s.cores);
    const bool  spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);
    if (s.form != 0)
        qvo::not_applicable("form=1 is qb's `send` (a generation handed to the peer's ring at once "
                            "instead of at the core's next flush); a CAF mail is enqueued in the "
                            "receiver's mailbox when it is sent, so form=0 is already that");

    Storage storage(s);
    Sink    sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> blocks;
        blocks.reserve(s.blocks);
        for (std::uint32_t b = 0; b < s.blocks; ++b)
            blocks.push_back(sys.spawn<qvocaf::kSpawnOptions>(block_fun, &storage, b));
        sys.spawn<qvocaf::kSpawnOptions>(sink_fun, std::move(blocks), &s, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: every block's state is final in the Storage,
    // reduced here, outside the window.
    qvo::Answer answer{checksum(storage), delivered(storage, sink.dones)};
    answer.observed[kObservedAhead] = ahead(storage);
    return answer;
}

}  // namespace savina_apsp_caf

int main(int argc, char **argv) {
    caf::init_global_meta_objects<caf::id_block::qvo_savina_apsp>();
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::apsp::params();
    spec.expected          = qvospec::savina::apsp::expected;
    spec.expected_messages = qvospec::savina::apsp::expected_messages;
    spec.work_unit         = qvospec::savina::apsp::kWorkUnit;
    spec.work_units        = qvospec::savina::apsp::work_units;
    spec.idiom_source      = "the thread-ring and nqueens adapters + CAF's custom_types_1 example "
                             "(type ID block) + CAF_ALLOW_UNSAFE_MESSAGE_TYPE";
    spec.idiom_note        = "neighbour handles wired once from the sink (Savina's "
                             "ApspNeighborMessage); one caf::message per generation, holding a "
                             "pointer to the tile, mailed to every neighbour; placement left to "
                             "the pool; form=1 not applicable";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the blocks are placed by the work-stealing pool: a block made ready by a tile stays on the "
        "sender's worker (worker::delay) unless the idle worker steals it, so which phases run on "
        "which core is the scheduler's decision where qb and the floor fix block-row bi on core "
        "bi * cores / grid -- see benchmarks/savina/apsp.md");

    return qvo::run(argc, argv, std::move(spec), savina_apsp_caf::body);
}
