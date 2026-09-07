// Raw ring probe: what does the SHAPE of a cross-core ring cost, with no qb code in the loop?
//
// The instrument behind docs/TUNING.md section 16 (Huly QB-184). Two threads, one per pinned
// CPU, exchange a token through two bounded rings of 64-byte slots — the shape of qb's mailbox
// ring — and the probe varies exactly one thing at a time: how the producer publishes, what the
// consumer does between two polls, and whether the producer re-reads the line it publishes on.
// A qb-only instrument like pass-cost and xcore-hop: it is not a cross-framework cell, tools/run.py
// cannot discover it, and its figure can never reach a published table. Linux / x86-64 only (it
// pins with pthread_setaffinity_np and needs movdir64b and AVX2 for two of the schemes); elsewhere
// it prints why and exits 2.
//
//   scheme   2line   producer: write the slot, then a separate index line (release), then a full
//                    fence (qb's notify()); consumer: poll the index line, then read the slot.
//                    This is qb's spsc ring.
//            1line   producer: write the payload, then a lap-tagged sequence in the SAME line
//                    (release); consumer: poll the slot's sequence. One line per hop.
//            1lineF  as 1line, but the whole line is written by ONE movdir64b (a direct store: no
//                    read-for-ownership on the producer) followed by sfence.
//            1lineA  as 1line, but the line is written by two 32-byte AVX2 stores, the half holding
//                    the sequence last.
//   gap      what the consumer does between two polls: 0 nothing (a tight poll), 1 `pause`,
//            2 `lfence; rdtsc`, 3 ~30 ns of dependent ALU work, 4 `rdtsc` alone, 5 `lfence; rdtsc`
//            then ~15 ns of ALU work, 6 one clock_gettime(CLOCK_MONOTONIC) (qb::mono_now()),
//            7 the clock then six independent L1-hit loads with compares, 8 the six loads then
//            the clock, 9 the six loads alone.
//   preload  1: before its store the producer loads the index line it publishes on — what
//            qb's spsc::enqueue did until 3.2 (write_index_ and its snapshot shared that line).
//   jitter   A-side random busy-wait 0..jitter_ns before each hop (rdtsc-paced, reported net):
//            breaks the phase lock of a two-thread ping-pong. See xcore-hop.cpp.
//
//   qvoprobe-raw-ring <2line|1line|1lineF|1lineA> [seconds=2] [cpu_a=0] [cpu_b=2] [jitter_ns=0] [gap=0] [preload=0]
//
// One line on stdout: scheme, preload, gap, round trips, ns per round trip, jitter, net ns per trip.

#if defined(__linux__) && defined(__x86_64__)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <x86intrin.h>

namespace {

inline std::uint64_t
now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr std::size_t N = 1024;

struct alignas(64) Slot {
    std::uint64_t seq; // 1line*: lap-tagged sequence, written last
    std::uint64_t payload[7];
};
struct alignas(128) Ring2 { // the two-line handshake
    alignas(128) std::atomic<std::uint64_t> widx{0};
    alignas(128) std::atomic<std::uint64_t> ridx{0};
    alignas(128) Slot slots[N];
};
struct alignas(128) Ring1 { // the one-line handshake
    alignas(128) Slot slots[N];
};

void
pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

enum Scheme { S2, S1, S1F, S1A };

// Six loads from six distinct lines nobody writes (a signal flag, a stop token, io counters,
// an activation list, a callback list, pipe cursors -- the checks of a qb pass), with compares.
alignas(64) volatile unsigned g_checks[6 * 16] = {};
unsigned                       g_sink          = 0;
int                            g_preload       = 0;

inline unsigned
cheap_checks() {
    unsigned acc = 0;
    for (int i = 0; i < 6; ++i)
        acc += (g_checks[i * 16] != 0);
    return acc;
}

inline void
idle_gap(int mode) {
    switch (mode) {
    case 1:
        _mm_pause();
        break;
    case 2:
        _mm_lfence();
        (void)__rdtsc();
        break;
    case 3: {
        volatile unsigned x = 1;
        for (int i = 0; i < 24; ++i)
            x = x * 3 + 1;
    } break;
    case 4:
        (void)__rdtsc();
        break;
    case 5: {
        _mm_lfence();
        (void)__rdtsc();
        volatile unsigned x = 1;
        for (int i = 0; i < 12; ++i)
            x = x * 3 + 1;
    } break;
    case 6:
        (void)now_ns();
        break;
    case 7:
        (void)now_ns();
        g_sink += cheap_checks();
        break;
    case 8:
        g_sink += cheap_checks();
        (void)now_ns();
        break;
    case 9:
        g_sink += cheap_checks();
        break;
    default:
        break;
    }
}

// ---- two lines ----
inline void
send2(Ring2 &r, std::uint64_t &w, std::uint64_t v) {
    if (g_preload) {
        const auto cur = r.widx.load(std::memory_order_relaxed); // the producer re-reads its own published line
        if (cur != w)
            __builtin_trap();
    }
    r.slots[w % N].payload[0] = v;
    r.widx.store(w + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst); // qb's notify() fence
    ++w;
}
inline std::uint64_t
recv2(Ring2 &r, std::uint64_t &rd, int gap) {
    while (r.widx.load(std::memory_order_acquire) == rd)
        idle_gap(gap);
    const auto v = r.slots[rd % N].payload[0];
    ++rd;
    return v;
}
// ---- one line, two stores ----
inline void
send1(Ring1 &r, std::uint64_t &w, std::uint64_t v) {
    Slot &s       = r.slots[w % N];
    s.payload[0]  = v;
    reinterpret_cast<std::atomic<std::uint64_t> &>(s.seq).store(w + 1, std::memory_order_release); // strictly increasing
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ++w;
}
inline std::uint64_t
recv1(Ring1 &r, std::uint64_t &rd, int gap) {
    Slot &s = r.slots[rd % N];
    while (reinterpret_cast<std::atomic<std::uint64_t> &>(s.seq).load(std::memory_order_acquire) != rd + 1)
        idle_gap(gap);
    const auto v = s.payload[0];
    ++rd;
    return v;
}
// ---- one line, movdir64b ----
inline void
send1F(Ring1 &r, std::uint64_t &w, std::uint64_t v) {
    alignas(64) Slot src;
    src.seq        = w + 1;
    src.payload[0] = v;
    _movdir64b(&r.slots[w % N], &src);
    _mm_sfence();
    ++w;
}
// ---- one line, two 32-byte AVX2 stores: the payload half first, then the half holding seq ----
inline void
send1A(Ring1 &r, std::uint64_t &w, std::uint64_t v) {
    alignas(64) Slot src;
    src.seq        = w + 1;
    src.payload[0] = v;
    auto *dst      = reinterpret_cast<char *>(&r.slots[w % N]);
    _mm256_store_si256(reinterpret_cast<__m256i *>(dst + 32),
                       _mm256_load_si256(reinterpret_cast<const __m256i *>(reinterpret_cast<char *>(&src) + 32)));
    _mm256_store_si256(reinterpret_cast<__m256i *>(dst), _mm256_load_si256(reinterpret_cast<const __m256i *>(&src)));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ++w;
}

void
send(Scheme s, Ring2 &r2, Ring1 &r1, std::uint64_t &w, std::uint64_t v) {
    switch (s) {
    case S2:
        send2(r2, w, v);
        break;
    case S1:
        send1(r1, w, v);
        break;
    case S1F:
        send1F(r1, w, v);
        break;
    case S1A:
        send1A(r1, w, v);
        break;
    }
}

} // namespace

int
main(int argc, char **argv) {
    const char  *sname  = argc > 1 ? argv[1] : "2line";
    const double secs   = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int    cpu_a  = argc > 3 ? std::atoi(argv[3]) : 0;
    const int    cpu_b  = argc > 4 ? std::atoi(argv[4]) : 2;
    const long   jitter = argc > 5 ? std::atol(argv[5]) : 0;
    const int    gap    = argc > 6 ? std::atoi(argv[6]) : 0;
    g_preload           = argc > 7 ? std::atoi(argv[7]) : 0;
    Scheme scheme       = S2;
    if (!std::strcmp(sname, "1line"))
        scheme = S1;
    else if (!std::strcmp(sname, "1lineF"))
        scheme = S1F;
    else if (!std::strcmp(sname, "1lineA"))
        scheme = S1A;
    else if (std::strcmp(sname, "2line")) {
        std::fprintf(stderr, "usage: %s <2line|1line|1lineF|1lineA> [seconds=2] [cpu_a=0] [cpu_b=2] [jitter_ns=0] [gap=0] [preload=0]\n",
                     argv[0]);
        return 2;
    }
    if ((scheme == S1F && !__builtin_cpu_supports("movdir64b")) || (scheme == S1A && !__builtin_cpu_supports("avx2"))) {
        std::fprintf(stderr, "%s: this CPU lacks the instruction that scheme needs\n", argv[0]);
        return 2;
    }

    // TSC calibration for the jitter: ticks per ns over a 50 ms window of the monotonic clock.
    double ns_per_tick = 0;
    {
        const auto m0 = now_ns();
        const auto c0 = __rdtsc();
        while (now_ns() - m0 < 50'000'000) {
        }
        const auto m1 = now_ns();
        const auto c1 = __rdtsc();
        ns_per_tick   = static_cast<double>(m1 - m0) / static_cast<double>(c1 - c0);
    }
    const std::uint64_t jitter_ticks = jitter > 0 ? static_cast<std::uint64_t>(static_cast<double>(jitter) / ns_per_tick) : 0;

    auto *ab2 = new Ring2;
    auto *ba2 = new Ring2;
    auto *ab1 = new Ring1;
    auto *ba1 = new Ring1;
    std::memset(ab1, 0, sizeof(Ring1));
    std::memset(ba1, 0, sizeof(Ring1));

    std::thread tb([&] {
        pin(cpu_b);
        std::uint64_t rd = 0, w = 0;
        for (;;) {
            const std::uint64_t v = scheme == S2 ? recv2(*ab2, rd, gap) : recv1(*ab1, rd, gap);
            if (v == ~0ull)
                break;
            send(scheme, *ba2, *ba1, w, v);
        }
    });
    pin(cpu_a);
    std::uint64_t rd = 0, w = 0, trips = 0, jitter_spent = 0, rng = 0x9E3779B97F4A7C15ull;
    const auto    t0      = now_ns();
    std::uint64_t elapsed = 0;
    for (;;) {
        if (jitter_ticks) {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            const std::uint64_t wait = (rng * 2685821657736338717ull) % jitter_ticks;
            const auto          c0   = __rdtsc();
            while (__rdtsc() - c0 < wait) {
            }
            jitter_spent += wait;
        }
        send(scheme, *ab2, *ab1, w, trips);
        if (scheme == S2)
            recv2(*ba2, rd, gap);
        else
            recv1(*ba1, rd, gap);
        ++trips;
        if ((trips & 0xFFF) == 0) {
            elapsed = now_ns() - t0;
            if (elapsed >= static_cast<std::uint64_t>(secs * 1e9))
                break;
        }
    }
    send(scheme, *ab2, *ab1, w, ~0ull);
    tb.join();
    const double jit_ns = static_cast<double>(jitter_spent) * ns_per_tick;
    std::printf("%s preload=%d gap=%d trips=%llu ns_per_trip=%.2f jitter_ns=%.0f net_ns_per_trip=%.2f\n", sname, g_preload, gap,
                static_cast<unsigned long long>(trips), static_cast<double>(elapsed) / static_cast<double>(trips), jit_ns,
                (static_cast<double>(elapsed) - jit_ns) / static_cast<double>(trips));
    return 0;
}

#else

#include <cstdio>

int
main() {
    std::fprintf(stderr, "qvoprobe-raw-ring: Linux / x86-64 only (pthread affinity, movdir64b, AVX2)\n");
    return 2;
}

#endif
