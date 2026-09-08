// Dispatch-population probe: what does ONE event cost a core that serves N actors, as N grows past
// what its caches hold?
//
// This is the instrument behind docs/TUNING.md section 18 (Huly QB-198). Like pass-cost it is a
// qb-only probe -- the quantity it isolates, the engine's per-event dispatch over a population
// whose objects have left the cache, has no counterpart in CAF or SObjectizer's schedulers -- so it
// is NOT declared through qvo_add_benchmark, its name does not start with "qvo-", and tools/run.py
// cannot discover it.
//
// One core, pinned, spinning, hosts N `Cell` actors of ~300 bytes (a session-sized object: the
// framework's 96-byte base and a 192-byte payload the handler touches once) and one `Driver`. Each
// pass the driver pushes a BATCH of `Hit` events to cells drawn uniformly at random, then one
// `Round` to itself; the cells' handler counts and touches one line of its payload. So every pass
// dispatches `batch` events over a population of N, and the figure is nanoseconds per event over a
// window measured with the monotonic clock -- the driver's push, the pipe walk, the router, the
// trampoline, the handler -- on a quiet host. N below ~64 keeps every cell in L1 and measures the
// dispatch alone; N in the thousands measures it over cold lines, which is what a core serving one
// actor per connection pays on every event, and what the dispatcher's prefetch (QB-198) is for.
//
//   qvoprobe-dispatch-population <actors> [batch=256] [seconds=2] [core_cpu=0] [latency_us=0]
//
// One line on stdout: actors, batch, rounds, events, elapsed ns, ns per event.

#include <qb/actor.h>
#include <qb/main.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

struct Hit : qb::Event {
    std::uint32_t seq = 0;
    explicit Hit(std::uint32_t const s) noexcept
        : seq(s) {}
};
struct Round : qb::Event {};

class Cell : public qb::Actor {
    std::uint64_t _hits = 0;
    std::uint8_t  _payload[192]{}; // the session's own state: one line of it is touched per hit

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Hit>(*this);
        co_return true;
    }

    void
    on(Hit const &e) {
        ++_hits;
        _payload[(e.seq * 64u) % sizeof _payload] ^= static_cast<std::uint8_t>(e.seq);
    }
};

class Driver : public qb::Actor {
    const std::shared_ptr<const std::vector<qb::ActorId>> _cells; ///< filled by main() before start()
    const std::uint32_t                                   _batch;
    const std::uint64_t                                   _window_ns;
    std::uint64_t                                         _rounds = 0;
    std::uint64_t                                         _lcg    = 0x9E3779B97F4A7C15ull;
    std::uint32_t                                         _seq    = 0;
    qb::mono_time                                         _t0{};

    [[nodiscard]] std::size_t
    draw() noexcept { // a 64-bit LCG, its high 32 bits scaled to the population: uniform enough, one multiply
        _lcg = _lcg * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::size_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(_lcg >> 32)) * _cells->size()) >> 32);
    }

public:
    Driver(std::shared_ptr<const std::vector<qb::ActorId>> cells, std::uint32_t const batch, std::uint64_t const window_ns)
        : _cells(std::move(cells))
        , _batch(batch)
        , _window_ns(window_ns) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Round>(*this);
        _t0 = qb::mono_now();
        push<Round>(id());
        co_return true;
    }

    void
    on(Round const &) {
        ++_rounds;
        if ((_rounds & 0x3FFu) == 0) {
            const auto elapsed = static_cast<std::uint64_t>((qb::mono_now() - _t0).count());
            if (elapsed >= _window_ns) {
                const double events = static_cast<double>(_rounds) * _batch;
                std::printf("actors=%zu batch=%u rounds=%llu events=%.0f elapsed_ns=%llu ns_per_event=%.2f\n", _cells->size(), _batch,
                            static_cast<unsigned long long>(_rounds), events, static_cast<unsigned long long>(elapsed),
                            static_cast<double>(elapsed) / events);
                std::fflush(stdout);
                for (auto const c : *_cells)
                    push<qb::KillEvent>(c);
                kill();
                return;
            }
        }
        auto const &cells = *_cells;
        for (std::uint32_t k = 0; k < _batch; ++k)
            push<Hit>(cells[draw()], _seq++);
        push<Round>(id()); // behind the batch: the next pass dispatches the batch, then this
    }
};

} // namespace

int
main(int argc, char **argv) {
    const long actors = argc > 1 ? std::atol(argv[1]) : 0;
    if (actors <= 0 || actors > 4'000'000) {
        std::fprintf(stderr, "usage: %s <actors> [batch=256] [seconds=2] [core_cpu=0] [latency_us=0]\n", argv[0]);
        return 2;
    }
    const long   batch    = argc > 2 ? std::atol(argv[2]) : 256;
    const double seconds  = argc > 3 ? std::atof(argv[3]) : 2.0;
    const int    core_cpu = argc > 4 ? std::atoi(argv[4]) : 0;
    const long   lat_us   = argc > 5 ? std::atol(argv[5]) : 0;
    if (batch <= 0 || batch > 65536) {
        std::fprintf(stderr, "batch must be in 1..65536\n");
        return 2;
    }
    const auto window = static_cast<std::uint64_t>(seconds * 1e9);

    qb::Main engine;
    auto     &core = engine.core(0);
    core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(core_cpu)});
    core.setLatency(qb::duration{std::chrono::microseconds{lat_us}});
    auto cells = std::make_shared<std::vector<qb::ActorId>>();
    cells->reserve(static_cast<std::size_t>(actors));
    for (long i = 0; i < actors; ++i)
        cells->push_back(core.addActor<Cell>());
    core.addActor<Driver>(cells, static_cast<std::uint32_t>(batch), window);
    engine.start();
    engine.join();
    return engine.hasError() ? 1 : 0;
}
