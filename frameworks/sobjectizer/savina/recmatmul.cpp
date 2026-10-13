// @benchmark     savina/recmatmul
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the nqueens adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder, a ready signal before the window) and
//                SObjectizer's message redirection -- `so_5::send(mbox, mhood)` hands the SAME
//                immutable message instance to another mbox (dev/so_5/send_functions.hpp), the
//                framework's own way to relay without a copy.
// @idiom-note    The master and the `workers` workers are one coop on the pool binder. A worker
//                answers a msg_work by splitting it (eight msg_work to the master's direct mbox)
//                or multiplying it (the shared kernel, into its quarter of C), then one msg_done;
//                the master REDIRECTS each created task, as received, to the worker Savina's
//                routing names, and stops the environment when every task it sent has its done.
//                Where a worker's demand runs is the thread_pool's decision.

#include <qvospec/savina/recmatmul.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_recmatmul_sobjectizer {

using namespace qvospec::savina::recmatmul;

struct msg_work final : public so_5::message_t {
    Task task;
    explicit msg_work(const Task &t) noexcept : task(t) {}
};
struct msg_done final : public so_5::signal_t {};
struct msg_ready final : public so_5::signal_t {};

// The worker's counts go straight to its Tally, a 128-byte slot of its own: an agent on the pool
// is not bound to a thread.
class worker_t final : public so_5::agent_t {
    const so_5::mbox_t m_master;
    const Config       m_config;
    Matrices          &m_matrices;
    Tally             &m_tally;

public:
    worker_t(context_t ctx, so_5::mbox_t master, const Config &config, Matrices &matrices,
             Tally &tally)
        : so_5::agent_t{std::move(ctx)}
        , m_master{std::move(master)}
        , m_config{config}
        , m_matrices{matrices}
        , m_tally{tally} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            ++m_tally.works;
            if (is_leaf(m->task, m_config)) {
                ++m_tally.leaves;
                multiply(m_matrices, m->task);
            } else {
                ++m_tally.splits;
                split(m->task, [&](const Task &child) { so_5::send<msg_work>(m_master, child); });
            }
            so_5::send<msg_done>(m_master);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_master); }
};

class master_t final : public so_5::agent_t {
    const Config              m_config;
    qvo::Watch               &m_watch;
    MasterCounts             &m_counts;
    std::vector<so_5::mbox_t> m_workers;
    std::size_t               m_ready{0};

    const so_5::mbox_t &worker_for(const Task &task) {
        ++m_counts.sent;
        return m_workers[route(task, m_config)];
    }

public:
    master_t(context_t ctx, const Config &config, qvo::Watch &watch, MasterCounts &counts)
        : so_5::agent_t{std::move(ctx)}, m_config{config}, m_watch{watch}, m_counts{counts} {}

    void add_worker(so_5::mbox_t mbox) { m_workers.push_back(std::move(mbox)); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_workers.size()) return;
            m_watch.start();
            const Task whole = root(m_config);
            so_5::send<msg_work>(worker_for(whole), whole);
        });
        // A task a worker created: the same message instance, redirected to its worker.
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            ++m_counts.created;
            so_5::send(worker_for(m->task), m);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done>) {
            if (++m_counts.dones != m_counts.sent) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<int>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;

    Matrices           matrices(c.length);  // A, B and a zero C, built before the environment
    MasterCounts       counts;
    std::vector<Tally> tallies(c.workers);
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            auto *master = coop.make_agent<master_t>(c, std::ref(watch), std::ref(counts));
            for (std::uint32_t w = 0; w < c.workers; ++w)
                master->add_worker(coop.make_agent<worker_t>(master->so_direct_mbox(), c,
                                                             std::ref(matrices),
                                                             std::ref(tallies[w]))
                                       ->so_direct_mbox());
        });
    });
    // launch() returned, every work thread joined: C, the tallies and the master's counts are
    // final, and reduced here, outside the window.
    return qvo::Answer{checksum(matrices, tallies, counts), delivered(tallies, counts)};
}

}  // namespace savina_recmatmul_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::recmatmul::params();
    spec.expected          = qvospec::savina::recmatmul::expected;
    spec.expected_messages = qvospec::savina::recmatmul::expected_messages;
    spec.work_unit         = qvospec::savina::recmatmul::kWorkUnit;
    spec.work_units        = qvospec::savina::recmatmul::work_units;
    spec.idiom_source      = "the nqueens adapter (direct mboxes, make_pool_binder, ready signal) + "
                             "message redirection so_5::send(mbox, mhood) (send_functions.hpp)";
    spec.idiom_note        = "one coop: the master redirects each created task, as received, to "
                             "worker (srC + scC) % workers; workers send tasks and one msg_done to "
                             "the master's direct mbox; thread_pool(cores) with fifo_t::individual "
                             "for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "Savina's routing sends every leaf of the defaults to five workers (0, 4, 8, 12, 16), but "
        "which pool thread runs a worker's next demand is the thread_pool's decision, so the "
        "leaves are balanced by the dispatcher where qb's cell fixes worker w on core "
        "floor(w x cores / workers)");

    return qvo::run(argc, argv, std::move(spec), savina_recmatmul_sobjectizer::body);
}
