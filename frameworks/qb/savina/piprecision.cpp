// @benchmark     savina/piprecision
// @framework     qb
// @idiom-source  the nqueens adapter beside this file (a master that opens the window once every
//                worker reported ready, worker w on VirtualCore w % cores, broadcast<qb::KillEvent>()
//                to end the run) and, for the payload, qb's two documented ways to carry bulk data
//                (.claude/rules/qb-api-constraints.md, Pipe.h): a pointer member -- here a
//                std::vector moved into the event -- or a Pipe::allocated_push tail.
// @idiom-note    form=0: a worker computes the term into a fresh buffer and moves it into its Result
//                event, which carries the buffer's pointer; the event is one bucket wherever it
//                goes, and the buffer is freed on the master's core when the event is disposed.
//                form=1: a worker reserves a ResultTail with room for the term behind it,
//                getPipe(master).allocated_push<ResultTail>(bytes), and computes the term straight
//                into that tail -- the payload travels inside the event, through the pipe and,
//                across cores, the master core's mailbox ring (up to 34 buckets of 64 bytes at the
//                default precision). The master answers each term with the next one to the same worker,
//                push<>ed, as Savina's does.

#include <qvospec/savina/piprecision.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_piprecision_qb {

using namespace qvospec::savina::piprecision;

struct Work : qb::Event {
    std::uint32_t term{0};
    explicit Work(std::uint32_t k) noexcept : term(k) {}
};
// form=0: the payload behind the event, in a heap buffer whose pointer the event carries. A
// std::vector member is trivially relocatable on every standard library qb supports (no pointer
// into its own storage), which a cross-core delivery requires.
struct Result : qb::Event {
    std::uint32_t     term{0};
    std::uint32_t     worker{0};
    Limb              proof{0};
    std::vector<Limb> limbs;
    Result(std::uint32_t k, std::uint32_t w, Limb p, std::vector<Limb> &&v) noexcept
        : term(k), worker(w), proof(p), limbs(std::move(v)) {}
};
// form=1: the payload in the event's own storage -- `count` limbs right behind the event, in the
// bytes allocated_push reserved. qb::Event is cache-line aligned, so the tail starts on a line.
struct ResultTail : qb::Event {
    std::uint32_t term{0};
    std::uint32_t worker{0};
    std::uint32_t count{0};
    Limb          proof{0};
    ResultTail(std::uint32_t k, std::uint32_t w, std::uint32_t n, Limb p) noexcept
        : term(k), worker(w), count(n), proof(p) {}
    Limb *tail() noexcept {
        return reinterpret_cast<Limb *>(reinterpret_cast<unsigned char *>(this) + sizeof(ResultTail));
    }
    const Limb *limbs() const noexcept {
        return reinterpret_cast<const Limb *>(reinterpret_cast<const unsigned char *>(this)
                                              + sizeof(ResultTail));
    }
};
struct Ready : qb::Event {};

// Filled before the engine starts (addActor assigns ids before the actors are constructed) and
// read-only from then on.
struct Field {
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
};

// The worker's count is the actor's own state, on the line its handler already touches
// (thread_ring::Counts says why); its slot receives it when the engine tears the actor down.
class WorkerActor final : public qb::Actor {
    const Field        &_field;
    const Context      &_context;
    const std::uint32_t _index;
    const bool          _tail_form;
    std::uint64_t       _works{0};
    Scratch             _scratch;
    Tally              &_slot;

public:
    WorkerActor(const Field &field, const Context &context, std::uint32_t index, bool tail_form,
                Tally &slot) noexcept
        : _field(field), _context(context), _index(index), _tail_form(tail_form), _slot(slot) {}

    ~WorkerActor() noexcept override { _slot.works = _works; }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Work>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Work const &event) {
        ++_works;
        if (!_tail_form) {
            std::vector<Limb> term;  // a fresh buffer per answer: it leaves with the event
            bbp_term(_context, event.term, _scratch, term);
            push<Result>(_field.master, event.term, _index, bbp_proof(_scratch),
                         std::move(term));
            return;
        }
        bbp_numerator(_context, event.term, _scratch);
        const std::size_t room = bbp_room(event.term, _scratch);
        auto &result = getPipe(_field.master)
                           .allocated_push<ResultTail>(room * sizeof(Limb), event.term, _index, 0u,
                                                       bbp_proof(_scratch));
        result.count = static_cast<std::uint32_t>(bbp_shift(event.term, _scratch, result.tail()));
    }
};

class MasterActor final : public qb::Actor {
    const Field  &_field;
    const Config  _config;
    qvo::Watch   &_watch;
    Fold         &_fold;
    std::size_t   _ready{0};
    bool          _stop{false};

    void hand_out(std::uint32_t worker) { push<Work>(_field.workers[worker], _fold.next()); }

    // Savina's Master.process(ResultMessage), the fold's arithmetic shared with every adapter.
    void receive(std::uint32_t term, std::uint32_t worker, const Limb *v, std::size_t n,
                 Limb proof) {
        if (worker >= _config.workers)
            qvo::fail("savina/piprecision: an answer names worker " + std::to_string(worker)
                      + ", which does not exist");
        if (_fold.add(term, v, n, proof)) _stop = true;
        if (!_stop) hand_out(worker);
        if (!_fold.complete()) return;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }

public:
    MasterActor(const Field &field, const Config &config, qvo::Watch &watch, Fold &fold) noexcept
        : _field(field), _config(config), _watch(watch), _fold(fold) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Result>(*this);
        registerEvent<ResultTail>(*this);
        co_return true;
    }

    // The window opens once every worker has reported in: every core running and warm. Savina's
    // StartMessage: the first `batch` terms, round-robin.
    void on(Ready const &) {
        if (++_ready != _config.workers) return;
        _watch.start();
        for (std::uint32_t t = 0; t < _config.batch; ++t) hand_out(t % _config.workers);
    }

    void on(Result const &event) {
        receive(event.term, event.worker, event.limbs.data(), event.limbs.size(), event.proof);
    }
    void on(ResultTail const &event) {
        receive(event.term, event.worker, event.limbs(), event.count, event.proof);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config  c           = config(p);
    const bool    tail_form = inline_payload(p);
    const auto    cores       = static_cast<int>(p.get("cores"));
    const bool    spin        = p.get("wait") != 0;
    const Context context     = make_context(c.precision);

    Fold               fold(c, context);
    std::vector<Tally> tallies(c.workers);
    Field              field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int k = 0; k < ncores; ++k) qvoqb::configure_core(engine, k, spin);

        field.master = engine.addActor<MasterActor>(0, std::cref(field), c, std::ref(watch),
                                                    std::ref(fold));
        field.workers.reserve(c.workers);
        for (std::uint32_t w = 0; w < c.workers; ++w)
            field.workers.push_back(engine.addActor<WorkerActor>(
                static_cast<qb::CoreId>(w % static_cast<std::uint32_t>(ncores)), std::cref(field),
                std::cref(context), w, tail_form, std::ref(tallies[w])));

        engine.start();
        engine.join();
    }
    // The engine is gone: every worker handed its count to its slot as it was destroyed, and the
    // fold and the tallies are reduced here, outside the window.
    qvo::Answer answer{checksum(fold, works(tallies), reference(p).proofs), 0};
    answer.observed[kObservedTerms] = fold.requested;
    return answer;
}

}  // namespace savina_piprecision_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::piprecision::params();
    // The payload form this adapter's table cell runs: the pointer (form=0) until the quiet-host
    // measurement of both forms names the faster one (benchmarks/savina/piprecision.md).
    spec.params["form"]    = 0;
    spec.expected          = qvospec::savina::piprecision::expected;
    spec.work_unit         = qvospec::savina::piprecision::kWorkUnit;
    spec.work_units        = qvospec::savina::piprecision::work_units;
    spec.observed_at_least = {{qvospec::savina::piprecision::kObservedTerms,
                               qvospec::savina::piprecision::min_terms}};
    spec.idiom_source      = "the nqueens adapter (ready handshake, actor a on VirtualCore a % cores, "
                             "broadcast<qb::KillEvent>()) + qb-api-constraints.md / Pipe.h: bulk data "
                             "behind a pointer member (form=0) or in an allocated_push tail (form=1)";
    spec.idiom_note        = "form=0: a std::vector of limbs moved into a one-bucket Result event; "
                             "form=1: the limbs computed into the tail of getPipe(master)."
                             "allocated_push<ResultTail>(bytes); master on core 0, worker w on "
                             "VirtualCore w % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "worker w lives on VirtualCore w % cores, fixed before start, the master on core 0: with "
        "cores=2 the answers of half the workers cross a core -- form=0 as one bucket and a pointer "
        "the master core then reads through, form=1 as the whole term, which the flush copies into "
        "the master core's mailbox ring; the run's `form` is in its params");

    return qvo::run(argc, argv, std::move(spec), savina_piprecision_qb::body);
}
