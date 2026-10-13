// @benchmark     savina/sor
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the thread-ring adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, wired before the environment starts, thread_pool(cores) with
//                fifo_t::individual through qvoso::make_pool_binder) and dev/so_5/send_functions.hpp
//                `send(Target, message_holder_t)` -- one message instance delivered to many mboxes.
// @idiom-note    One agent per cell, Savina's runner and peer as the two owners. A value is ONE
//                `msg_value` (generation, cell, value) per update, made once through
//                message_holder_t::make and sent to every neighbour's direct mbox; a report a
//                `msg_report` to the owner. Where a cell RUNS is the thread_pool's decision.

#include <qvospec/savina/sor.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_sor_sobjectizer {

using namespace qvospec::savina::sor;

struct msg_ready final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};
struct msg_half final : public so_5::signal_t {};
struct msg_value final : public so_5::message_t {
    std::uint32_t generation;
    std::uint32_t from;
    std::int64_t  value;
    msg_value(std::uint32_t g, std::uint32_t f, std::int64_t v) noexcept
        : generation{g}, from{f}, value{v} {}
};
struct msg_report final : public so_5::message_t {
    std::uint32_t cell;
    std::uint32_t updates;
    std::int64_t  value;
    std::uint64_t received;
    std::uint64_t work;
    msg_report(std::uint32_t c, std::uint32_t u, std::int64_t v, std::uint64_t r, std::uint64_t w) noexcept
        : cell{c}, updates{u}, value{v}, received{r}, work{w} {}
};

class cell_t final : public so_5::agent_t {
    Cell                      m_cell;
    std::vector<so_5::mbox_t> m_neighbours;  // in for_each_neighbour's order
    so_5::mbox_t              m_owner;
    so_5::mbox_t              m_runner;

    void publish() {
        const auto msg = so_5::message_holder_t<msg_value>::make(m_cell.generation(), m_cell.index(),
                                                                 m_cell.value());
        for (const auto &n : m_neighbours) so_5::send(n, msg);
    }
    void report_if_due() {
        if (m_cell.report_due())
            so_5::send<msg_report>(m_owner, m_cell.index(), m_cell.updates(), m_cell.value(),
                                   m_cell.received(), m_cell.work());
    }

public:
    cell_t(context_t ctx, const Shape &s, std::uint32_t index, std::int64_t value)
        : so_5::agent_t{std::move(ctx)}, m_cell(s, index, value) {}

    void wire(std::vector<so_5::mbox_t> neighbours, so_5::mbox_t owner, so_5::mbox_t runner) {
        m_neighbours = std::move(neighbours);
        m_owner      = std::move(owner);
        m_runner     = std::move(runner);
    }

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_start>) {
                if (m_cell.start()) publish();
                report_if_due();
            })
            .event([this](so_5::mhood_t<msg_value> v) {
                if (m_cell.accept(v->generation, v->from, v->value)) {
                    m_cell.update();
                    if (m_cell.publishes()) publish();
                }
                report_if_due();
            });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_runner); }
};

// Savina's SorPeer: starts its half, collects its reports, reports the half to the runner.
class peer_t final : public so_5::agent_t {
    Owner                    &m_owner;
    std::vector<so_5::mbox_t> m_cells;
    so_5::mbox_t              m_runner;

public:
    peer_t(context_t ctx, Owner &owner) : so_5::agent_t{std::move(ctx)}, m_owner{owner} {}

    void wire(std::vector<so_5::mbox_t> cells, so_5::mbox_t runner) {
        m_cells  = std::move(cells);
        m_runner = std::move(runner);
    }

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_start>) {
                m_owner.count();
                for (const auto &c : m_cells) so_5::send<msg_start>(c);
            })
            .event([this](so_5::mhood_t<msg_report> r) {
                if (m_owner.record(r->cell, r->value, r->updates, r->received, r->work))
                    so_5::send<msg_half>(m_runner);
            });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_runner); }
};

class runner_t final : public so_5::agent_t {
    Owner                    &m_owner;
    qvo::Watch               &m_watch;
    const std::size_t         m_expected_ready;
    std::vector<so_5::mbox_t> m_cells;  // the cells the runner starts
    so_5::mbox_t              m_peer;
    std::size_t               m_ready{0};
    bool                      m_half{false};

    void finish_if_done() {
        if (!m_half || !m_owner.complete()) return;
        m_watch.stop();
        so_environment().stop();
    }

public:
    runner_t(context_t ctx, Owner &owner, qvo::Watch &watch, std::size_t expected_ready)
        : so_5::agent_t{std::move(ctx)}, m_owner{owner}, m_watch{watch}, m_expected_ready{expected_ready} {}

    void wire(std::vector<so_5::mbox_t> cells, so_5::mbox_t peer) {
        m_cells = std::move(cells);
        m_peer  = std::move(peer);
    }

    void so_define_agent() override {
        // The window opens once every cell and the peer have started: every work thread running
        // and warm. The runner starts its half and the peer, which starts the other.
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_ready>) {
                if (++m_ready != m_expected_ready) return;
                m_watch.start();
                for (const auto &c : m_cells) so_5::send<msg_start>(c);
                so_5::send<msg_start>(m_peer);
            })
            .event([this](so_5::mhood_t<msg_report> r) {
                m_owner.record(r->cell, r->value, r->updates, r->received, r->work);
                finish_if_done();
            })
            .event([this](so_5::mhood_t<msg_half>) {
                m_owner.count();
                m_half = true;
                finish_if_done();
            });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape s    = shape(p);
    const bool  spin = p.get("wait") != 0;

    const std::vector<std::int64_t> start = initial(s);
    Owner                           runner(s, true);
    Owner                           peer(s, false);

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(
            qvoso::make_pool_binder(env, static_cast<int>(s.cores), spin), [&](so_5::coop_t &coop) {
                auto *rn = coop.make_agent<runner_t>(std::ref(runner), std::ref(watch),
                                                     static_cast<std::size_t>(s.cells) + 1);
                auto *pr = coop.make_agent<peer_t>(std::ref(peer));
                std::vector<cell_t *> cells;
                cells.reserve(s.cells);
                for (std::uint32_t c = 0; c < s.cells; ++c)
                    cells.push_back(coop.make_agent<cell_t>(std::cref(s), c, start[c]));
                std::vector<so_5::mbox_t> mine, theirs;
                for (std::uint32_t c = 0; c < s.cells; ++c) {
                    std::vector<so_5::mbox_t> neighbours;
                    Cell(s, c, 0).for_each_neighbour(
                        [&](std::uint32_t n) { neighbours.push_back(cells[n]->so_direct_mbox()); });
                    const bool r = runner_owns(s, c);
                    cells[c]->wire(std::move(neighbours),
                                   r ? rn->so_direct_mbox() : pr->so_direct_mbox(),
                                   rn->so_direct_mbox());
                    (r ? mine : theirs).push_back(cells[c]->so_direct_mbox());
                }
                rn->wire(std::move(mine), pr->so_direct_mbox());
                pr->wire(std::move(theirs), rn->so_direct_mbox());
            });
    });

    // launch() returned, every work thread joined: the owners' reports are final, reduced here in
    // cell order, outside the window.
    return qvo::Answer{checksum(s, runner, peer), delivered(s, runner, peer)};
}

}  // namespace savina_sor_sobjectizer

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
    spec.idiom_source      = "the thread-ring adapter + dev/so_5/send_functions.hpp "
                             "send(Target, message_holder_t)";
    spec.idiom_note        = "agents on their DIRECT mboxes, Savina's runner and peer as owners; one "
                             "msg_value per update made once (message_holder_t) and sent to every "
                             "neighbour; thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the cells are placed by the thread_pool: whichever pinned work thread is free takes the "
        "next demand, so which values cross a core is the dispatcher's decision where qb and the "
        "floor put each half of the grid on its own core -- see benchmarks/savina/sor.md");

    return qvo::run(argc, argv, std::move(spec), savina_sor_sobjectizer::body);
}
