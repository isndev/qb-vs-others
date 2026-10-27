// Wait-primitives probe: what does an idle poll of a peer's cache line cost on arm64, by primitive?
//
// The instrument behind the macOS half of Huly QB-181 (docs/TUNING.md section 23). Two threads,
// two 128-byte-aligned lines (A publishes on line a, B on line b), a ping-pong of N round trips:
// A publishes k on a and waits for b to read k; B waits for a to read k and publishes k on b. What
// the probe varies is the one thing a spinning VirtualCore's idle pass decides -- what sits between
// two loads of the peer's line:
//
//   tight    nothing: a plain acquire-load loop (the speculative poll).
//   ordered  one `isb; mrs cntvct_el0` between two loads -- qb's 3.3 pacing read (`qb::detail::
//            tsc_ticks_ordered`) at the idle poll's floor of 0.
//   yield    one `yield` between two loads -- what `qb::spin_loop_pause()` is on arm64.
//   wfe      `ldaxr` on the line, then `wfe`: the exclusive monitor arms on the load and a store
//            to the line by the peer clears it, which is an event; the core sleeps in between.
//   bound    `wfe` with nobody storing to the line, 2000 samples: how long it sleeps before the
//            generic-timer event stream (or an interrupt) wakes it -- the wake bound of a wait on
//            the wrong line, and the reason a Mailbox with several producers needs one doorbell
//            line before `wfe` can carry its idle pass.
//
// A qb-only instrument like pass-cost, xcore-hop and raw-ring: not a cross-framework cell,
// tools/run.py cannot discover it, its figure can never reach a published table. arm64 only (the
// four primitives are arm64 instructions; macOS pins nothing, so the two threads are not placed);
// elsewhere it prints why and exits 2.
//
//   qvoprobe-wait-primitives [rounds=2000000] [launches=5] [mode=all|tight|ordered|yield|wfe|bound]
//
// One line per mode on stdout: the mode, the median / min / max over the launches of ns per round
// trip, and the loads of the peer's line per round trip on each side; for `bound`, the quantiles
// of the sleep in ns.

#if defined(__aarch64__)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct alignas(128) Line {
    std::atomic<std::uint64_t> v{0};
    char                       pad[120];
};
Line line_a, line_b;

enum class Wait { Tight, Ordered, Yield, Wfe };

inline std::uint64_t
ordered_counter() {
    std::uint64_t val;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(val) : : "memory");
    return val;
}

// Wait until the line holds `want`, by the chosen primitive; returns the loads it made.
inline std::uint64_t
wait_for(Line &line, std::uint64_t const want, Wait const w) {
    std::uint64_t polls = 0;
    switch (w) {
        case Wait::Tight:
            while (line.v.load(std::memory_order_acquire) != want)
                ++polls;
            return polls;
        case Wait::Ordered:
            while (line.v.load(std::memory_order_acquire) != want) {
                ++polls;
                (void) ordered_counter();
            }
            return polls;
        case Wait::Yield:
            while (line.v.load(std::memory_order_acquire) != want) {
                ++polls;
                __asm__ volatile("yield" ::: "memory");
            }
            return polls;
        case Wait::Wfe: {
            std::uint64_t cur;
            for (;;) {
                __asm__ __volatile__("ldaxr %0, [%1]" : "=r"(cur) : "r"(&line.v) : "memory");
                if (cur == want)
                    return polls;
                ++polls;
                __asm__ __volatile__("wfe" ::: "memory");
            }
        }
    }
    return polls;
}

double
run_once(Wait const w, std::uint64_t const rounds, double &polls_a_out, double &polls_b_out) {
    line_a.v.store(0);
    line_b.v.store(0);
    std::atomic<int> go{0};
    std::uint64_t    polls_b = 0;
    std::thread      b([&] {
        while (go.load(std::memory_order_acquire) == 0) {
        }
        for (std::uint64_t k = 1; k <= rounds; ++k) {
            polls_b += wait_for(line_a, k, w);
            line_b.v.store(k, std::memory_order_release);
        }
    });
    std::uint64_t polls_a = 0;
    const auto    t0      = std::chrono::steady_clock::now();
    go.store(1, std::memory_order_release);
    for (std::uint64_t k = 1; k <= rounds; ++k) {
        line_a.v.store(k, std::memory_order_release);
        polls_a += wait_for(line_b, k, w);
    }
    const auto t1 = std::chrono::steady_clock::now();
    b.join();
    polls_a_out = static_cast<double>(polls_a) / static_cast<double>(rounds);
    polls_b_out = static_cast<double>(polls_b) / static_cast<double>(rounds);
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(rounds);
}

void
bound(int const samples) {
    std::vector<double> ns;
    ns.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        std::uint64_t cur;
        const auto    t0 = std::chrono::steady_clock::now();
        __asm__ __volatile__("ldaxr %0, [%1]" : "=r"(cur) : "r"(&line_a.v) : "memory");
        __asm__ __volatile__("wfe" ::: "memory");
        const auto t1 = std::chrono::steady_clock::now();
        (void) cur;
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    const auto q = [&](double const p) { return ns[static_cast<std::size_t>(p * static_cast<double>(ns.size() - 1))]; };
    std::printf("%-8s wfe with no store, %d samples: min %.0f  p10 %.0f  median %.0f  p90 %.0f  max %.0f ns\n", "bound", samples, ns.front(), q(0.10),
                q(0.50), q(0.90), ns.back());
}

} // namespace

int
main(int argc, char **argv) {
    const std::uint64_t rounds   = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const int           launches = argc > 2 ? std::atoi(argv[2]) : 5;
    const std::string   only     = argc > 3 ? argv[3] : "all";
    struct Mode {
        const char *name;
        Wait        w;
    };
    const Mode modes[] = {{"tight", Wait::Tight}, {"ordered", Wait::Ordered}, {"yield", Wait::Yield}, {"wfe", Wait::Wfe}};
    for (const auto &m : modes) {
        if (only != "all" && only != m.name)
            continue;
        std::vector<double> v;
        double              pa = 0, pb = 0, pa_sum = 0, pb_sum = 0;
        for (int r = 0; r < launches; ++r) {
            v.push_back(run_once(m.w, rounds, pa, pb));
            pa_sum += pa;
            pb_sum += pb;
        }
        std::sort(v.begin(), v.end());
        std::printf("%-8s round trip ns: median %.1f  min %.1f  max %.1f  (%d launches x %llu rounds)  loads of the peer's line per trip: a %.1f b %.1f\n",
                    m.name, v[v.size() / 2], v.front(), v.back(), launches, static_cast<unsigned long long>(rounds), pa_sum / launches, pb_sum / launches);
        std::fflush(stdout);
    }
    if (only == "all" || only == "bound")
        bound(2000);
    return 0;
}

#else

#include <cstdio>

int
main() {
    std::puts("qvoprobe-wait-primitives: an arm64 instrument (isb/mrs cntvct_el0, yield, ldaxr/wfe); this build is not arm64. "
              "See docs/TUNING.md section 23.");
    return 2;
}

#endif
