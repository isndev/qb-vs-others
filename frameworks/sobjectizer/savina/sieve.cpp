// @benchmark     savina/sieve
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the fork-join-create adapter beside this file (an agent created at run time inside
//                a cooperation registered with the environment, on qvoso::make_pool_binder's pool)
//                and dev/so_5/send_functions.hpp's redirection of an existing message,
//                `so_5::send(mbox, mhood_t<M>)` (5.5.19).
// @idiom-note    The reference's actors one for one. A filter that needs a successor introduces a
//                new top-level coop holding it -- the reference's `context.system.actorOf`, an
//                actor of the system, not a child -- on the same pool, and reads its direct mbox
//                at once (`so_define_agent()` runs during registration, so a send right after is
//                delivered, not lost). A candidate passed on is the msg_candidate the filter
//                received, redirected, as the reference re-sends its LongBox. Messages from one
//                sender to one mbox keep their order, which the sieve needs. The pool places every
//                filter, so the spec's `place` has no counterpart: place=1 is not applicable.

#include <qvospec/savina/sieve.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_sieve_sobjectizer {

using namespace qvospec::savina::sieve;

struct msg_candidate final : public so_5::message_t {
    std::uint64_t number;
    explicit msg_candidate(std::uint64_t n) noexcept : number(n) {}
};
struct msg_exit final : public so_5::signal_t {};
struct msg_ready final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};

// The run's shared, read-only wiring: the pool's binder, the slots and the primes' vectors the body
// owns, one per filter, each written only by its filter (a line of its own).
struct Field {
    so_5::disp_binder_shptr_t                binder;
    std::vector<Tally>                      *tallies{nullptr};
    std::vector<std::vector<std::uint64_t>> *primes{nullptr};
    std::uint64_t                            local{1};
    qvo::Watch                              *watch{nullptr};
    so_5::mbox_t                             producer;
};

class filter_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    Filter              m_filter;
    so_5::mbox_t        m_next;

    void grow(std::uint64_t prime) {
        if (m_index + 1 >= m_field.tallies->size()) too_many_filters();
        so_environment().introduce_coop(m_field.binder, [&](so_5::coop_t &coop) {
            m_next = coop.make_agent<filter_t>(std::cref(m_field), m_index + 1, prime)->so_direct_mbox();
        });
    }

public:
    filter_t(context_t ctx, const Field &field, std::uint32_t index, std::uint64_t prime)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_index{index}
        , m_filter(prime, field.local, (*field.tallies)[index], (*field.primes)[index]) {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_candidate> m) {
            switch (m_filter.take(m->number)) {
            case Filter::Verdict::pass: so_5::send(m_next, m); break;
            case Filter::Verdict::grow: grow(m->number); break;
            default: break;  // dropped, or kept here
            }
        });
        // The Exit follows the last candidate down the chain; the last filter's ends the run.
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++(*m_field.tallies)[m_index].exits;
            if (m_filter.linked()) {
                so_5::send<msg_exit>(m_next);
                return;
            }
            m_field.watch->stop();
            so_environment().stop();
        });
    }

    void so_evt_start() override {
        if (m_index == 0) so_5::send<msg_ready>(m_field.producer);  // the first filter, before start
    }
};

class producer_t final : public so_5::agent_t {
    const std::uint64_t m_limit;
    qvo::Watch         &m_watch;
    std::uint64_t      &m_starts;
    so_5::mbox_t        m_first;

public:
    producer_t(context_t ctx, std::uint64_t limit, qvo::Watch &watch, std::uint64_t &starts)
        : so_5::agent_t{std::move(ctx)}, m_limit{limit}, m_watch{watch}, m_starts{starts} {}

    void set_first(so_5::mbox_t first) { m_first = std::move(first); }

    void so_define_agent() override {
        // The first filter has started: the window opens.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            m_watch.start();
            so_5::send<msg_start>(*this);
        });
        // Savina's NumberProducerActor: every candidate from this one handler, then the Exit.
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            ++m_starts;
            for (std::uint64_t n = 3; n < m_limit; n += 2) so_5::send<msg_candidate>(m_first, n);
            so_5::send<msg_exit>(m_first);
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto limit = static_cast<std::uint64_t>(p.get("limit"));
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;
    if (p.get("place") != 0)
        qvo::not_applicable("SObjectizer's thread_pool places every agent itself; the spec's static "
                            "filter placement (place=1) has no counterpart -- read the place=0 cell");

    const auto                              nfilt = static_cast<std::size_t>(filters(p));
    std::vector<Tally>                      tallies(nfilt);
    std::vector<std::vector<std::uint64_t>> primes(nfilt);
    std::uint64_t                           starts = 0;
    Field                                   field;
    field.tallies = &tallies;
    field.primes  = &primes;
    field.local   = static_cast<std::uint64_t>(p.get("local"));
    field.watch   = &watch;

    so_5::launch([&](so_5::environment_t &env) {
        field.binder = qvoso::make_pool_binder(env, cores, spin);
        // The producer and the first filter are separate top-level coops, like every filter the
        // run creates.
        producer_t *producer = nullptr;
        env.introduce_coop(field.binder, [&](so_5::coop_t &coop) {
            producer       = coop.make_agent<producer_t>(limit, std::ref(watch), std::ref(starts));
            field.producer = producer->so_direct_mbox();
        });
        env.introduce_coop(field.binder, [&](so_5::coop_t &coop) {
            producer->set_first(coop.make_agent<filter_t>(std::cref(field), std::uint32_t{0},
                                                          std::uint64_t{2})
                                    ->so_direct_mbox());
        });
    });

    // launch() returned, every work thread joined: every tally is final, and the tallies and the
    // primes are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, primes), delivered(tallies, starts)};
}

}  // namespace savina_sieve_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sieve::params();
    spec.expected          = qvospec::savina::sieve::expected;
    spec.expected_messages = qvospec::savina::sieve::expected_messages;
    spec.work_unit         = qvospec::savina::sieve::kWorkUnit;
    spec.work_units        = qvospec::savina::sieve::work_units;
    spec.idiom_source      = "the fork-join-create adapter + so_5::send(mbox, mhood_t) redirection "
                             "(dev/so_5/send_functions.hpp)";
    spec.idiom_note        = "a filter needing a successor introduces a top-level coop holding it on "
                             "the same thread_pool; a passed candidate is the received message, "
                             "redirected; thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "every filter is its own coop on the thread_pool, which places it: the spec's `place` axis "
        "(where qb and the floor put the filters) has no counterpart, and place=1 is reported not "
        "applicable -- see benchmarks/savina/sieve.md");
    spec.caveats.emplace_back(
        "every filter created inside the window pays one coop registration on top of the agent -- "
        "SObjectizer's only way to add an agent at run time; nine at the defaults, one per prime "
        "but the first with local=1");

    return qvo::run(argc, argv, std::move(spec), savina_sieve_sobjectizer::body);
}
