// @benchmark     savina/philosophers
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the chameneos adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, messages derived from so_5::message_t, signals from so_5::signal_t, the
//                field's mboxes shared before start) and dev/so_5/disp/thread_pool/pub.hpp via
//                qvoso::make_pool_binder.
// @idiom-note    Hungry is msg_hungry(i) to the arbitrator's direct mbox, the answer the signal
//                msg_eat or msg_denied to `field[i]`, Done msg_done(i), Start the signal msg_start
//                to the philosopher's OWN direct mbox and Exit msg_exit(i, fold, messages).
//                Arbitrator and philosophers on a thread_pool of `cores` pinned work threads with
//                fifo_t::individual, so which thread runs the arbitrator -- and whether its queue
//                is written cross-core -- is the dispatcher's decision.

#include <qvospec/savina/philosophers.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <limits>
#include <vector>

namespace savina_philosophers_sobjectizer {

using namespace qvospec::savina::philosophers;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_hungry final : public so_5::message_t {
    std::uint32_t philosopher;
    explicit msg_hungry(std::uint32_t p) noexcept : philosopher(p) {}
};
struct msg_done final : public so_5::message_t {
    std::uint32_t philosopher;
    explicit msg_done(std::uint32_t p) noexcept : philosopher(p) {}
};
struct msg_exit final : public so_5::message_t {
    std::uint32_t philosopher;
    std::uint64_t chk;
    std::uint64_t received;
    msg_exit(std::uint32_t p, std::uint64_t c, std::uint64_t r) noexcept
        : philosopher(p), chk(c), received(r) {}
};
struct msg_eat final : public so_5::signal_t {};
struct msg_denied final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};
struct msg_ready final : public so_5::signal_t {};

struct Field {
    std::vector<so_5::mbox_t> philosophers;
    so_5::mbox_t              arbitrator;
};

class philosopher_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    const std::uint64_t m_rounds;
    std::uint64_t       m_starts{0};
    std::uint64_t       m_eats{0};
    std::uint64_t       m_chk{0};
    std::uint64_t       m_received{0};

public:
    philosopher_t(context_t ctx, const Field &field, std::uint32_t index, std::uint64_t rounds)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_index{index}, m_rounds{rounds} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            ++m_received;
            m_chk += term(kStart, m_index, ++m_starts);
            so_5::send<msg_hungry>(m_field.arbitrator, m_index);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_denied>) {
            so_5::send<msg_hungry>(m_field.arbitrator, m_index);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_eat>) {
            ++m_received;
            m_chk += term(kEat, m_index, ++m_eats);
            so_5::send<msg_done>(m_field.arbitrator, m_index);
            if (m_eats < m_rounds) {
                so_5::send<msg_start>(so_direct_mbox());
                return;
            }
            so_5::send<msg_exit>(m_field.arbitrator, m_index, m_chk, m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.arbitrator); }
};

class arbitrator_t final : public so_5::agent_t {
    static constexpr std::uint32_t kFree = std::numeric_limits<std::uint32_t>::max();

    const Field               &m_field;
    const std::uint32_t        m_n;
    qvo::Watch                &m_watch;
    Sink                      &m_sink;
    std::vector<std::uint32_t> m_owner;
    std::vector<std::uint64_t> m_grants;
    std::vector<std::uint64_t> m_dones;
    std::uint64_t              m_chk{0};
    std::uint64_t              m_received{0};
    std::size_t                m_ready{0};
    std::size_t                m_exited{0};
    bool                       m_violation{false};

public:
    arbitrator_t(context_t ctx, const Field &field, std::uint32_t n, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_n{n}
        , m_watch{watch}
        , m_sink{sink}
        , m_owner(n, kFree)
        , m_grants(n, 0)
        , m_dones(n, 0) {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_n) return;
            m_watch.start();
            for (const auto &p : m_field.philosophers) so_5::send<msg_start>(p);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_hungry> m) {
            const std::uint32_t i     = m->philosopher;
            const std::uint32_t left  = i;
            const std::uint32_t right = (i + 1) % m_n;
            if (m_owner[left] != kFree || m_owner[right] != kFree) {
                so_5::send<msg_denied>(m_field.philosophers[i]);
                return;
            }
            m_owner[left] = m_owner[right] = i;
            ++m_received;
            m_chk += term(kGrant, i, ++m_grants[i]);
            so_5::send<msg_eat>(m_field.philosophers[i]);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done> m) {
            const std::uint32_t i     = m->philosopher;
            const std::uint32_t left  = i;
            const std::uint32_t right = (i + 1) % m_n;
            ++m_received;
            m_chk += term(kDone, i, ++m_dones[i]);
            if (m_owner[left] != i || m_owner[right] != i) m_violation = true;
            m_owner[left] = m_owner[right] = kFree;
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit> m) {
            ++m_received;
            m_chk += term(kExit, m->philosopher, m_dones[m->philosopher]) + m->chk;
            m_received += m->received;
            if (++m_exited != m_n) return;
            m_watch.stop();
            m_sink.checksum = m_chk + (m_violation ? kForkViolation : 0);
            m_sink.messages = m_received;
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n      = static_cast<std::uint32_t>(p.get("philosophers"));
    const auto rounds = static_cast<std::uint64_t>(p.get("rounds"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;
    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.arbitrator = coop.make_agent<arbitrator_t>(std::cref(field), n, std::ref(watch),
                                                             std::ref(sink))
                                   ->so_direct_mbox();
            field.philosophers.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i)
                field.philosophers.push_back(
                    coop.make_agent<philosopher_t>(std::cref(field), i, rounds)->so_direct_mbox());
        });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_philosophers_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::philosophers::params();
    spec.expected          = qvospec::savina::philosophers::expected;
    spec.expected_messages = qvospec::savina::philosophers::expected_messages;
    spec.work_unit         = qvospec::savina::philosophers::kWorkUnit;
    spec.work_units        = qvospec::savina::philosophers::work_units;
    spec.idiom_source      = "the chameneos adapter + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "agents on their DIRECT mboxes, msg_hungry / msg_eat / msg_denied / "
                             "msg_done / msg_start (to its own mbox) / msg_exit, the field's "
                             "mboxes shared before start, thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the arbitrator and the 20 philosophers are placed by the thread_pool, so whether the "
        "arbitrator's queue is written cross-core is the dispatcher's decision; qb's cell pins "
        "the arbitrator alone on core 0 and every philosopher on the far side -- see "
        "benchmarks/savina/philosophers.md");
    spec.caveats.emplace_back(
        "how many requests the arbitrator refuses depends on the interleaving and is neither "
        "asserted nor reported; every refused request and its retry are delivered and timed");

    return qvo::run(argc, argv, std::move(spec), savina_philosophers_sobjectizer::body);
}
