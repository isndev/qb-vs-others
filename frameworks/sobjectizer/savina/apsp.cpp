// @benchmark     savina/apsp
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the thread-ring adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, wired before the environment starts, thread_pool(cores) with
//                fifo_t::individual through qvoso::make_pool_binder) and dev/so_5/send_functions.hpp
//                `send(Target, message_holder_t)` -- one message instance delivered to many mboxes.
// @idiom-note    One agent per block. A generation is ONE `msg_tile` (generation, block id,
//                pointer to the tile in the spec's double buffers), made once through
//                message_holder_t::make and sent to every neighbour's direct mbox: the instance is
//                shared, as Savina's Akka actor shares one ApspResultMessage. Where a block RUNS is
//                the thread_pool's decision.

#include <qvospec/savina/apsp.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_apsp_sobjectizer {

using namespace qvospec::savina::apsp;

struct Sink {
    std::uint64_t dones{0};  // the Dones the sink's handler ran
};

struct msg_ready final : public so_5::signal_t {};
struct msg_initial final : public so_5::signal_t {};
struct msg_done final : public so_5::signal_t {};
struct msg_tile final : public so_5::message_t {
    std::uint32_t       generation;
    std::uint32_t       from;
    const std::int64_t *tile;
    msg_tile(std::uint32_t g, std::uint32_t f, const std::int64_t *t) noexcept
        : generation{g}, from{f}, tile{t} {}
};

class block_t final : public so_5::agent_t {
    Block                     m_block;
    std::vector<so_5::mbox_t> m_neighbours;  // in for_each_neighbour's order
    so_5::mbox_t              m_sink;

    void publish(std::uint32_t g, const std::int64_t *tile) {
        const auto msg = so_5::message_holder_t<msg_tile>::make(g, m_block.id(), tile);
        for (const auto &n : m_neighbours) so_5::send(n, msg);
    }

public:
    block_t(context_t ctx, Storage &storage, std::uint32_t id)
        : so_5::agent_t{std::move(ctx)}, m_block(storage, id) {}

    void wire(std::vector<so_5::mbox_t> neighbours, so_5::mbox_t sink) {
        m_neighbours = std::move(neighbours);
        m_sink       = std::move(sink);
    }

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_initial>) { publish(0, m_block.on_initial()); })
            .event([this](so_5::mhood_t<msg_tile> t) {
                if (!m_block.accept(t->generation, t->from, t->tile)) return;
                do {
                    const std::int64_t *tile = m_block.compute();
                    if (m_block.publishes()) publish(m_block.generation(), tile);
                } while (m_block.ready());
                if (m_block.finished()) so_5::send<msg_done>(m_sink);
            });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_sink); }
};

class sink_t final : public so_5::agent_t {
    const std::size_t         m_blocks;
    qvo::Watch               &m_watch;
    Sink                     &m_sink;
    std::vector<so_5::mbox_t> m_targets;
    std::size_t               m_ready{0};

public:
    sink_t(context_t ctx, std::size_t blocks, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_blocks{blocks}, m_watch{watch}, m_sink{sink} {}

    void set_targets(std::vector<so_5::mbox_t> targets) { m_targets = std::move(targets); }

    void so_define_agent() override {
        // The window opens once every block has started: every work thread running and warm.
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_ready>) {
                if (++m_ready != m_blocks) return;
                m_watch.start();
                for (const auto &t : m_targets) so_5::send<msg_initial>(t);
            })
            .event([this](so_5::mhood_t<msg_done>) {
                if (++m_sink.dones != m_blocks) return;
                m_watch.stop();
                so_environment().stop();
            });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape s    = shape(p);
    const bool  spin = p.get("wait") != 0;
    if (s.form != 0)
        qvo::not_applicable("form=1 is qb's `send` (a generation handed to the peer's ring at once "
                            "instead of at the core's next flush); a SObjectizer send is pushed to "
                            "the receiver's demand queue when it is sent, so form=0 is already that");

    Storage storage(s);
    Sink    sink;

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, static_cast<int>(s.cores), spin),
                           [&](so_5::coop_t &coop) {
                               auto *sk = coop.make_agent<sink_t>(s.blocks, std::ref(watch),
                                                                  std::ref(sink));
                               std::vector<block_t *> blocks;
                               blocks.reserve(s.blocks);
                               for (std::uint32_t b = 0; b < s.blocks; ++b)
                                   blocks.push_back(coop.make_agent<block_t>(std::ref(storage), b));
                               std::vector<so_5::mbox_t> targets;
                               targets.reserve(s.blocks);
                               for (std::uint32_t b = 0; b < s.blocks; ++b) {
                                   std::vector<so_5::mbox_t> neighbours;
                                   neighbours.reserve(s.neighbours);
                                   Block(storage, b).for_each_neighbour([&](std::uint32_t n) {
                                       neighbours.push_back(blocks[n]->so_direct_mbox());
                                   });
                                   blocks[b]->wire(std::move(neighbours), sk->so_direct_mbox());
                                   targets.push_back(blocks[b]->so_direct_mbox());
                               }
                               sk->set_targets(std::move(targets));
                           });
    });

    // launch() returned, every work thread joined: every block's state is final in the Storage,
    // reduced here, outside the window.
    qvo::Answer answer{checksum(storage), delivered(storage, sink.dones)};
    answer.observed[kObservedAhead] = ahead(storage);
    return answer;
}

}  // namespace savina_apsp_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::apsp::params();
    spec.expected          = qvospec::savina::apsp::expected;
    spec.expected_messages = qvospec::savina::apsp::expected_messages;
    spec.work_unit         = qvospec::savina::apsp::kWorkUnit;
    spec.work_units        = qvospec::savina::apsp::work_units;
    spec.idiom_source      = "the thread-ring adapter + dev/so_5/send_functions.hpp "
                             "send(Target, message_holder_t)";
    spec.idiom_note        = "agents on their DIRECT mboxes; one msg_tile per generation, holding a "
                             "pointer to the tile, made once (message_holder_t) and sent to every "
                             "neighbour; thread_pool(cores) with fifo_t::individual for cores>=2; "
                             "form=1 not applicable";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the blocks are placed by the thread_pool: whichever pinned work thread is free takes the "
        "next demand, so which phases run on which core is the dispatcher's decision where qb and "
        "the floor fix block-row bi on core bi * cores / grid -- see benchmarks/savina/apsp.md");

    return qvo::run(argc, argv, std::move(spec), savina_apsp_sobjectizer::body);
}
