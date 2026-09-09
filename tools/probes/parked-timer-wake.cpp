// QB-196 probe: how late does a PARKED VirtualCore fire a timer it owns?
//
// The instrument behind docs/TUNING.md section 19 and Huly QB-196. A qb-only probe, not a
// cross-framework cell (the schedulers of CAF and SObjectizer own no timers on their workers), so it
// is NOT declared through qvo_add_benchmark and its name does not start with "qvo-".
//
// `parked-io-wake` (section 10) asks what a parked core pays to be woken by a SOCKET: nothing since
// axis N, the backend's wait ends on the completion. This one asks the other question: a core that
// has nothing to do but a TIMER due in `delay` idles through its idle-spin floor, then parks with
// the park bounded by that timer -- and wakes when the backend's wait says so. The lateness measured
// here is the granularity of that wait. What it found: `epoll_wait` takes whole milliseconds and
// libev rounds up, so a 100 us timer on a parked core fired 1010 us late on Linux until qev asked
// the kernel in nanoseconds (`epoll_pwait2`, 60 us since -- the thread's timer slack); on Windows
// the same ceiling reads 1.0-1.4 ms at p50 and 2.4 at p99 whatever the system timer resolution
// says (15.625 ms when it was read), the kernel's waits being tickless and its coalescing the
// rest. The controls are latency 0 (the core never parks and judges the timer on the busy pass's
// clock) and idle_spin 0 (the core parks on its first idle pass, so the wait is the whole story).
//
// One core, one actor. Each round arms one libev timer of `delay` on the core's own loop from inside
// the previous timer's callback, and records its lateness: fired-at minus (armed-at + delay), in
// microseconds. Nothing else runs on the core.
//
//   qvoprobe-parked-timer-wake <latency_us> <delay_us> [rounds=2000] [idle_spin_us=-1] [core_cpu=0] [timer_period_ms=0]
//
// [timer_period_ms] > 0 calls timeBeginPeriod(period) on Windows before measuring -- the "raise the
// resolution" option of QB-196, process-wide; measured, it moves the p50 by 1-30 % and the tails not
// at all -- and is ignored elsewhere. The core is pinned to
// `core_cpu` by the probe itself. Run it on a quiet host; every figure recorded is p50 over 2000
// rounds after 50 warm-ups.
//
// One line on stdout: min / p50 / mean / p90 / p99 / max of the lateness, in microseconds.
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#endif

namespace {

struct Config {
    long long           delay_us = 100;
    int                 rounds   = 2000;
    int                 warmups  = 50;
    std::vector<double> samples;
    std::atomic<bool>   done{false};
};

class TimerActor : public qb::Actor {
    Config                               &_cfg; // addActor copies its arguments into a tuple: the Config comes by pointer
    int                                   _round = 0;
    std::chrono::steady_clock::time_point _armed;

    void
    arm() {
        _armed = std::chrono::steady_clock::now();
        qb::io::async::callback([this] { fired(); }, std::chrono::microseconds{_cfg.delay_us});
    }

    void
    fired() {
        const auto   now      = std::chrono::steady_clock::now();
        const double lateness = std::chrono::duration<double, std::micro>(now - _armed).count() - static_cast<double>(_cfg.delay_us);
        if (_round >= _cfg.warmups)
            _cfg.samples.push_back(lateness);
        if (++_round >= _cfg.warmups + _cfg.rounds) {
            _cfg.done.store(true, std::memory_order_release);
            kill();
            return;
        }
        arm();
    }

public:
    explicit TimerActor(Config *cfg)
        : _cfg(*cfg) {}

    qb::io::async::task<bool>
    onInit() override {
        _cfg.samples.reserve(static_cast<std::size_t>(_cfg.rounds));
        arm();
        co_return true;
    }
};

} // namespace

int
main(int argc, char **argv) {
    const long long latency_us = argc > 1 ? std::atoll(argv[1]) : 1000;
    Config          cfg;
    cfg.delay_us              = argc > 2 ? std::atoll(argv[2]) : 100;
    cfg.rounds                = argc > 3 ? std::atoi(argv[3]) : 2000;
    const long long spin_us   = argc > 4 ? std::atoll(argv[4]) : -1;
    const int       core_cpu  = argc > 5 ? std::atoi(argv[5]) : 0;
    const int       period_ms = argc > 6 ? std::atoi(argv[6]) : 0;

#if defined(_WIN32)
    if (period_ms > 0 && timeBeginPeriod(static_cast<UINT>(period_ms)) != TIMERR_NOERROR)
        std::fprintf(stderr, "note: timeBeginPeriod(%d) refused\n", period_ms);
#else
    (void) period_ms;
#endif

    qb::Main engine;
    auto    &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{latency_us}});
    if (spin_us >= 0)
        core.setIdleSpin(qb::duration{std::chrono::microseconds{spin_us}});
    core.addActor<TimerActor>(&cfg);
    engine.start();
    engine.join();
    if (engine.hasError() || !cfg.done.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "engine failed\n");
        return 1;
    }

#if defined(_WIN32)
    if (period_ms > 0)
        timeEndPeriod(static_cast<UINT>(period_ms));
#endif

    auto &samples = cfg.samples;
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        return samples[std::min(samples.size() - 1, static_cast<std::size_t>(p * samples.size()))];
    };
    double mean = 0;
    for (double s : samples)
        mean += s;
    mean /= static_cast<double>(samples.size());
    std::printf("latency=%lldus delay=%lldus idle_spin=%s period=%dms rounds=%d  lateness min=%.1f p50=%.1f mean=%.1f p90=%.1f p99=%.1f "
                "max=%.1f (us)\n",
                latency_us, cfg.delay_us, spin_us >= 0 ? (std::to_string(spin_us) + "us").c_str() : "default", period_ms, cfg.rounds,
                samples.front(), pct(0.5), mean, pct(0.9), pct(0.99), samples.back());
    return 0;
}
