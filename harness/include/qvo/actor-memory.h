#ifndef QVO_ACTOR_MEMORY_H
#define QVO_ACTOR_MEMORY_H

#include <qvo/process-memory.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace qvo::probe {

struct Population {
    std::size_t born = 0;
    std::size_t ready = 0;
    std::size_t alive = 0;
    std::size_t destroyed = 0;
    std::size_t infrastructure_ready = 0;
    const char *error = nullptr;
};

// The proof storage is allocated and touched before the first memory reading.
// Only the adapter's object lifetime and dispatched readiness message update it.
class PopulationLedger {
public:
    PopulationLedger(std::size_t actors, std::size_t infrastructure_count);
    void born(std::size_t id);
    void ready(std::size_t id);
    void destroyed(std::size_t id);
    void infrastructure_ready_signal(std::size_t identity);
    Population snapshot() const;
    bool wait_live(std::chrono::milliseconds timeout);
    bool is_live() const;
    bool is_destroyed() const;

private:
    bool live_locked() const;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<unsigned char> _actors;
    std::vector<unsigned char> _infrastructure;
    Population _population;
};

struct MemoryConfig {
    std::size_t actors = 0;
    unsigned cores = 1; // Configured worker budget, not an observed OS-thread count.
    std::chrono::milliseconds timeout{30000};
};

struct RegionMemory {
    std::size_t cached_bytes;
    std::size_t lent_bytes;
    std::size_t peak_bytes;
};

// One owner-thread snapshot, taken by that core's fixed infrastructure actor.
// These are the whole thread's pooled arena counts, never actor-attributed RSS.
struct CoreArenaMemory {
    std::size_t core = 0;
    std::size_t live_blocks = 0;
    std::size_t chunks = 0;
    std::size_t chunk_bytes = 0;
    bool captured = false;
};

struct MemoryAdapter {
    const char *framework;
    const char *version;
    const char *caveat;
    bool floor;
    std::optional<std::size_t> object_size;
    // Observer tables are sized/touched here, before the first memory reading.
    std::function<void(const MemoryConfig &)> prepare;
    // Optional observer array, sized/touched by prepare before the baseline.
    // Its owners finish writing before run returns; formatting only reads copies.
    const std::vector<CoreArenaMemory> *qb_thread_arenas = nullptr;
    // Global allocator counters, NEVER actor-attributed bytes.
    std::function<std::optional<RegionMemory>()> regions;
    // Calls capture exactly once with every target alive; returns only after all
    // target objects and runtime resources have been destroyed, including on throw.
    std::function<void(const MemoryConfig &, PopulationLedger &, const std::function<void()> &)> run;
};

// The build selects a distinct template specialization for each separate control target.
#ifndef QVO_MEMORY_CONTROL_DROP_LAST
#define QVO_MEMORY_CONTROL_DROP_LAST 0
#endif

template <bool Drop>
std::size_t memory_spawn_count(std::size_t requested) noexcept {
    if constexpr (Drop) {
        if (requested == 0) return 0;
        static const bool announced = [requested] {
            std::fprintf(stderr, "actor-memory plant-fired: omitted requested identity %zu\n", requested - 1);
            return true;
        }();
        (void)announced;
        return requested - 1;
    } else {
        return requested;
    }
}

int actor_memory_main(int argc, char **argv, const MemoryAdapter &adapter);

} // namespace qvo::probe
#endif
