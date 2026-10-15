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
//   frames the record-framing control (Huly QB-819), no timing: TCP is a byte stream, so a read
//          can end inside an 8-byte record. The peer writes `kFrameRecords` sequence numbers,
//          record i in two writes split after byte 1 + i % 8 (8: whole), and before each next
//          write waits until the core has read every byte sent so far -- so each split point is
//          IMPOSED on the handler, one fragment per read. The handler reassembles exactly as in
//          `wake`; the probe prints how many records arrived and how many were out of sequence,
//          and exits 1 unless every record arrived, in order, with nothing left over.
//
// usage: qvoprobe-io-pass <pass|wake|timer|frames> [seconds=2] [core_cpu=0] [latency_us=0] [gap_us=100] [poll_interval_us=default]
//
//   poll_interval_us < 0 keeps the engine's default (1 us on a QB-191 tree, "every pass" before it
//   -- the knob does not exist there, so the option is only honoured when the tree has it).
//   `frames` ignores `seconds` and `gap_us`.
//
// Prints one line: mode, counts, ns per pass (pass mode) or the latency percentiles (wake mode).
// In `wake`, `bytes=` is the number of 8-byte timestamps received (the field's historical name).

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

// `frames` mode: the bytes the core has read (the peer's handshake), the peer's end of the sequence,
// and the verdict main() returns.
constexpr std::uint64_t    kFrameRecords = 8 * 64; // each of the eight split points 64 times
std::atomic<std::uint64_t> g_read_bytes{0};
std::atomic<bool>          g_frames_sent{false};
std::atomic<bool>          g_frames_ok{false};

// A stream socket may take fewer bytes than it was given: write the rest rather than drop them.
bool
write_all(qb::io::tcp::socket &sock, const char *data, std::size_t size) {
    while (size > 0) {
        const int n = sock.write(data, size);
        if (n <= 0)
            return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

class Holder : public qb::Actor, public qb::ICallback {
    qb::io::tcp::socket *_sock;
    const std::uint64_t  _window_ns;
    const bool           _pass_mode;
    const bool           _frames_mode;
    std::uint64_t        _passes = 0;
    std::uint64_t        _bytes  = 0; ///< 8-byte records received (timestamps in `wake`)
    std::uint64_t        _out_of_sequence = 0;
    char                 _carry[8]{}; ///< the 1..7 bytes of a record a read ended inside
    std::size_t          _carry_n = 0;
    qb::mono_time        _t0{};
    std::vector<std::uint64_t> _lat;
    qb::io::async::event::io  *_watch = nullptr;

public:
    Holder(qb::io::tcp::socket *sock, std::uint64_t window_ns, bool pass_mode, bool frames_mode)
        : _sock(sock)
        , _window_ns(window_ns)
        , _pass_mode(pass_mode)
        , _frames_mode(frames_mode) {}

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
        if (_frames_mode) {
            if (g_frames_sent.load(std::memory_order_acquire))
                finish();
            return;
        }
        if ((_passes & 0xFFu) == 0 && elapsed_ns() >= _window_ns)
            finish();
    }

    void
    on(qb::io::async::event::io &) {
        // TCP delivers bytes, not records: a read can end inside an 8-byte record. The 1..7 bytes
        // of that record are carried to the next read, ahead of what it returns, instead of being
        // dropped -- which lost the record and misaligned every later one (Huly QB-819).
        char buf[sizeof _carry + 64];
        std::memcpy(buf, _carry, _carry_n);
        const auto n = _sock->read(buf + _carry_n, 64);
        if (n <= 0)
            return;
        const auto        now  = static_cast<std::uint64_t>(qb::mono_now().time_since_epoch().count());
        const std::size_t have = _carry_n + static_cast<std::size_t>(n);
        std::size_t       off  = 0;
        for (; off + 8 <= have; off += 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, buf + off, 8);
            if (_frames_mode) {
                if (value != _bytes)
                    ++_out_of_sequence;
            } else if (_lat.size() < _lat.capacity())
                _lat.push_back(now - value);
            ++_bytes;
        }
        _carry_n = have - off;
        std::memcpy(_carry, buf + off, _carry_n);
        if (_frames_mode)
            g_read_bytes.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_release);
    }

private:
    [[nodiscard]] std::uint64_t
    elapsed_ns() const {
        return static_cast<std::uint64_t>((qb::mono_now() - _t0).count());
    }
    void
    finish() {
        const auto elapsed = elapsed_ns();
        if (_frames_mode) {
            const bool ok = _bytes == kFrameRecords && _out_of_sequence == 0 && _carry_n == 0;
            std::printf("frames records=%llu expected=%llu out_of_sequence=%llu leftover_bytes=%zu %s\n",
                        static_cast<unsigned long long>(_bytes), static_cast<unsigned long long>(kFrameRecords),
                        static_cast<unsigned long long>(_out_of_sequence), _carry_n, ok ? "OK" : "MISMATCH");
            g_frames_ok.store(ok);
        } else if (_pass_mode) {
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
    const bool pass   = argc > 1 && std::strcmp(argv[1], "pass") == 0;
    const bool wake   = argc > 1 && std::strcmp(argv[1], "wake") == 0;
    const bool timer  = argc > 1 && std::strcmp(argv[1], "timer") == 0;
    const bool frames = argc > 1 && std::strcmp(argv[1], "frames") == 0;
    if (!pass && !wake && !timer && !frames) {
        std::fprintf(stderr, "usage: %s <pass|wake|timer|frames> [seconds=2] [core_cpu=0] [latency_us=0] [gap_us=100] [poll_interval_us=default]\n", argv[0]);
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
    core.addActor<Holder>(timer ? nullptr : &accepted, window, pass || timer, frames);

    std::thread       writer;
    std::atomic<bool> writer_ok{true};
    if (wake) {
        writer = std::thread([&client, gap_us] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::microseconds{gap_us});
                const auto now = static_cast<std::uint64_t>(qb::mono_now().time_since_epoch().count());
                if (!write_all(client, reinterpret_cast<const char *>(&now), 8))
                    break;
            }
        });
    } else if (frames) {
        writer = std::thread([&client, &writer_ok] {
            // Waits until the core has read `target` bytes, so the next write starts a new read.
            const auto read_up_to = [](std::uint64_t target) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                while (g_read_bytes.load(std::memory_order_acquire) < target) {
                    if (std::chrono::steady_clock::now() > deadline)
                        return false;
                    std::this_thread::yield();
                }
                return true;
            };
            std::uint64_t sent = 0;
            bool          ok   = true;
            for (std::uint64_t i = 0; ok && i < kFrameRecords; ++i) {
                char record[8];
                std::memcpy(record, &i, 8);
                const std::size_t split = 1 + static_cast<std::size_t>(i % 8); // 8: the record whole
                ok = write_all(client, record, split) && read_up_to(sent += split);
                if (ok && split < 8)
                    ok = write_all(client, record + split, 8 - split) && read_up_to(sent += 8 - split);
            }
            if (!ok)
                std::fprintf(stderr, "frames: the peer could not write, or the core did not read, %llu bytes\n",
                             static_cast<unsigned long long>(sent));
            writer_ok.store(ok);
            g_frames_sent.store(true, std::memory_order_release);
        });
    }
    engine.start();
    engine.join();
    g_stop.store(true);
    if (writer.joinable())
        writer.join();
    if (engine.hasError())
        return 1;
    if (frames)
        return g_frames_ok.load() && writer_ok.load() ? 0 : 1;
    return 0;
}
