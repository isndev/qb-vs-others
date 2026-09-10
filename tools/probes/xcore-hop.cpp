// Cross-core hop probe: what does a `push<>` hop between two cores cost over a `send<>` hop?
//
// The companion of pass-cost.cpp (docs/TUNING.md section 15, Huly QB-182/QB-183). A qb-only
// probe, not a cross-framework cell, for the same reason: the quantity it measures -- the gap
// between qb's two cross-core transports -- has no counterpart in CAF or SObjectizer. It is NOT
// declared through qvo_add_benchmark and its name does not start with "qvo-", so tools/run.py
// cannot discover it and it can never reach a published table.
//
// Two actors, one per core, each core pinned to its own CPU and in spin mode unless told
// otherwise. A token goes A -> B -> A; every hop crosses cores, and both ends build a fresh
// event with the same call, so the two modes differ in exactly one thing:
//
//   send    `send<Token>(peer)`: the eager transport -- the event goes straight into the peer
//           core's mailbox ring from inside the handler (VirtualCore::try_send).
//   push    `push<Token>(peer)`: the ordered transport -- the event lands in this core's
//           outbound pipe for the peer and leaves in the pass's flush (`__flush_all__`), so
//           ns(push) - ns(send) is what the pipe and the flush's PLACE IN THE PASS cost a hop,
//           the quantity QB-183 moves.
//
// Nothing reads a clock inside the window except every 4096 round trips at the A end. Every
// figure is nanoseconds per round trip (two hops) over a window of `seconds`, on a quiet host,
// and the two-core cells of the Savina grids cannot resolve it: their launch-to-launch spread
// is +/-3 to 5 % where the expected effect is a few nanoseconds of a ~100 ns hop.
//
// A two-actor ping-pong PHASE-LOCKS: the producer's store lands at a fixed point of the
// consumer's idle-pass cadence, so a change of a few nanoseconds anywhere in the pass can move
// the round trip by a whole poll period (measured: +80 ns for a 5 ns delay before the flush,
// docs/TUNING.md section 16). `jitter_ns` breaks the lock: before every hop the A end
// busy-waits a uniformly random 0..jitter_ns (rdtsc-paced, calibrated at start), and the
// figure reported NET of that wait is the latency at a random phase -- what a real workload,
// whose stores are not synchronised to its peers' polls, actually sees. Compare variants at
// the same jitter; 0 (the default) reproduces the locked figure.
//
//   qvoprobe-xcore-hop <push|send> [seconds=2] [cpu_a=0] [cpu_b=2] [latency_us=0] [jitter_ns=0]
//
// One line on stdout: mode, round trips, elapsed ns, ns per round trip, jitter, net ns per trip.
//
// x86-64 only for real (the jitter is `rdtsc`-paced and the TSC is what it calibrates); elsewhere
// the same source builds to a stub that says so and exits 2, like raw-ring.cpp -- measured on
// arm64/AppleClang 21, where <x86intrin.h> does not compile and, with QVO_BUILD_PROBES on by
// default, this one TU took the whole qb-vs-others build with it.

#if defined(__x86_64__) || defined(_M_X64)

#include <qb/actor.h>
#include <qb/main.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace {

struct Token : qb::Event {};

// The B end: answer every token with a fresh one, through the transport under test.
class Echo : public qb::Actor {
    const bool _ordered;

public:
    explicit Echo(bool ordered)
        : _ordered(ordered) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Token>(*this);
        co_return true;
    }

    void
    on(Token const &event) {
        if (_ordered)
            push<Token>(event.getSource());
        else
            send<Token>(event.getSource());
    }
};

// The A end: starts the token, counts round trips, checks the deadline every 4096 of them.
class Origin : public qb::Actor {
    const qb::ActorId   _peer;
    const bool          _ordered;
    const std::uint64_t _window_ns;
    const std::uint64_t _jitter_ticks; // 0 = no jitter
    const double        _ns_per_tick;
    std::uint64_t       _trips        = 0;
    std::uint64_t       _jitter_spent = 0; // TSC ticks busy-waited on the critical path
    std::uint64_t       _rng          = 0x9E3779B97F4A7C15ull;
    qb::mono_time       _t0{};

public:
    Origin(qb::ActorId peer, bool ordered, std::uint64_t window_ns, std::uint64_t jitter_ticks, double ns_per_tick)
        : _peer(peer)
        , _ordered(ordered)
        , _window_ns(window_ns)
        , _jitter_ticks(jitter_ticks)
        , _ns_per_tick(ns_per_tick) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Token>(*this);
        _t0 = qb::mono_now();
        hop();
        co_return true;
    }

    void
    on(Token const &) {
        ++_trips;
        if ((_trips & 0xFFFu) == 0) {
            const auto now     = qb::mono_now();
            const auto elapsed = static_cast<std::uint64_t>((now - _t0).count());
            if (elapsed >= _window_ns) {
                const double jitter_ns = static_cast<double>(_jitter_spent) * _ns_per_tick;
                std::printf("%s trips=%llu elapsed_ns=%llu ns_per_trip=%.2f jitter_ns=%.0f net_ns_per_trip=%.2f\n",
                            _ordered ? "push" : "send", static_cast<unsigned long long>(_trips),
                            static_cast<unsigned long long>(elapsed), static_cast<double>(elapsed) / static_cast<double>(_trips),
                            jitter_ns, (static_cast<double>(elapsed) - jitter_ns) / static_cast<double>(_trips));
                std::fflush(stdout);
                send<qb::KillEvent>(_peer);
                kill();
                return;
            }
        }
        hop();
    }

private:
    void
    hop() {
        if (_jitter_ticks) {
            // xorshift64*: cheap, and its quality is irrelevant here -- any spread over the
            // poll period breaks the lock.
            _rng ^= _rng >> 12;
            _rng ^= _rng << 25;
            _rng ^= _rng >> 27;
            const std::uint64_t wait = (_rng * 2685821657736338717ull) % _jitter_ticks;
            const auto          t0   = __rdtsc();
            while (__rdtsc() - t0 < wait) {
            }
            _jitter_spent += wait;
        }
        if (_ordered)
            push<Token>(_peer);
        else
            send<Token>(_peer);
    }
};

} // namespace

int
main(int argc, char **argv) {
    const bool ordered = argc > 1 && std::strcmp(argv[1], "push") == 0;
    const bool eager   = argc > 1 && std::strcmp(argv[1], "send") == 0;
    if (!ordered && !eager) {
        std::fprintf(stderr, "usage: %s <push|send> [seconds=2] [cpu_a=0] [cpu_b=2] [latency_us=0] [jitter_ns=0]\n", argv[0]);
        return 2;
    }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int    cpu_a   = argc > 3 ? std::atoi(argv[3]) : 0;
    const int    cpu_b   = argc > 4 ? std::atoi(argv[4]) : 2;
    const long   lat_us  = argc > 5 ? std::atol(argv[5]) : 0;
    const long   jitter  = argc > 6 ? std::atol(argv[6]) : 0;
    const auto   window  = static_cast<std::uint64_t>(seconds * 1e9);

    // TSC calibration for the jitter: ticks per ns over a 50 ms window of the monotonic clock.
    double ns_per_tick = 0.0;
    if (jitter > 0) {
        const auto m0 = qb::mono_now();
        const auto c0 = __rdtsc();
        while ((qb::mono_now() - m0).count() < 50'000'000) {
        }
        const auto m1 = qb::mono_now();
        const auto c1 = __rdtsc();
        ns_per_tick   = static_cast<double>((m1 - m0).count()) / static_cast<double>(c1 - c0);
    }
    const auto jitter_ticks = jitter > 0 ? static_cast<std::uint64_t>(static_cast<double>(jitter) / ns_per_tick) : 0ull;

    qb::Main engine;
    auto &core_a = engine.core(0);
    auto &core_b = engine.core(1);
    core_a.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(cpu_a)});
    core_b.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(cpu_b)});
    core_a.setLatency(qb::duration{std::chrono::microseconds{lat_us}});
    core_b.setLatency(qb::duration{std::chrono::microseconds{lat_us}});
    const auto peer = core_b.addActor<Echo>(ordered);
    core_a.addActor<Origin>(peer, ordered, window, jitter_ticks, ns_per_tick);
    engine.start();
    engine.join();
    return engine.hasError() ? 1 : 0;
}

#else

#include <cstdio>

int
main() {
    std::fprintf(stderr, "qvoprobe-xcore-hop: x86-64 only (the jitter is rdtsc-paced)\n");
    return 2;
}

#endif
