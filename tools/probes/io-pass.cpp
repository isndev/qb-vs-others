// qvoprobe-io-pass -- what a core pays per pass for a QUIET socket, and what a byte on that
// socket waits for (Huly QB-191, the io poll cadence).
//
// One core, pinned, spin mode unless told otherwise, one actor on it that owns the accepted end
// of a loopback TCP pair as a raw `event::io` watcher (the shape every qb-io session registers).
// Two modes:
//
//   pass   the actor drives itself with a self-event chain (one event per pass, like pass-cost)
//          while the socket stays quiet: ns per pass = the pass plus whatever the io loop costs
//          for one registered fd -- the backend poll on every pass before QB-191, one per
//          interval after.
//   wake   a peer thread writes an 8-byte monotonic timestamp every `gap_us` while the core spins
//          idle; the handler reads it and records now - sent: the latency of a byte on a quiet
//          socket, p50 / p99 / max over the window. The cadence's cost is here, bounded by one
//          interval; its benefit is the `pass` figure.
//   timer  no socket: the actor holds one far libev timer (`async::callback` an hour out -- the
//          shape a `sleep`, a retry or a keep-alive leaves on a busy core) and drives itself as
//          in `pass`: ns per pass = the pass plus what the loop costs for a timer that is not
//          due (`ev_run` on every pass before QB-190, the inline gate after).
//
// usage: qvoprobe-io-pass <pass|wake|timer> [seconds=2] [core_cpu=0] [latency_us=0] [gap_us=100] [poll_interval_us=default]
//
//   poll_interval_us < 0 keeps the engine's default (1 us on a QB-191 tree, "every pass" before it
//   -- the knob does not exist there, so the option is only honoured when the tree has it).
//
// Prints one line: mode, counts, ns per pass (pass mode) or the latency percentiles (wake mode).

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct Tick : qb::Event {};

std::atomic<bool> g_stop{false};

class Holder : public qb::Actor, public qb::ICallback {
    qb::io::tcp::socket *_sock;
    const std::uint64_t  _window_ns;
    const bool           _pass_mode;
    std::uint64_t        _passes = 0;
    std::uint64_t        _bytes  = 0;
    qb::mono_time        _t0{};
    std::vector<std::uint64_t> _lat;
    qb::io::async::event::io  *_watch = nullptr;

public:
    Holder(qb::io::tcp::socket *sock, std::uint64_t window_ns, bool pass_mode)
        : _sock(sock)
        , _window_ns(window_ns)
        , _pass_mode(pass_mode) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        if (_sock) {
            _watch = &qb::io::async::listener::current.registerEvent<qb::io::async::event::io>(*this);
            _watch->start(_sock->native_handle(), EV_READ);
        } else
            qb::io::async::callback([] {}, std::chrono::hours{1}); // the far timer of `timer` mode
        _lat.reserve(1 << 16);
        _t0 = qb::mono_now();
        if (_pass_mode)
            push<Tick>(id());
        else
            registerCallback(*this); // the window check, once per pass, cheap
        co_return true;
    }

    void
    on(Tick &) {
        ++_passes;
        if ((_passes & 0xFFFu) == 0 && elapsed_ns() >= _window_ns) {
            finish();
            return;
        }
        push<Tick>(id());
    }

    void
    on(const qb::LoopEvent &) override {
        ++_passes;
        if ((_passes & 0xFFu) == 0 && elapsed_ns() >= _window_ns)
            finish();
    }

    void
    on(qb::io::async::event::io &) {
        std::uint64_t sent = 0;
        char          buf[64];
        const auto    n = _sock->read(buf, sizeof buf);
        if (n <= 0)
            return;
        const auto now = static_cast<std::uint64_t>(qb::mono_now().time_since_epoch().count());
        for (int off = 0; off + 8 <= n; off += 8) {
            std::memcpy(&sent, buf + off, 8);
            ++_bytes;
            if (_lat.size() < _lat.capacity())
                _lat.push_back(now - sent);
        }
    }

private:
    [[nodiscard]] std::uint64_t
    elapsed_ns() const {
        return static_cast<std::uint64_t>((qb::mono_now() - _t0).count());
    }
    void
    finish() {
        const auto elapsed = elapsed_ns();
        if (_pass_mode) {
            std::printf("%s passes=%llu elapsed_ns=%llu ns_per_pass=%.2f\n", _sock ? "pass" : "timer", static_cast<unsigned long long>(_passes),
                        static_cast<unsigned long long>(elapsed), static_cast<double>(elapsed) / static_cast<double>(_passes));
        } else {
            std::sort(_lat.begin(), _lat.end());
            const auto pct = [&](double p) { return _lat.empty() ? 0ull : static_cast<unsigned long long>(_lat[std::min(_lat.size() - 1, static_cast<std::size_t>(p * static_cast<double>(_lat.size())))]); };
            std::printf("wake bytes=%llu passes=%llu elapsed_ns=%llu ns_per_pass=%.2f wake_p50_ns=%llu wake_p99_ns=%llu wake_max_ns=%llu\n",
                        static_cast<unsigned long long>(_bytes), static_cast<unsigned long long>(_passes), static_cast<unsigned long long>(elapsed),
                        static_cast<double>(elapsed) / static_cast<double>(_passes), pct(0.5), pct(0.99), _lat.empty() ? 0ull : static_cast<unsigned long long>(_lat.back()));
        }
        std::fflush(stdout);
        g_stop.store(true);
        if (_watch)
            _watch->stop();
        unregisterCallback();
        kill();
    }
};

} // namespace

int
main(int argc, char **argv) {
    const bool pass  = argc > 1 && std::strcmp(argv[1], "pass") == 0;
    const bool wake  = argc > 1 && std::strcmp(argv[1], "wake") == 0;
    const bool timer = argc > 1 && std::strcmp(argv[1], "timer") == 0;
    if (!pass && !wake && !timer) {
        std::fprintf(stderr, "usage: %s <pass|wake|timer> [seconds=2] [core_cpu=0] [latency_us=0] [gap_us=100] [poll_interval_us=default]\n", argv[0]);
        return 2;
    }
    const double seconds  = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int    core_cpu = argc > 3 ? std::atoi(argv[3]) : 0;
    const long   lat_us   = argc > 4 ? std::atol(argv[4]) : 0;
    const long   gap_us   = argc > 5 ? std::atol(argv[5]) : 100;
    const long   poll_us  = argc > 6 ? std::atol(argv[6]) : -1;
    const auto   window   = static_cast<std::uint64_t>(seconds * 1e9);

    // The loopback pair: a listener on an ephemeral port, connect, accept -- all here, before the
    // engine. `timer` mode owns no socket.
    qb::io::tcp::listener acceptor;
    qb::io::tcp::socket   client;
    qb::io::tcp::socket   accepted;
    if (!timer) {
        if (acceptor.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done) {
            std::fprintf(stderr, "listen failed\n");
            return 1;
        }
        if (client.connect_v4("127.0.0.1", acceptor.local_endpoint().port()) != qb::io::SocketStatus::Done) {
            std::fprintf(stderr, "connect failed\n");
            return 1;
        }
        if (acceptor.accept(accepted) != qb::io::SocketStatus::Done) {
            std::fprintf(stderr, "accept failed\n");
            return 1;
        }
        accepted.set_nonblocking(true);
    }

    qb::Main engine;
    auto    &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{lat_us}});
#ifdef QVO_QB_HAS_IO_POLL_INTERVAL
    if (poll_us >= 0)
        core.setIoPollInterval(qb::duration{std::chrono::microseconds{poll_us}});
#else
    (void) poll_us;
#endif
    core.addActor<Holder>(timer ? nullptr : &accepted, window, pass || timer);

    std::thread writer;
    if (wake) {
        writer = std::thread([&client, gap_us] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::microseconds{gap_us});
                const auto now = static_cast<std::uint64_t>(qb::mono_now().time_since_epoch().count());
                client.write(reinterpret_cast<const char *>(&now), 8);
            }
        });
    }
    engine.start();
    engine.join();
    g_stop.store(true);
    if (writer.joinable())
        writer.join();
    return engine.hasError() ? 1 : 0;
}
