// Ask-cost probe: what does a `co_await qb::ask<E>()` round trip cost over a plain push/reply one?
//
// A qb-only probe like pass-cost.cpp and xcore-hop.cpp (docs/TUNING.md section 17, Huly QB-185):
// the quantity it measures -- qb's coroutine request/reply machinery on top of its own event
// passes -- has no counterpart in CAF or SObjectizer. NOT declared through qvo_add_benchmark and
// not `qvo-` prefixed, so tools/run.py cannot discover it.
//
// One core, pinned, spin mode unless told otherwise, two actors on it:
//
//   push   the asker's `on(Echo&)` pushes a fresh Echo to the responder, which `reply()`s it:
//          two passes per round trip and nothing else -- the floor the ask has to pay on top of.
//   ask    the asker spawns ONE coroutine that loops `co_await qb::ask<Echo>(ctx, responder, 0s, i)`
//          (the emplace form: the request is built in the pipe slot); the responder `reply()`s;
//          the asker's `on(Echo&)` routes the answer with `resolve_ask()`. What the loop pays
//          beyond `push` is the ask machinery: the `task<E>` frame, the registry slot, the
//          cancellation hook, the awaiter's delivery and the resume of the waiting frame.
//   stream the asker's coroutine loops `qb::ask_stream<Feed>(ctx, responder, req, 0s)` and drains
//          it with `co_await s.next()`; the responder answers each request with `chunks`
//          `yield_answer()`s and one `end_stream()`. The figure is per CHUNK: what a streamed
//          reply pays over a pushed event -- the buffer, the wake of the parked `next()` and the
//          resume of the consumer -- with the per-stream setup amortised over `chunks`.
//
// Nothing reads a clock inside the window except every 4096 round trips (chunks). One line on
// stdout: mode, round trips (chunks), elapsed ns, ns per round trip (chunk).
//
//   qvoprobe-ask-cost <push|ask|stream> [seconds=2] [core_cpu=0] [latency_us=0] [chunks=64] [timeout_ms=0]
//
//   `timeout_ms` > 0 gives every ask (or every stream `next()`) a deadline, the documented idiom:
//   what the timer costs is the difference against 0.

#include <qb/actor.h>
#include <qb/core/patterns/request.h>
#include <qb/core/patterns/streaming.h>
#include <qb/main.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Echo : qb::AskEvent {
    std::uint64_t seq;
    explicit Echo(std::uint64_t s) noexcept
        : seq(s) {}
};

struct Feed : qb::StreamRequest<std::uint32_t> {
    std::uint32_t count = 0;
};

class Responder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        registerEvent<Feed>(*this);
        co_return true;
    }
    void
    on(Echo &e) {
        reply(e);
    }
    void
    on(Feed &e) {
        for (std::uint32_t i = 0; i < e.count; ++i)
            qb::yield_answer(*this, e, i);
        qb::end_stream(*this, e);
    }
};

// Shared by both modes: counts round trips, checks the deadline every 4096, kills both actors at
// the end. `hop()` is what differs -- a push, or nothing (the coroutine loop asks by itself).
class Asker : public qb::Actor {
protected:
    const qb::ActorId   _peer;
    const std::uint64_t _window_ns;
    const char *const   _mode;
    std::uint64_t       _trips = 0;
    qb::mono_time       _t0{};

    // true when the window is over (and the engine is being torn down)
    bool
    count() {
        ++_trips;
        if ((_trips & 0xFFFu) != 0)
            return false;
        const auto now     = qb::mono_now();
        const auto elapsed = static_cast<std::uint64_t>((now - _t0).count());
        if (elapsed < _window_ns)
            return false;
        std::printf("%s trips=%llu elapsed_ns=%llu ns_per_trip=%.2f\n", _mode, static_cast<unsigned long long>(_trips),
                    static_cast<unsigned long long>(elapsed), static_cast<double>(elapsed) / static_cast<double>(_trips));
        std::fflush(stdout);
        send<qb::KillEvent>(_peer);
        kill();
        return true;
    }

public:
    Asker(qb::ActorId peer, std::uint64_t window_ns, const char *mode)
        : _peer(peer)
        , _window_ns(window_ns)
        , _mode(mode) {}
};

class PushAsker final : public Asker {
public:
    using Asker::Asker;
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        _t0 = qb::mono_now();
        push<Echo>(_peer, 0);
        co_return true;
    }
    void
    on(Echo &e) {
        if (count())
            return;
        push<Echo>(_peer, e.seq + 1);
    }
};

class AskAsker final : public Asker {
    const qb::duration _timeout;

public:
    AskAsker(qb::ActorId peer, std::uint64_t window_ns, const char *mode, qb::duration timeout)
        : Asker(peer, window_ns, mode)
        , _timeout(timeout) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        _t0 = qb::mono_now();
        spawn([this, peer = _peer, timeout = _timeout](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (std::uint64_t i = 0;; ++i) {
                const Echo r = co_await qb::ask<Echo>(ctx, peer, timeout, i);
                if (r.seq != i)
                    std::abort();
                if (count())
                    co_return;
            }
        });
        co_return true;
    }
    void
    on(Echo &e) {
        if (resolve_ask(e))
            return;
        std::abort(); // every Echo the asker receives is the answer to its own ask
    }
};

class StreamAsker final : public Asker {
    const std::uint32_t _chunks;
    const qb::duration  _timeout;

public:
    StreamAsker(qb::ActorId peer, std::uint64_t window_ns, const char *mode, std::uint32_t chunks, qb::duration timeout)
        : Asker(peer, window_ns, mode)
        , _chunks(chunks)
        , _timeout(timeout) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        _t0 = qb::mono_now();
        spawn([this, peer = _peer, chunks = _chunks, timeout = _timeout](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (;;) {
                Feed req;
                req.count = chunks;
                auto          s        = qb::ask_stream(ctx, peer, req, timeout);
                std::uint32_t expected = 0;
                while (auto chunk = co_await s.next()) {
                    if (chunk->chunk != expected++)
                        std::abort();
                    if (count())
                        co_return;
                }
                if (expected != chunks)
                    std::abort();
            }
        });
        co_return true;
    }
    void
    on(Feed &e) {
        if (resolve_ask(e))
            return;
        std::abort();
    }
};

} // namespace

int
main(int argc, char **argv) {
    const bool ask    = argc > 1 && std::strcmp(argv[1], "ask") == 0;
    const bool push   = argc > 1 && std::strcmp(argv[1], "push") == 0;
    const bool stream = argc > 1 && std::strcmp(argv[1], "stream") == 0;
    if (!ask && !push && !stream) {
        std::fprintf(stderr, "usage: %s <push|ask|stream> [seconds=2] [core_cpu=0] [latency_us=0] [chunks=64] [timeout_ms=0]\n", argv[0]);
        return 2;
    }
    const double seconds  = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int    core_cpu = argc > 3 ? std::atoi(argv[3]) : 0;
    const long   lat_us   = argc > 4 ? std::atol(argv[4]) : 0;
    const auto   chunks   = argc > 5 ? static_cast<std::uint32_t>(std::atoi(argv[5])) : 64u;
    const long   to_ms    = argc > 6 ? std::atol(argv[6]) : 0;
    const auto   window   = static_cast<std::uint64_t>(seconds * 1e9);
    const auto   timeout  = qb::duration{std::chrono::milliseconds{to_ms}};

    qb::Main engine;
    auto &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{lat_us}});
    const auto peer = core.addActor<Responder>();
    if (ask)
        core.addActor<AskAsker>(peer, window, to_ms > 0 ? "ask+timeout" : "ask", timeout);
    else if (stream)
        core.addActor<StreamAsker>(peer, window, to_ms > 0 ? "stream+timeout" : "stream", chunks, timeout);
    else
        core.addActor<PushAsker>(peer, window, "push");
    engine.start();
    engine.join();
    return engine.hasError() ? 1 : 0;
}
