// @benchmark     savina/quicksort
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the fib adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                `so_5::introduce_child_coop(*this, binder, lambda)` for an agent created at run
//                time, `so_deregister_agent_coop_normally()` for its own end, thread_pool via
//                qvoso::make_pool_binder) and dev/sample/so_5/mutable_msg_agents/main.cpp --
//                SObjectizer's OWN sample of a `mutable_msg` whose content the receiver may change
//                (dev/so_5/mhood.hpp: a mutable message goes to an MPSC mbox only, which a direct
//                mbox is).
// @idiom-note    A Sort and a Result are `mutable_msg`s carrying their values as a
//                `std::vector<uint64_t>`: the sender moves the vector into the message and the
//                receiver moves it out through `mutable_mhood_t`, so the elements are never copied
//                in transit -- the equivalent of qb's unique_ptr and CAF's unshared message. Every
//                child is one child coop of its parent (an agent ends by deregistering its own coop,
//                so two siblings cannot share one -- the fib adapter's measured lesson); the root
//                and its two seeds are top-level coops built before the window, all bound to one
//                thread_pool of `cores` pinned work threads with fifo_t::individual, which places
//                the tree.

#include <qvospec/savina/quicksort.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <utility>

namespace savina_quicksort_sobjectizer {

using namespace qvospec::savina::quicksort;

struct Sink {
    Values output;
    Counts counts;
};

// Both sent and received as so_5::mutable_msg<...>.
struct msg_sort final : public so_5::message_t {
    Values data;
    explicit msg_sort(Values d) noexcept : data(std::move(d)) {}
};
struct msg_result final : public so_5::message_t {
    Values   data;
    Counts   counts;
    Position position;
    msg_result(Values d, Counts c, Position p) noexcept : data(std::move(d)), counts(c), position(p) {}
};
struct msg_ready final : public so_5::signal_t {};

class sort_t final : public so_5::agent_t {
    const so_5::mbox_t              m_parent;
    const Position                  m_position;
    const so_5::disp_binder_shptr_t m_binder;
    const std::size_t               m_threshold;
    const so_5::mbox_t              m_driver;  // the root and the seeds announce themselves
    so_5::mbox_t                    m_seeds[2];
    Values                          m_held;
    Counts                          m_counts;
    std::uint32_t                   m_fragments{0};

    void finish(Values data) {
        so_5::send<so_5::mutable_msg<msg_result>>(m_parent, std::move(data), m_counts, m_position);
        so_deregister_agent_coop_normally();
    }

    // One coop per child: an agent ends by deregistering ITS coop, and a coop shared by two
    // siblings would end the other one with the first Result.
    so_5::mbox_t child(Position position) {
        so_5::mbox_t mbox;
        so_5::introduce_child_coop(*this, m_binder, [&](so_5::coop_t &coop) {
            mbox = coop.make_agent<sort_t>(so_direct_mbox(), position, m_binder, m_threshold,
                                           so_5::mbox_t{})
                       ->so_direct_mbox();
        });
        return mbox;
    }

public:
    sort_t(context_t ctx, so_5::mbox_t parent, Position position, so_5::disp_binder_shptr_t binder,
           std::size_t threshold, so_5::mbox_t driver)
        : so_5::agent_t{std::move(ctx)}
        , m_parent{std::move(parent)}
        , m_position{position}
        , m_binder{std::move(binder)}
        , m_threshold{threshold}
        , m_driver{std::move(driver)} {}

    // The root's seeds, set before any coop is registered.
    void seeds(so_5::mbox_t left, so_5::mbox_t right) {
        m_seeds[0] = std::move(left);
        m_seeds[1] = std::move(right);
    }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_sort> cmd) {
            ++m_counts.sorts;
            Values &data = cmd->data;
            if (data.size() < m_threshold) {
                sort_leaf(data);
                finish(std::move(data));
                return;
            }
            Split              s     = split(data);
            const bool         root  = m_position == Position::kInitial;
            const so_5::mbox_t left  = root ? m_seeds[0] : child(Position::kLeft);
            const so_5::mbox_t right = root ? m_seeds[1] : child(Position::kRight);
            so_5::send<so_5::mutable_msg<msg_sort>>(left, std::move(s.less));
            so_5::send<so_5::mutable_msg<msg_sort>>(right, std::move(s.greater));
            m_held      = std::move(s.equal);
            m_fragments = 1;
        });
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_result> cmd) {
            m_counts.add_result(cmd->counts);
            join(m_held, cmd->data, cmd->position);
            if (++m_fragments == 3) finish(std::move(m_held));
        });
    }

    void so_evt_start() override {
        if (m_driver) so_5::send<msg_ready>(m_driver);
    }
};

// The driver: hands the input to the root once the root and the seeds are up, and closes the
// window on the root's Result.
class driver_t final : public so_5::agent_t {
    Values      &m_input;
    qvo::Watch  &m_watch;
    Sink        &m_sink;
    so_5::mbox_t m_root;
    std::size_t  m_ready{0};

public:
    driver_t(context_t ctx, Values &input, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_input{input}, m_watch{watch}, m_sink{sink} {}

    void root(so_5::mbox_t mbox) { m_root = std::move(mbox); }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != 3) return;
            m_watch.start();
            so_5::send<so_5::mutable_msg<msg_sort>>(m_root, std::move(m_input));
        });
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_result> cmd) {
            m_watch.stop();
            m_sink.counts.add_result(cmd->counts);
            m_sink.output = std::move(cmd->data);
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores     = static_cast<int>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;
    const auto threshold = static_cast<std::size_t>(p.get("threshold"));

    Sink   sink;
    Values values = input(p);
    so_5::launch([&](so_5::environment_t &env) {
        const auto binder = qvoso::make_pool_binder(env, cores, spin);
        // Four top-level coops -- the driver, the root, each seed: an agent ends by deregistering
        // its own -- all built and wired BEFORE the first is registered.
        auto  driver_coop = env.make_coop(binder);
        auto *driver      = driver_coop->make_agent<driver_t>(std::ref(values), std::ref(watch),
                                                         std::ref(sink));
        auto  root_coop   = env.make_coop(binder);
        auto *root = root_coop->make_agent<sort_t>(driver->so_direct_mbox(), Position::kInitial,
                                                   binder, threshold, driver->so_direct_mbox());
        so_5::mbox_t                          seeds[2];
        std::vector<so_5::coop_unique_holder_t> seed_coops;
        for (std::size_t s = 0; s < 2; ++s) {
            auto coop = env.make_coop(binder);
            seeds[s]  = coop->make_agent<sort_t>(root->so_direct_mbox(),
                                                s == 0 ? Position::kLeft : Position::kRight, binder,
                                                threshold, driver->so_direct_mbox())
                           ->so_direct_mbox();
            seed_coops.push_back(std::move(coop));
        }
        root->seeds(seeds[0], seeds[1]);
        driver->root(root->so_direct_mbox());
        env.register_coop(std::move(driver_coop));
        env.register_coop(std::move(root_coop));
        for (auto &coop : seed_coops) env.register_coop(std::move(coop));
    });

    // launch() returned, every work thread joined; reduced here, outside the window.
    const Output o = measure(sink.output);
    explain(o, sink.counts, p);
    return qvo::Answer{answer(o, sink.counts), sink.counts.sorts + sink.counts.results};
}

}  // namespace savina_quicksort_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::quicksort::params();
    spec.expected          = qvospec::savina::quicksort::expected;
    spec.expected_messages = qvospec::savina::quicksort::expected_messages;
    spec.work_unit         = qvospec::savina::quicksort::kWorkUnit;
    spec.work_units        = qvospec::savina::quicksort::work_units;
    spec.idiom_source      = "the fib adapter (introduce_child_coop + so_deregister_agent_coop_"
                             "normally) + dev/sample/so_5/mutable_msg_agents (a mutable_msg the "
                             "receiver may change)";
    spec.idiom_note        = "mutable_msg<msg_sort> / mutable_msg<msg_result> carrying a moved "
                             "vector; one child coop per child; thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer creates agents only inside a cooperation registered with the environment, so "
        "every split here pays two coop registrations and, as each child ends, a deregistration "
        "on top of the agents themselves -- the framework's own dynamic-agent idiom (savina/fib "
        "measured why two siblings cannot share a coop)");
    spec.caveats.emplace_back(
        "children bind to the same thread_pool as their parent and the pool places them, so with "
        "cores=2 the tree is balanced by the dispatcher; qb's cell keeps each half of the first "
        "split on its seed's core because qb has no cross-core spawn -- see "
        "benchmarks/savina/quicksort.md");
    spec.caveats.emplace_back(
        "SObjectizer's multi-threaded environment runs the FINAL deregistration of every coop on a "
        "dedicated thread it starts itself (coop_repo_t::start, dev/so_5/impl/"
        "mt_env_infrastructure.cpp), OUTSIDE the `cores` work-thread budget and inside the pinned "
        "CPU set; one coop per actor here -- see benchmarks/savina/fib.md, \"The measured window\"");

    return qvo::run(argc, argv, std::move(spec), savina_quicksort_sobjectizer::body);
}
