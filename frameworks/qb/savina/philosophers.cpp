// @benchmark     savina/philosophers
// @framework     qb
// @idiom-source  the chameneos and big adapters beside this file (one event mutated into its own
//                answer and `reply()`ed) and qb/llm/qb.llm.md for `push<>`, `reply()` and
//                `kill()`; Actor.h documents reply() as "the most efficient way to send a
//                response back to the sender of an event".
// @idiom-note    ONE event per philosopher carries the whole conversation: Hungry goes to the
//                arbitrator, which mutates it into Eat or Denied and `reply()`s it; the
//                philosopher mutates a Denied back into Hungry and `reply()`s it (the busy retry),
//                and an Eat into Done and `reply()`s it -- so a request, its answer, its retry and
//                its release never allocate a second event. Start (to itself) and Exit are fresh
//                `push<>`es. Not `qb::ask` from a coroutine: that adds a frame resume and a
//                correlation lookup to every request, and the refused requests are most of the
//                traffic here -- bank-transaction is the adapter that measures ask. The
//                arbitrator lives alone on VirtualCore 0 and the philosophers on the others, so
//                with cores=2 every request and every answer crosses a core.

#include <qvospec/savina/philosophers.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <limits>
#include <vector>

namespace savina_philosophers_qb {

using namespace qvospec::savina::philosophers;

enum Step : std::uint8_t { kHungry = 0, kGranted = 1, kDenied = 2, kReleased = 3 };

// The one hot event: Hungry going in, Eat / Denied coming back, Hungry again (a retry) or Done
// (the forks released) going in once more.
struct Request : qb::Event {
    std::uint32_t philosopher;
    Step          step{kHungry};
    explicit Request(std::uint32_t p) noexcept : philosopher(p) {}
};
struct Start : qb::Event {};
struct Ready : qb::Event {};
struct Exit : qb::Event {
    std::uint32_t philosopher;
    std::uint64_t chk;
    std::uint64_t received;
    Exit(std::uint32_t p, std::uint64_t c, std::uint64_t r) noexcept
        : philosopher(p), chk(c), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t refused{0};
};

struct Field {
    std::vector<qb::ActorId> philosophers;
    qb::ActorId              arbitrator;
};

class Philosopher final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _index;
    const std::uint64_t _rounds;
    std::uint64_t       _starts{0};
    std::uint64_t       _eats{0};
    std::uint64_t       _chk{0};
    std::uint64_t       _received{0};

public:
    Philosopher(const Field &field, std::uint32_t index, std::uint64_t rounds) noexcept
        : _field(field), _index(index), _rounds(rounds) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Start>(*this);
        registerEvent<Request>(*this);
        push<Ready>(_field.arbitrator);
        co_return true;
    }

    void on(Start const &) {
        ++_received;
        _chk += term(kStart, _index, ++_starts);
        push<Request>(_field.arbitrator, _index);
    }

    void on(Request &event) {
        if (event.step == kDenied) {
            event.step = kHungry;
            reply(event);
            return;
        }
        ++_received;
        _chk += term(kEat, _index, ++_eats);
        // Done(i) is the Eat recycled, and it leaves BEFORE the Exit below: reply() publishes
        // into the arbitrator's ring now (or appends to the pipe), push<> appends to the pipe
        // after it -- the arbitrator sees this philosopher's last Done before its Exit.
        event.step = kReleased;
        reply(event);
        if (_eats < _rounds) {
            push<Start>(id());
            return;
        }
        push<Exit>(_field.arbitrator, _index, _chk, _received);
        kill();
    }
};

class Arbitrator final : public qb::Actor {
    static constexpr std::uint32_t kFree = std::numeric_limits<std::uint32_t>::max();

    const Field               &_field;
    const std::uint32_t        _n;
    qvo::Watch                &_watch;
    Sink                      &_sink;
    std::vector<std::uint32_t> _owner;   // per fork: the philosopher holding it, or kFree
    std::vector<std::uint64_t> _grants;  // per philosopher
    std::vector<std::uint64_t> _dones;   // per philosopher
    std::uint64_t              _chk{0};
    std::uint64_t              _received{0};
    std::uint64_t              _refused{0};  // observed, not asserted: the scheduler's number
    std::size_t                _ready{0};
    std::size_t                _exited{0};
    bool                       _violation{false};

public:
    Arbitrator(const Field &field, std::uint32_t n, qvo::Watch &watch, Sink &sink)
        : _field(field)
        , _n(n)
        , _watch(watch)
        , _sink(sink)
        , _owner(n, kFree)
        , _grants(n, 0)
        , _dones(n, 0) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Request>(*this);
        registerEvent<Exit>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _n) return;
        _watch.start();
        for (const auto &p : _field.philosophers) push<Start>(p);
    }

    void on(Request &event) {
        const std::uint32_t i     = event.philosopher;
        const std::uint32_t left  = i;
        const std::uint32_t right = (i + 1) % _n;
        if (event.step == kHungry) {
            if (_owner[left] != kFree || _owner[right] != kFree) {
                ++_refused;
                event.step = kDenied;
                reply(event);
                return;
            }
            _owner[left] = _owner[right] = i;
            ++_received;
            _chk += term(kGrant, i, ++_grants[i]);
            event.step = kGranted;
            reply(event);
            return;
        }
        // kReleased: Done(i).
        ++_received;
        _chk += term(kDone, i, ++_dones[i]);
        if (_owner[left] != i || _owner[right] != i) _violation = true;
        _owner[left] = _owner[right] = kFree;
    }

    void on(Exit const &event) {
        ++_received;
        _chk += term(kExit, event.philosopher, _dones[event.philosopher]) + event.chk;
        _received += event.received;
        if (++_exited != _n) return;
        _watch.stop();
        _sink.checksum = _chk + (_violation ? kForkViolation : 0);
        _sink.messages = _received;
        _sink.refused  = _refused;
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto n      = static_cast<std::uint32_t>(p.get("philosophers"));
    const auto rounds = static_cast<std::uint64_t>(p.get("rounds"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.arbitrator =
            engine.addActor<Arbitrator>(0, std::cref(field), n, std::ref(watch), std::ref(sink));
        field.philosophers.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            // The arbitrator has core 0 to itself when there is more than one; the philosophers
            // share the rest, so every request and every answer crosses a core.
            const int core = ncores == 1 ? 0 : 1 + static_cast<int>(i) % (ncores - 1);
            field.philosophers.push_back(engine.addActor<Philosopher>(
                static_cast<qb::CoreId>(core), std::cref(field), i, rounds));
        }

        engine.start();
        engine.join();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedRefused] = sink.refused;
    return answer;
}

}  // namespace savina_philosophers_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::philosophers::params();
    spec.expected          = qvospec::savina::philosophers::expected;
    spec.expected_messages = qvospec::savina::philosophers::expected_messages;
    spec.work_unit         = qvospec::savina::philosophers::kWorkUnit;
    spec.work_units        = qvospec::savina::philosophers::work_units;
    spec.idiom_source      = "the chameneos/big adapters (reply() recycling) + qb/llm/qb.llm.md";
    spec.idiom_note        = "one Request event per philosopher, mutated Hungry -> Eat/Denied at "
                             "the arbitrator and Denied -> Hungry / Eat -> Done at the "
                             "philosopher, each time reply()ed; Start and Exit are push<>; "
                             "arbitrator alone on VirtualCore 0, philosophers on the others";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "placement is fixed before start: the arbitrator alone on VirtualCore 0 and the "
        "philosophers on the remaining cores (all on core 0 when cores=1), so with cores=2 every "
        "request and every answer crosses a core -- the pools place the arbitrator and the "
        "philosophers wherever stealing puts them");
    spec.caveats.emplace_back(
        "how many requests the arbitrator REFUSES (Savina's 'Num retries') depends on the "
        "interleaving: it is REPORTED beside the cell (observed `refused`), never asserted; every "
        "refused request and its retry are delivered and timed, so a framework whose scheduling "
        "makes philosophers collide more often does more work in the same cell -- that is the "
        "benchmark, as Savina defines it, and the observation says how much more");

    return qvo::run(argc, argv, std::move(spec), savina_philosophers_qb::body);
}
