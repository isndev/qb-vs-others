// @benchmark     savina/filterbank
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the thread-ring adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, one coop on qvoso::make_pool_binder) and dev/so_5/send_functions.hpp's
//                redirection of an existing message, `so_5::send(mbox, mhood_t<M>)` and
//                `so_5::send(mbox, message_holder_t<M>)` (5.5.19, 5.6.0).
// @idiom-note    The reference's actors one for one, and its message objects: where
//                FilterBankAkkaActorBenchmark re-sends the message it received -- the branches to
//                every bank, a bank to its first delay, a sampler keeping its sample -- the agent
//                redirects the immutable msg_value it received, and a sampler zeroing one sends a
//                preallocated zero, the reference's SampleFilterActor.ZERO_RESULT; a delay, an FIR,
//                a tagger and a combine send a new message, as the reference does. Messages from
//                one sender to one mbox keep their order: the Exit follows every value.

#include <qvospec/savina/filterbank.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_filterbank_sobjectizer {

using namespace qvospec::savina::filterbank;

struct msg_value final : public so_5::message_t {
    double value;
    explicit msg_value(double v) noexcept : value(v) {}
};
struct msg_tagged final : public so_5::message_t {
    std::uint32_t channel;
    double        value;
    msg_tagged(std::uint32_t c, double v) noexcept : channel(c), value(v) {}
};
struct msg_round final : public so_5::message_t {
    std::vector<double> values;
    explicit msg_round(std::vector<double> v) : values(std::move(v)) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_next final : public so_5::signal_t {};
struct msg_boot final : public so_5::signal_t {};
struct msg_exit final : public so_5::signal_t {};

// Every agent's tally is its slot (a line of its own); `next` is its successor's direct mbox and
// `producer` the roll call's, both wired before the environment starts.
class node_t : public so_5::agent_t {
protected:
    Tally       *m_tally;
    so_5::mbox_t m_next;
    so_5::mbox_t m_producer;

    void pass_exit() {
        ++m_tally->exits;
        so_5::send<msg_exit>(m_next);
    }

public:
    node_t(context_t ctx, Tally &tally) : so_5::agent_t{std::move(ctx)}, m_tally{&tally} {}
    void wire(so_5::mbox_t next, so_5::mbox_t producer) {
        m_next     = std::move(next);
        m_producer = std::move(producer);
    }
    void so_evt_start() override { so_5::send<msg_ready>(m_producer); }
};

class producer_t final : public so_5::agent_t {
    Tally              &m_tally;
    const std::uint64_t m_simulations;
    const std::size_t   m_actors;  // every other agent, which reports in
    qvo::Watch         &m_watch;
    so_5::mbox_t        m_source;
    std::uint64_t       m_sent{0};
    std::size_t         m_ready{0};

public:
    producer_t(context_t ctx, Tally &tally, std::uint64_t simulations, std::size_t actors,
               qvo::Watch &watch)
        : so_5::agent_t{std::move(ctx)}
        , m_tally{tally}
        , m_simulations{simulations}
        , m_actors{actors}
        , m_watch{watch} {}

    void set_source(so_5::mbox_t source) { m_source = std::move(source); }

    void so_define_agent() override {
        // Every other agent has started: open the window with the reference's first Next.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_actors) return;
            m_watch.start();
            so_5::send<msg_next>(*this);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_next>) {
            ++m_tally.count;
            if (m_sent == m_simulations) {
                so_5::send<msg_exit>(m_source);
                return;
            }
            ++m_sent;
            so_5::send<msg_boot>(m_source);
        });
    }
};

class source_t final : public node_t {
    std::uint64_t m_current{0};

public:
    using node_t::node_t;
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_boot>) {
            ++m_tally->count;
            so_5::send<msg_value>(m_next, sample(m_current++));
            so_5::send<msg_next>(m_producer);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

// The branches: the sample it received, redirected to every bank, as the reference forwards
// `theMsg` to each.
class branches_t final : public so_5::agent_t {
    Tally                    *m_tally;
    std::vector<so_5::mbox_t> m_banks;
    so_5::mbox_t              m_producer;

public:
    branches_t(context_t ctx, Tally &tally) : so_5::agent_t{std::move(ctx)}, m_tally{&tally} {}
    void wire(std::vector<so_5::mbox_t> banks, so_5::mbox_t producer) {
        m_banks    = std::move(banks);
        m_producer = std::move(producer);
    }
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            for (const auto &b : m_banks) so_5::send(b, m);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++m_tally->exits;
            for (const auto &b : m_banks) so_5::send<msg_exit>(b);
        });
    }
    void so_evt_start() override { so_5::send<msg_ready>(m_producer); }
};

// A bank's own agent: the sample redirected to its first delay.
class bank_t final : public node_t {
public:
    using node_t::node_t;
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            so_5::send(m_next, m);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

class delay_t final : public node_t {
    Delay m_delay;

public:
    delay_t(context_t ctx, Tally &tally, std::uint32_t length)
        : node_t{std::move(ctx), tally}, m_delay(length) {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            so_5::send<msg_value>(m_next, m_delay.step(m->value));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

class fir_t final : public node_t {
    Fir m_fir;

public:
    fir_t(context_t ctx, Tally &tally, std::uint32_t peek, const double *coefficients)
        : node_t{std::move(ctx), tally}, m_fir(peek, coefficients) {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            double out = 0;
            if (m_fir.step(m->value, out)) so_5::send<msg_value>(m_next, out);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

// The sampler: the sample it keeps is the message it received, redirected; the ones it zeroes
// are one preallocated zero, the reference's ZERO_RESULT.
class sampler_t final : public node_t {
    Sampler                           m_sampler;
    so_5::message_holder_t<msg_value> m_zero;

public:
    sampler_t(context_t ctx, Tally &tally, std::uint32_t rate)
        : node_t{std::move(ctx), tally}
        , m_sampler(rate)
        , m_zero{so_5::message_holder_t<msg_value>::make(0.0)} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            if (m_sampler.keeps()) so_5::send(m_next, m);
            else so_5::send(m_next, m_zero);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

class tagger_t final : public node_t {
    std::uint32_t m_channel;

public:
    tagger_t(context_t ctx, Tally &tally, std::uint32_t channel)
        : node_t{std::move(ctx), tally}, m_channel{channel} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            so_5::send<msg_tagged>(m_next, m_channel, m->value);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

class integrator_t final : public node_t {
    Join                m_join;
    std::vector<double> m_round;
    std::uint32_t       m_channels;

public:
    integrator_t(context_t ctx, Tally &tally, std::uint32_t channels, std::uint64_t rounds)
        : node_t{std::move(ctx), tally}, m_join(channels, rounds), m_round(channels), m_channels{channels} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_tagged> m) {
            m_tally->add(m->value);
            if (!m_join.put(m->channel, m->value)) return;
            bool more = true;
            while (more) {
                more = m_join.take(m_round.data());
                so_5::send<msg_round>(m_next, m_round);
            }
        });
        // One Exit per channel; the last one goes on, behind the last round.
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            if (++m_tally->exits == m_channels) so_5::send<msg_exit>(m_next);
        });
    }
};

class combine_t final : public node_t {
    std::uint32_t m_channels;

public:
    combine_t(context_t ctx, Tally &tally, std::uint32_t channels)
        : node_t{std::move(ctx), tally}, m_channels{channels} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_round> m) {
            m_tally->add_round(m->values.data(), m_channels);
            so_5::send<msg_value>(m_next, combine(m->values.data(), m_channels));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) { pass_exit(); });
    }
};

class sink_t final : public node_t {
    Sequence   &m_sequence;  // a line of its own the body owns
    qvo::Watch &m_watch;

public:
    sink_t(context_t ctx, Tally &tally, Sequence &sequence, qvo::Watch &watch)
        : node_t{std::move(ctx), tally}, m_sequence{sequence}, m_watch{watch} {}
    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_value> m) {
            m_tally->add(m->value);
            m_sequence.take(m->value);
        });
        // The Exit follows the last sum through every FIFO: it ends the run.
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++m_tally->exits;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto simulations = static_cast<std::uint64_t>(p.get("simulations"));
    const auto columns     = static_cast<std::uint32_t>(p.get("columns"));
    const auto channels    = static_cast<std::uint32_t>(p.get("channels"));
    const auto cores       = static_cast<int>(p.get("cores"));
    const bool spin        = p.get("wait") != 0;
    const auto nrounds     = rounds(p);

    const Coefficients         coefficients(channels, columns);
    std::vector<Tally>         tallies(actors(channels));
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(nrounds));
    SequenceSlot               out{Sequence(sequence.data(), nrounds)};

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            auto *producer = coop.make_agent<producer_t>(std::ref(tallies[kProducer]), simulations,
                                                         std::size_t{actors(channels)} - 1,
                                                         std::ref(watch));
            const so_5::mbox_t pm = producer->so_direct_mbox();
            auto *snk = coop.make_agent<sink_t>(std::ref(tallies[sink(channels)]),
                                                std::ref(out.sequence), std::ref(watch));
            snk->wire(so_5::mbox_t{}, pm);
            auto *cmb = coop.make_agent<combine_t>(std::ref(tallies[combiner(channels)]), channels);
            cmb->wire(snk->so_direct_mbox(), pm);
            auto *itg = coop.make_agent<integrator_t>(std::ref(tallies[integrator(channels)]),
                                                      channels, nrounds);
            itg->wire(cmb->so_direct_mbox(), pm);
            std::vector<so_5::mbox_t> banks(channels);
            for (std::uint32_t j = 0; j < channels; ++j) {
                const auto slot = [&](Role role) { return std::ref(tallies[bank_actor(j, role)]); };
                node_t *next = coop.make_agent<tagger_t>(slot(kTagger), j);
                next->wire(itg->so_direct_mbox(), pm);
                const auto chain = [&](node_t *n) {
                    n->wire(next->so_direct_mbox(), pm);
                    next = n;
                };
                chain(coop.make_agent<fir_t>(slot(kFir2), columns, coefficients.f(j)));
                chain(coop.make_agent<delay_t>(slot(kDelay2), columns - 1));
                chain(coop.make_agent<sampler_t>(slot(kSampler), columns));
                chain(coop.make_agent<fir_t>(slot(kFir1), columns, coefficients.h(j)));
                chain(coop.make_agent<delay_t>(slot(kDelay1), columns - 1));
                chain(coop.make_agent<bank_t>(slot(kBank)));
                banks[j] = next->so_direct_mbox();
            }
            auto *brs = coop.make_agent<branches_t>(std::ref(tallies[kBranches]));
            brs->wire(std::move(banks), pm);
            auto *src = coop.make_agent<source_t>(std::ref(tallies[kSource]));
            src->wire(brs->so_direct_mbox(), pm);
            producer->set_source(src->so_direct_mbox());
        });
    });

    // launch() returned, every work thread joined: every tally is final, and the tallies and the
    // sequence are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence), delivered(tallies)};
}

}  // namespace savina_filterbank_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::filterbank::params();
    spec.expected          = qvospec::savina::filterbank::expected;
    spec.expected_messages = qvospec::savina::filterbank::expected_messages;
    spec.work_unit         = qvospec::savina::filterbank::kWorkUnit;
    spec.work_units        = qvospec::savina::filterbank::work_units;
    spec.idiom_source      = "the thread-ring adapter + so_5::send(mbox, mhood_t / message_holder_t) "
                             "redirection (dev/so_5/send_functions.hpp)";
    spec.idiom_note        = "agents on their DIRECT mboxes; the received msg_value redirected where "
                             "the reference re-sends its message, a preallocated zero for the "
                             "sampler; thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the banks' agents are placed by the thread_pool: how the eight banks' FIRs spread over the "
        "work threads is the dispatcher's decision, where qb and the floor fix bank j on core "
        "j % cores -- see benchmarks/savina/filterbank.md");

    return qvo::run(argc, argv, std::move(spec), savina_filterbank_sobjectizer::body);
}
