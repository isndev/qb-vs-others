// @benchmark     savina/piprecision
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the nqueens adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder, a ready signal before the window).
// @idiom-note    The master and the `workers` workers are one coop on the pool binder. form=0: a
//                worker answers each msg_work with a msg_result whose std::vector of limbs is
//                MOVED in -- SObjectizer allocates the message object, the limbs stay in the
//                buffer the worker filled -- and the master folds it and sends that worker the
//                next term, as Savina's does. form=1 (the payload copied into the message's own
//                storage) has no SObjectizer counterpart: a message is a C++ object of a fixed
//                type -- not applicable. Where a worker's demand runs is the thread_pool's
//                decision.

#include <qvospec/savina/piprecision.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_piprecision_sobjectizer {

using namespace qvospec::savina::piprecision;

struct msg_work final : public so_5::message_t {
    std::uint32_t term;
    explicit msg_work(std::uint32_t k) noexcept : term(k) {}
};
struct msg_result final : public so_5::message_t {
    std::uint32_t     term;
    std::uint32_t     worker;
    Limb              proof;
    std::vector<Limb> limbs;
    msg_result(std::uint32_t k, std::uint32_t w, Limb p, std::vector<Limb> &&v) noexcept
        : term(k), worker(w), proof(p), limbs(std::move(v)) {}
};
struct msg_ready final : public so_5::signal_t {};

// The worker's count goes straight to its Tally, a 128-byte slot of its own: an agent on the pool
// is not bound to a thread.
class worker_t final : public so_5::agent_t {
    const so_5::mbox_t  m_master;
    const Context      &m_context;
    const std::uint32_t m_index;
    Tally              &m_tally;
    Scratch             m_scratch;

public:
    worker_t(context_t ctx, so_5::mbox_t master, const Context &context, std::uint32_t index,
             Tally &tally)
        : so_5::agent_t{std::move(ctx)}
        , m_master{std::move(master)}
        , m_context{context}
        , m_index{index}
        , m_tally{tally} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_work> m) {
            ++m_tally.works;
            std::vector<Limb> limbs;  // a fresh buffer per answer: it leaves with the message
            bbp_term(m_context, m->term, m_scratch, limbs);
            so_5::send<msg_result>(m_master, m->term, m_index, bbp_proof(m_scratch),
                                   std::move(limbs));
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_master); }
};

class master_t final : public so_5::agent_t {
    const Config              m_config;
    qvo::Watch               &m_watch;
    Fold                     &m_fold;
    std::vector<so_5::mbox_t> m_workers;
    std::size_t               m_ready{0};
    bool                      m_stop{false};

    void hand_out(std::uint32_t worker) { so_5::send<msg_work>(m_workers[worker], m_fold.next()); }

public:
    master_t(context_t ctx, const Config &config, qvo::Watch &watch, Fold &fold)
        : so_5::agent_t{std::move(ctx)}, m_config{config}, m_watch{watch}, m_fold{fold} {}

    void add_worker(so_5::mbox_t mbox) { m_workers.push_back(std::move(mbox)); }

    void so_define_agent() override {
        // Savina's StartMessage: the first `batch` terms, round-robin.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_workers.size()) return;
            m_watch.start();
            for (std::uint32_t t = 0; t < m_config.batch; ++t) hand_out(t % m_config.workers);
        });
        // Savina's Master.process(ResultMessage).
        so_subscribe_self().event([this](so_5::mhood_t<msg_result> m) {
            if (m->worker >= m_config.workers)
                qvo::fail("savina/piprecision: an answer names worker " + std::to_string(m->worker)
                          + ", which does not exist");
            if (m_fold.add(m->term, m->limbs.data(), m->limbs.size(), m->proof)) m_stop = true;
            if (!m_stop) hand_out(m->worker);
            if (!m_fold.complete()) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<int>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;
    if (inline_payload(p))
        qvo::not_applicable("form=1 carries the payload in the message's own storage, a "
                            "variable-length tail behind the event (qb's allocated_push); a "
                            "SObjectizer message is a C++ object of a fixed type, so its "
                            "counterpart is form=0, the vector moved into the message");
    const Context context = make_context(c.precision);

    Fold               fold(c, context);
    std::vector<Tally> tallies(c.workers);
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            auto *master = coop.make_agent<master_t>(c, std::ref(watch), std::ref(fold));
            for (std::uint32_t w = 0; w < c.workers; ++w)
                master->add_worker(coop.make_agent<worker_t>(master->so_direct_mbox(),
                                                             std::cref(context), w,
                                                             std::ref(tallies[w]))
                                       ->so_direct_mbox());
        });
    });
    // launch() returned, every work thread joined: the fold and every tally are final, and reduced
    // here, outside the window.
    qvo::Answer answer{checksum(fold, works(tallies), reference(p).proofs), 0};
    answer.observed[kObservedTerms] = fold.requested;
    return answer;
}

}  // namespace savina_piprecision_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::piprecision::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::piprecision::expected;
    spec.work_unit         = qvospec::savina::piprecision::kWorkUnit;
    spec.work_units        = qvospec::savina::piprecision::work_units;
    spec.observed_at_least = {{qvospec::savina::piprecision::kObservedTerms,
                               qvospec::savina::piprecision::min_terms}};
    spec.idiom_source      = "the nqueens adapter (direct mboxes, make_pool_binder, ready signal)";
    spec.idiom_note        = "one coop: the master sends msg_work to a worker's direct mbox, the "
                             "worker answers a msg_result with the limbs vector moved in; "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the payload is a std::vector moved into the message (form=0): SObjectizer allocates the "
        "message object, the limbs stay in the worker's buffer and the master reads them where "
        "they are; which pool thread computes a term is the thread_pool's decision");

    return qvo::run(argc, argv, std::move(spec), savina_piprecision_sobjectizer::body);
}
