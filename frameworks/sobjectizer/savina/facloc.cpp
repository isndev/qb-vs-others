// @benchmark     savina/facloc
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the uct and fork-join-create adapters beside this file (agent_t subclasses on
//                their DIRECT mboxes, thread_pool via qvoso::make_pool_binder,
//                `so_5::introduce_child_coop(*this, binder, lambda)` for agents created at run
//                time, `environment_t::deregister_coop(handle, dereg_reason::normal)` to end them).
// @idiom-note    Every agent belongs to a COOPERATION and a coop's agents end together: a split
//                creates the four children at once and they end at once -- after their
//                ConfirmExits -- so a split registers ONE child coop holding the four, and the
//                parent deregisters it once all four have confirmed, before it confirms itself.
//                The producer and the root share the first coop. `so_define_agent()` runs during
//                registration, so a send to a child's direct mbox right after the call is
//                delivered, not lost.

#include <qvospec/savina/facloc.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <array>
#include <optional>
#include <vector>

namespace savina_facloc_sobjectizer {

using namespace qvospec::savina::facloc;

struct Sink {
    Tally         tally;
    std::uint64_t producer_messages{0};
};

struct msg_customer final : public so_5::message_t {
    Customer c;
    explicit msg_customer(const Customer &cu) noexcept : c(cu) {}
};
struct msg_next final : public so_5::signal_t {};
struct msg_facility final : public so_5::message_t {  // Savina's FacilityMsg from a child
    std::uint64_t fid;
    std::uint32_t depth;
    msg_facility(std::uint64_t f, std::uint32_t d) noexcept : fid(f), depth(d) {}
};
struct msg_exit final : public so_5::signal_t {};
struct msg_confirm final : public so_5::message_t {
    Tally tally;
    explicit msg_confirm(const Tally &t) noexcept : tally(t) {}
};

// Shared by every agent, read-only once the run starts.
struct Field {
    Config                    cfg{};
    so_5::mbox_t              producer;
    so_5::mbox_t              root;
    qvo::Watch               *watch{nullptr};
    Sink                     *sink{nullptr};
};

class quadrant_t final : public so_5::agent_t {
    const Field                &m_field;
    const so_5::disp_binder_shptr_t m_binder;
    const so_5::mbox_t          m_parent;  // null for the root
    Quadrant                    m_q;
    std::array<so_5::mbox_t, 4> m_children;
    so_5::coop_handle_t         m_children_coop;
    std::uint32_t               m_confirms{0};
    bool                        m_exiting{false};
    Tally                       m_tally;

    void split() {
        std::array<Seed, 4> seeds = m_q.split();
        if (m_parent) so_5::send<msg_facility>(m_parent, m_q.pid(), m_q.depth());
        so_5::introduce_child_coop(*this, m_binder, [&](so_5::coop_t &coop) {
            for (unsigned k = 0; k < 4; ++k)
                m_children[k] =
                    coop.make_agent<quadrant_t>(m_field, m_binder, so_direct_mbox(), std::move(seeds[k]))
                        ->so_direct_mbox();
            m_children_coop = coop.handle();
        });
    }

    // A quadrant's end: its ConfirmExit to its parent -- or, at the root, the end of the run.
    void confirm() {
        if (m_q.open()) so_environment().deregister_coop(m_children_coop, so_5::dereg_reason::normal);
        m_tally.own(m_q, !m_parent);
        if (m_parent) {
            so_5::send<msg_confirm>(m_parent, m_tally);
            return;
        }
        m_field.watch->stop();
        m_field.sink->tally = m_tally;
        so_environment().stop();
    }

public:
    quadrant_t(context_t ctx, const Field &field, so_5::disp_binder_shptr_t binder,
               so_5::mbox_t parent, Seed &&seed)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_binder{std::move(binder)}
        , m_parent{std::move(parent)}
        , m_q{std::move(seed), field.cfg.threshold} {}

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_customer> m) {
                ++m_tally.messages;
                if (m_exiting) fail("a customer after RequestExit");
                if (m_q.open()) so_5::send<msg_customer>(m_children[m_q.route(m->c)], m->c);
                else if (m_q.add(m->c)) split();
                if (!m_parent) so_5::send<msg_next>(m_field.producer);  // Savina: the next one
            })
            // Savina's notifyParentOfFacility, on up to the root.
            .event([this](so_5::mhood_t<msg_facility> m) {
                ++m_tally.messages;
                m_tally.notif_w += m->fid;
                if (m_parent) so_5::send<msg_facility>(m_parent, m->fid, m->depth);
            })
            .event([this](so_5::mhood_t<msg_exit>) {
                ++m_tally.messages;
                if (m_exiting) fail("a second RequestExit");
                m_exiting = true;
                if (!m_q.open()) return confirm();
                for (const auto &c : m_children) so_5::send<msg_exit>(c);
            })
            .event([this](so_5::mhood_t<msg_confirm> m) {
                ++m_tally.messages;
                m_tally.add(m->tally);
                if (++m_confirms > 4) fail("a fifth ConfirmExit");
                if (m_confirms == 4) confirm();
            });
    }
};

class producer_t final : public so_5::agent_t {
    const Field                 &m_field;
    const std::vector<Customer> &m_customers;
    std::size_t                  m_produced{0};

    void produce() { so_5::send<msg_customer>(m_field.root, m_customers[m_produced++]); }

public:
    producer_t(context_t ctx, const Field &field, const std::vector<Customer> &customers)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_customers{customers} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_next>) {
            ++m_field.sink->producer_messages;
            if (m_produced < m_customers.size()) return produce();
            so_5::send<msg_exit>(m_field.root);
        });
    }

    // The window opens once the producer is bound to its work thread, the root registered with it.
    void so_evt_start() override {
        m_field.watch->start();
        produce();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores = static_cast<int>(p.get("cores") < 1 ? 1 : p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink  sink;
    Field field;
    field.cfg                       = config(p);
    field.watch                     = &watch;
    field.sink                      = &sink;
    const std::vector<Customer> all = customers(field.cfg);  // drawn before the window
    so_5::launch([&](so_5::environment_t &env) {
        // Only agents/coops retain the binder: their teardown precedes the environment's.
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            field.producer = coop.make_agent<producer_t>(std::cref(field), std::cref(all))->so_direct_mbox();
            field.root     = coop.make_agent<quadrant_t>(std::cref(field), binder, so_5::mbox_t{},
                                                         root_seed(field.cfg))
                             ->so_direct_mbox();
        });
    });
    return qvo::Answer{checksum(sink.tally), sink.tally.messages + sink.producer_messages};
}

}  // namespace savina_facloc_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::facloc::params();
    spec.expected          = qvospec::savina::facloc::expected;
    spec.expected_messages = qvospec::savina::facloc::expected_messages;
    spec.work_unit         = qvospec::savina::facloc::kWorkUnit;
    spec.work_units        = qvospec::savina::facloc::work_units;
    spec.idiom_source      = "the uct and fork-join-create adapters + so_5::introduce_child_coop "
                             "(environment.hpp) + environment_t::deregister_coop";
    spec.idiom_note        = "a split registers one child coop holding the four children on their "
                             "direct mboxes; the parent deregisters it once all four have "
                             "confirmed; thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer creates agents only inside a cooperation registered with the environment: "
        "every split here registers one child coop of four agents and its parent deregisters it "
        "after their ConfirmExits -- 341 coops per repetition at the defaults");
    spec.caveats.emplace_back(
        "quadrants bind to the same thread_pool as their parent and the pool places them, so with "
        "cores=2 the quadtree is spread over the work threads dynamically; qb's cell keeps each of "
        "the root's subtrees on one core because qb has no cross-core spawn -- see "
        "benchmarks/savina/facloc.md");

    return qvo::run(argc, argv, std::move(spec), savina_facloc_sobjectizer::body);
}
