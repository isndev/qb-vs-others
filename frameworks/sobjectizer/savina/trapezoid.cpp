// @benchmark     savina/trapezoid
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the nqueens adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder, a ready signal before the window).
// @idiom-note    The master and the `workers` workers are one coop on the pool binder. The master
//                sends the whole fan-out from one handler, one msg_work per worker's direct mbox;
//                a worker answers its msg_work with one msg_result after the shared kernel has
//                integrated its range, and the master stops the environment at the last one. Where
//                a worker's demand runs is the thread_pool's decision.

#include <qvospec/savina/trapezoid.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_trapezoid_sobjectizer {

using namespace qvospec::savina::trapezoid;

struct Sink {
    std::vector<Record> records;
    double              total{0.0};
};

struct msg_work final : public so_5::message_t {
    std::uint64_t begin;
    std::uint64_t end;
    msg_work(std::uint64_t b, std::uint64_t e) noexcept : begin(b), end(e) {}
};
struct msg_result final : public so_5::message_t {
    double        area;
    std::uint32_t worker;
    msg_result(double a, std::uint32_t w) noexcept : area(a), worker(w) {}
};
struct msg_ready final : public so_5::signal_t {};

// The worker's count goes straight to its Tally, a 128-byte slot of its own: an agent on the pool
// is not bound to a thread.
class worker_t final : public so_5::agent_t {
    const so_5::mbox_t  m_master;
    const std::uint32_t m_index;
    const double        m_left;
    const double        m_h;
    Tally              &m_tally;

public:
    worker_t(context_t ctx, so_5::mbox_t master, std::uint32_t index, double left, double h,
             Tally &tally)
        : so_5::agent_t{std::move(ctx)}
        , m_master{std::move(master)}
        , m_index{index}
        , m_left{left}
        , m_h{h}
        , m_tally{tally} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            ++m_tally.works;
            so_5::send<msg_result>(m_master, integrate(m->begin, m->end, m_left, m_h), m_index);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_master); }
};

class master_t final : public so_5::agent_t {
    const Config              m_config;
    qvo::Watch               &m_watch;
    Sink                     &m_sink;
    std::vector<so_5::mbox_t> m_workers;
    std::size_t               m_ready{0};
    std::uint64_t             m_received{0};

public:
    master_t(context_t ctx, const Config &config, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_config{config}, m_watch{watch}, m_sink{sink} {}

    void add_worker(so_5::mbox_t mbox) { m_workers.push_back(std::move(mbox)); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_workers.size()) return;
            m_watch.start();
            for (std::uint32_t w = 0; w < m_config.workers; ++w)
                so_5::send<msg_work>(m_workers[w], first_piece(w, m_config),
                                     first_piece(w + 1, m_config));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_result> m) {
            if (m->worker >= m_sink.records.size())
                qvo::fail("savina/trapezoid: a result names worker " + std::to_string(m->worker)
                          + ", which does not exist");
            m_sink.records[m->worker].add(m->area);
            if (++m_received != m_config.workers) return;
            m_sink.total = fold(m_sink.records);
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<int>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;

    Sink sink;
    sink.records.assign(c.workers, Record{});
    std::vector<Tally> tallies(c.workers);
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            auto *master = coop.make_agent<master_t>(c, std::ref(watch), std::ref(sink));
            for (std::uint32_t w = 0; w < c.workers; ++w)
                master->add_worker(coop.make_agent<worker_t>(master->so_direct_mbox(), w, c.left,
                                                             c.h, std::ref(tallies[w]))
                                       ->so_direct_mbox());
        });
    });
    // launch() returned, every work thread joined: every record and tally is final, and reduced
    // here, outside the window.
    return qvo::Answer{checksum(sink.records, sink.total, tallies), delivered(sink.records, tallies)};
}

}  // namespace savina_trapezoid_sobjectizer

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
    spec.idiom_source      = "the nqueens adapter (direct mboxes, make_pool_binder, ready signal)";
    spec.idiom_note        = "one coop: the master sends one msg_work per worker from one handler, "
                             "each worker one msg_result to the master's direct mbox; "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "which pool thread runs a worker's one demand is the thread_pool's decision, so the "
        "integration is balanced by the dispatcher where qb's cell keeps worker w on core "
        "w % cores");

    return qvo::run(argc, argv, std::move(spec), savina_trapezoid_sobjectizer::body);
}
