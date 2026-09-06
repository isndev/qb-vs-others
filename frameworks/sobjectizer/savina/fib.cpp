// @benchmark     savina/fib
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the big adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder) and dev/so_5/environment.hpp's
//                `so_5::introduce_child_coop(*this, binder, lambda)` -- SObjectizer's own way for
//                an agent to create agents at run time -- with
//                `so_deregister_agent_coop_normally()` for the agent's own end.
// @idiom-note    SObjectizer has no bare "spawn an agent": every agent belongs to a COOPERATION,
//                and a child created at run time is a child coop of one agent, registered with
//                the environment. A node introduces one child coop PER child -- an agent ends by
//                deregistering its own coop, so two siblings sharing one would take each other
//                down with the first response (measured: error 185 `add_child() can be processed
//                only when coop is registered` on the pool) -- sends fib(n-1) / fib(n-2) to their
//                direct mboxes, and deregisters its own coop right after sending its response,
//                which is what a SObjectizer program does to end an agent. `so_define_agent()`
//                runs during registration, so a send to the child's direct mbox right after the
//                call is delivered, not lost.

#include <qvospec/savina/fib.h>

#include "../so_support.h"

#include <so_5/all.hpp>

namespace savina_fib_sobjectizer {

using namespace qvospec::savina::fib;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_request final : public so_5::message_t {
    std::uint32_t n;
    explicit msg_request(std::uint32_t nn) noexcept : n(nn) {}
};
struct msg_response final : public so_5::message_t {
    std::uint64_t value;
    std::uint64_t chk;
    std::uint64_t messages;
    msg_response(std::uint64_t v, std::uint64_t c, std::uint64_t m) noexcept
        : value(v), chk(c), messages(m) {}
};
struct msg_ready final : public so_5::signal_t {};

class fib_t final : public so_5::agent_t {
    const so_5::mbox_t              m_parent;
    const so_5::disp_binder_shptr_t m_binder;
    const bool                      m_seed;
    std::uint64_t                   m_value{0};
    std::uint64_t                   m_chk{0};
    std::uint64_t                   m_messages{0};
    std::uint32_t                   m_pending{0};

    void respond(std::uint64_t value, std::uint64_t chk) {
        so_5::send<msg_response>(m_parent, value, chk, m_messages);
        so_deregister_agent_coop_normally();
    }

public:
    fib_t(context_t ctx, so_5::mbox_t parent, so_5::disp_binder_shptr_t binder, bool seed)
        : so_5::agent_t{std::move(ctx)}
        , m_parent{std::move(parent)}
        , m_binder{std::move(binder)}
        , m_seed{seed} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_request> m) {
            ++m_messages;
            if (m->n <= 2) {
                respond(1, qvo::mix(1));
                return;
            }
            // One coop per child: a node ends by deregistering ITS coop, and a coop shared by two
            // siblings would end the other one with the first response.
            so_5::mbox_t a, b;
            so_5::introduce_child_coop(*this, m_binder, [&](so_5::coop_t &coop) {
                a = coop.make_agent<fib_t>(so_direct_mbox(), m_binder, false)->so_direct_mbox();
            });
            so_5::introduce_child_coop(*this, m_binder, [&](so_5::coop_t &coop) {
                b = coop.make_agent<fib_t>(so_direct_mbox(), m_binder, false)->so_direct_mbox();
            });
            so_5::send<msg_request>(a, m->n - 1);
            so_5::send<msg_request>(b, m->n - 2);
            m_pending = 2;
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_response> m) {
            m_messages += 1 + m->messages;
            m_value += m->value;
            m_chk += m->chk;
            if (--m_pending == 0) respond(m_value, qvo::mix(m_value) + m_chk);
        });
    }

    void so_evt_start() override {
        if (m_seed) so_5::send<msg_ready>(m_parent);
    }
};

// The root of the tree: asks the two seeds and closes the window on the second answer.
class sink_t final : public so_5::agent_t {
    const std::uint32_t m_n;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    so_5::mbox_t        m_seeds[2];
    std::size_t         m_ready{0};
    std::size_t         m_done{0};

public:
    sink_t(context_t ctx, std::uint32_t n, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_n{n}, m_watch{watch}, m_sink{sink} {}

    void seed(std::size_t s, so_5::mbox_t mbox) { m_seeds[s] = std::move(mbox); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != 2) return;
            m_watch.start();
            so_5::send<msg_request>(m_seeds[0], m_n - 1);
            so_5::send<msg_request>(m_seeds[1], m_n - 2);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_response> m) {
            m_sink.checksum += m->chk;
            m_sink.messages += 1 + m->messages;
            if (++m_done != 2) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n     = static_cast<std::uint32_t>(p.get("n"));
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink sink;
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        // The sink and the seeds are separate top-level coops: a seed's own deregistration must
        // not take the sink with it, and the sink's must not be a parent waiting on a tree.
        sink_t *sink_agent = nullptr;
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            sink_agent = coop.make_agent<sink_t>(n, std::ref(watch), std::ref(sink));
        });
        for (std::size_t s = 0; s < 2; ++s)
            env.introduce_coop(binder, [&](so_5::coop_t &coop) {
                sink_agent->seed(
                    s, coop.make_agent<fib_t>(sink_agent->so_direct_mbox(), binder, true)
                           ->so_direct_mbox());
            });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fib_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fib::params();
    spec.expected          = qvospec::savina::fib::expected;
    spec.expected_messages = qvospec::savina::fib::expected_messages;
    spec.work_unit         = qvospec::savina::fib::kWorkUnit;
    spec.work_units        = qvospec::savina::fib::work_units;
    spec.idiom_source      = "the big adapter + so_5::introduce_child_coop (environment.hpp) + "
                             "so_deregister_agent_coop_normally";
    spec.idiom_note        = "a node introduces one child coop per child, on their direct "
                             "mboxes, and deregisters its own coop after responding; "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer creates agents only inside a cooperation registered with the environment, "
        "so every node here pays one coop registration and one deregistration on top of the "
        "agent itself -- that is the framework's own dynamic-agent idiom, not an adapter choice "
        "(two siblings cannot share a coop: an agent ends by deregistering its coop, which would "
        "take the sibling down with it -- measured as error 185 on the pool)");
    spec.caveats.emplace_back(
        "children bind to the same thread_pool as their parent and the pool places them, so with "
        "cores=2 the tree is balanced by the dispatcher; qb's cell keeps each sub-tree on its "
        "seed's core because qb has no cross-core spawn -- see benchmarks/savina/fib.md");

    return qvo::run(argc, argv, std::move(spec), savina_fib_sobjectizer::body);
}
