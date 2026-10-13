// @benchmark     savina/uct
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the fib and fork-join-create adapters beside this file (agent_t subclasses on
//                their DIRECT mboxes, thread_pool via qvoso::make_pool_binder) and
//                dev/so_5/environment.hpp's `so_5::introduce_child_coop(*this, binder, lambda)` --
//                SObjectizer's own way for an agent to create agents at run time -- with
//                `environment_t::deregister_coop(handle, dereg_reason::normal)` to end a coop.
// @idiom-note    SObjectizer has no bare "spawn an agent": every agent belongs to a COOPERATION,
//                and a coop's agents end together. Savina's node creates its `binomial` children
//                at once and they end at once -- after their reports -- so a grant registers ONE
//                child coop holding the whole sibling group, and the parent deregisters that coop
//                once all of them have reported, before it reports itself (a node's own coop is
//                its parent's to end; fib's one-coop-per-child exists because its siblings end
//                apart). `so_define_agent()` runs during registration, so a send to a child's
//                direct mbox right after the call is delivered, not lost.

#include <qvospec/savina/uct.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_uct_sobjectizer {

using namespace qvospec::savina::uct;

struct Sink {
    Tally tally;
};

struct msg_try final : public so_5::signal_t {};
struct msg_request final : public so_5::message_t {  // Savina's ShouldGenerateChildrenMessage
    so_5::mbox_t  node;
    std::uint64_t id;
    std::uint32_t depth;
    msg_request(so_5::mbox_t n, std::uint64_t i, std::uint32_t d) : node(std::move(n)), id(i), depth(d) {}
};
struct msg_generate final : public so_5::message_t {
    std::uint32_t comp;
    explicit msg_generate(std::uint32_t c) noexcept : comp(c) {}
};
struct msg_urgent_generate final : public so_5::message_t {
    std::uint32_t urgent_child;
    std::uint32_t comp;
    msg_urgent_generate(std::uint32_t u, std::uint32_t c) noexcept : urgent_child(u), comp(c) {}
};
struct msg_update final : public so_5::message_t {
    std::uint32_t index;
    explicit msg_update(std::uint32_t i) noexcept : index(i) {}
};
struct msg_traverse final : public so_5::signal_t {};
struct msg_report final : public so_5::message_t {
    Tally tally;
    explicit msg_report(const Tally &t) noexcept : tally(t) {}
};

// What every node shares, read-only once the run starts.
struct Field {
    const Plan               *plan{nullptr};
    so_5::disp_binder_shptr_t binder;
    so_5::mbox_t              root;
};

class node_t final : public so_5::agent_t {
    const Field              &m_field;
    const so_5::mbox_t        m_parent;
    const std::uint64_t       m_id;
    const std::uint32_t       m_depth;
    const std::uint32_t       m_comp;
    const std::uint32_t       m_index;
    const bool                m_urgent;
    bool                      m_granted{false};
    bool                      m_traversed{false};
    std::uint32_t             m_reports{0};
    std::uint64_t             m_updated{0};
    Tally                     m_tally;
    std::vector<so_5::mbox_t> m_children;
    so_5::coop_handle_t       m_children_coop;

    void generate(std::uint32_t comp, bool urgent, std::uint32_t urgent_child) {
        if (m_granted) fail("a second grant at a node");
        m_granted = true;
        so_5::send<msg_update>(m_parent, m_index);
        const std::uint32_t B = m_field.plan->cfg.binomial;
        m_children.reserve(B);
        so_5::introduce_child_coop(*this, m_field.binder, [&](so_5::coop_t &coop) {
            for (std::uint32_t i = 0; i < B; ++i)
                m_children.push_back(coop.make_agent<node_t>(m_field, so_direct_mbox(),
                                                             child_id(m_id, i, B), m_depth + 1,
                                                             comp, i, urgent && i == urgent_child)
                                         ->so_direct_mbox());
            m_children_coop = coop.handle();
        });
        for (const auto &c : m_children) so_5::send<msg_try>(c);
    }

    void finish() {
        if (m_granted) so_environment().deregister_coop(m_children_coop, so_5::dereg_reason::normal);
        m_tally.own(m_id, m_depth, m_comp, m_urgent, m_granted);
        so_5::send<msg_report>(m_parent, m_tally);
    }

public:
    node_t(context_t ctx, const Field &field, so_5::mbox_t parent, std::uint64_t id,
           std::uint32_t depth, std::uint32_t comp, std::uint32_t index, bool urgent)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_parent{std::move(parent)}
        , m_id{id}
        , m_depth{depth}
        , m_comp{comp}
        , m_index{index}
        , m_urgent{urgent} {}

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_try>) {
                ++m_tally.messages;
                m_tally.work += try_work(m_id, m_field.plan->cfg.work);
                so_5::send<msg_request>(m_field.root, so_direct_mbox(), m_id, m_depth);
            })
            .event([this](so_5::mhood_t<msg_generate> m) {
                ++m_tally.messages;
                generate(m->comp, false, 0);
            })
            .event([this](so_5::mhood_t<msg_urgent_generate> m) {
                ++m_tally.messages;
                generate(m->comp, true, m->urgent_child);
            })
            .event([this](so_5::mhood_t<msg_update> m) {
                ++m_tally.messages;
                const std::uint64_t bit = std::uint64_t{1} << m->index;
                if (m_updated & bit) fail("an UpdateGrant repeated");
                m_updated |= bit;
                m_tally.update_w += child_id(m_id, m->index, m_field.plan->cfg.binomial);
            })
            .event([this](so_5::mhood_t<msg_traverse>) {
                ++m_tally.messages;
                if (m_traversed) fail("a node traversed twice");
                m_traversed = true;
                m_tally.work += traverse_work(m_id, m_comp, m_field.plan->cfg.work);
                if (m_children.empty()) return finish();
                for (const auto &c : m_children) so_5::send<msg_traverse>(c);
            })
            .event([this](so_5::mhood_t<msg_report> m) {
                ++m_tally.messages;
                m_tally.add(m->tally);
                if (++m_reports > m_children.size()) fail("a report beyond the node's children");
                if (m_reports == m_children.size()) finish();
            });
    }
};

class root_t final : public so_5::agent_t {
    Field                    &m_field;
    qvo::Watch               &m_watch;
    Sink                     &m_sink;
    std::uint64_t             m_requests{0};
    std::uint64_t             m_grants{0};
    bool                      m_complete{false};
    std::uint32_t             m_reports{0};
    std::uint64_t             m_updated{0};
    Tally                     m_tally;
    std::vector<so_5::mbox_t> m_children;
    so_5::coop_handle_t       m_children_coop;

public:
    root_t(context_t ctx, Field &field, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_watch{watch}, m_sink{sink} {}

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_request> m) {
                ++m_tally.messages;
                if (m_complete) fail("a request after the tree is complete");
                ++m_requests;
                const Grant g = decide(*m_field.plan, m->id, m->depth);
                if (g.granted) {
                    ++m_grants;
                    if (g.urgent) so_5::send<msg_urgent_generate>(m->node, g.urgent_child, g.comp);
                    else so_5::send<msg_generate>(m->node, g.comp);
                }
                const std::uint32_t B = m_field.plan->cfg.binomial;
                if (m_requests != B + B * m_grants) return;
                m_complete = true;  // every node there is has asked: the tree is complete
                for (const auto &c : m_children) so_5::send<msg_traverse>(c);
            })
            .event([this](so_5::mhood_t<msg_update> m) {
                ++m_tally.messages;
                const std::uint64_t bit = std::uint64_t{1} << m->index;
                if (m_updated & bit) fail("an UpdateGrant repeated at the root");
                m_updated |= bit;
                m_tally.update_w += child_id(0, m->index, m_field.plan->cfg.binomial);
            })
            .event([this](so_5::mhood_t<msg_report> m) {
                ++m_tally.messages;
                m_tally.add(m->tally);
                if (++m_reports > m_children.size()) fail("a report beyond the root's children");
                if (m_reports != m_children.size()) return;
                so_environment().deregister_coop(m_children_coop, so_5::dereg_reason::normal);
                m_watch.stop();
                m_sink.tally = m_tally;
                so_environment().stop();
            });
    }

    // Savina's generateTree, once the root is bound to its work thread: the window opens here.
    void so_evt_start() override {
        m_field.root = so_direct_mbox();
        m_watch.start();
        const std::uint32_t B = m_field.plan->cfg.binomial;
        so_5::introduce_child_coop(*this, m_field.binder, [&](so_5::coop_t &coop) {
            for (std::uint32_t i = 0; i < B; ++i)
                m_children.push_back(coop.make_agent<node_t>(m_field, so_direct_mbox(),
                                                             child_id(0, i, B), 1u,
                                                             m_field.plan->root_comp, i, false)
                                         ->so_direct_mbox());
            m_children_coop = coop.handle();
        });
        for (const auto &c : m_children) so_5::send<msg_try>(c);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Plan plan  = qvospec::savina::uct::plan(p, false);  // the root's decisions, before the window
    const auto cores = static_cast<int>(p.get("cores") < 1 ? 1 : p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink  sink;
    Field field;
    field.plan = &plan;
    so_5::launch([&](so_5::environment_t &env) {
        field.binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(field.binder, [&](so_5::coop_t &coop) {
            coop.make_agent<root_t>(std::ref(field), std::ref(watch), std::ref(sink));
        });
    });
    return qvo::Answer{checksum(sink.tally), sink.tally.messages};
}

}  // namespace savina_uct_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::uct::params();
    spec.expected          = qvospec::savina::uct::expected;
    spec.expected_messages = qvospec::savina::uct::expected_messages;
    spec.work_unit         = qvospec::savina::uct::kWorkUnit;
    spec.work_units        = qvospec::savina::uct::work_units;
    spec.idiom_source      = "the fib and fork-join-create adapters + so_5::introduce_child_coop "
                             "(environment.hpp) + environment_t::deregister_coop";
    spec.idiom_note        = "a grant registers one child coop holding the node's sibling group on "
                             "their direct mboxes; the parent deregisters it once all of them have "
                             "reported; thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer creates agents only inside a cooperation registered with the environment: "
        "every grant here registers one child coop of `binomial` agents and its parent deregisters "
        "it after their reports -- 5 999 coops per repetition at the defaults -- the framework's "
        "own dynamic-agent idiom for a sibling group that ends together");
    spec.caveats.emplace_back(
        "nodes bind to the same thread_pool as their parent and the pool places them, so with "
        "cores=2 the tree is balanced by the dispatcher as it grows; qb's cell keeps each of the "
        "root's subtrees on one core because qb has no cross-core spawn -- see "
        "benchmarks/savina/uct.md");
    spec.caveats.emplace_back(
        "SObjectizer's multi-threaded environment runs the FINAL deregistration of every coop on a "
        "dedicated thread it starts itself (coop_repo_t::start, dev/so_5/impl/"
        "mt_env_infrastructure.cpp), outside the `cores` work-thread budget and inside the pinned "
        "CPU set -- see benchmarks/savina/fork-join-create.md, \"The measured window\"");

    return qvo::run(argc, argv, std::move(spec), savina_uct_sobjectizer::body);
}
