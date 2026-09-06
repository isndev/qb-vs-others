// @benchmark     savina/chameneos
// @framework     caf 1.1.0
// @idiom-source  the big adapter beside this file (function-based behaviors, stateful_actor,
//                handles exchanged before the window, built-in atoms from caf/type_id.hpp).
// @idiom-note    A request is `(get_atom, uint32 creature, uint32 colour)`, an announcement
//                `(put_atom, uint32 k, uint32 other_colour)`, the exit `(close_atom)` and the
//                report `(ok_atom, uint64 meetings, uint64 acc, uint64 messages)`. The mall
//                holds the creature handle vector from one wiring message before the window, so
//                an announcement goes to `creatures[index]` and no hot message carries a handle.
//                The mall and the creatures are placed by the work-stealing pool: whether the
//                mall's mailbox is a cross-core queue is the scheduler's decision.

#include <qvospec/savina/chameneos.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_chameneos_caf {

using namespace qvospec::savina::chameneos;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct creature_state {
    caf::actor    mall;
    std::uint32_t index{0};
    Colour        colour{kYellow};
    std::uint64_t meetings{0};
    std::uint64_t acc{0};
    std::uint64_t received{0};
};

struct mall_state {
    std::vector<caf::actor> creatures;
    std::uint32_t           meetings{0};
    std::uint32_t           k{0};
    bool                    waiting{false};
    std::uint32_t           waiting_index{0};
    Colour                  waiting_colour{kYellow};
    std::uint64_t           received{0};
    std::uint64_t           total_meetings{0};
    std::size_t             wired{0};
    std::size_t             counted{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior creature_fun(caf::stateful_actor<creature_state> *self, std::uint32_t index) {
    auto &st  = self->state();
    st.index  = index;
    st.colour = initial_colour(index);

    auto request = [self] {
        auto &s = self->state();
        self->mail(caf::get_atom_v, s.index, static_cast<std::uint32_t>(s.colour)).send(s.mall);
    };

    return {
        // Wiring handshake, once, outside the window.
        [self](caf::put_atom, caf::actor mall) {
            self->state().mall = std::move(mall);
            self->mail(caf::ok_atom_v).send(self->state().mall);
        },
        [self, request](caf::tick_atom) {
            ++self->state().received;
            request();
        },
        [self, request](caf::put_atom, std::uint32_t k, std::uint32_t other) {
            auto &s = self->state();
            ++s.received;
            s.colour = complement(s.colour, static_cast<Colour>(other));
            s.acc += qvo::mix(k);
            ++s.meetings;
            request();
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.received;
            self->mail(caf::ok_atom_v, s.meetings, s.acc, s.received).send(s.mall);
            self->quit();
        },
    };
}

caf::behavior mall_fun(caf::stateful_actor<mall_state> *self, std::vector<caf::actor> creatures,
                       std::uint32_t meetings, qvo::Watch *watch, Sink *sink) {
    auto &st     = self->state();
    st.creatures = std::move(creatures);
    st.meetings  = meetings;
    st.watch     = watch;
    st.sink      = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &c : st.creatures) self->mail(caf::put_atom_v, me).send(c);

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.creatures.size()) return;
            s.watch->start();
            for (auto &c : s.creatures) self->mail(caf::tick_atom_v).send(c);
        },
        [self](caf::get_atom, std::uint32_t index, std::uint32_t colour) {
            auto &s = self->state();
            ++s.received;
            if (s.k == s.meetings) {
                self->mail(caf::close_atom_v).send(s.creatures[index]);
                return;
            }
            if (!s.waiting) {
                s.waiting        = true;
                s.waiting_index  = index;
                s.waiting_colour = static_cast<Colour>(colour);
                return;
            }
            s.waiting = false;
            self->mail(caf::put_atom_v, s.k, static_cast<std::uint32_t>(s.waiting_colour))
                .send(s.creatures[index]);
            self->mail(caf::put_atom_v, s.k, colour).send(s.creatures[s.waiting_index]);
            ++s.k;
        },
        [self](caf::ok_atom, std::uint64_t meetings, std::uint64_t acc, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += acc;
            s.sink->messages += received;
            s.total_meetings += meetings;
            if (++s.counted != s.creatures.size()) return;
            s.sink->checksum += qvo::mix(s.total_meetings);
            s.sink->messages += s.received;
            s.watch->stop();
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto creatures = static_cast<std::uint32_t>(p.get("chameneos"));
    const auto meetings  = static_cast<std::uint32_t>(p.get("meetings"));
    const auto cores     = static_cast<std::size_t>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> field;
        field.reserve(creatures);
        for (std::uint32_t c = 0; c < creatures; ++c)
            field.push_back(sys.spawn<qvocaf::kSpawnOptions>(creature_fun, c));
        sys.spawn<qvocaf::kSpawnOptions>(mall_fun, field, meetings, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_chameneos_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::chameneos::params();
    spec.expected          = qvospec::savina::chameneos::expected;
    spec.expected_messages = qvospec::savina::chameneos::expected_messages;
    spec.work_unit         = qvospec::savina::chameneos::kWorkUnit;
    spec.work_units        = qvospec::savina::chameneos::work_units;
    spec.idiom_source      = "the big adapter + caf/type_id.hpp built-in atoms";
    spec.idiom_note        = "(get_atom, creature, colour) / (put_atom, k, other) / close_atom / "
                             "(ok_atom, meetings, acc, messages); creature handles delivered once "
                             "before the window; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the mall and the 100 creatures are placed by the work-stealing pool, so whether the "
        "mall's mailbox is a cross-core queue is the scheduler's decision; qb's cell pins the "
        "mall alone on core 0 and every creature on the far side -- see "
        "benchmarks/savina/chameneos.md");

    return qvo::run(argc, argv, std::move(spec), savina_chameneos_caf::body);
}
