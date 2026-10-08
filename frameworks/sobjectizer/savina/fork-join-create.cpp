// @benchmark     savina/fork-join-create
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the fib adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder) and dev/so_5/environment.hpp's
//                `so_5::introduce_child_coop(*this, binder, lambda)` -- SObjectizer's own way for
//                an agent to create agents at run time -- with
//                `so_deregister_agent_coop_normally()` for the agent's own end.
// @idiom-note    SObjectizer has no bare "spawn an agent": every agent belongs to a COOPERATION.
//                A forked actor ends itself after its one message, and an agent ends by
//                deregistering its coop, so each forked actor is its own child coop of its
//                creator -- the shape fib measured. One coop holding a creator's whole share
//                would register the share in one transaction, but no agent of it could end
//                after its message without ending all the others, so it is not this benchmark's
//                actor (Savina's actor calls exit() after its one message) and is not used.
//                `so_define_agent()` runs during registration, so a send to the child's direct
//                mbox right after the call is delivered, not lost.

#include <qvospec/savina/fork-join-create.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_fork_join_create_sobjectizer {

using namespace qvospec::savina::fork_join_create;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_ready final : public so_5::signal_t {};
struct msg_fork final : public so_5::signal_t {};
struct msg_job final : public so_5::message_t {
    std::uint64_t index;
    explicit msg_job(std::uint64_t i) noexcept : index(i) {}
};
struct msg_done final : public so_5::message_t {
    std::uint64_t value;
    std::uint64_t messages;
    msg_done(std::uint64_t v, std::uint64_t m) noexcept : value(v), messages(m) {}
};
struct msg_summary final : public so_5::message_t {
    std::uint64_t chk;
    std::uint64_t messages;
    msg_summary(std::uint64_t c, std::uint64_t m) noexcept : chk(c), messages(m) {}
};

// One forked actor: answers its creator once and deregisters its own coop.
class fork_t final : public so_5::agent_t {
    const so_5::mbox_t  m_creator;
    const std::uint64_t m_self;
    const int           m_work;

public:
    fork_t(context_t ctx, so_5::mbox_t creator, std::uint64_t self, int work)
        : so_5::agent_t{std::move(ctx)}, m_creator{std::move(creator)}, m_self{self}, m_work{work} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_job> m) {
            so_5::send<msg_done>(m_creator, job_value(m_self, m->index, m_work), std::uint64_t{1});
            so_deregister_agent_coop_normally();
        });
    }
};

// One creator per core: forks the indices i with i % creators == s, folds their answers, reports
// one summary to the driver, and ends.
class creator_t final : public so_5::agent_t {
    const so_5::mbox_t              m_driver;
    const so_5::disp_binder_shptr_t m_binder;
    const std::uint64_t             m_first;
    const std::uint64_t             m_stride;
    const std::uint64_t             m_actors;
    const std::uint64_t             m_share;
    const int                       m_work;
    std::uint64_t                   m_acc{0};
    std::uint64_t                   m_messages{0};
    std::uint64_t                   m_done{0};

    void summarise() {
        so_5::send<msg_summary>(m_driver, m_acc, m_messages);
        so_deregister_agent_coop_normally();
    }

public:
    creator_t(context_t ctx, so_5::mbox_t driver, so_5::disp_binder_shptr_t binder,
              std::uint64_t first, std::uint64_t stride, std::uint64_t actors, int work)
        : so_5::agent_t{std::move(ctx)}
        , m_driver{std::move(driver)}
        , m_binder{std::move(binder)}
        , m_first{first}
        , m_stride{stride}
        , m_actors{actors}
        , m_share{share(actors, stride, first)}
        , m_work{work} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_fork>) {
            ++m_messages;
            for (std::uint64_t i = m_first; i < m_actors; i += m_stride) {
                so_5::mbox_t child;
                so_5::introduce_child_coop(*this, m_binder, [&](so_5::coop_t &coop) {
                    child = coop.make_agent<fork_t>(so_direct_mbox(), i, m_work)->so_direct_mbox();
                });
                so_5::send<msg_job>(child, i);
            }
            if (m_share == 0) summarise();  // more creators than actors: nothing to join
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done> m) {
            m_acc += m->value;
            m_messages += 1 + m->messages;
            if (++m_done == m_share) summarise();
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_driver); }
};

// The driver: opens the window once every creator is up, closes it on the last summary.
class driver_t final : public so_5::agent_t {
    qvo::Watch               &m_watch;
    Sink                     &m_sink;
    std::vector<so_5::mbox_t> m_creators;
    std::size_t               m_ready{0};
    std::size_t               m_done{0};

public:
    // The creators' slots are sized up front and each is written once, before its creator's coop
    // is registered: a creator's ready signal can only follow its own slot's write, and the
    // driver reads the slots only after the LAST ready, so no slot is read before it is set.
    driver_t(context_t ctx, std::uint64_t ncreat, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_watch{watch}
        , m_sink{sink}
        , m_creators(static_cast<std::size_t>(ncreat)) {}

    void set_creator(std::uint64_t s, so_5::mbox_t mbox) {
        m_creators[static_cast<std::size_t>(s)] = std::move(mbox);
    }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_creators.size()) return;
            m_watch.start();
            for (const auto &c : m_creators) so_5::send<msg_fork>(c);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_summary> m) {
            m_sink.checksum += m->chk;
            m_sink.messages += 1 + m->messages;
            if (++m_done != m_creators.size()) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint64_t>(p.get("actors"));
    const auto work   = static_cast<int>(p.get("work"));
    const auto ncreat = creators(p);
    const bool spin   = p.get("wait") != 0;

    Sink sink;
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, static_cast<int>(ncreat), spin);
        // The driver and each creator are separate top-level coops: a creator's own
        // deregistration must not take the driver with it.
        driver_t *driver = nullptr;
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            driver = coop.make_agent<driver_t>(ncreat, std::ref(watch), std::ref(sink));
        });
        for (std::uint64_t s = 0; s < ncreat; ++s)
            env.introduce_coop(binder, [&](so_5::coop_t &coop) {
                driver->set_creator(s, coop.make_agent<creator_t>(driver->so_direct_mbox(), binder,
                                                                  s, ncreat, actors, work)
                                           ->so_direct_mbox());
            });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fork_join_create_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join_create::params();
    spec.expected          = qvospec::savina::fork_join_create::expected;
    spec.expected_messages = qvospec::savina::fork_join_create::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join_create::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join_create::work_units;
    spec.idiom_source      = "the fib adapter + so_5::introduce_child_coop (environment.hpp) + "
                             "so_deregister_agent_coop_normally";
    spec.idiom_note        = "creator s forks its share as one child coop per forked agent, on "
                             "their direct mboxes; a forked agent sends one msg_done and "
                             "deregisters its own coop; thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer creates agents only inside a cooperation registered with the environment, "
        "so every forked actor here pays one coop registration and one deregistration on top of "
        "the agent itself -- that is the framework's own dynamic-agent idiom for an agent that "
        "ends after one message, not an adapter choice (a coop holding a creator's whole share "
        "cannot end one agent without ending all of them)");
    spec.caveats.emplace_back(
        "forked agents bind to the same thread_pool as their creator and the pool places them, "
        "so with cores=2 the forked actors are balanced by the dispatcher; qb's cell keeps each "
        "creator's actors on its core because qb has no cross-core spawn -- see "
        "benchmarks/savina/fork-join-create.md");
    spec.caveats.emplace_back(
        "SObjectizer's multi-threaded environment runs the FINAL deregistration of every coop "
        "(unbinding the agent from the dispatcher, releasing the coop and its agent) on a "
        "dedicated thread it starts itself (coop_repo_t::start, dev/so_5/impl/"
        "mt_env_infrastructure.cpp), handed each finished coop by the work threads under a shared "
        "mutex: 40 000 coops per repetition here. That thread is OUTSIDE the `cores` work-thread "
        "budget, runs inside the pinned CPU set competing with the work threads, and the coops "
        "still in its chain when the window closes are released after it -- see "
        "benchmarks/savina/fork-join-create.md, \"The measured window\"");

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_create_sobjectizer::body);
}
