// @benchmark     savina/nqueens
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the big adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder, a ready signal before the window) and
//                SObjectizer's message redirection -- `so_5::send(mbox, mhood)` hands the SAME
//                immutable message instance to another mbox (dev/so_5/send_functions.hpp), the
//                framework's own way to relay without a copy.
// @idiom-note    The master and the `workers` workers are one coop on the pool binder. A worker
//                answers a msg_work by running the shared kernel and sending each child item and
//                each result to the master's direct mbox, then one msg_done; the master REDIRECTS
//                each child item, as received, to the next worker of its rotation, and stops the
//                environment when every item it forwarded has its done. Where a worker's demand
//                runs is the thread_pool's decision.

#include <qvospec/savina/nqueens.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_nqueens_sobjectizer {

using namespace qvospec::savina::nqueens;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_work final : public so_5::message_t {
    Board board;
    explicit msg_work(Board b) noexcept : board(b) {}
};
struct msg_result final : public so_5::message_t {
    std::uint64_t hash;
    explicit msg_result(std::uint64_t h) noexcept : hash(h) {}
};
struct msg_done final : public so_5::message_t {
    std::uint64_t chk;
    explicit msg_done(std::uint64_t c) noexcept : chk(c) {}
};
struct msg_ready final : public so_5::signal_t {};

class worker_t final : public so_5::agent_t {
    const so_5::mbox_t m_master;
    const int          m_size;
    const int          m_threshold;

public:
    worker_t(context_t ctx, so_5::mbox_t master, int size, int threshold)
        : so_5::agent_t{std::move(ctx)}
        , m_master{std::move(master)}
        , m_size{size}
        , m_threshold{threshold} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            process(
                m->board, m_size, m_threshold,
                [&](const Board &child) { so_5::send<msg_work>(m_master, child); },
                [&](std::uint64_t hash) { so_5::send<msg_result>(m_master, hash); });
            so_5::send<msg_done>(m_master, done_value(m->board));
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_master); }
};

class master_t final : public so_5::agent_t {
    qvo::Watch               &m_watch;
    Sink                     &m_sink;
    std::vector<so_5::mbox_t> m_workers;
    std::size_t               m_ready{0};
    std::size_t               m_next{0};
    std::uint64_t             m_sent{0};
    std::uint64_t             m_completed{0};

    const so_5::mbox_t &next_worker() {
        const auto &mbox = m_workers[m_next];
        if (++m_next == m_workers.size()) m_next = 0;
        ++m_sent;
        return mbox;
    }

public:
    master_t(context_t ctx, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_watch{watch}, m_sink{sink} {}

    void add_worker(so_5::mbox_t mbox) { m_workers.push_back(std::move(mbox)); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_workers.size()) return;
            m_watch.start();
            so_5::send<msg_work>(next_worker(), Board{});  // the empty board, depth 0
        });
        // A child item from a worker: the same message instance, redirected to the next worker.
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            ++m_sink.messages;
            so_5::send(next_worker(), m);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_result> m) {
            ++m_sink.messages;
            m_sink.checksum += m->hash;
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done> m) {
            m_sink.messages += 2;  // this done, and the item its worker received
            m_sink.checksum += m->chk;
            if (++m_completed != m_sent) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto size      = static_cast<int>(p.get("size"));
    const auto threshold = static_cast<int>(p.get("threshold"));
    const auto workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto cores     = static_cast<int>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;

    Sink sink;
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            auto *master = coop.make_agent<master_t>(std::ref(watch), std::ref(sink));
            for (std::uint32_t w = 0; w < workers; ++w)
                master->add_worker(
                    coop.make_agent<worker_t>(master->so_direct_mbox(), size, threshold)
                        ->so_direct_mbox());
        });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_nqueens_sobjectizer

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
    spec.idiom_source      = "the big adapter (direct mboxes, make_pool_binder, ready signal) + "
                             "message redirection so_5::send(mbox, mhood) (send_functions.hpp)";
    spec.idiom_note        = "one coop: the master redirects each child item, as received, to "
                             "the next worker round-robin; workers send items, results and one "
                             "msg_done to the master's direct mbox; thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the master hands items out round-robin like every other cell, but which pool thread runs "
        "a worker's next demand is the thread_pool's decision, so the search is balanced by the "
        "dispatcher where qb's cell keeps worker w on core w % cores");

    return qvo::run(argc, argv, std::move(spec), savina_nqueens_sobjectizer::body);
}
