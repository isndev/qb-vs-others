// @benchmark     savina/bitonicsort
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the thread-ring adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, each agent's tally a slot of its own) and dev/sample/so_5/
//                mutable_msg_agents/main.cpp -- SObjectizer's OWN sample of a `mutable_msg` handed
//                on with `so_5::send(next, std::move(cmd))`, no copy and no allocation (dev/so_5/
//                send_functions.hpp, "redirection of a message from existing message hood"); the
//                logmap adapter for the group binding (so_support.h, group_coops()).
// @idiom-note    The reference's ValueMessage and DataMessage are `mutable_msg`s, so a value that
//                passes through unchanged -- a stage, a step, a partition, a splitter, a
//                DataValueAdapter -- is the received message redirected, and only a compare-exchange,
//                a joiner and a ValueDataAdapter allocate one. One agent class per kind of the
//                reference's network (the spec's topology(), 44 772 agents at the defaults), created
//                before the window in dependency order. TWO bindings, FAIRNESS.md 1.1: every agent
//                on the thread_pool with fifo_t::individual (the default, qvoso::make_pool_binder),
//                or -- QVO_SO_GROUP_COOPS=1 -- each STEP of the network one coop bound with
//                fifo_t::cooperation on the same pool, SObjectizer's spelling of qb keeping a step
//                on one core. Which is the table cell is for a quiet-host sweep to decide.

#include <qvospec/savina/bitonicsort.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <map>
#include <utility>
#include <vector>

namespace savina_bitonicsort_sobjectizer {

using namespace qvospec::savina::bitonicsort;

// The binding the cell uses when QVO_SO_GROUP_COOPS is unset: individual, until a sweep says
// otherwise (so_support.h, group_coops()).
inline constexpr bool kGroupedByDefault = false;

// Both sent and received as so_5::mutable_msg<...>.
struct msg_value final : public so_5::message_t {
    std::uint64_t value;
    explicit msg_value(std::uint64_t v) noexcept : value(v) {}
};
struct msg_data final : public so_5::message_t {
    std::uint32_t order;
    std::uint64_t value;
    msg_data(std::uint32_t o, std::uint64_t v) noexcept : order(o), value(v) {}
};
struct msg_exit final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};
struct msg_ready final : public so_5::signal_t {};

using value_hood = so_5::mutable_mhood_t<msg_value>;
using data_hood  = so_5::mutable_mhood_t<msg_data>;
using value_msg  = so_5::mutable_msg<msg_value>;
using data_msg   = so_5::mutable_msg<msg_data>;

// The network as the agents see it: every mbox filled before the first coop is registered.
struct Net {
    const Topology           *topology{nullptr};
    const Values             *input{nullptr};
    std::vector<so_5::mbox_t> mboxes;
    std::vector<Tally>       *tallies{nullptr};
    Values                   *out{nullptr};
    qvo::Watch               *watch{nullptr};
};

// What every node shares: its place in the network, its slot, and its report to the sink once it
// has started (outside the window).
class node_t : public so_5::agent_t {
protected:
    const Net          &m_net;
    const Node         &m_node;
    Tally              &m_tally;
    const so_5::mbox_t  m_next;

    node_t(context_t ctx, const Net &net, std::uint32_t index)
        : so_5::agent_t{std::move(ctx)}
        , m_net{net}
        , m_node{net.topology->nodes[index]}
        , m_tally{(*net.tallies)[index]}
        , m_next{m_node.next == kNone ? so_5::mbox_t{} : net.mboxes[m_node.next]} {}

public:
    void so_evt_start() override { so_5::send<msg_ready>(m_net.mboxes[Topology::kSink]); }
};

class source_t final : public node_t {
public:
    source_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            m_tally.signal();
            for (const std::uint64_t v : *m_net.input) so_5::send<value_msg>(m_next, v);
            so_5::send<msg_exit>(m_next);
        });
    }
};

// A stage, a step, a partition: what it receives goes on to one agent, unchanged.
class forward_t final : public node_t {
public:
    forward_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](value_hood cmd) {
            m_tally.value(cmd->value);
            so_5::send(m_next, std::move(cmd));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            so_5::send<msg_exit>(m_next);
        });
    }
};

class splitter_t final : public node_t {
    std::vector<so_5::mbox_t> m_receivers;
    Splitter                  m_splitter;

public:
    splitter_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {
        for (std::uint32_t k = 0; k < m_node.lanes; ++k)
            m_receivers.push_back(net.mboxes[net.topology->receivers[m_node.first + k]]);
    }
    void so_define_agent() override {
        so_subscribe_self().event([this](value_hood cmd) {
            m_tally.value(cmd->value);
            so_5::send(m_receivers[m_splitter.next(m_node)], std::move(cmd));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            for (const auto &r : m_receivers) so_5::send<msg_exit>(r);
        });
    }
};

class compare_t final : public node_t {
    Comparator m_cmp;

public:
    compare_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](value_hood cmd) {
            m_tally.value(cmd->value);
            std::uint64_t a = 0, b = 0;
            if (m_cmp.accept(cmd->value, m_node.up, a, b)) {
                so_5::send<data_msg>(m_next, m_node.order, a);
                so_5::send<data_msg>(m_next, m_node.order, b);
            }
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            so_5::send<msg_exit>(m_next);
        });
    }
};

class joiner_t final : public node_t {
    Joiner m_joiner;

public:
    joiner_t(context_t ctx, const Net &net, std::uint32_t index)
        : node_t{std::move(ctx), net, index}, m_joiner{m_node} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](data_hood cmd) {
            m_tally.value(cmd->value);
            m_joiner.accept(cmd->order, cmd->value, [this](std::uint64_t v) { so_5::send<value_msg>(m_next, v); });
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            if (m_joiner.close()) so_5::send<msg_exit>(m_next);
        });
    }
};

class value_data_t final : public node_t {
public:
    value_data_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](value_hood cmd) {
            m_tally.value(cmd->value);
            so_5::send<data_msg>(m_next, m_node.order, cmd->value);
        });
        so_subscribe_self().event([this](data_hood cmd) {
            m_tally.value(cmd->value);
            so_5::send(m_next, std::move(cmd));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            so_5::send<msg_exit>(m_next);
        });
    }
};

class data_value_t final : public node_t {
public:
    data_value_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](value_hood cmd) {
            m_tally.value(cmd->value);
            so_5::send(m_next, std::move(cmd));
        });
        so_subscribe_self().event([this](data_hood cmd) {
            m_tally.value(cmd->value);
            so_5::send<value_msg>(m_next, cmd->value);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            so_5::send<msg_exit>(m_next);
        });
    }
};

// The sink: opens the window once every other agent has started, records the values, and closes
// the window on the first Exit.
class sink_t final : public node_t {
    std::size_t m_ready{0};
    bool        m_done{false};

public:
    sink_t(context_t ctx, const Net &net, std::uint32_t index) : node_t{std::move(ctx), net, index} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_net.topology->nodes.size() - 1) return;
            m_net.watch->start();
            so_5::send<msg_start>(m_net.mboxes[m_net.topology->source()]);
        });
        so_subscribe_self().event([this](value_hood cmd) {
            m_tally.value(cmd->value);
            m_net.out->push_back(cmd->value);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            m_tally.signal();
            if (m_done) return;
            m_done = true;
            m_net.watch->stop();
            so_environment().stop();
        });
    }
    void so_evt_start() override {}
};

so_5::agent_t *make(so_5::coop_t &coop, const Net &net, std::uint32_t i) {
    switch (net.topology->nodes[i].kind) {
    case Kind::kSource: return coop.make_agent<source_t>(std::cref(net), i);
    case Kind::kForward: return coop.make_agent<forward_t>(std::cref(net), i);
    case Kind::kSplitter: return coop.make_agent<splitter_t>(std::cref(net), i);
    case Kind::kCompare: return coop.make_agent<compare_t>(std::cref(net), i);
    case Kind::kJoiner: return coop.make_agent<joiner_t>(std::cref(net), i);
    case Kind::kValueData: return coop.make_agent<value_data_t>(std::cref(net), i);
    case Kind::kDataValue: return coop.make_agent<data_value_t>(std::cref(net), i);
    case Kind::kSink: return coop.make_agent<sink_t>(std::cref(net), i);
    }
    return nullptr;
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    const Reference   &ref = reference(p);
    const Topology    &t   = ref.net;
    const Values       in  = input(p);
    std::vector<Tally> tallies(t.nodes.size());
    Values             out;
    out.reserve(in.size() + 1);
    Net net;
    net.topology = &t;
    net.input    = &in;
    net.tallies  = &tallies;
    net.out      = &out;
    net.watch    = &watch;
    net.mboxes.reserve(t.nodes.size());

    so_5::launch([&](so_5::environment_t &env) {
        // Every coop is built and every mbox filled BEFORE the first is registered: an agent reads
        // the network from its first handler on. Agents are created in dependency order.
        std::vector<so_5::coop_unique_holder_t> coops;
        if (cores >= 2 && qvoso::group_coops(kGroupedByDefault)) {
            // The group binding: step s -- every agent the spec places with it -- is one coop on
            // fifo_t::cooperation, all on ONE pool.
            using so_5::disp::thread_pool::bind_params_t;
            using so_5::disp::thread_pool::fifo_t;
            const auto pool  = qvoso::make_pool_dispatcher(env, cores, spin);
            const auto group = pool.binder(bind_params_t{}.fifo(fifo_t::cooperation));
            std::map<std::uint32_t, std::size_t> coop_of_step;
            for (std::uint32_t i = 0; i < t.nodes.size(); ++i) {
                const std::uint32_t s  = t.nodes[i].step;
                auto                it = coop_of_step.find(s);
                if (it == coop_of_step.end()) {
                    it = coop_of_step.emplace(s, coops.size()).first;
                    coops.push_back(env.make_coop(group));
                }
                net.mboxes.push_back(make(*coops[it->second], net, i)->so_direct_mbox());
            }
        } else {
            // cores=1 (one_thread), or the individual binding.
            coops.push_back(env.make_coop(qvoso::make_pool_binder(env, cores, spin)));
            for (std::uint32_t i = 0; i < t.nodes.size(); ++i)
                net.mboxes.push_back(make(*coops.front(), net, i)->so_direct_mbox());
        }
        for (auto &coop : coops) env.register_coop(std::move(coop));
    });

    // launch() returned, every work thread joined: every tally is final, and reduced here, outside
    // the window.
    explain(tallies, out, p);
    return qvo::Answer{checksum(tallies, out), delivered(tallies)};
}

}  // namespace savina_bitonicsort_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bitonicsort::params();
    spec.expected          = qvospec::savina::bitonicsort::expected;
    spec.expected_messages = qvospec::savina::bitonicsort::expected_messages;
    spec.work_unit         = qvospec::savina::bitonicsort::kWorkUnit;
    spec.work_units        = qvospec::savina::bitonicsort::work_units;
    spec.idiom_source      = "the thread-ring adapter + dev/sample/so_5/mutable_msg_agents (a "
                             "mutable_msg redirected with so_5::send(next, std::move(cmd))) + the "
                             "logmap adapter's group binding";
    const bool grouped     = qvoso::group_coops(savina_bitonicsort_sobjectizer::kGroupedByDefault);
    spec.idiom_note        = grouped
                                 ? "mutable_msg values redirected in place; each step of the "
                                   "network one coop on fifo_t::cooperation, the thread_pool(cores) "
                                   "shared"
                                 : "mutable_msg values redirected in place; every agent on "
                                   "thread_pool(cores) with fifo_t::individual for cores>=2";
    // The grouped binding is not this adapter's default: its documents say so FIRST
    // (so_support.h, group_coops()). cores=1 is one_thread either way.
    if (grouped) {
        spec.caveats.push_back(qvoso::group_coops_caveat(
            "each step of the network with the agents the spec places with it, at cores >= 2; "
            "cores=1 is one_thread either way"));
        for (auto &c : qvoso::pool_caveats("fifo_t::cooperation for each step's coop (one demand "
                                           "queue per step, served by one work thread at a time)"))
            spec.caveats.push_back(std::move(c));
    } else {
        spec.caveats = qvoso::pool_caveats();
    }
    spec.caveats.emplace_back(
        "the network's 44 772 agents (at the defaults) are placed by the thread_pool: whichever "
        "work thread is free takes the next demand, so how many hops cross a core is the "
        "dispatcher's decision, where qb's cell cuts the steps into two contiguous halves -- see "
        "benchmarks/savina/bitonicsort.md");

    return qvo::run(argc, argv, std::move(spec), savina_bitonicsort_sobjectizer::body);
}
