// @benchmark     savina/counting
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the ping-pong adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, messages derived from so_5::message_t, active_obj for cores>=2) and
//                dev/sample/so_5/ping_pong_minimal/main.cpp for the agent shape.
// @idiom-note    Two agents, so the same active_obj deal as ping-pong: one pinned work thread
//                each, the counter's mailbox a real cross-thread MPSC queue. `so_5::send<>` is
//                FIFO per mbox, so the retrieve cannot overtake an increment and no ordering
//                trick is needed.

#include <qvospec/savina/counting.h>

#include "../so_support.h"

#include <so_5/all.hpp>

namespace savina_counting_sobjectizer {

using namespace qvospec::savina::counting;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_increment final : public so_5::message_t {
    std::uint64_t index;
    explicit msg_increment(std::uint64_t i) noexcept : index(i) {}
};
struct msg_retrieve final : public so_5::signal_t {};
struct msg_result final : public so_5::message_t {
    std::uint64_t acc;
    std::uint64_t count;
    msg_result(std::uint64_t a, std::uint64_t c) noexcept : acc(a), count(c) {}
};
struct msg_hello final : public so_5::signal_t {};
struct msg_ack final : public so_5::signal_t {};

class counter_t final : public so_5::agent_t {
    so_5::mbox_t  m_peer;
    std::uint64_t m_acc{0};
    std::uint64_t m_count{0};

public:
    explicit counter_t(context_t ctx) : so_5::agent_t{std::move(ctx)} {}

    void set_peer(so_5::mbox_t peer) { m_peer = std::move(peer); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_hello>) {
            so_5::send<msg_ack>(m_peer);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_increment> m) {
            m_acc += qvo::mix(m->index);
            ++m_count;
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_retrieve>) {
            so_5::send<msg_result>(m_peer, m_acc, m_count);
        });
    }
};

class producer_t final : public so_5::agent_t {
    const so_5::mbox_t  m_peer;
    const std::uint64_t m_n;
    qvo::Watch         &m_watch;
    Sink               &m_sink;

public:
    producer_t(context_t ctx, so_5::mbox_t peer, std::uint64_t n, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_peer{std::move(peer)}
        , m_n{n}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        // The handshake completes before the window opens: both work threads are running and
        // warm, the same discipline as the qb and CAF adapters.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ack>) {
            m_watch.start();
            for (std::uint64_t i = 0; i < m_n; ++i) so_5::send<msg_increment>(m_peer, i);
            so_5::send<msg_retrieve>(m_peer);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_result> r) {
            m_watch.stop();
            m_sink.checksum = r->acc;
            m_sink.messages = r->count + 2;  // the increments the counter saw, retrieve, result
            so_environment().stop();
        });
    }

    void so_evt_start() override { so_5::send<msg_hello>(m_peer); }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n     = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink sink;

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            auto *counter  = coop.make_agent<counter_t>();
            auto *producer = coop.make_agent<producer_t>(counter->so_direct_mbox(), n,
                                                         std::ref(watch), std::ref(sink));
            counter->set_peer(producer->so_direct_mbox());
        });
    });

    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_counting_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::counting::params();
    spec.expected          = qvospec::savina::counting::expected;
    spec.expected_messages = qvospec::savina::counting::expected_messages;
    spec.work_unit         = qvospec::savina::counting::kWorkUnit;
    spec.work_units        = qvospec::savina::counting::work_units;
    spec.idiom_source      = "the ping-pong adapter + dev/sample/so_5/ping_pong_minimal/main.cpp";
    spec.idiom_note        = "agent_t subclasses on their DIRECT mboxes, messages derived from "
                             "so_5::message_t, active_obj dispatcher for cores>=2 and one_thread "
                             "below it";
    spec.caveats           = qvoso::caveats();

    return qvo::run(argc, argv, std::move(spec), savina_counting_sobjectizer::body);
}
