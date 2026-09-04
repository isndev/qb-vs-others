// @benchmark     savina/thread-ring
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the ping-pong adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, messages derived from so_5::message_t) and dev/so_5/disp/thread_pool/
//                pub.hpp for the many-agent dispatcher -- see qvoso::make_pool_binder.
// @idiom-note    One hundred agents on a thread_pool of `cores` pinned work threads with
//                fifo_t::individual, SObjectizer's shipped shape for many agents on a small
//                thread budget (active_obj would run a hundred threads against two CPUs). The
//                token is one message type re-sent hop after hop; each agent learns its
//                successor's direct mbox once, before the environment starts.

#include <qvospec/savina/thread-ring.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_thread_ring_sobjectizer {

using namespace qvospec::savina::thread_ring;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_token final : public so_5::message_t {
    std::uint64_t remaining;
    std::uint64_t acc;
    msg_token(std::uint64_t r, std::uint64_t a) noexcept : remaining(r), acc(a) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_result final : public so_5::message_t {
    std::uint64_t acc;
    explicit msg_result(std::uint64_t a) noexcept : acc(a) {}
};

class ring_t final : public so_5::agent_t {
    so_5::mbox_t m_next;
    so_5::mbox_t m_sink;

public:
    explicit ring_t(context_t ctx) : so_5::agent_t{std::move(ctx)} {}

    void wire(so_5::mbox_t next, so_5::mbox_t sink) {
        m_next = std::move(next);
        m_sink = std::move(sink);
    }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_token> t) {
            const std::uint64_t acc       = t->acc + qvo::mix(t->remaining);
            const std::uint64_t remaining = t->remaining - 1;
            if (remaining == 0)
                so_5::send<msg_result>(m_sink, acc);
            else
                so_5::send<msg_token>(m_next, remaining, acc);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_sink); }
};

class sink_t final : public so_5::agent_t {
    const std::uint64_t m_hops;
    const std::size_t   m_actors;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    so_5::mbox_t        m_first;
    std::size_t         m_ready{0};

public:
    sink_t(context_t ctx, std::uint64_t hops, std::size_t actors, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_hops{hops}
        , m_actors{actors}
        , m_watch{watch}
        , m_sink{sink} {}

    void set_first(so_5::mbox_t first) { m_first = std::move(first); }

    void so_define_agent() override {
        // The window opens once every ring agent has started: every work thread running and
        // warm, so the measured span is message passing and not agent start-up.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_actors) return;
            m_watch.start();
            so_5::send<msg_token>(m_first, m_hops, std::uint64_t{0});
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_result> r) {
            m_watch.stop();
            m_sink.checksum = r->acc;
            m_sink.messages = m_hops + 1;
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::size_t>(p.get("actors"));
    const auto hops   = static_cast<std::uint64_t>(p.get("hops"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink sink;

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            auto *sk = coop.make_agent<sink_t>(hops, actors, std::ref(watch), std::ref(sink));
            std::vector<ring_t *> ring;
            ring.reserve(actors);
            for (std::size_t i = 0; i < actors; ++i) ring.push_back(coop.make_agent<ring_t>());
            for (std::size_t i = 0; i < actors; ++i) {
                const std::size_t next = i + 1 == actors ? 0 : i + 1;
                ring[i]->wire(ring[next]->so_direct_mbox(), sk->so_direct_mbox());
            }
            sk->set_first(ring[0]->so_direct_mbox());
        });
    });

    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_thread_ring_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::thread_ring::params();
    spec.expected          = qvospec::savina::thread_ring::expected;
    spec.expected_messages = qvospec::savina::thread_ring::expected_messages;
    spec.work_unit         = qvospec::savina::thread_ring::kWorkUnit;
    spec.work_units        = qvospec::savina::thread_ring::work_units;
    spec.idiom_source      = "the ping-pong adapter + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "agents on their DIRECT mboxes, one msg_token re-sent per hop, "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the ring's agents are placed by the thread_pool: whichever pinned work thread is free "
        "takes the next demand, so with cores=2 a hop crosses a core only when the pool decides "
        "so, where qb's cell crosses on EVERY hop -- see benchmarks/savina/thread-ring.md");

    return qvo::run(argc, argv, std::move(spec), savina_thread_ring_sobjectizer::body);
}
