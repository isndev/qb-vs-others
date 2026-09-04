// @benchmark     savina/big
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the ping-pong adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, messages derived from so_5::message_t) and dev/so_5/disp/thread_pool/
//                pub.hpp for the many-agent dispatcher -- qvoso::make_pool_binder.
// @idiom-note    120 agents on a thread_pool of `cores` pinned work threads with
//                fifo_t::individual. Every agent holds the field's direct mboxes (one shared
//                vector, filled before the environment starts), so a pong goes straight to
//                `field[pinger]` and no hot message carries an mbox. Ping and pong are two
//                message types because one agent handles both.

#include <qvospec/savina/big.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_big_sobjectizer {

using namespace qvospec::savina::big;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_ping final : public so_5::message_t {
    std::uint32_t pinger;
    std::uint32_t k;
    msg_ping(std::uint32_t p, std::uint32_t kk) noexcept : pinger(p), k(kk) {}
};
struct msg_pong final : public so_5::message_t {
    std::uint64_t value;
    explicit msg_pong(std::uint64_t v) noexcept : value(v) {}
};
struct msg_start final : public so_5::signal_t {};
struct msg_ready final : public so_5::signal_t {};
struct msg_done final : public so_5::message_t {
    std::uint64_t acc;
    std::uint64_t received;
    msg_done(std::uint64_t a, std::uint64_t r) noexcept : acc(a), received(r) {}
};

// The field's direct mboxes plus the sink's, filled inside introduce_coop before the
// environment starts and read-only from then on.
struct Field {
    std::vector<so_5::mbox_t> actors;
    so_5::mbox_t              sink;
};

class big_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_self;
    const std::uint32_t m_pings;
    TargetSequence      m_seq;
    std::uint64_t       m_acc{0};
    std::uint64_t       m_received{0};
    std::uint32_t       m_sent{0};

    void ping() {
        const std::uint32_t k = m_sent++;
        so_5::send<msg_ping>(m_field.actors[m_seq.next()], m_self, k);
    }

public:
    big_t(context_t ctx, const Field &field, std::uint32_t self, std::uint32_t actors,
          std::uint32_t pings)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_self{self}
        , m_pings{pings}
        , m_seq{self, actors} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            ++m_received;
            ping();
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_ping> m) {
            so_5::send<msg_pong>(m_field.actors[m->pinger], pong_value(m->pinger, m_self, m->k));
        });
        // Two deliveries proven by one pong: the ping it answers and itself. Pings received
        // from peers are not counted: they may keep arriving after the done has been sent.
        so_subscribe_self().event([this](so_5::mhood_t<msg_pong> m) {
            m_received += 2;
            m_acc += m->value;
            if (m_sent < m_pings)
                ping();
            else
                so_5::send<msg_done>(m_field.sink, m_acc, m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.sink); }
};

class sink_t final : public so_5::agent_t {
    const Field &m_field;
    qvo::Watch  &m_watch;
    Sink        &m_sink;
    std::size_t  m_ready{0};
    std::size_t  m_done{0};

public:
    sink_t(context_t ctx, const Field &field, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_watch{watch}, m_sink{sink} {}

    void so_define_agent() override {
        // The window opens once every agent has started: every work thread running and warm.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_field.actors.size()) return;
            m_watch.start();
            for (const auto &a : m_field.actors) so_5::send<msg_start>(a);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done> d) {
            m_sink.checksum += d->acc;
            m_sink.messages += d->received + 1;
            if (++m_done != m_field.actors.size()) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto pings  = static_cast<std::uint32_t>(p.get("pings"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.sink = coop.make_agent<sink_t>(std::cref(field), std::ref(watch), std::ref(sink))
                             ->so_direct_mbox();
            field.actors.reserve(actors);
            for (std::uint32_t a = 0; a < actors; ++a)
                field.actors.push_back(
                    coop.make_agent<big_t>(std::cref(field), a, actors, pings)->so_direct_mbox());
        });
    });

    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_big_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::big::params();
    spec.expected          = qvospec::savina::big::expected;
    spec.expected_messages = qvospec::savina::big::expected_messages;
    spec.work_unit         = qvospec::savina::big::kWorkUnit;
    spec.work_units        = qvospec::savina::big::work_units;
    spec.idiom_source      = "the ping-pong adapter + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "agents on their DIRECT mboxes, msg_ping / msg_pong, the field's "
                             "mboxes shared before start, thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the 120 agents are placed by the thread_pool, so how many pings cross a core is the "
        "dispatcher's decision; qb's cell fixes actor a on core a % cores -- see "
        "benchmarks/savina/big.md");

    return qvo::run(argc, argv, std::move(spec), savina_big_sobjectizer::body);
}
