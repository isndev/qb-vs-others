// @benchmark     savina/fork-join
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("push = ordered, any event") and the counting adapter beside
//                this file, whose stream shape this one is with sixty destinations.
// @idiom-note    `push<>` for the job stream: 600 000 events batched into per-core pipes and
//                flushed in bulk. The master lives on VirtualCore 0 and worker w on core
//                w % cores, so with cores=2 half the jobs are same-core pipe writes and half are
//                cross-core -- the split the spec asks for.

#include <qvospec/savina/fork-join.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_fork_join_qb {

using namespace qvospec::savina::fork_join;

struct Job : qb::Event {
    std::uint64_t index{0};
    explicit Job(std::uint64_t i) noexcept : index(i) {}
};
struct Ready : qb::Event {};
struct Done : qb::Event {
    std::uint64_t acc{0};
    std::uint64_t received{0};
    Done(std::uint64_t a, std::uint64_t r) noexcept : acc(a), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct Pool {
    std::vector<qb::ActorId> workers;
    qb::ActorId              master;
};

class WorkerActor final : public qb::Actor {
    const Pool         &_pool;
    const std::uint64_t _self;
    const std::uint64_t _messages;
    const int           _work;
    std::uint64_t       _acc{0};
    std::uint64_t       _received{0};

public:
    WorkerActor(const Pool &pool, std::uint64_t self, std::uint64_t messages, int work) noexcept
        : _pool(pool), _self(self), _messages(messages), _work(work) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Job>(*this);
        push<Ready>(_pool.master);
        co_return true;
    }

    void on(Job const &event) {
        _acc += job_value(_self, event.index, _work);
        if (++_received == _messages) push<Done>(_pool.master, _acc, _received);
    }
};

class MasterActor final : public qb::Actor {
    const Pool         &_pool;
    const std::uint64_t _messages;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::uint32_t       _ready{0};
    std::uint32_t       _done{0};

public:
    MasterActor(const Pool &pool, std::uint64_t messages, qvo::Watch &watch, Sink &sink) noexcept
        : _pool(pool), _messages(messages), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _pool.workers.size()) return;
        _watch.start();
        for (std::uint64_t i = 0; i < _messages; ++i)
            for (const auto &w : _pool.workers) push<Job>(w, i);
    }

    void on(Done const &event) {
        _sink.checksum += event.acc;
        _sink.messages += event.received + 1;
        if (++_done == _pool.workers.size()) {
            _watch.stop();
            broadcast<qb::KillEvent>();
        }
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors   = static_cast<std::uint32_t>(p.get("actors"));
    const auto messages = static_cast<std::uint64_t>(p.get("messages"));
    const auto work     = static_cast<int>(p.get("work"));
    const auto cores    = static_cast<int>(p.get("cores"));
    const bool spin     = p.get("wait") != 0;

    Sink sink;
    Pool pool;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        pool.master = engine.addActor<MasterActor>(0, std::cref(pool), messages, std::ref(watch),
                                                   std::ref(sink));
        pool.workers.reserve(actors);
        for (std::uint32_t w = 0; w < actors; ++w)
            pool.workers.push_back(engine.addActor<WorkerActor>(
                static_cast<qb::CoreId>(w % static_cast<std::uint32_t>(ncores)), std::cref(pool),
                w, messages, work));

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fork_join_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join::params();
    spec.expected          = qvospec::savina::fork_join::expected;
    spec.expected_messages = qvospec::savina::fork_join::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md (push = ordered) + the counting adapter";
    spec.idiom_note        = "push<> job stream into per-core pipes; master on VirtualCore 0, "
                             "worker w on core w % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "worker w lives on VirtualCore w % cores, fixed before start: qb does not balance load, "
        "so with cores=2 the master's own core runs half the workers AND the master's dispatch "
        "loop while the other core runs the other half -- see benchmarks/savina/fork-join.md");

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_qb::body);
}
