// @benchmark     savina/bitonicsort
// @framework     caf 1.1.0
// @idiom-source  the thread-ring adapter beside this file (function-based behaviors, a bare
//                `uint64` for the hot message, built-in atoms for the rest, each actor's tally a slot
//                of its own written in the handler) and caf/type_id.hpp's built-in atoms:
//                `open_atom` starts the source, `close_atom` is the reference's ExitMessage,
//                `tick_atom` / `ok_atom` the readiness handshake.
// @idiom-note    The reference's ValueMessage is a bare `uint64`, its DataMessage `(uint32 order,
//                uint64 value)` -- two signatures, no atom needed to tell them apart. One behavior
//                per kind of the reference's network (the spec's topology(), 44 772 actors at the
//                defaults), each around the spec's Splitter / Comparator / Joiner, every actor
//                spawned before the window in dependency order with the handles it sends to. The
//                work-stealing pool places every actor; CAF's mailbox is FIFO per sender, which the
//                joiners' lanes and the Exit wave need.

#include <qvospec/savina/bitonicsort.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace savina_bitonicsort_caf {

using namespace qvospec::savina::bitonicsort;

// The network as the actors see it: filled while spawning, before any message flows except the
// readiness handshake, and read-only once the driver starts the window.
struct Net {
    const Topology         *topology{nullptr};
    const Values           *input{nullptr};
    std::vector<caf::actor> actors;
    std::vector<Tally>     *tallies{nullptr};
    Values                 *out{nullptr};
    qvo::Watch             *watch{nullptr};
};

// Every network actor answers the driver's tick once, outside the window.
auto handshake(caf::event_based_actor *self) {
    return [self](caf::tick_atom, caf::actor driver) { self->mail(caf::ok_atom_v).send(driver); };
}

caf::behavior source_fun(caf::event_based_actor *self, Net *net, std::uint32_t index, caf::actor next) {
    Tally *tally = &(*net->tallies)[index];
    return {
        handshake(self),
        [self, net, tally, next](caf::open_atom) {
            tally->signal();
            for (const std::uint64_t v : *net->input) self->mail(v).send(next);
            self->mail(caf::close_atom_v).send(next);
        },
    };
}

caf::behavior forward_fun(caf::event_based_actor *self, Tally *tally, caf::actor next) {
    return {
        handshake(self),
        [self, tally, next](std::uint64_t v) {
            tally->value(v);
            self->mail(v).send(next);
        },
        [self, tally, next](caf::close_atom) {
            tally->signal();
            self->mail(caf::close_atom_v).send(next);
        },
    };
}

struct splitter_state {
    std::vector<caf::actor> receivers;
    Splitter                splitter;
};

caf::behavior splitter_fun(caf::stateful_actor<splitter_state> *self, Tally *tally, const Node *node,
                           std::vector<caf::actor> receivers) {
    self->state().receivers = std::move(receivers);
    return {
        handshake(self),
        [self, tally, node](std::uint64_t v) {
            auto &s = self->state();
            tally->value(v);
            self->mail(v).send(s.receivers[s.splitter.next(*node)]);
        },
        [self, tally](caf::close_atom) {
            tally->signal();
            for (const auto &r : self->state().receivers) self->mail(caf::close_atom_v).send(r);
        },
    };
}

caf::behavior compare_fun(caf::stateful_actor<Comparator> *self, Tally *tally, const Node *node,
                          caf::actor next) {
    return {
        handshake(self),
        [self, tally, node, next](std::uint64_t v) {
            tally->value(v);
            std::uint64_t a = 0, b = 0;
            if (self->state().accept(v, node->up, a, b)) {
                self->mail(node->order, a).send(next);
                self->mail(node->order, b).send(next);
            }
        },
        [self, tally, next](caf::close_atom) {
            tally->signal();
            self->mail(caf::close_atom_v).send(next);
        },
    };
}

// CAF default-constructs an actor's state; the joiner's lanes are sized from its node at spawn.
struct joiner_state {
    std::optional<Joiner> joiner;
};

caf::behavior joiner_fun(caf::stateful_actor<joiner_state> *self, Tally *tally, const Node *node,
                         caf::actor next) {
    self->state().joiner.emplace(*node);
    return {
        handshake(self),
        [self, tally, next](std::uint32_t order, std::uint64_t v) {
            tally->value(v);
            self->state().joiner->accept(order, v, [self, &next](std::uint64_t out) { self->mail(out).send(next); });
        },
        [self, tally, next](caf::close_atom) {
            tally->signal();
            if (self->state().joiner->close()) self->mail(caf::close_atom_v).send(next);
        },
    };
}

caf::behavior value_data_fun(caf::event_based_actor *self, Tally *tally, std::uint32_t order, caf::actor next) {
    return {
        handshake(self),
        [self, tally, order, next](std::uint64_t v) {
            tally->value(v);
            self->mail(order, v).send(next);
        },
        [self, tally, next](std::uint32_t o, std::uint64_t v) {
            tally->value(v);
            self->mail(o, v).send(next);
        },
        [self, tally, next](caf::close_atom) {
            tally->signal();
            self->mail(caf::close_atom_v).send(next);
        },
    };
}

caf::behavior data_value_fun(caf::event_based_actor *self, Tally *tally, caf::actor next) {
    return {
        handshake(self),
        [self, tally, next](std::uint64_t v) {
            tally->value(v);
            self->mail(v).send(next);
        },
        [self, tally, next](std::uint32_t, std::uint64_t v) {
            tally->value(v);
            self->mail(v).send(next);
        },
        [self, tally, next](caf::close_atom) {
            tally->signal();
            self->mail(caf::close_atom_v).send(next);
        },
    };
}

// The sink records the values and closes the window on the first Exit, then ends every actor.
struct sink_state {
    bool done{false};
};
caf::behavior sink_fun(caf::stateful_actor<sink_state> *self, Net *net, Tally *tally) {
    return {
        handshake(self),
        [net, tally](std::uint64_t v) {
            tally->value(v);
            net->out->push_back(v);
        },
        [self, net, tally](caf::close_atom) {
            tally->signal();
            if (self->state().done) return;
            self->state().done = true;
            net->watch->stop();
            for (std::size_t i = 1; i < net->actors.size(); ++i)
                self->send_exit(net->actors[i], caf::exit_reason::user_shutdown);
            self->quit();
        },
    };
}

// Spawned after the whole network: ticks every actor, opens the window on the last answer.
struct driver_state {
    std::size_t ready{0};
};
caf::behavior driver_fun(caf::stateful_actor<driver_state> *self, Net *net) {
    const auto me = caf::actor_cast<caf::actor>(self);
    for (const auto &a : net->actors) self->mail(caf::tick_atom_v, me).send(a);
    return {
        [self, net](caf::ok_atom) {
            if (++self->state().ready != net->actors.size()) return;
            net->watch->start();
            self->mail(caf::open_atom_v).send(net->actors.back());  // the source
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores = static_cast<std::size_t>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

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
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        // Dependency order: every actor's targets are spawned before it (bitonicsort.h, Topology).
        net.actors.reserve(t.nodes.size());
        for (std::uint32_t i = 0; i < t.nodes.size(); ++i) {
            const Node &node  = t.nodes[i];
            Tally      *tally = &tallies[i];
            const auto  next  = [&] { return node.next == kNone ? caf::actor{} : net.actors[node.next]; };
            caf::actor  a;
            switch (node.kind) {
            case Kind::kSource: a = sys.spawn<qvocaf::kSpawnOptions>(source_fun, &net, i, next()); break;
            case Kind::kForward: a = sys.spawn<qvocaf::kSpawnOptions>(forward_fun, tally, next()); break;
            case Kind::kSplitter: {
                std::vector<caf::actor> receivers;
                for (std::uint32_t k = 0; k < node.lanes; ++k)
                    receivers.push_back(net.actors[t.receivers[node.first + k]]);
                a = sys.spawn<qvocaf::kSpawnOptions>(splitter_fun, tally, &node, std::move(receivers));
                break;
            }
            case Kind::kCompare: a = sys.spawn<qvocaf::kSpawnOptions>(compare_fun, tally, &node, next()); break;
            case Kind::kJoiner: a = sys.spawn<qvocaf::kSpawnOptions>(joiner_fun, tally, &node, next()); break;
            case Kind::kValueData:
                a = sys.spawn<qvocaf::kSpawnOptions>(value_data_fun, tally, node.order, next());
                break;
            case Kind::kDataValue: a = sys.spawn<qvocaf::kSpawnOptions>(data_value_fun, tally, next()); break;
            case Kind::kSink: a = sys.spawn<qvocaf::kSpawnOptions>(sink_fun, &net, tally); break;
            }
            net.actors.push_back(std::move(a));
        }
        sys.spawn<qvocaf::kSpawnOptions>(driver_fun, &net);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: every tally is final, and reduced here, outside
    // the window.
    explain(tallies, out, p);
    return qvo::Answer{checksum(tallies, out), delivered(tallies)};
}

}  // namespace savina_bitonicsort_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bitonicsort::params();
    spec.expected          = qvospec::savina::bitonicsort::expected;
    spec.expected_messages = qvospec::savina::bitonicsort::expected_messages;
    spec.work_unit         = qvospec::savina::bitonicsort::kWorkUnit;
    spec.work_units        = qvospec::savina::bitonicsort::work_units;
    spec.idiom_source      = "the thread-ring adapter + caf/type_id.hpp built-in atoms "
                             "(open_atom, close_atom, tick_atom, ok_atom)";
    spec.idiom_note        = "bare uint64 value, (uint32 order, uint64 value) stamped value, "
                             "close_atom Exit; one behavior per kind of the reference network, "
                             "spawned before the window; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the network's 44 772 actors (at the defaults) are placed by the work-stealing pool: a "
        "message's receiver runs on the sender's worker unless the idle worker steals it, so how "
        "many of the hops cross a core is the scheduler's decision, where qb's cell cuts the steps "
        "into two contiguous halves -- see benchmarks/savina/bitonicsort.md");

    return qvo::run(argc, argv, std::move(spec), savina_bitonicsort_caf::body);
}
