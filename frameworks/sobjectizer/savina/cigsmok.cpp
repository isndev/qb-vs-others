// @benchmark     savina/cigsmok
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the chameneos adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, messages derived from so_5::message_t, payload-free ones as signals, the
//                field's mboxes shared before start) and dev/so_5/disp/thread_pool/pub.hpp via
//                qvoso::make_pool_binder.
// @idiom-note    The reference's actors one for one, as agents on their direct mboxes:
//                StartSmoking is msg_smoke(round, period) to the chosen smoker, StartedSmoking
//                msg_started(round, smoker) back, Exit a signal, Report msg_report(partial,
//                messages). Savina's arbiter builds its smokers in its constructor; SObjectizer
//                creates agents inside a cooperation, so the arbiter and its smokers are one coop
//                registered before the window. The smoker sends msg_started before it smokes; a
//                send is queued at the receiver at once, and whether the arbiter's next decision
//                runs during the smoke is the thread_pool's decision (`cores` pinned work threads,
//                fifo_t::individual).

#include <qvospec/savina/cigsmok.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <cstdio>
#include <vector>

namespace savina_cigsmok_sobjectizer {

using namespace qvospec::savina::cigsmok;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t stale{0};
};

struct msg_ready final : public so_5::signal_t {};  // handshake, outside the window
struct msg_start final : public so_5::signal_t {};
struct msg_smoke final : public so_5::message_t {
    std::uint64_t round;
    std::uint32_t period;
    msg_smoke(std::uint64_t r, std::uint32_t p) noexcept : round(r), period(p) {}
};
struct msg_started final : public so_5::message_t {
    std::uint64_t round;
    std::uint32_t smoker;
    msg_started(std::uint64_t r, std::uint32_t s) noexcept : round(r), smoker(s) {}
};
struct msg_exit final : public so_5::signal_t {};
struct msg_report final : public so_5::message_t {
    std::uint64_t partial;
    std::uint64_t messages;
    msg_report(std::uint64_t p, std::uint64_t m) noexcept : partial(p), messages(m) {}
};

// The mboxes, filled while the coop is built -- before any agent starts -- and read-only after.
struct Field {
    so_5::mbox_t              arbiter;
    std::vector<so_5::mbox_t> smokers;
};

class smoker_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_number;
    std::uint64_t       m_acc{0};  // the smoker's terms of the checksum (cigsmok.h)
    std::uint64_t       m_received{0};

public:
    smoker_t(context_t ctx, const Field &field, std::uint32_t number)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_number{number} {}

    void so_define_agent() override {
        so_subscribe_self()
            // StartSmoking: acknowledge first -- the ingredients are off the table -- then smoke.
            .event([this](so_5::mhood_t<msg_smoke> m) {
                ++m_received;
                const std::uint64_t round  = m->round;
                const std::uint32_t period = m->period;
                so_5::send<msg_started>(m_field.arbiter, round, m_number);
                m_acc += smoke_term(m_number, round, period);
            })
            .event([this](so_5::mhood_t<msg_exit>) {
                ++m_received;
                m_acc += exit_term(m_number);
                so_5::send<msg_report>(m_field.arbiter, m_acc, m_received);
            });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.arbiter); }
};

class arbiter_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint64_t m_rounds;
    const std::uint64_t m_smoke;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    std::size_t         m_ready{0};
    std::uint64_t       m_outstanding{0};  // the round last put on the table (diagnostic only)
    std::uint64_t       m_played{0};       // StartedSmoking received while not exiting
    bool                m_exiting{false};
    std::size_t         m_reports{0};
    std::uint64_t       m_acc{0};  // the arbiter's terms of the checksum (cigsmok.h)
    std::uint64_t       m_messages{0};  // reported by the smokers
    std::uint64_t       m_received{0};
    std::uint64_t       m_stale{0};

    // Put the ingredients on the table for round `round`: the draw names the smoker and the period.
    void choose(std::uint64_t round) {
        const std::uint32_t smoker = smoker_of(round, m_field.smokers.size());
        m_outstanding              = round;
        so_5::send<msg_smoke>(m_field.smokers[smoker], round, period_of(round, m_smoke));
    }

public:
    arbiter_t(context_t ctx, const Field &field, std::uint64_t rounds, std::uint64_t smoke,
              qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_rounds{rounds}
        , m_smoke{smoke}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        so_subscribe_self()
            // Every smoker is up: open the window with the reference's StartMessage.
            .event([this](so_5::mhood_t<msg_ready>) {
                if (++m_ready != m_field.smokers.size()) return;
                m_watch.start();
                so_5::send<msg_start>(so_direct_mbox());
            })
            .event([this](so_5::mhood_t<msg_start>) {
                ++m_received;
                choose(0);
            })
            // StartedSmoking. Every one is folded into the sum and, as in the reference, every
            // one plays the next round until the last: a duplicate puts a second round on the
            // table and the run completes with a wrong sum. One naming another round than the
            // last put on the table is counted for stderr, nothing more.
            .event([this](so_5::mhood_t<msg_started> m) {
                ++m_received;
                m_acc += ack_term(m->smoker, m->round);
                if (m_exiting || m->round != m_outstanding) ++m_stale;
                if (m_exiting) return;
                if (++m_played < m_rounds) {
                    choose(m_played);
                    return;
                }
                m_exiting = true;
                for (const auto &smoker : m_field.smokers) so_5::send<msg_exit>(smoker);
            })
            .event([this](so_5::mhood_t<msg_report> m) {
                ++m_received;
                m_acc += m->partial;
                m_messages += m->messages;
                if (++m_reports != m_field.smokers.size()) return;
                m_sink.checksum = m_acc;
                m_sink.messages = m_messages + m_received;
                m_sink.stale    = m_stale;
                m_watch.stop();
                so_environment().stop();
            });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds  = at_least_one(p.get("rounds"), "rounds");
    const auto smokers = at_least_one(p.get("smokers"), "smokers");
    const auto smoke   = at_least_one(p.get("smoke"), "smoke");
    const auto cores   = static_cast<int>(p.get("cores"));
    const bool spin    = p.get("wait") != 0;

    Sink  sink;
    Field field;
    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.arbiter = coop.make_agent<arbiter_t>(std::cref(field), rounds, smoke,
                                                       std::ref(watch), std::ref(sink))
                                ->so_direct_mbox();
            field.smokers.reserve(static_cast<std::size_t>(smokers));
            for (std::uint32_t j = 0; j < smokers; ++j)
                field.smokers.push_back(
                    coop.make_agent<smoker_t>(std::cref(field), j)->so_direct_mbox());
        });
    });
    if (sink.stale != 0)
        std::fprintf(stderr,
                     "savina/cigsmok sobjectizer: %llu StartedSmoking did not name the "
                     "round last chosen\n",
                     static_cast<unsigned long long>(sink.stale));
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_cigsmok_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::cigsmok::params();
    spec.expected          = qvospec::savina::cigsmok::expected;
    spec.expected_messages = qvospec::savina::cigsmok::expected_messages;
    spec.work_unit         = qvospec::savina::cigsmok::kWorkUnit;
    spec.work_units        = qvospec::savina::cigsmok::work_units;
    spec.idiom_source      = "the chameneos adapter + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "the reference's actors one for one on direct mboxes, payload-free "
                             "messages as signals; the arbiter and its smokers one coop built "
                             "before the window; msg_started sent before the smoke; "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the arbiter and the 200 smokers are placed by the thread_pool, so whether a smoke runs "
        "beside the arbiter's next decision or holds the thread it needs is the dispatcher's "
        "decision; qb's cell fixes the arbiter on core 0 and smoker j on core (j + 1) % cores -- "
        "see benchmarks/savina/cigsmok.md");

    return qvo::run(argc, argv, std::move(spec), savina_cigsmok_sobjectizer::body);
}
