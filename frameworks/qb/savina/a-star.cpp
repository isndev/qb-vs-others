// @benchmark     savina/a-star
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("reply vs forward": `forward(dest, e)` re-routes a received
//                event without building a new one, from a non-const `on(Event &)`) and the big
//                adapter beside this file for a field of statically placed actors.
// @idiom-note    The master RELAYS a handed-back node with `forward(worker, event)` -- the Work
//                event a worker pushed is the one the next worker receives, qb's cheapest relay --
//                and starts the search with one `push<Work>`. A worker pushes each node of its
//                frontier back as a Work, then one Ack: both on the same pipe to the master, so the
//                frontier always lands before the acknowledgement that counts it. Actor a lives on
//                VirtualCore a % cores (master = 0, search worker w = w + 1): no rebalancing beyond
//                the master's round-robin, which is Savina's own distribution.

#include <qvospec/savina/a-star.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_a_star_qb {

using namespace qvospec::savina::a_star;

struct Work : qb::Event {
    std::uint32_t node{0};
    explicit Work(std::uint32_t n) noexcept : node(n) {}
};
struct Ack : qb::Event {
    std::uint64_t chk{0};
    std::uint64_t nodes{0};
    Ack(std::uint64_t c, std::uint64_t n) noexcept : chk(c), nodes(n) {}
};
struct Ready : qb::Event {};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t work_messages{0};  // observed, and asserted >= min_work_messages
};

// The ids, filled before the engine starts and read-only from then on.
struct Field {
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
};

class Worker final : public qb::Actor {
    const Field               &_field;
    const Grid                &_grid;
    Claims                    &_claims;
    const std::uint32_t        _threshold;
    const int                  _work;
    std::vector<std::uint32_t> _queue;

public:
    Worker(const Field &field, const Grid &grid, Claims &claims, std::uint32_t threshold,
           int work) noexcept
        : _field(field), _grid(grid), _claims(claims), _threshold(threshold), _work(work) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Work>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Work const &event) {
        const Chunk c = search(_grid, _claims, event.node, _threshold, _work, _queue,
                               [this](std::uint32_t node) { push<Work>(_field.master, node); });
        push<Ack>(_field.master, c.chk, c.nodes);
    }
};

// Savina's Master: hands every node it is given to the next worker, round-robin, and counts the
// acknowledgements; the run is over when every work message sent has been acknowledged.
class Master final : public qb::Actor {
    const Field  &_field;
    qvo::Watch   &_watch;
    Sink         &_sink;
    std::size_t   _ready{0};
    std::uint64_t _sent{0};
    std::uint64_t _completed{0};

    qb::ActorId next_worker() noexcept { return _field.workers[_sent++ % _field.workers.size()]; }

public:
    Master(const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Work>(*this);
        registerEvent<Ack>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.workers.size()) return;
        _watch.start();
        push<Work>(next_worker(), Grid::kOrigin);
    }

    // A node a worker handed back: its own event goes on to the next worker.
    void on(Work &event) {
        ++_sink.messages;
        forward(next_worker(), event);
    }

    void on(Ack const &event) {
        // The acknowledgement and the work message it proves delivered.
        _sink.messages += 2;
        _sink.checksum += event.chk;
        if (++_completed != _sent) return;
        _watch.stop();
        _sink.work_messages = _sent;
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto threshold = static_cast<std::uint32_t>(p.get("threshold"));
    const auto work      = static_cast<int>(p.get("work"));
    const auto cores     = static_cast<std::uint32_t>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;

    const Grid grid(static_cast<std::uint32_t>(p.get("grid")));
    Claims     claims(grid.nodes());
    Sink       sink;
    Field      field;
    {
        qb::Main engine;

        const std::uint32_t ncores = cores < 1 ? 1 : cores;
        for (std::uint32_t c = 0; c < ncores; ++c)
            qvoqb::configure_core(engine, static_cast<int>(c), spin);

        field.master = engine.addActor<Master>(0, std::cref(field), std::ref(watch),
                                               std::ref(sink));
        field.workers.reserve(workers);
        for (std::uint32_t w = 0; w < workers; ++w)
            field.workers.push_back(engine.addActor<Worker>(
                static_cast<qb::CoreId>((w + 1) % ncores), std::cref(field), std::cref(grid),
                std::ref(claims), threshold, work));

        engine.start();
        engine.join();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedWorkMessages] = sink.work_messages;
    return answer;
}

}  // namespace savina_a_star_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::a_star::params();
    spec.expected          = qvospec::savina::a_star::expected;
    spec.work_unit         = qvospec::savina::a_star::kWorkUnit;
    spec.work_units        = qvospec::savina::a_star::work_units;
    spec.observed_at_least[qvospec::savina::a_star::kObservedWorkMessages] =
        qvospec::savina::a_star::min_work_messages;
    spec.idiom_source      = "qb/llm/qb.llm.md: forward(dest, e) from a non-const on(Event &) "
                             "+ the big adapter's statically placed field";
    spec.idiom_note        = "the master relays each handed-back Work with forward() to the next "
                             "worker round-robin; a worker pushes its frontier back as Work events "
                             "then one Ack on the same pipe; actor a on VirtualCore a % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "actor a lives on VirtualCore a % cores (master = 0, search worker w = w + 1), fixed "
        "before start: the only redistribution is the master's round-robin, Savina's own, so with "
        "cores=2 a node handed back crosses a core whenever its next worker lives on the other one");
    spec.caveats.emplace_back(
        "the claim slots are shared memory every worker CASes, as in Savina's own implementation "
        "-- the one input not passed by message, identical for every framework (a-star.h, Claims)");

    return qvo::run(argc, argv, std::move(spec), savina_a_star_qb::body);
}
