// QB-48 probe: how often does an IDLE core actually wake, for a given `latency`, through each of its two
// parks?
//
// A qb-only instrument (the schedulers of CAF and SObjectizer park on their own terms), so it is NOT
// declared through qvo_add_benchmark and its name does not start with "qvo-". docs/TUNING.md section 19
// is its reading guide, beside `parked-timer-wake`.
//
// `parked-timer-wake` measures the lateness of a TIMER on a parked core -- the loop park (a core that
// owns io watchers sleeps in `ev_run(EVRUN_ONCE)` capped by `latency`). This one asks the plainer
// question the tuning guide has to answer: a core with NOTHING to do and `setLatency(L)` -- how long
// does it really sleep between two looks at its mailbox? The answer is the wait's granularity on the
// park the core takes: the CONDITION-VARIABLE park when the core owns no io watcher (`std::
// condition_variable::wait_for`, which MSVC rounds to whole milliseconds and hands to
// `SleepConditionVariableSRW`), the LOOP park when it owns one (a far `async::callback` here). Both
// are asked for `latency`; what they deliver is what this prints.
//
// One core, one actor with a registered `on(LoopEvent)` callback, the idle-spin floor set to zero so
// that every wake is exactly one pass (a wait that returns with nothing to do parks again on the next
// pass): passes per second IS wakes per second, and its inverse the mean park. Nothing else runs on
// the core; the process sleeps `seconds` on the main thread and then stops the engine.
//
//   qvoprobe-parked-cadence <latency_us> <cv|loop> [seconds=2] [core_cpu=0]
//
// Prints one line: the park kind, `latency`, the passes counted, the seconds, wakes/s and the mean
// park in microseconds (1e6 / wakes per second).
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

struct Config {
    bool                       hold_loop = false;
    std::atomic<std::uint64_t> passes{0};
};

class CountingActor
    : public qb::Actor
    , public qb::ICallback {
    Config &_cfg;

public:
    explicit CountingActor(Config *cfg)
        : _cfg(*cfg) {}

    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        if (_cfg.hold_loop)
            qb::io::async::callback([] {}, std::chrono::hours{1}); // an io watcher the core owns: the loop park
        co_return true;
    }

    void
    on(qb::LoopEvent const &) override {
        _cfg.passes.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

int
main(int argc, char **argv) {
    const long long latency_us = argc > 1 ? std::atoll(argv[1]) : 1000;
    const char     *park       = argc > 2 ? argv[2] : "cv";
    const double    seconds    = argc > 3 ? std::atof(argv[3]) : 2.0;
    const int       core_cpu   = argc > 4 ? std::atoi(argv[4]) : 0;
    Config          cfg;
    cfg.hold_loop = std::strcmp(park, "loop") == 0;

    qb::Main engine;
    auto    &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{latency_us}});
    core.setIdleSpin(qb::duration::zero());
    core.addActor<CountingActor>(&cfg);
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{200}); // the core is up and idle
    cfg.passes.store(0, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>{seconds});
    const auto   passes  = cfg.passes.load(std::memory_order_relaxed);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    qb::Main::stop();
    engine.join();
    if (engine.hasError()) {
        std::fprintf(stderr, "engine failed\n");
        return 1;
    }
    const double per_s = static_cast<double>(passes) / elapsed;
    std::printf("park=%s latency=%lldus passes=%llu seconds=%.2f wakes_per_s=%.1f mean_park=%.1fus\n", cfg.hold_loop ? "loop" : "cv",
                latency_us, static_cast<unsigned long long>(passes), elapsed, per_s, per_s > 0 ? 1e6 / per_s : 0.0);
    return 0;
}
