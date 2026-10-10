// @benchmark     savina/logmap
// @framework     sobjectizer 5.8.5.1
// @idiom-source  dev/sample/so_5/mutable_msg_agents/main.cpp -- SObjectizer's OWN sample of a
//                mutable message handed along a chain of agents, each modifying it and resending
//                the SAME instance with `so_5::send(next, std::move(cmd))`; dev/so_5/mhood.hpp
//                ("If mhood_t<mutable_msg<M>> is used then a redirection must be done this way");
//                and the bank-transaction adapter beside this file (agents on their DIRECT mboxes,
//                the field's mboxes shared before start, dev/so_5/disp/thread_pool/pub.hpp via
//                qvoso::make_pool_binder).
// @idiom-note    The ask is one MUTABLE message per chain, redirected: the worker sends
//                `mutable_msg<msg_term>{term, sender}` to its computer's direct mbox, the computer
//                overwrites the term with the next one and redirects the same instance to the
//                sender's mbox, and the worker, when it still owes terms, redirects it back (it
//                already carries the term to grow from) -- so a series' whole chain moves ONE
//                message object and constructs or allocates no message per hop, SObjectizer's own
//                idiom for a message passed along (it exists to avoid exactly that copy). What a
//                delivery still costs is the dispatcher's: the thread_pool allocates one demand
//                node per push (dev/so_5/disp/thread_pool/impl/basic_event_queue.hpp, push()),
//                one_thread a slot of its deque. SObjectizer messages carry no sender, so
//                the term carries the worker's index, as Savina's ComputeMessage carries its
//                sender. NextTerm, GetTerm, the readiness and the stop are signals. The held
//                NextTerm requests are a count. Master, workers and computers on a thread_pool of
//                `cores` pinned work threads with fifo_t::individual, placed by the dispatcher.
//                QVO_SO_GROUP_COOPS=1 binds each series as its own coop with fifo_t::cooperation
//                on the same pool instead (so_support.h, the group sweep): a SWEEP document, not
//                a table cell, until a quiet host says it is the faster form (FAIRNESS.md 1.1).

#include <qvospec/savina/logmap.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_logmap_sobjectizer {

using namespace qvospec::savina::logmap;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t held{0};
};

struct msg_ready final : public so_5::signal_t {};  // handshake, outside the window
struct msg_next final : public so_5::signal_t {};   // master -> worker: one more term
struct msg_get final : public so_5::signal_t {};    // master -> worker: answer when done
struct msg_exit final : public so_5::signal_t {};   // master -> computer: Savina's StopMessage
// worker -> computer: the current term; computer -> worker, the same instance: the next one.
// Sent and received as so_5::mutable_msg<msg_term>.
struct msg_term final : public so_5::message_t {
    double        term;
    std::uint32_t sender;
    msg_term(double t, std::uint32_t s) noexcept : term(t), sender(s) {}
};
struct msg_result final : public so_5::message_t {
    std::uint32_t index;
    std::uint64_t chain;
    std::uint64_t received;
    std::uint64_t held;
    msg_result(std::uint32_t i, std::uint64_t c, std::uint64_t r, std::uint64_t h) noexcept
        : index(i), chain(c), received(r), held(h) {}
};
struct msg_report final : public so_5::message_t {
    std::uint32_t index;
    std::uint64_t served;
    std::uint64_t received;
    msg_report(std::uint32_t i, std::uint64_t s, std::uint64_t r) noexcept
        : index(i), served(s), received(r) {}
};

struct Field {
    so_5::mbox_t              master;
    std::vector<so_5::mbox_t> workers;
    std::vector<so_5::mbox_t> computers;
};

class computer_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    const double        m_rate;
    std::uint64_t       m_served{0};
    std::uint64_t       m_received{0};

public:
    computer_t(context_t ctx, const Field &field, std::uint32_t index)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_index{index}
        , m_rate{rate_of(index)} {}

    void so_define_agent() override {
        // Savina's RateComputer: the next term, the same message back to its sender.
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_term> cmd) {
            ++m_received;
            ++m_served;
            cmd->term = next_term(cmd->term, m_rate);
            const auto &to = m_field.workers[cmd->sender];
            so_5::send(to, std::move(cmd));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++m_received;
            so_5::send<msg_report>(m_field.master, m_index, m_served, m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class worker_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    double              m_term;
    std::uint64_t       m_chain;
    std::uint64_t       m_owed{0};  // NextTerm requests held while an answer is awaited
    bool                m_awaiting{false};
    bool                m_get_pending{false};
    std::uint64_t       m_received{0};
    std::uint64_t       m_held{0};

    void answer() {
        so_5::send<msg_result>(m_field.master, m_index, m_chain, m_received, m_held);
    }

public:
    worker_t(context_t ctx, const Field &field, std::uint32_t index)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_index{index}
        , m_term{start_of(index)}
        , m_chain{chain_seed(index)} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_next>) {
            ++m_received;
            if (m_awaiting) {
                ++m_owed;
                ++m_held;
                return;
            }
            m_awaiting = true;
            so_5::send<so_5::mutable_msg<msg_term>>(m_field.computers[m_index], m_term, m_index);
        });
        // The computer's answer. Still owing a term: the same message goes straight back with it.
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_term> cmd) {
            ++m_received;
            m_term  = cmd->term;
            m_chain = chain_step(m_chain, m_term);
            if (m_owed) {
                --m_owed;
                // cmd->term is already the term to grow from.
                so_5::send(m_field.computers[m_index], std::move(cmd));
                return;
            }
            m_awaiting = false;
            if (m_get_pending) answer();
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_get>) {
            ++m_received;
            if (m_awaiting)
                m_get_pending = true;  // answered when the last answer lands
            else
                answer();
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class master_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint64_t m_terms;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    std::size_t         m_ready{0};
    std::size_t         m_results{0};
    std::size_t         m_reports{0};
    std::uint64_t       m_received{0};

public:
    master_t(context_t ctx, const Field &field, std::uint64_t terms, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_terms{terms}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_field.workers.size() + m_field.computers.size()) return;
            m_watch.start();
            // The reference's loop: term by term, worker by worker, then one GetTerm each.
            for (std::uint64_t k = 0; k < m_terms; ++k)
                for (const auto &w : m_field.workers) so_5::send<msg_next>(w);
            for (const auto &w : m_field.workers) so_5::send<msg_get>(w);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_result> r) {
            ++m_received;
            m_sink.checksum += series_key(r->index, r->chain);
            m_sink.messages += r->received;
            m_sink.held += r->held;
            if (++m_results != m_field.workers.size()) return;
            for (const auto &c : m_field.computers) so_5::send<msg_exit>(c);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_report> r) {
            ++m_received;
            m_sink.checksum += computer_key(r->index, r->served);
            m_sink.messages += r->received;
            if (++m_reports != m_field.computers.size()) return;
            m_sink.messages += m_received;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto terms  = static_cast<std::uint64_t>(p.get("terms"));
    const auto series = static_cast<std::uint32_t>(p.get("series"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;
    field.workers.reserve(series);
    field.computers.reserve(series);
    so_5::launch([&](so_5::environment_t &env) {
        if (cores >= 2 && qvoso::group_coops()) {
            // The group sweep (so_support.h): series i -- its worker and its computer -- is one
            // coop bound with fifo_t::cooperation, the master one bound with fifo_t::individual,
            // all on ONE pool. Every coop is built and the field filled BEFORE the first is
            // registered: an agent reads the field from its first handler on.
            using so_5::disp::thread_pool::bind_params_t;
            using so_5::disp::thread_pool::fifo_t;
            const auto pool  = qvoso::make_pool_dispatcher(env, cores, spin);
            const auto group = pool.binder(bind_params_t{}.fifo(fifo_t::cooperation));
            auto master = env.make_coop(pool.binder(bind_params_t{}.fifo(fifo_t::individual)));
            field.master = master->make_agent<master_t>(std::cref(field), terms, std::ref(watch),
                                                        std::ref(sink))
                               ->so_direct_mbox();
            std::vector<so_5::coop_unique_holder_t> pairs;
            pairs.reserve(series);
            for (std::uint32_t i = 0; i < series; ++i) {
                auto pair = env.make_coop(group);
                field.computers.push_back(
                    pair->make_agent<computer_t>(std::cref(field), i)->so_direct_mbox());
                field.workers.push_back(
                    pair->make_agent<worker_t>(std::cref(field), i)->so_direct_mbox());
                pairs.push_back(std::move(pair));
            }
            env.register_coop(std::move(master));
            for (auto &pair : pairs) env.register_coop(std::move(pair));
            return;
        }
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.master = coop.make_agent<master_t>(std::cref(field), terms, std::ref(watch),
                                                     std::ref(sink))
                               ->so_direct_mbox();
            for (std::uint32_t i = 0; i < series; ++i) {
                field.computers.push_back(
                    coop.make_agent<computer_t>(std::cref(field), i)->so_direct_mbox());
                field.workers.push_back(
                    coop.make_agent<worker_t>(std::cref(field), i)->so_direct_mbox());
            }
        });
    });
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kHeld] = sink.held;
    return answer;
}

}  // namespace savina_logmap_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::logmap::params();
    spec.expected          = qvospec::savina::logmap::expected;
    spec.expected_messages = qvospec::savina::logmap::expected_messages;
    spec.work_unit         = qvospec::savina::logmap::kWorkUnit;
    spec.work_units        = qvospec::savina::logmap::work_units;
    spec.idiom_source      = "dev/sample/so_5/mutable_msg_agents/main.cpp (a mutable message "
                             "redirected along a chain) + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "one mutable_msg<msg_term> per round trip, redirected computer -> "
                             "worker -> computer with so_5::send(mbox, std::move(cmd)); the term "
                             "carries its sender's index; held NextTerms are a count; "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    if (qvoso::group_coops())
        spec.caveats.insert(spec.caveats.begin(),
                            qvoso::group_coops_caveat(
                                "series i: its worker and its rate computer, at cores >= 2; "
                                "cores=1 is one_thread either way"));
    spec.caveats.emplace_back(
        "the master, the workers and the computers are placed by the thread_pool: which thread "
        "runs a worker's or a computer's next demand is the dispatcher's decision, so a round "
        "trip may cross a core; qb's cell pins series i (worker + computer) on core "
        "(1 + i) % cores -- see benchmarks/savina/logmap.md");
    spec.caveats.emplace_back(
        "a chain redirects ONE mutable message (so_5::send(mbox, std::move(cmd)), SObjectizer's "
        "redirection of a mutable message): no message is allocated per hop, though the "
        "thread_pool's queue allocates one demand node per delivery (thread_pool/impl/"
        "basic_event_queue.hpp, push()); qb's reply() re-sends the received event as a copy into "
        "its pipe; NextTerm and GetTerm are signals, which carry no instance");

    return qvo::run(argc, argv, std::move(spec), savina_logmap_sobjectizer::body);
}
