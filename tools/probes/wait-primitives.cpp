// Wait-primitives probe: what does an idle poll of a peer's cache line cost, by primitive -- on arm64 (the macOS half
// of Huly QB-181, docs/TUNING.md section 23) and on x86-64 (the x86 half, section 28). Two threads, two 128-byte-aligned
// lines (A publishes on line a, B on line b), a ping-pong of N round trips: A publishes k on a and waits for b to read k;
// B waits for a to read k and publishes k on b. What the probe varies is the one thing a spinning VirtualCore's idle
// pass decides -- what sits between two loads of the peer's line.
//
// arm64:
//   tight    nothing: a plain acquire-load loop (the speculative poll).
//   ordered  one `isb; mrs cntvct_el0` between two loads -- qb's 3.3 pacing read (`qb::detail::
//            tsc_ticks_ordered`) at the idle poll's floor of 0.
//   yield    one `yield` between two loads -- what `qb::spin_loop_pause()` is on arm64.
//   wfe      `ldaxr` on the line, then `wfe`: the exclusive monitor arms on the load and a store
//            to the line by the peer clears it, which is an event; the core sleeps in between.
//   dmb      one `dmb ishld; mrs cntvct_el0` between two loads -- a load-load barrier before the
//            counter read, the ordering without `isb`'s instruction synchronisation.
//   bound    `wfe` with nobody storing to the line, 2000 samples: how long it sleeps before the
//            generic-timer event stream (or an interrupt) wakes it -- the wake bound of a wait on
//            the wrong line, and the reason a Mailbox with several producers needs one doorbell
//            line before `wfe` can carry its idle pass.
//
// x86-64:
//   tight    nothing: the speculative poll.
//   ordered  one `lfence; rdtsc` between two loads -- qb's 3.3 pacing read on x86 at floor 0.
//   pause    one `pause` between two loads -- `qb::spin_loop_pause()` on x86 (33 ns on Golden Cove, QB-180).
//   tpause   `tpause` (C0.1, the light state) to a deadline `ticks` TSC ticks ahead between two loads: the pause of a
//            chosen length, where `pause` has a fixed one -- the pacing primitive QB-181 asks about (WAITPKG).
//   umwait   `umonitor` on the peer's line, then `umwait` (C0.1) to a far deadline: the core sleeps until the line is
//            written, the deadline passes or an interrupt lands -- the hardware wait on the line (WAITPKG).
//   bound    `umwait` with nobody storing to the line, 2000 samples, deadline about a millisecond ahead: how long it
//            sleeps before something else wakes it -- the wake bound of a wait on the wrong line (a Mailbox has N - 1
//            producer lines; `umonitor` watches one).
//   WAITPKG is read from CPUID.(7,0):ECX[5] at start; without it the three modes that need it print `waitpkg absent`
//   and the probe exits 2 after the others ran. The two threads are pinned to the first two CPUs of the process's
//   affinity set (qb's rule: core i on the i-th CPU it may run on), on Linux and on Windows. `tpause_ticks` defaults to
//   40 (about 13 ns at the i9-12900K's 3.2 GHz TSC: the length of the clock read it would replace).
//
// A qb-only instrument like pass-cost, xcore-hop and raw-ring: not a cross-framework cell, tools/run.py cannot discover
// it, its figure can never reach a published table. On any other ISA it prints why and exits 2.
//
//   qvoprobe-wait-primitives [rounds=2000000] [launches=5] [mode=all|<one of the ISA's modes>] [tpause_ticks=40]
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

enum class Wait { Tight, Ordered, Yield, Wfe, Dmb };

inline std::uint64_t
ordered_counter() {
    std::uint64_t val;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(val) : : "memory");
    return val;
}

inline std::uint64_t
dmb_counter() {
    std::uint64_t val;
    __asm__ __volatile__("dmb ishld\n\tmrs %0, cntvct_el0" : "=r"(val) : : "memory");
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
        case Wait::Dmb:
            while (line.v.load(std::memory_order_acquire) != want) {
                ++polls;
                (void) dmb_counter();
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
    const Mode modes[] = {{"tight", Wait::Tight}, {"ordered", Wait::Ordered}, {"yield", Wait::Yield}, {"wfe", Wait::Wfe}, {"dmb", Wait::Dmb}};
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

#elif defined(__x86_64__) || defined(_M_X64)

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

#if defined(_WIN32)
#include <immintrin.h>
#include <intrin.h>
#include <windows.h>
#else
#include <cpuid.h>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#endif

namespace {

struct alignas(128) Line {
    std::atomic<std::uint64_t> v{0};
    char                       pad[120];
};
Line line_a, line_b;

enum class Wait { Tight, Ordered, Pause, Tpause, Umwait };

bool
has_waitpkg() {
#if defined(_WIN32)
    int r[4];
    __cpuidex(r, 7, 0);
    return ((r[2] >> 5) & 1) != 0;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d))
        return false;
    return ((c >> 5) & 1) != 0;
#endif
}

// The CPUs this process may run on, in order: the two threads take the first two (qb's placement rule).
std::vector<int>
allowed_cpus() {
    std::vector<int> cpus;
#if defined(_WIN32)
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
        for (int i = 0; i < 64; ++i)
            if ((process >> i) & 1u)
                cpus.push_back(i);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set))
                cpus.push_back(i);
#endif
    return cpus;
}

void
pin_to(int const cpu) {
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

inline std::uint64_t
ordered_counter() {
#if defined(_WIN32)
    _mm_lfence();
    return __rdtsc();
#else
    std::uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#endif
}

#if defined(_WIN32)
inline void
tpause_until(std::uint64_t const deadline) {
    _tpause(1u /* C0.1 */, deadline);
}
inline void
umwait_line(void const *line, std::uint64_t const deadline) {
    _umonitor(const_cast<void *>(line));
    _umwait(1u /* C0.1 */, deadline);
}
#else
__attribute__((target("waitpkg"))) inline void
tpause_until(std::uint64_t const deadline) {
    _tpause(1u /* C0.1 */, deadline);
}
__attribute__((target("waitpkg"))) inline void
umwait_line(void const *line, std::uint64_t const deadline) {
    _umonitor(const_cast<void *>(line));
    _umwait(1u /* C0.1 */, deadline);
}
#endif

// Wait until the line holds `want`, by the chosen primitive; returns the loads it made.
inline std::uint64_t
wait_for(Line &line, std::uint64_t const want, Wait const w, std::uint64_t const tpause_ticks) {
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
        case Wait::Pause:
            while (line.v.load(std::memory_order_acquire) != want) {
                ++polls;
                _mm_pause();
            }
            return polls;
        case Wait::Tpause:
            while (line.v.load(std::memory_order_acquire) != want) {
                ++polls;
                tpause_until(__rdtsc() + tpause_ticks);
            }
            return polls;
        case Wait::Umwait:
            while (line.v.load(std::memory_order_acquire) != want) {
                ++polls;
                umwait_line(&line.v, __rdtsc() + (std::uint64_t{1} << 24)); // ~5 ms at 3.2 GHz: the far deadline
            }
            return polls;
    }
    return polls;
}

double
run_once(Wait const w, std::uint64_t const rounds, std::uint64_t const tpause_ticks, int const cpu_a, int const cpu_b, double &polls_a_out,
         double &polls_b_out) {
    line_a.v.store(0);
    line_b.v.store(0);
    std::atomic<int> go{0};
    std::uint64_t    polls_b = 0;
    std::thread      b([&] {
        pin_to(cpu_b);
        while (go.load(std::memory_order_acquire) == 0) {
        }
        for (std::uint64_t k = 1; k <= rounds; ++k) {
            polls_b += wait_for(line_a, k, w, tpause_ticks);
            line_b.v.store(k, std::memory_order_release);
        }
    });
    pin_to(cpu_a);
    std::uint64_t polls_a = 0;
    const auto    t0      = std::chrono::steady_clock::now();
    go.store(1, std::memory_order_release);
    for (std::uint64_t k = 1; k <= rounds; ++k) {
        line_a.v.store(k, std::memory_order_release);
        polls_a += wait_for(line_b, k, w, tpause_ticks);
    }
    const auto t1 = std::chrono::steady_clock::now();
    b.join();
    polls_a_out = static_cast<double>(polls_a) / static_cast<double>(rounds);
    polls_b_out = static_cast<double>(polls_b) / static_cast<double>(rounds);
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(rounds);
}

void
bound(int const samples) {
    // `umwait` on a line nobody writes, deadline about a millisecond ahead: the sleep ends on an interrupt, on the
    // deadline, or on whatever else the core is woken for.
    std::vector<double> ns;
    ns.reserve(static_cast<std::size_t>(samples));
    Line idle;
    for (int i = 0; i < samples; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        umwait_line(&idle.v, __rdtsc() + 3'200'000ull); // ~1 ms at 3.2 GHz
        const auto t1 = std::chrono::steady_clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    const auto q = [&](double const p) { return ns[static_cast<std::size_t>(p * static_cast<double>(ns.size() - 1))]; };
    std::printf("%-8s umwait with no store, %d samples: min %.0f  p10 %.0f  median %.0f  p90 %.0f  max %.0f ns\n", "bound", samples,
                ns.front(), q(0.10), q(0.50), q(0.90), ns.back());
}

} // namespace

int
main(int argc, char **argv) {
    const std::uint64_t rounds       = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const int           launches     = argc > 2 ? std::atoi(argv[2]) : 5;
    const std::string   only         = argc > 3 ? argv[3] : "all";
    const std::uint64_t tpause_ticks = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 40;
    const bool          waitpkg      = has_waitpkg();
    const auto          cpus         = allowed_cpus();
    if (cpus.size() < 2) {
        std::fprintf(stderr, "qvoprobe-wait-primitives: two CPUs in the process's affinity set are needed\n");
        return 2;
    }
    std::printf("x86-64: waitpkg %s, threads pinned to CPUs %d and %d, tpause %llu ticks\n", waitpkg ? "yes" : "no", cpus[0], cpus[1],
                static_cast<unsigned long long>(tpause_ticks));
    struct Mode {
        const char *name;
        Wait        w;
        bool        needs_waitpkg;
    };
    const Mode modes[] = {{"tight", Wait::Tight, false},
                          {"ordered", Wait::Ordered, false},
                          {"pause", Wait::Pause, false},
                          {"tpause", Wait::Tpause, true},
                          {"umwait", Wait::Umwait, true}};
    int        rc      = 0;
    for (const auto &m : modes) {
        if (only != "all" && only != m.name)
            continue;
        if (m.needs_waitpkg && !waitpkg) {
            std::printf("%-8s waitpkg absent\n", m.name);
            rc = 2;
            continue;
        }
        std::vector<double> v;
        double              pa = 0, pb = 0, pa_sum = 0, pb_sum = 0;
        for (int r = 0; r < launches; ++r) {
            v.push_back(run_once(m.w, rounds, tpause_ticks, cpus[0], cpus[1], pa, pb));
            pa_sum += pa;
            pb_sum += pb;
        }
        std::sort(v.begin(), v.end());
        std::printf("%-8s round trip ns: median %.1f  min %.1f  max %.1f  (%d launches x %llu rounds)  loads of the peer's line per trip: a %.1f b %.1f\n",
                    m.name, v[v.size() / 2], v.front(), v.back(), launches, static_cast<unsigned long long>(rounds), pa_sum / launches,
                    pb_sum / launches);
        std::fflush(stdout);
    }
    if (only == "all" || only == "bound") {
        if (waitpkg) {
            pin_to(cpus[0]);
            bound(2000);
        } else {
            std::printf("%-8s waitpkg absent\n", "bound");
            rc = 2;
        }
    }
    return rc;
}

#else

#include <cstdio>

int
main() {
    std::puts("qvoprobe-wait-primitives: an arm64 (isb/mrs cntvct_el0, yield, ldaxr/wfe) and x86-64 (lfence/rdtsc, pause, tpause, "
              "umonitor/umwait) instrument; this build is neither. See docs/TUNING.md sections 23 and 28.");
    return 2;
}

#endif
