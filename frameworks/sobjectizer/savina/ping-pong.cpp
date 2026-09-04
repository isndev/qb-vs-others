// @benchmark     savina/ping-pong
// @framework     sobjectizer 5.8.5.1
// @idiom-source  SObjectizer's own samples, shipped in the 5.8.5.1 tree:
//                  dev/sample/so_5/ping_pong_minimal/main.cpp -- agent_t, so_subscribe, so_evt_start
//                  dev/sample/so_5/ping_pong/main.cpp         -- active_obj vs default dispatcher
//                  dev/so_5/disp/mpsc_queue_traits/pub.hpp    -- combined/simple lock factories
// @idiom-note    DEVIATION FROM THE SAMPLE, in SObjectizer's favour: both shipped ping-pong
//                samples route messages through one SHARED mbox that both agents subscribe to.
//                That is the simplest thing to write, and it is not the fastest thing
//                SObjectizer can do -- a shared mbox is multi-consumer and pays for it. This
//                implementation uses each agent's DIRECT mbox (`so_direct_mbox()`), which is the
//                framework's own fast path. FAIRNESS.md 1.1: where a framework offers a faster
//                idiom than the one its documentation leads with, the faster one is what enters
//                the table.

#include <qvospec/savina/ping-pong.h>

#include <so_5/all.hpp>

#include <chrono>

namespace savina_ping_pong_sobjectizer {

using namespace qvospec::savina::ping_pong;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

// Derived from so_5::message_t rather than sent as a bare user type: SObjectizer wraps arbitrary
// user types in an envelope, and deriving avoids that wrapper. Again the faster of the framework's
// two spellings.
struct msg_ball final : public so_5::message_t {
    std::uint64_t seq;
    explicit msg_ball(std::uint64_t s) noexcept : seq(s) {}
};

struct msg_hello final : public so_5::signal_t {};
struct msg_ack final : public so_5::signal_t {};

class ponger_t final : public so_5::agent_t {
    so_5::mbox_t m_peer;

public:
    explicit ponger_t(context_t ctx) : so_5::agent_t{std::move(ctx)} {}

    void set_peer(so_5::mbox_t peer) { m_peer = std::move(peer); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_hello>) {
            so_5::send<msg_ack>(m_peer);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_ball> ball) {
            so_5::send<msg_ball>(m_peer, ball->seq);
        });
    }
};

class pinger_t final : public so_5::agent_t {
    const so_5::mbox_t  m_peer;
    const std::uint64_t m_rounds;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    std::uint64_t       m_acc{0};
    std::uint64_t       m_delivered{0};

public:
    pinger_t(context_t ctx, so_5::mbox_t peer, std::uint64_t rounds, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_peer{std::move(peer)}
        , m_rounds{rounds}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        // The handshake round trip completes before the window opens, so both agents have been
        // scheduled at least once and their work threads are warm. Same discipline as the qb and
        // CAF adapters.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ack>) {
            m_watch.start();
            so_5::send<msg_ball>(m_peer, m_rounds - 1);
        });

        so_subscribe_self().event([this](so_5::mhood_t<msg_ball> ball) {
            m_acc += qvo::mix(ball->seq);
            m_delivered += 2;
            if (ball->seq) {
                so_5::send<msg_ball>(m_peer, ball->seq - 1);
            } else {
                m_watch.stop();
                m_sink.checksum = m_acc;
                m_sink.messages = m_delivered;
                so_environment().stop();
            }
        });
    }

    void so_evt_start() override { so_5::send<msg_hello>(m_peer); }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink sink;

    so_5::launch([&](so_5::environment_t &env) {
        // The spin/park lever, SObjectizer's half.
        //
        // A dispatcher's MPSC queue takes a lock factory. `combined_lock_factory(d)` spins for `d`
        // before falling back to a mutex; `simple_lock_factory()` is mutex + condition variable
        // only. Read from dev/so_5/disp/mpsc_queue_traits/pub.hpp of the pinned SObjectizer.
        // A spin budget far longer than any single round trip is this framework's way of spelling
        // busy-spin, and it is what makes qb's setLatency(0) a comparable setting.
        auto tune = [spin](auto &q) {
            if (spin)
                q.lock_factory(so_5::disp::mpsc_queue_traits::combined_lock_factory(
                    std::chrono::seconds{10}));
            else
                q.lock_factory(so_5::disp::mpsc_queue_traits::simple_lock_factory());
        };

        so_5::disp_binder_shptr_t binder;
        if (cores >= 2) {
            // active_obj: one work thread per agent -- SObjectizer's nearest equivalent to qb
            // placing each actor on its own VirtualCore.
            binder = so_5::disp::active_obj::make_dispatcher(
                         env, "qvo-ao",
                         so_5::disp::active_obj::disp_params_t{}.tune_queue_params(tune))
                         .binder();
        } else {
            binder = so_5::disp::one_thread::make_dispatcher(
                         env, "qvo-ot",
                         so_5::disp::one_thread::disp_params_t{}.tune_queue_params(tune))
                         .binder();
        }

        env.introduce_coop(std::move(binder), [&](so_5::coop_t &coop) {
            auto *ponger = coop.make_agent<ponger_t>();
            auto *pinger = coop.make_agent<pinger_t>(ponger->so_direct_mbox(), rounds,
                                                     std::ref(watch), std::ref(sink));
            ponger->set_peer(pinger->so_direct_mbox());
        });
    });

    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_ping_pong_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::ping_pong::params();
    spec.expected          = qvospec::savina::ping_pong::expected;
    spec.expected_messages = qvospec::savina::ping_pong::expected_messages;
    spec.idiom_source      = "SObjectizer 5.8.5.1 dev/sample/so_5/ping_pong{,_minimal}/main.cpp";
    spec.idiom_note        = "agent_t subclasses on their DIRECT mboxes (faster than the samples' "
                             "shared mbox), messages derived from so_5::message_t, active_obj "
                             "dispatcher for cores>=2 and one_thread below it";
    spec.caveats           = {
        "cores>=2 uses the active_obj dispatcher (one thread per agent); cores=1 uses one_thread. "
        "SObjectizer has no notion of pinning an agent to a specific CPU, so 'cores' is a thread "
        "budget as it is for CAF, not a placement as it is for qb",
        "wait=1 maps to combined_lock_factory with a 10 s spin budget (never reached within a "
        "round trip); wait=0 maps to simple_lock_factory (mutex + condition variable)"};

    return qvo::run(argc, argv, std::move(spec), savina_ping_pong_sobjectizer::body);
}
