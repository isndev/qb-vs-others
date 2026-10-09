// @benchmark     savina/a-star
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the big adapter beside this file (agent_t subclasses on their DIRECT mboxes, one
//                coop on qvoso::make_pool_binder, the field's mboxes shared before start) and
//                dev/so_5/send_functions.hpp's "send function for redirection of a message from
//                existing message hood" -- `so_5::send(mbox, cmd)` with the received mhood_t.
// @idiom-note    The master REDIRECTS a handed-back node: the msg_work instance a worker sent is
//                the one the next worker receives, SObjectizer's own zero-copy relay. A worker
//                sends its frontier and then its acknowledgement to the master's direct mbox from
//                one handler, so the frontier lands first. thread_pool(cores) with
//                fifo_t::individual for cores>=2: one demand queue per agent, and the pool places.

#include <qvospec/savina/a-star.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_a_star_sobjectizer {

using namespace qvospec::savina::a_star;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t work_messages{0};  // observed, and asserted >= min_work_messages
};

struct msg_work final : public so_5::message_t {
    std::uint32_t node;
    explicit msg_work(std::uint32_t n) noexcept : node(n) {}
};
struct msg_ack final : public so_5::message_t {
    std::uint64_t chk;
    std::uint64_t nodes;
    msg_ack(std::uint64_t c, std::uint64_t n) noexcept : chk(c), nodes(n) {}
};
struct msg_ready final : public so_5::signal_t {};

// The direct mboxes, filled inside introduce_coop before the environment starts and read-only from
// then on.
struct Field {
    so_5::mbox_t              master;
    std::vector<so_5::mbox_t> workers;
};

class worker_t final : public so_5::agent_t {
    const Field               &m_field;
    const Grid                &m_grid;
    Claims                    &m_claims;
    const std::uint32_t        m_threshold;
    const int                  m_work;
    std::vector<std::uint32_t> m_queue;

public:
    worker_t(context_t ctx, const Field &field, const Grid &grid, Claims &claims,
             std::uint32_t threshold, int work)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_grid{grid}
        , m_claims{claims}
        , m_threshold{threshold}
        , m_work{work} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            const Chunk c = search(m_grid, m_claims, m->node, m_threshold, m_work, m_queue,
                                   [this](std::uint32_t node) {
                                       so_5::send<msg_work>(m_field.master, node);
                                   });
            so_5::send<msg_ack>(m_field.master, c.chk, c.nodes);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class master_t final : public so_5::agent_t {
    const Field  &m_field;
    qvo::Watch   &m_watch;
    Sink         &m_sink;
    std::size_t   m_ready{0};
    std::uint64_t m_sent{0};
    std::uint64_t m_completed{0};

    const so_5::mbox_t &next_worker() noexcept {
        return m_field.workers[m_sent++ % m_field.workers.size()];
    }

public:
    master_t(context_t ctx, const Field &field, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_watch{watch}, m_sink{sink} {}

    void so_define_agent() override {
        // The window opens once every worker has started: every work thread running and warm.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_field.workers.size()) return;
            m_watch.start();
            so_5::send<msg_work>(next_worker(), Grid::kOrigin);
        });
        // A node a worker handed back: the same message goes on to the next worker.
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            ++m_sink.messages;
            so_5::send(next_worker(), m);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_ack> m) {
            // The acknowledgement and the work message it proves delivered.
            m_sink.messages += 2;
            m_sink.checksum += m->chk;
            if (++m_completed != m_sent) return;
            m_watch.stop();
            m_sink.work_messages = m_sent;
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto threshold = static_cast<std::uint32_t>(p.get("threshold"));
    const auto work      = static_cast<int>(p.get("work"));
    const auto cores     = static_cast<int>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;

    const Grid grid(static_cast<std::uint32_t>(p.get("grid")));
    Claims     claims(grid.nodes());
    Sink       sink;
    Field      field;

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.master = coop.make_agent<master_t>(std::cref(field), std::ref(watch),
                                                     std::ref(sink))
                               ->so_direct_mbox();
            field.workers.reserve(workers);
            for (std::uint32_t w = 0; w < workers; ++w)
                field.workers.push_back(coop.make_agent<worker_t>(std::cref(field),
                                                                  std::cref(grid),
                                                                  std::ref(claims), threshold,
                                                                  work)
                                            ->so_direct_mbox());
        });
    });

    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedWorkMessages] = sink.work_messages;
    return answer;
}

}  // namespace savina_a_star_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::a_star::params();
    spec.expected          = qvospec::savina::a_star::expected;
    spec.work_unit         = qvospec::savina::a_star::kWorkUnit;
    spec.work_units        = qvospec::savina::a_star::work_units;
    spec.observed_at_least[qvospec::savina::a_star::kObservedWorkMessages] =
        qvospec::savina::a_star::min_work_messages;
    spec.idiom_source      = "the big adapter + dev/so_5/send_functions.hpp message redirection "
                             "(so_5::send(mbox, mhood_t))";
    spec.idiom_note        = "agents on their DIRECT mboxes; the master redirects each handed-back "
                             "msg_work instance to the next worker round-robin; thread_pool(cores) "
                             "with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the workers are placed by the thread_pool, so how many hand-backs cross a core is the "
        "dispatcher's decision; qb's cell fixes actor a on core a % cores -- see "
        "benchmarks/savina/a-star.md");
    spec.caveats.emplace_back(
        "the claim slots are shared memory every worker CASes, as in Savina's own implementation "
        "-- the one input not passed by message, identical for every framework (a-star.h, Claims)");

    return qvo::run(argc, argv, std::move(spec), savina_a_star_sobjectizer::body);
}
