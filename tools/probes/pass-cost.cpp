// Pass-cost probe: what does ONE pass of a VirtualCore cost, and what does each event it carries
// add to it?
//
// This is the instrument behind docs/TUNING.md section 15 (Huly QB-182). It is a qb-only probe,
// not a cross-framework cell: the quantity it measures -- the fixed cost of the engine's loop pass,
// which every actor hop on a lightly loaded core pays in full and a saturated core amortises over a
// burst -- has no counterpart in CAF or SObjectizer, whose schedulers have no such pass. It is
// therefore NOT declared through qvo_add_benchmark and its name does not start with "qvo-", so
// tools/run.py cannot discover it and it can never reach a published table.
//
// One core, pinned, in spin mode unless told otherwise, hosts one actor. Two ways to count passes:
//
//   <k>     k independent self-event chains: the actor pushes each event it receives back to
//           itself, so every pass carries EXACTLY k events through the self pipe, the router and
//           the handler, and the handler counts them -- passes = events / k. No tick is registered
//           and nothing reads a clock inside the window (the handler checks the deadline every
//           65 536 events). With k = 1 and k = 2 the two unknowns separate: the per-event cost is
//           ns(2) - ns(1) and the fixed per-pass cost is 2 * ns(1) - ns(2).
//   tick    no event at all: the actor registers the per-pass tick (`qb::ICallback`) and counts
//           `LoopEvent::iteration`. This pass is NOT the fixed cost alone: the tick phase samples
//           the wall clock for `LoopEvent::now` and an idle pass reads the idle clock (the pacing
//           QB-180 measured), so it reports what a callback-driven idle core actually pays.
//
// Every figure is nanoseconds per pass over a window of `seconds` measured with the monotonic
// clock, on a quiet host.
//
//   qvoprobe-pass-cost <k|tick> [seconds=2] [core_cpu=0] [latency_us=0]
//
// One line on stdout: mode, passes, elapsed ns, ns per pass, events.

#include <qb/actor.h>
#include <qb/main.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Tick : qb::Event {};

class ChainCounter : public qb::Actor {
    const unsigned      _chains;
    const std::uint64_t _window_ns;
    std::uint64_t       _events = 0;
    qb::mono_time       _t0{};

public:
    ChainCounter(unsigned chains, std::uint64_t window_ns)
        : _chains(chains)
        , _window_ns(window_ns) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        _t0 = qb::mono_now();
        for (unsigned i = 0; i < _chains; ++i)
            push<Tick>(id());
        co_return true;
    }

    void
    on(Tick &) {
        ++_events;
        if ((_events & 0xFFFFu) == 0) {
            const auto now     = qb::mono_now();
            const auto elapsed = static_cast<std::uint64_t>((now - _t0).count());
            if (elapsed >= _window_ns) {
                const double passes = static_cast<double>(_events) / _chains;
                std::printf("chains=%u passes=%.0f elapsed_ns=%llu ns_per_pass=%.2f events=%llu\n", _chains, passes,
                            static_cast<unsigned long long>(elapsed), static_cast<double>(elapsed) / passes,
                            static_cast<unsigned long long>(_events));
                std::fflush(stdout);
                kill();
                return;
            }
        }
        push<Tick>(id()); // keep exactly `_chains` events in flight: one per chain per pass
    }
};

class TickCounter : public qb::Actor, public qb::ICallback {
    const std::uint64_t _window_ns;
    std::uint64_t       _first_pass = 0;
    qb::mono_time       _t0{};

public:
    explicit TickCounter(std::uint64_t window_ns)
        : _window_ns(window_ns) {}

    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &ev) override {
        if (_first_pass == 0) {
            _first_pass = ev.iteration;
            _t0         = qb::mono_now();
            return;
        }
        if (((ev.iteration - _first_pass) & 0xFFFFu) != 0)
            return;
        const auto now     = qb::mono_now();
        const auto elapsed = static_cast<std::uint64_t>((now - _t0).count());
        if (elapsed < _window_ns)
            return;
        const auto passes = ev.iteration - _first_pass;
        std::printf("tick passes=%llu elapsed_ns=%llu ns_per_pass=%.2f events=0\n", static_cast<unsigned long long>(passes),
                    static_cast<unsigned long long>(elapsed), static_cast<double>(elapsed) / static_cast<double>(passes));
        std::fflush(stdout);
        kill();
    }
};

} // namespace

int
main(int argc, char **argv) {
    const bool     tick   = argc > 1 && std::strcmp(argv[1], "tick") == 0;
    const unsigned chains = (argc > 1 && !tick) ? static_cast<unsigned>(std::atoi(argv[1])) : 0u;
    if (argc < 2 || (!tick && (chains == 0 || chains > 1024))) {
        std::fprintf(stderr, "usage: %s <k|tick> [seconds=2] [core_cpu=0] [latency_us=0]   (k = self-event chains, 1..1024)\n", argv[0]);
        return 2;
    }
    const double seconds  = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int    core_cpu = argc > 3 ? std::atoi(argv[3]) : 0;
    const long   lat_us   = argc > 4 ? std::atol(argv[4]) : 0;
    const auto   window   = static_cast<std::uint64_t>(seconds * 1e9);

    qb::Main engine;
    auto &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{lat_us}});
    if (tick)
        core.addActor<TickCounter>(window);
    else
        core.addActor<ChainCounter>(chains, window);
    engine.start();
    engine.join();
    return engine.hasError() ? 1 : 0;
}
