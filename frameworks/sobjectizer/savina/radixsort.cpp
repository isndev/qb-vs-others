// @benchmark     savina/radixsort
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the thread-ring adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, one coop on qvoso::make_pool_binder) and dev/so_5/send_functions.hpp's
//                redirection of an existing message -- `so_5::send(mbox, mhood_t<M>)` and
//                `so_5::send(mbox, message_holder_t<M>)`, "a version of send for redirection of a
//                message from existing message hood" (5.5.19, 5.6.0).
// @idiom-note    The reference's actors one for one, and its message object: Savina's SortActor
//                re-sends the ValueMessage it received (`nextActor ! vm`) and stores the held ones
//                as the objects themselves. SObjectizer's immutable messages can be re-sent the
//                same way, so a stage redirects the msg_value it received and holds a
//                message_holder_t for each held one -- no message is allocated after the source's.
//                Messages from one sender to one mbox keep their order, which the sort needs.

#include <qvospec/savina/radixsort.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_radixsort_sobjectizer {

using namespace qvospec::savina::radixsort;

struct Sink {
    std::uint64_t starts{0};  // the Starts the source's handler ran
};

struct msg_value final : public so_5::message_t {
    std::uint64_t value;
    explicit msg_value(std::uint64_t v) noexcept : value(v) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};

class source_t final : public so_5::agent_t {
    const std::uint64_t m_values;
    const long long     m_seed;
    const long long     m_bits;
    const std::size_t   m_actors;  // the stages and the validation that report in
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    so_5::mbox_t        m_first;
    std::size_t         m_ready{0};

public:
    source_t(context_t ctx, std::uint64_t values, long long seed, long long bits,
             std::size_t actors, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_values{values}
        , m_seed{seed}
        , m_bits{bits}
        , m_actors{actors}
        , m_watch{watch}
        , m_sink{sink} {}

    void set_first(so_5::mbox_t first) { m_first = std::move(first); }

    void so_define_agent() override {
        // The window opens once every stage and the validation have started: every work thread
        // running.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_actors) return;
            m_watch.start();
            so_5::send<msg_start>(*this);
        });
        // Savina's IntSourceActor: every value from this one handler.
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            ++m_sink.starts;
            Source source(m_seed, m_bits);
            for (std::uint64_t i = 0; i < m_values; ++i) so_5::send<msg_value>(m_first, source.next());
        });
    }
};

// A stage: its Counts are its tally slot's (a line of its own).
class stage_t final : public so_5::agent_t {
    Stage                                          m_stage;
    so_5::mbox_t                                   m_next;
    so_5::mbox_t                                   m_source;
    std::vector<so_5::message_holder_t<msg_value>> m_held;

public:
    stage_t(context_t ctx, std::uint32_t index, std::uint64_t values, Tally &tally)
        : so_5::agent_t{std::move(ctx)}, m_stage(index, values, tally) {
        m_held.reserve(static_cast<std::size_t>(values));
    }

    void wire(so_5::mbox_t next, so_5::mbox_t source) {
        m_next   = std::move(next);
        m_source = std::move(source);
    }

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            if (m_stage.take(m->value)) so_5::send(m_next, m);
            else m_held.push_back(m.make_holder());
            if (m_stage.complete())
                for (auto &h : m_held) so_5::send(m_next, std::move(h));
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_source); }
};

class validation_t final : public so_5::agent_t {
    Validation  &m_validation;  // a line of its own the body owns
    qvo::Watch  &m_watch;
    so_5::mbox_t m_source;

public:
    validation_t(context_t ctx, Validation &validation, qvo::Watch &watch)
        : so_5::agent_t{std::move(ctx)}, m_validation{validation}, m_watch{watch} {}

    void set_source(so_5::mbox_t source) { m_source = std::move(source); }

    void so_define_agent() override {
        // The last value ends the run; one after it fails it (radixsort::overflow).
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            if (!m_validation.take(m->value)) return;
            m_watch.stop();
            so_environment().stop();
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_source); }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto values = static_cast<std::uint64_t>(p.get("values"));
    const auto bits   = static_cast<std::uint32_t>(p.get("bits"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink                       sink;
    std::vector<Tally>         tallies(bits);
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(values));
    ValidationSlot             validator{Validation(sequence.data(), values)};

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            auto *source = coop.make_agent<source_t>(values, p.get("seed"), p.get("bits"),
                                                     std::size_t{bits} + 1, std::ref(watch),
                                                     std::ref(sink));
            auto *last   = coop.make_agent<validation_t>(std::ref(validator.validation),
                                                         std::ref(watch));
            last->set_source(source->so_direct_mbox());
            std::vector<stage_t *> stages(bits);
            for (std::uint32_t k = 0; k < bits; ++k)
                stages[k] = coop.make_agent<stage_t>(k, values, std::ref(tallies[k]));
            for (std::uint32_t k = 0; k < bits; ++k)
                stages[k]->wire(k + 1 < bits ? stages[k + 1]->so_direct_mbox()
                                             : last->so_direct_mbox(),
                                source->so_direct_mbox());
            source->set_first(stages.front()->so_direct_mbox());
        });
    });

    // launch() returned, every work thread joined: every tally is final, and the tallies and the
    // sequence are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence),
                       delivered(tallies, validator.validation.count(), sink.starts)};
}

}  // namespace savina_radixsort_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::radixsort::params();
    spec.expected          = qvospec::savina::radixsort::expected;
    spec.expected_messages = qvospec::savina::radixsort::expected_messages;
    spec.work_unit         = qvospec::savina::radixsort::kWorkUnit;
    spec.work_units        = qvospec::savina::radixsort::work_units;
    spec.idiom_source      = "the thread-ring adapter + so_5::send(mbox, mhood_t / message_holder_t) "
                             "redirection (dev/so_5/send_functions.hpp)";
    spec.idiom_note        = "agents on their DIRECT mboxes; a stage redirects the msg_value it "
                             "received and holds message_holder_t for the flush, as the reference "
                             "re-sends its ValueMessage; thread_pool(cores) with fifo_t::individual "
                             "for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the chain's agents are placed by the thread_pool: whichever pinned work thread is free "
        "takes the next demand, where qb and the floor cut the chain into two contiguous halves -- "
        "see benchmarks/savina/radixsort.md");

    return qvo::run(argc, argv, std::move(spec), savina_radixsort_sobjectizer::body);
}
