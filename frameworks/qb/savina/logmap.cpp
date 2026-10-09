// @benchmark     savina/logmap
// @framework     qb
// @idiom-source  qb/src/qb/core/Actor.h (`reply()`: "the most efficient way to send a response
//                back to the sender of an event. The original event object is reused") and
//                qb/llm/qb.llm.md -- "reply vs forward" (both reuse the received event; `reply`
//                swaps dest and source), "`send<T>()` is unordered; `push<T>()` is ordered", and
//                "Put the actors that talk to each other most on the SAME core".
// @idiom-note    The ask is qb's own request/reply on ONE event: the worker pushes
//                `Compute{term}` to its computer, the computer overwrites the term with the next
//                one and `reply()`s the same event, and the worker, when it still owes terms,
//                writes its new term into that event and `reply()`s it back -- so a series' whole
//                chain of round trips recycles the event in place and allocates nothing. `push`,
//                not `send`, everywhere: the master's burst must reach each worker in order
//                (every NextTerm before the GetTerm), and a round trip's two hops stay on one core
//                where `send` has nothing to publish early. No `qb::ask`: its correlation registry
//                and coroutine resume buy nothing for a worker with one request in flight and one
//                peer (bank-transaction is the shape that prices `qb::ask`). A worker and its
//                computer talk to nobody else, so they share a VirtualCore -- series i on core
//                (1 + i) % cores, the master on core 0 -- and no round trip crosses a core; the
//                held NextTerm requests are a count (they carry nothing).

#include <qvospec/savina/logmap.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_logmap_qb {

using namespace qvospec::savina::logmap;

struct Ready : qb::Event {};     // handshake, outside the window
struct NextTerm : qb::Event {};  // master -> worker: compute one more term
struct GetTerm : qb::Event {};   // master -> worker: answer once everything asked is done
// worker -> computer: the current term; computer -> worker, the same event: the next one.
struct Compute : qb::Event {
    double term;
    explicit Compute(double t) noexcept : term(t) {}
};
// worker -> master: the answer to GetTerm.
struct Result : qb::Event {
    std::uint32_t index;
    std::uint64_t chain;
    std::uint64_t received;
    std::uint64_t held;
    Result(std::uint32_t i, std::uint64_t c, std::uint64_t r, std::uint64_t h) noexcept
        : index(i), chain(c), received(r), held(h) {}
};
struct Exit : qb::Event {};  // master -> computer: Savina's StopMessage, answered by a Report
struct Report : qb::Event {
    std::uint32_t index;
    std::uint64_t served;
    std::uint64_t received;
    Report(std::uint32_t i, std::uint64_t s, std::uint64_t r) noexcept
        : index(i), served(s), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t held{0};
};

// The ids, filled before the engine starts (an id is assigned at addActor time, the actor is
// constructed at start) and read-only from then on.
struct Field {
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
    std::vector<qb::ActorId> computers;
};

class Computer final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _index;
    const double        _rate;
    std::uint64_t       _served{0};
    std::uint64_t       _received{0};

public:
    Computer(const Field &field, std::uint32_t index) noexcept
        : _field(field), _index(index), _rate(rate_of(index)) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Compute>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    // Savina's RateComputer: the next term, back to whoever asked.
    void on(Compute &event) {
        ++_received;
        ++_served;
        event.term = next_term(event.term, _rate);
        reply(event);
    }

    void on(Exit const &) {
        ++_received;
        push<Report>(_field.master, _index, _served, _received);
    }
};

class Worker final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _index;
    double              _term;
    std::uint64_t       _chain;
    std::uint64_t       _owed{0};      // NextTerm requests held while an answer is awaited
    bool                _awaiting{false};
    bool                _get_pending{false};
    std::uint64_t       _received{0};
    std::uint64_t       _held{0};

    void answer() { push<Result>(_field.master, _index, _chain, _received, _held); }

public:
    Worker(const Field &field, std::uint32_t index) noexcept
        : _field(field), _index(index), _term(start_of(index)), _chain(chain_seed(index)) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<NextTerm>(*this);
        registerEvent<GetTerm>(*this);
        registerEvent<Compute>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(NextTerm const &) {
        ++_received;
        if (_awaiting) {
            ++_owed;
            ++_held;
            return;
        }
        _awaiting = true;
        push<Compute>(_field.computers[_index], _term);
    }

    // The computer's answer. Still owing a term: the same event goes straight back with it.
    void on(Compute &event) {
        ++_received;
        _term  = event.term;
        _chain = chain_step(_chain, _term);
        if (_owed) {
            --_owed;
            reply(event);  // event.term is already the term to grow from
            return;
        }
        _awaiting = false;
        if (_get_pending) answer();
    }

    void on(GetTerm const &) {
        ++_received;
        if (_awaiting)
            _get_pending = true;  // answered when the last answer lands (nothing is owed then)
        else
            answer();
    }
};

class Master final : public qb::Actor {
    const Field        &_field;
    const std::uint64_t _terms;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::size_t         _ready{0};
    std::size_t         _results{0};
    std::size_t         _reports{0};
    std::uint64_t       _received{0};

public:
    Master(const Field &field, std::uint64_t terms, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _terms(terms), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Result>(*this);
        registerEvent<Report>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.workers.size() + _field.computers.size()) return;
        _watch.start();
        // The reference's loop: term by term, worker by worker, then one GetTerm each.
        for (std::uint64_t k = 0; k < _terms; ++k)
            for (const auto &w : _field.workers) push<NextTerm>(w);
        for (const auto &w : _field.workers) push<GetTerm>(w);
    }

    void on(Result const &event) {
        ++_received;
        _sink.checksum += series_key(event.index, event.chain);
        _sink.messages += event.received;
        _sink.held += event.held;
        if (++_results != _field.workers.size()) return;
        for (const auto &c : _field.computers) push<Exit>(c);
    }

    void on(Report const &event) {
        ++_received;
        _sink.checksum += computer_key(event.index, event.served);
        _sink.messages += event.received;
        if (++_reports != _field.computers.size()) return;
        _sink.messages += _received;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto terms  = static_cast<std::uint64_t>(p.get("terms"));
    const auto series = static_cast<std::uint32_t>(p.get("series"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.master = engine.addActor<Master>(0, std::cref(field), terms, std::ref(watch),
                                               std::ref(sink));
        field.workers.reserve(series);
        field.computers.reserve(series);
        for (std::uint32_t i = 0; i < series; ++i) {
            // Series i -- its worker and its computer, which talk to nobody else -- on one core.
            const auto core = static_cast<qb::CoreId>((1 + static_cast<int>(i)) % ncores);
            field.computers.push_back(engine.addActor<Computer>(core, std::cref(field), i));
            field.workers.push_back(engine.addActor<Worker>(core, std::cref(field), i));
        }

        engine.start();
        engine.join();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kHeld] = sink.held;
    return answer;
}

}  // namespace savina_logmap_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::logmap::params();
    spec.expected          = qvospec::savina::logmap::expected;
    spec.expected_messages = qvospec::savina::logmap::expected_messages;
    spec.work_unit         = qvospec::savina::logmap::kWorkUnit;
    spec.work_units        = qvospec::savina::logmap::work_units;
    spec.idiom_source      = "qb/src/qb/core/Actor.h reply() + qb/llm/qb.llm.md (reply vs forward, "
                             "push vs send, same-core placement)";
    spec.idiom_note        = "push<Compute> to the computer, which reply()s the same event with "
                             "the next term; a worker still owing terms reply()s it straight "
                             "back, so a chain recycles one event; held NextTerms are a count; "
                             "master on VirtualCore 0, series i (worker + computer) on core "
                             "(1 + i) % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "placement is fixed before start: the master on VirtualCore 0, series i's worker AND "
        "computer together on core (1 + i) % cores, so no round trip of a chain crosses a core "
        "-- qb.llm.md's rule for actors that talk only to each other; the pools place each "
        "worker and computer freely and may split a pair");
    spec.caveats.emplace_back(
        "the master's NextTerm burst (terms x series pushes from one handler) reaches a remote "
        "core at the handler's end, through the pass's flush; the chains on that core start "
        "after it, as they do in every framework whose master sends the whole burst first");

    return qvo::run(argc, argv, std::move(spec), savina_logmap_qb::body);
}
