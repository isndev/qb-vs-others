// @benchmark     savina/filterbank
// @framework     caf 1.1.0
// @idiom-source  the thread-ring adapter beside this file (function-based behaviors,
//                stateful_actor, built-in atoms for the control messages, a bare scalar as the hot
//                message) and caf/type_id.hpp's CAF_BEGIN_TYPE_ID_BLOCK / CAF_ADD_TYPE_ID -- CAF's
//                own way to give a message type an id -- for the round's `std::vector<double>`.
// @idiom-note    The reference's actors one for one, spawned from the end of the chain back as
//                FilterBankAkkaActorBenchmark nests them (each actor handed its successor):
//                the sink, the combine, the integrator, every bank's tagger, FIRs, delays, sampler
//                and own actor, the branches, the source, the producer. A sample is a bare
//                `double`, a tagged value `(uint32 channel, double)`, a round a
//                `std::vector<double>`; Next is `tick_atom`, Boot `open_atom`, Exit
//                `close_atom`, which each actor passes on behind its last value and then quits,
//                as the reference's onPostExit does. Before the window the producer asks every
//                actor to report in (`get_atom` -> `ok_atom`; the source learns the producer
//                from it). The pool places everything.

#include <qvospec/savina/filterbank.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>
#include <caf/type_id.hpp>

#include <utility>
#include <vector>

// The round's message type, given an id the way CAF's manual gives a custom type one.
CAF_BEGIN_TYPE_ID_BLOCK(qvo_filterbank, caf::first_custom_type_id)
CAF_ADD_TYPE_ID(qvo_filterbank, (std::vector<double>))
CAF_END_TYPE_ID_BLOCK(qvo_filterbank)

namespace savina_filterbank_caf {

using namespace qvospec::savina::filterbank;

// Each actor's Counts are its tally slot's (a line of its own; CAF's actor objects come from the
// heap and two of them may share one).
struct producer_state {
    caf::actor              source;
    std::vector<caf::actor> actors;  // every other actor, asked to report in
    std::uint64_t           simulations{0};
    std::uint64_t           sent{0};
    std::size_t             ready{0};
    Tally                  *tally{nullptr};
    qvo::Watch             *watch{nullptr};
};

struct source_state {
    caf::actor    producer;
    caf::actor    branches;
    std::uint64_t current{0};
    Tally        *tally{nullptr};
};

struct branches_state {
    std::vector<caf::actor> banks;
    Tally                  *tally{nullptr};
};

template <typename Filter>
struct stage_state {
    Filter     filter;
    caf::actor next;
    Tally     *tally{nullptr};
};

struct tagger_state {
    caf::actor    integrator;
    std::uint32_t channel{0};
    Tally        *tally{nullptr};
};

struct integrator_state {
    Join                join;
    std::vector<double> round;
    caf::actor          combine;
    std::uint32_t       channels{0};
    Tally              *tally{nullptr};
};

struct combine_state {
    caf::actor    sink;
    std::uint32_t channels{0};
    Tally        *tally{nullptr};
};

struct sink_state {
    Sequence   *sequence{nullptr};  // a line of its own the body owns
    Tally      *tally{nullptr};
    qvo::Watch *watch{nullptr};
};

// The reply every actor gives the producer's roll call, outside the window.
template <typename Self>
auto report_in(Self *self) {
    return [self](caf::get_atom, caf::actor producer) { self->mail(caf::ok_atom_v).send(producer); };
}

// The four filters behind one interface (stage_fun's); the bank's own actor forwards.
struct Forward {
    bool step(double value, double &out) noexcept {
        out = value;
        return true;
    }
};
struct DelayStep {
    Delay delay;
    bool  step(double value, double &out) noexcept {
        out = delay.step(value);
        return true;
    }
};
struct FirStep {
    Fir  fir;
    bool step(double value, double &out) noexcept { return fir.step(value, out); }
};
struct SamplerStep {
    Sampler sampler;
    bool    step(double value, double &out) noexcept {
        out = sampler.step(value);
        return true;
    }
};

template <typename Filter>
caf::behavior stage_fun(caf::stateful_actor<stage_state<Filter>> *self, Filter filter,
                        caf::actor next, Tally *tally) {
    auto &st  = self->state();
    st.filter = std::move(filter);
    st.next   = std::move(next);
    st.tally  = tally;
    return {
        report_in(self),
        [self](double value) {
            auto &s = self->state();
            s.tally->add(value);
            double out = 0;
            if (s.filter.step(value, out)) self->mail(out).send(s.next);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.tally->exits;
            self->mail(caf::close_atom_v).send(s.next);
            self->quit();
        },
    };
}

caf::behavior tagger_fun(caf::stateful_actor<tagger_state> *self, std::uint32_t channel,
                         caf::actor integrator, Tally *tally) {
    auto &st      = self->state();
    st.integrator = std::move(integrator);
    st.channel    = channel;
    st.tally      = tally;
    return {
        report_in(self),
        [self](double value) {
            auto &s = self->state();
            s.tally->add(value);
            self->mail(s.channel, value).send(s.integrator);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.tally->exits;
            self->mail(caf::close_atom_v).send(s.integrator);
            self->quit();
        },
    };
}

caf::behavior integrator_fun(caf::stateful_actor<integrator_state> *self, std::uint32_t channels,
                             std::uint64_t rounds, caf::actor combine, Tally *tally) {
    auto &st    = self->state();
    st.join     = Join(channels, rounds);
    st.round.resize(channels);
    st.combine  = std::move(combine);
    st.channels = channels;
    st.tally    = tally;
    return {
        report_in(self),
        [self](std::uint32_t channel, double value) {
            auto &s = self->state();
            s.tally->add(value);
            if (!s.join.put(channel, value)) return;
            bool more = true;
            while (more) {
                more = s.join.take(s.round.data());
                self->mail(s.round).send(s.combine);
            }
        },
        // One Exit per channel; the last one goes on, behind the last round.
        [self](caf::close_atom) {
            auto &s = self->state();
            if (++s.tally->exits != s.channels) return;
            self->mail(caf::close_atom_v).send(s.combine);
            self->quit();
        },
    };
}

caf::behavior combine_fun(caf::stateful_actor<combine_state> *self, std::uint32_t channels,
                          caf::actor sink, Tally *tally) {
    auto &st    = self->state();
    st.sink     = std::move(sink);
    st.channels = channels;
    st.tally    = tally;
    return {
        report_in(self),
        [self](const std::vector<double> &round) {
            auto &s = self->state();
            s.tally->add_round(round.data(), s.channels);
            self->mail(combine(round.data(), s.channels)).send(s.sink);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.tally->exits;
            self->mail(caf::close_atom_v).send(s.sink);
            self->quit();
        },
    };
}

caf::behavior sink_fun(caf::stateful_actor<sink_state> *self, Sequence *sequence, Tally *tally,
                       qvo::Watch *watch) {
    auto &st    = self->state();
    st.sequence = sequence;
    st.tally    = tally;
    st.watch    = watch;
    return {
        report_in(self),
        [self](double value) {
            auto &s = self->state();
            s.tally->add(value);
            s.sequence->take(value);
        },
        // The Exit follows the last sum through every FIFO: it ends the run.
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.tally->exits;
            s.watch->stop();
            self->quit();
        },
    };
}

caf::behavior branches_fun(caf::stateful_actor<branches_state> *self,
                           std::vector<caf::actor> banks, Tally *tally) {
    auto &st = self->state();
    st.banks = std::move(banks);
    st.tally = tally;
    return {
        report_in(self),
        [self](double value) {
            auto &s = self->state();
            s.tally->add(value);
            for (auto &b : s.banks) self->mail(value).send(b);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.tally->exits;
            for (auto &b : s.banks) self->mail(caf::close_atom_v).send(b);
            self->quit();
        },
    };
}

caf::behavior source_fun(caf::stateful_actor<source_state> *self, caf::actor branches,
                         Tally *tally) {
    auto &st    = self->state();
    st.branches = std::move(branches);
    st.tally    = tally;
    return {
        // The roll call also tells the source who paces it.
        [self](caf::get_atom, caf::actor producer) {
            self->state().producer = producer;
            self->mail(caf::ok_atom_v).send(producer);
        },
        [self](caf::open_atom) {  // Boot
            auto &s = self->state();
            ++s.tally->count;
            self->mail(sample(s.current++)).send(s.branches);
            self->mail(caf::tick_atom_v).send(s.producer);
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.tally->exits;
            self->mail(caf::close_atom_v).send(s.branches);
            self->quit();
        },
    };
}

caf::behavior producer_fun(caf::stateful_actor<producer_state> *self, caf::actor source,
                           std::vector<caf::actor> actors, std::uint64_t simulations, Tally *tally,
                           qvo::Watch *watch) {
    auto &st       = self->state();
    st.source      = std::move(source);
    st.actors      = std::move(actors);
    st.simulations = simulations;
    st.tally       = tally;
    st.watch       = watch;
    const auto me  = caf::actor_cast<caf::actor>(self);
    for (auto &a : st.actors) self->mail(caf::get_atom_v, me).send(a);
    return {
        // Every actor has been scheduled once: open the window with the reference's first Next.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.actors.size()) return;
            s.watch->start();
            self->mail(caf::tick_atom_v).send(caf::actor_cast<caf::actor>(self));
        },
        [self](caf::tick_atom) {  // Next
            auto &s = self->state();
            ++s.tally->count;
            if (s.sent == s.simulations) {
                self->mail(caf::close_atom_v).send(s.source);
                self->quit();
                return;
            }
            ++s.sent;
            self->mail(caf::open_atom_v).send(s.source);
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto simulations = static_cast<std::uint64_t>(p.get("simulations"));
    const auto columns     = static_cast<std::uint32_t>(p.get("columns"));
    const auto channels    = static_cast<std::uint32_t>(p.get("channels"));
    const auto cores       = static_cast<std::size_t>(p.get("cores"));
    const bool spin        = p.get("wait") != 0;
    const auto nrounds     = rounds(p);
    qvocaf::refuse_spin_if_detached(spin);

    const Coefficients         coefficients(channels, columns);
    std::vector<Tally>         tallies(actors(channels));
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(nrounds));
    SequenceSlot               out{Sequence(sequence.data(), nrounds)};
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> all;  // every actor but the producer
        const auto spawn = [&](auto &&...args) {
            all.push_back(sys.spawn<qvocaf::kSpawnOptions>(std::forward<decltype(args)>(args)...));
            return all.back();
        };
        const caf::actor snk = spawn(sink_fun, &out.sequence, &tallies[sink(channels)], &watch);
        const caf::actor cmb = spawn(combine_fun, channels, snk, &tallies[combiner(channels)]);
        const caf::actor itg =
            spawn(integrator_fun, channels, nrounds, cmb, &tallies[integrator(channels)]);
        std::vector<caf::actor> banks(channels);
        for (std::uint32_t j = 0; j < channels; ++j) {
            const auto slot = [&](Role role) { return &tallies[bank_actor(j, role)]; };
            caf::actor next = spawn(tagger_fun, j, itg, slot(kTagger));
            next = spawn(stage_fun<FirStep>, FirStep{Fir(columns, coefficients.f(j))}, next, slot(kFir2));
            next = spawn(stage_fun<DelayStep>, DelayStep{Delay(columns - 1)}, next, slot(kDelay2));
            next = spawn(stage_fun<SamplerStep>, SamplerStep{Sampler(columns)}, next, slot(kSampler));
            next = spawn(stage_fun<FirStep>, FirStep{Fir(columns, coefficients.h(j))}, next, slot(kFir1));
            next = spawn(stage_fun<DelayStep>, DelayStep{Delay(columns - 1)}, next, slot(kDelay1));
            banks[j] = spawn(stage_fun<Forward>, Forward{}, next, slot(kBank));
        }
        const caf::actor brs = spawn(branches_fun, banks, &tallies[kBranches]);
        const caf::actor src = spawn(source_fun, brs, &tallies[kSource]);
        sys.spawn<qvocaf::kSpawnOptions>(producer_fun, src, all, simulations, &tallies[kProducer],
                                         &watch);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: every tally is final, and the tallies and the
    // sequence are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence), delivered(tallies)};
}

}  // namespace savina_filterbank_caf

int main(int argc, char **argv) {
    caf::init_global_meta_objects<caf::id_block::qvo_filterbank>();
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::filterbank::params();
    spec.expected          = qvospec::savina::filterbank::expected;
    spec.expected_messages = qvospec::savina::filterbank::expected_messages;
    spec.work_unit         = qvospec::savina::filterbank::kWorkUnit;
    spec.work_units        = qvospec::savina::filterbank::work_units;
    spec.idiom_source      = "the thread-ring adapter + CAF_ADD_TYPE_ID (caf/type_id.hpp) for the "
                             "round's std::vector<double>";
    spec.idiom_note        = "the reference's actors one for one, spawned from the end back, each "
                             "handed its successor; a bare double per sample, built-in atoms for "
                             "Next / Boot / Exit; every actor quits behind its Exit; placement left "
                             "to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the banks' actors are placed by the work-stealing pool: how the eight banks' FIRs spread "
        "over the workers is the scheduler's decision, where qb and the floor fix bank j on core "
        "j % cores -- see benchmarks/savina/filterbank.md");

    return qvo::run(argc, argv, std::move(spec), savina_filterbank_caf::body);
}
