// Footprint probe: what does an engine of N cores hold in memory AT REST, and what does the first
// traffic on every pipe add to it? (Huly QB-63)
//
// This is the instrument behind the memory half of docs/TUNING.md: the quadratic terms of an
// engine's footprint. Two structures grow with the SQUARE of the core count -- one per (source,
// destination) pair -- and this probe separates them by what it does before reading the process's
// resident set:
//
//   idle       N cores, one actor each, no event at all: what `Main::start()` allocates and TOUCHES
//              up front -- the per-producer mailbox rings (`SharedCoreCommunication::Mailbox`, one
//              SPSC ring of `MaxRingEvents` buckets per producer core, value-initialised, so
//              resident from the start) and each core's receive buffer.
//   broadcast  the actor of core 0 broadcasts one event: every outbound pipe of core 0 links its
//              first segment (`segment_pool`, 256 KB segments carved eight to a 2 MB slab), and
//              every core receives once -- N pipes used, N slabs touched at most.
//   mesh       every actor pushes one event to every other actor: all N*(N-1) peer pipes link a
//              segment -- the shape a fan-out topology reaches, and the one the segmented pipe
//              (QB-43) keeps resident by design: a consumed segment goes back to its owner's pool,
//              a pool never shrinks on its own.
//
// The figures are the process's resident set and its private commit (Linux: VmRSS / VmHWM from
// /proc/self/status; Windows: WorkingSetSize / PrivateUsage), read from the main thread once the
// engine has settled (the actors are parked at `latency` -- this is a memory probe, not a timing
// one), then again after the engine stopped and its cores were destroyed, so what is RETURNED is
// measured too. Every line also carries the arithmetic the reading is compared against.
//
//   qvoprobe-pipe-footprint <cores> <idle|broadcast|mesh> [settle_ms=1000] [latency_us=100]
//
// One line on stdout per phase: phase, cores, mode, rss_kb, private_kb, and the model.

#include <qb/actor.h>
#include <qb/main.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
//
#include <psapi.h>
#else
#include <fstream>
#endif

namespace {

struct Mem {
    std::uint64_t rss_kb     = 0; ///< resident set (working set on Windows)
    std::uint64_t private_kb = 0; ///< private commit (Windows) / RSS high-water mark (Linux)
};

Mem
read_mem() {
    Mem m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
        m.rss_kb     = pmc.WorkingSetSize / 1024u;
        m.private_kb = pmc.PrivateUsage / 1024u;
    }
#else
    std::ifstream st("/proc/self/status");
    std::string   line;
    while (std::getline(st, line)) {
        if (line.rfind("VmRSS:", 0) == 0)
            m.rss_kb = std::strtoull(line.c_str() + 6, nullptr, 10);
        else if (line.rfind("VmHWM:", 0) == 0)
            m.private_kb = std::strtoull(line.c_str() + 6, nullptr, 10);
    }
#endif
    return m;
}

struct Ping : qb::Event {};

enum class Mode { idle, broadcast, mesh };

std::atomic<std::uint64_t> g_received{0};

class Node : public qb::Actor {
    const Mode                                            _mode;
    const std::shared_ptr<const std::vector<qb::ActorId>> _ids;
    const std::size_t                                     _slot;

public:
    Node(Mode mode, std::shared_ptr<const std::vector<qb::ActorId>> ids, std::size_t slot)
        : _mode(mode)
        , _ids(std::move(ids))
        , _slot(slot) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        if (_mode == Mode::broadcast && _slot == 0)
            broadcast<Ping>(); // core 0's every outbound pipe, once
        else if (_mode == Mode::mesh)
            for (std::size_t i = 0; i < _ids->size(); ++i)
                if (i != _slot)
                    push<Ping>((*_ids)[i]); // every peer pipe of this core, once
        co_return true;
    }

    void
    on(Ping const &) {
        g_received.fetch_add(1, std::memory_order_relaxed);
    }
};

void
print(char const *phase, unsigned cores, char const *mode, Mem const &m, Mem const &base, std::uint64_t model_kb) {
    std::printf("phase=%s cores=%u mode=%s rss_kb=%llu private_kb=%llu delta_rss_kb=%lld delta_private_kb=%lld model_kb=%llu received=%llu\n",
                phase, cores, mode, static_cast<unsigned long long>(m.rss_kb), static_cast<unsigned long long>(m.private_kb),
                static_cast<long long>(m.rss_kb) - static_cast<long long>(base.rss_kb),
                static_cast<long long>(m.private_kb) - static_cast<long long>(base.private_kb), static_cast<unsigned long long>(model_kb),
                static_cast<unsigned long long>(g_received.load()));
    std::fflush(stdout);
}

} // namespace

int
main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <cores> <idle|broadcast|mesh> [settle_ms=1000] [latency_us=100]\n", argv[0]);
        return 2;
    }
    const unsigned cores  = static_cast<unsigned>(std::atoi(argv[1]));
    const char    *mode_s = argv[2];
    Mode           mode;
    if (std::strcmp(mode_s, "idle") == 0)
        mode = Mode::idle;
    else if (std::strcmp(mode_s, "broadcast") == 0)
        mode = Mode::broadcast;
    else if (std::strcmp(mode_s, "mesh") == 0)
        mode = Mode::mesh;
    else {
        std::fprintf(stderr, "unknown mode %s\n", mode_s);
        return 2;
    }
    if (cores == 0 || cores > 256) {
        std::fprintf(stderr, "cores must be 1..256\n");
        return 2;
    }
    const int  settle_ms = argc > 3 ? std::atoi(argv[3]) : 1000;
    const long lat_us    = argc > 4 ? std::atol(argv[4]) : 100;

    // The model, in KiB: the rings every engine start allocates and touches (one SPSC ring of
    // MaxRingEvents buckets per (destination, producer) pair -- a mailbox has one producer per
    // core, `Main.cpp` `nb_producers = getNbCore()`), and one 256 KB pipe segment per pipe the
    // mode uses.
    constexpr std::uint64_t bucket     = QB_LOCKFREE_EVENT_BUCKET_BYTES;
    constexpr std::uint64_t ring_kb    = (65535u / bucket) * bucket / 1024u; // MaxRingEvents buckets
    constexpr std::uint64_t segment_kb = 256;
    const std::uint64_t     rings_kb   = static_cast<std::uint64_t>(cores) * cores * ring_kb;
    const std::uint64_t     pipes_used = mode == Mode::idle        ? 0
                                         : mode == Mode::broadcast ? cores - 1
                                                                   : static_cast<std::uint64_t>(cores) * (cores - 1);
    const std::uint64_t     model_kb   = rings_kb + pipes_used * segment_kb;

    const Mem base = read_mem();
    print("baseline", cores, mode_s, base, base, 0);

    auto ids = std::make_shared<std::vector<qb::ActorId>>();
    {
        qb::Main engine;
        for (unsigned c = 0; c < cores; ++c)
            engine.core(c).setLatency(qb::duration{std::chrono::microseconds{lat_us}});
        // Ids are known before start: `addActor` returns them, and the vector is shared read-only.
        auto shared = std::shared_ptr<const std::vector<qb::ActorId>>(ids);
        for (unsigned c = 0; c < cores; ++c)
            ids->push_back(engine.core(c).addActor<Node>(mode, shared, static_cast<std::size_t>(c)));
        engine.start(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
        print("settled", cores, mode_s, read_mem(), base, model_kb);
        engine.stop();
        engine.join();
        print("stopped", cores, mode_s, read_mem(), base, model_kb);
    }
    print("destroyed", cores, mode_s, read_mem(), base, 0);
    return 0;
}
