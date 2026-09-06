// Axis N probe: how long does a PARKED VirtualCore take to notice a socket becoming readable?
//
// This is the instrument behind docs/TUNING.md section 10. It is a qb-only probe, not a
// cross-framework cell: the question it answers -- what an idle actor core that owns io watchers
// pays to be woken by one of them -- has no counterpart in CAF or SObjectizer, whose schedulers own
// no sockets. It is therefore NOT declared through qvo_add_benchmark and its name does not start
// with "qvo-", so tools/run.py cannot discover it and it can never reach a published table.
//
// One core hosts an echo actor (qb::Actor + use<>::tcp::server). A raw blocking-socket client on
// another thread sleeps `gap` between requests -- longer than the idle-spin floor, so the core has
// parked in Mailbox::wait() when the request lands -- then sends one line and times the round trip.
// The control is the same exchange with gap < idle spin (the core is polling) and with latency 0
// (the core never parks). The difference between the two is what axis N (park inside the ev loop)
// buys: before it, a parked core with io watchers slept on the mailbox's condition variable for
// its whole `latency`, and a readable socket had no way to end that sleep.
//
//   qvoprobe-parked-io-wake <latency_us> <gap_us> [rounds] [idle_spin_us] [core_cpu client_cpu]
//
// The core is pinned to `core_cpu` (default 0) and the client thread to `client_cpu` (default 2)
// by the probe ITSELF, and that is load-bearing: a client that spins its gap on the core's own CPU
// keeps the core from running through its idle period at all, so the core never sees the idle-spin
// floor elapse, never parks, and the control reads as if there were nothing to fix. Measured on
// Windows with only a process-wide affinity mask of {0, 2}: the scheduler placed the client on the
// core's CPU in a per-launch random fraction of rounds, and the control's `latency=1000 gap=200`
// cell answered p50 19-20 us with a p90 anywhere between 23 us and 1.8 ms from one launch to the
// next. Linux's load balancer spreads the two busy threads and hides the same trap.
//
// One line on stdout: min / p50 / mean / p90 / p99 / max of the round trip, in microseconds.
// Run it on a quiet host; every figure in section 10 is p50 over 2000 rounds after 50 warm-ups.

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/socket.h>
#include <qb/main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

// Pin the calling thread to one CPU. macOS has no thread affinity API; the probe says so once
// and runs unpinned there, which docs/TUNING.md section 10 records as that host's caveat.
bool pin_current_thread(int cpu) {
#if defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void) cpu;
    return false;
#endif
}

using namespace qb::io;

class EchoActor;

class EchoSession : public use<EchoSession>::tcp::client<EchoActor> {
public:
    using Protocol = qb::protocol::text::command<EchoSession>;
    explicit EchoSession(IOServer &server) : client(server) {}
    void on(Protocol::message &&msg) { *this << msg.text << Protocol::end; }
};

class EchoActor : public qb::Actor, public use<EchoActor>::tcp::server<EchoSession> {
    std::atomic<std::uint16_t> *_port;

public:
    explicit EchoActor(std::atomic<std::uint16_t> *port) : _port(port) {}

    qb::io::async::task<bool> onInit() override {
        if (transport().listen_v4(0, "127.0.0.1") != 0)
            co_return false;
        start();
        _port->store(transport().local_endpoint().port(), std::memory_order_release);
        co_return true;
    }
    void on(IOSession &) {}
};

void sleep_precise(std::chrono::microseconds us) {
    // A hybrid sleep: OS sleep for the bulk, then spin the tail so the gap is what we asked for
    // (Windows Sleep() granularity is 1 ms even with timeBeginPeriod(1)).
    const auto until = std::chrono::steady_clock::now() + us;
    if (us > std::chrono::milliseconds(2))
        std::this_thread::sleep_for(us - std::chrono::milliseconds(2));
    while (std::chrono::steady_clock::now() < until)
        ;
}

} // namespace

int main(int argc, char **argv) {
    const long long latency_us = argc > 1 ? std::atoll(argv[1]) : 1000;
    const long long gap_us     = argc > 2 ? std::atoll(argv[2]) : 2000;
    const int       rounds     = argc > 3 ? std::atoi(argv[3]) : 2000;
    const long long spin_us    = argc > 4 ? std::atoll(argv[4]) : -1;
    const int       core_cpu   = argc > 5 ? std::atoi(argv[5]) : 0;
    const int       client_cpu = argc > 6 ? std::atoi(argv[6]) : 2;

    if (!pin_current_thread(client_cpu))
        std::fprintf(stderr, "note: client thread NOT pinned to cpu %d (unsupported here)\n", client_cpu);

    std::atomic<std::uint16_t> port{0};

    qb::Main engine;
    auto &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{latency_us}});
    if (spin_us >= 0)
        core.setIdleSpin(qb::duration{std::chrono::microseconds{spin_us}});
    core.addActor<EchoActor>(&port);
    engine.start();

    while (port.load(std::memory_order_acquire) == 0 && !engine.hasError())
        std::this_thread::yield();
    if (engine.hasError()) {
        std::fprintf(stderr, "engine failed to start\n");
        return 1;
    }

    tcp::socket sock;
    if (sock.connect_v4("127.0.0.1", port.load()) != 0) {
        std::fprintf(stderr, "connect failed\n");
        qb::Main::stop();
        engine.join();
        return 1;
    }
    sock.set_nonblocking(false);

    const std::string req = "ping-0123456789\n";
    char               buf[64];

    auto round_trip = [&]() -> double {
        const auto t0 = std::chrono::steady_clock::now();
        if (sock.write(req.data(), req.size()) != static_cast<int>(req.size()))
            return -1.0;
        std::size_t got = 0;
        while (got < req.size()) {
            const int n = sock.read(buf + got, sizeof(buf) - got);
            if (n <= 0)
                return -1.0;
            got += static_cast<std::size_t>(n);
        }
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };

    // warm-up
    for (int i = 0; i < 50; ++i)
        if (round_trip() < 0) {
            std::fprintf(stderr, "round trip failed\n");
            return 1;
        }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    for (int i = 0; i < rounds; ++i) {
        sleep_precise(std::chrono::microseconds{gap_us});
        const double us = round_trip();
        if (us < 0) {
            std::fprintf(stderr, "round trip failed\n");
            return 1;
        }
        samples.push_back(us);
    }

    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) { return samples[std::min(samples.size() - 1, static_cast<std::size_t>(p * samples.size()))]; };
    double mean = 0;
    for (double s : samples)
        mean += s;
    mean /= static_cast<double>(samples.size());
    std::printf("latency=%lldus gap=%lldus idle_spin=%s rounds=%d  min=%.1f p50=%.1f mean=%.1f p90=%.1f p99=%.1f max=%.1f (us)\n",
                latency_us, gap_us, spin_us >= 0 ? (std::to_string(spin_us) + "us").c_str() : "default", rounds,
                samples.front(), pct(0.5), mean, pct(0.9), pct(0.99), samples.back());

    sock.disconnect();
    qb::Main::stop();
    engine.join();
    return 0;
}
