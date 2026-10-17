#include <qvo/actor-memory.h>
#include <qvo/build-info.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace qvo::probe {

PopulationLedger::PopulationLedger(std::size_t actors, std::size_t infrastructure_count)
    : _actors(actors, 0), _infrastructure(infrastructure_count, 0) {}

void PopulationLedger::born(std::size_t id) {
    std::lock_guard lock(_mutex);
    if (id >= _actors.size() || _actors[id] != 0) _population.error = "duplicate or invalid actor birth";
    else { _actors[id] = 1; ++_population.born; ++_population.alive; }
    _changed.notify_all();
}
void PopulationLedger::ready(std::size_t id) {
    std::lock_guard lock(_mutex);
    if (id >= _actors.size() || _actors[id] != 1) _population.error = "duplicate or invalid actor readiness";
    else { _actors[id] = 2; ++_population.ready; }
    _changed.notify_all();
}
void PopulationLedger::destroyed(std::size_t id) {
    std::lock_guard lock(_mutex);
    if (id >= _actors.size() || (_actors[id] != 1 && _actors[id] != 2))
        _population.error = "duplicate or invalid actor destruction";
    else { _actors[id] = 3; --_population.alive; ++_population.destroyed; }
    _changed.notify_all();
}
void PopulationLedger::infrastructure_ready_signal(std::size_t identity) {
    std::lock_guard lock(_mutex);
    if (identity >= _infrastructure.size() || _infrastructure[identity]) _population.error = "duplicate or invalid infrastructure readiness";
    else { _infrastructure[identity] = 1; ++_population.infrastructure_ready; }
    _changed.notify_all();
}
Population PopulationLedger::snapshot() const {
    std::lock_guard lock(_mutex);
    return _population;
}
bool PopulationLedger::live_locked() const {
    return !_population.error && _population.born == _actors.size() && _population.ready == _actors.size()
        && _population.alive == _actors.size() && _population.destroyed == 0
        && _population.infrastructure_ready == _infrastructure.size();
}
bool PopulationLedger::is_live() const {
    std::lock_guard lock(_mutex);
    return live_locked();
}
bool PopulationLedger::wait_live(std::chrono::milliseconds timeout) {
    std::unique_lock lock(_mutex);
    _changed.wait_for(lock, timeout, [&] { return _population.error || _population.destroyed || live_locked(); });
    return live_locked();
}
bool PopulationLedger::is_destroyed() const {
    std::lock_guard lock(_mutex);
    return !_population.error && _population.born == _actors.size() && _population.ready == _actors.size()
        && _population.alive == 0 && _population.destroyed == _actors.size();
}

namespace {

std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
            result += escaped;
        } else result += static_cast<char>(c);
    }
    return result + '"';
}

std::size_t integer(const char *text, std::size_t maximum) {
    const std::string_view value(text);
    std::size_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number > maximum)
        throw std::runtime_error("invalid integer argument");
    return number;
}

// Bounds joins as well as readiness. The outer collector must preserve stderr
// and refuse this nonzero exit; no accepted document is emitted on this path.
class Deadline {
public:
    explicit Deadline(std::chrono::milliseconds timeout) : _thread([this, timeout] {
        std::unique_lock lock(_mutex);
        _ready = true;
        _changed.notify_all();
        if (!_changed.wait_for(lock, timeout, [&] { return _done; })) {
            std::fputs("actor-memory: process deadline exceeded\n", stderr);
            std::fflush(stderr);
            std::_Exit(1);
        }
    }) {
        std::unique_lock lock(_mutex);
        _changed.wait(lock, [&] { return _ready; });
    }
    ~Deadline() {
        { std::lock_guard lock(_mutex); _done = true; }
        _changed.notify_one();
        _thread.join();
    }
private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _done = false;
    bool _ready = false;
    std::thread _thread;
};

struct Sample {
    ProcessMemory memory{};
    Population population;
    std::optional<RegionMemory> regions;
};

Sample sample(const MemoryAdapter &adapter, const PopulationLedger &ledger) {
    const auto reading = read_process_memory();
    if (!reading.value) throw std::runtime_error(reading.error ? reading.error : "memory unavailable");
    return {*reading.value, ledger.snapshot(), adapter.regions ? adapter.regions() : std::nullopt};
}

void print_sample(const char *phase, const Sample &s) {
    std::cout << "{\"phase\":" << quote(phase) << ",\"rss_kib\":" << s.memory.rss_kb
              << ",\"secondary_kib\":" << s.memory.secondary_kb
              << ",\"secondary_metric\":" << quote(secondary_memory_metric_name(s.memory.secondary_metric))
              << ",\"born\":" << s.population.born << ",\"ready\":" << s.population.ready
              << ",\"alive\":" << s.population.alive << ",\"destroyed\":" << s.population.destroyed
              << ",\"infrastructure_ready\":" << s.population.infrastructure_ready << ",\"qb_global_regions\":";
    if (s.regions) std::cout << "{\"cached_bytes\":" << s.regions->cached_bytes
                            << ",\"lent_bytes\":" << s.regions->lent_bytes
                            << ",\"peak_bytes\":" << s.regions->peak_bytes << '}';
    else std::cout << "null";
    std::cout << '}';
}


void print_core_arenas(const std::vector<CoreArenaMemory> *arenas) {
    if (!arenas) { std::cout << "null"; return; }
    std::cout << "{\"snapshot_stage\":\"sentinel_readiness_after_startup_init\","
                 "\"scope\":\"whole_owner_thread_pooled_arena\",\"infrastructure_actors\":" << arenas->size()
              << ",\"cores\":[";
    bool first = true;
    for (const auto &arena : *arenas) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"core\":" << arena.core << ",\"live_blocks\":" << arena.live_blocks
                  << ",\"chunks\":" << arena.chunks << ",\"chunk_bytes\":" << arena.chunk_bytes
                  << ",\"infrastructure_actors\":1}";
    }
    std::cout << "]}";
}

} // namespace

int actor_memory_main(int argc, char **argv, const MemoryAdapter &adapter) {
    MemoryConfig config;
    try {
        bool actors = false, cores = false, timeout = false;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::runtime_error("options require a value");
            const std::string_view key(argv[i]);
            if (key == "--actors" && !actors) { config.actors = integer(argv[i + 1], 60000); actors = true; }
            else if (key == "--cores" && !cores) { config.cores = static_cast<unsigned>(integer(argv[i + 1], 16)); cores = true; }
            else if (key == "--timeout-ms" && !timeout) {
                config.timeout = std::chrono::milliseconds{integer(argv[i + 1], 300000)}; timeout = true;
            } else throw std::runtime_error("unknown or duplicate option");
        }
        if (!actors || !cores || config.cores == 0 || config.timeout.count() < 100)
            throw std::runtime_error("require --actors 0..60000 --cores 1..16 [--timeout-ms 100..300000]");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "actor-memory: %s\n", error.what());
        return 2;
    }
    try {
        for (const auto *name : {"QVO_QB_PARK_US", "QVO_QB_IDLE_SPIN_US", "QVO_CAF_AGGRESSIVE_POLL",
                                 "QVO_CAF_STEAL_INTERVAL", "QVO_SO_SPIN_WAIT_US"})
            if (std::getenv(name)) throw std::runtime_error("sweep environment overrides are not supported by actor-memory");
        Deadline deadline(config.timeout + std::chrono::seconds{5});
        PopulationLedger ledger(config.actors, config.cores);
        if (adapter.prepare) adapter.prepare(config);
        const Sample before = sample(adapter, ledger); // Fail before constructing the adapter-owned actor runtime.
        Sample live;
        unsigned captures = 0;
        adapter.run(config, ledger, [&] {
            if (++captures != 1) throw std::runtime_error("duplicate live capture");
            if (!ledger.wait_live(config.timeout)) throw std::runtime_error("population not fully alive and ready");
            std::this_thread::sleep_for(std::chrono::milliseconds{100}); // common settling interval, never the proof of life
            if (!ledger.is_live()) throw std::runtime_error("population disappeared before memory capture");
            live = sample(adapter, ledger);
            if (!ledger.is_live()) throw std::runtime_error("population changed during memory capture");
        });
        if (captures != 1 || !ledger.is_destroyed()) throw std::runtime_error("population destruction not verified");
        const Sample destroyed = sample(adapter, ledger);
        if (before.memory.secondary_metric != live.memory.secondary_metric
            || live.memory.secondary_metric != destroyed.memory.secondary_metric)
            throw std::runtime_error("secondary memory metric changed between phases");
        if (adapter.qb_thread_arenas) {
            const auto &arenas = *adapter.qb_thread_arenas;
            if (arenas.size() != config.cores) throw std::runtime_error("qb arena snapshot core count mismatch");
            for (std::size_t core = 0; core < arenas.size(); ++core) {
                const auto &arena = arenas[core];
                if (!arena.captured || arena.core != core || arena.live_blocks == 0 || arena.chunks == 0 || arena.chunk_bytes == 0)
                    throw std::runtime_error("qb arena owner-thread snapshot missing or invalid");
            }
        }
        std::cout << "{\"schema\":\"qvo-actor-memory/1\",\"verified\":true,\"framework\":" << quote(adapter.framework)
                  << ",\"framework_version\":" << quote(adapter.version) << ",\"role\":" << quote(adapter.floor ? "floor" : "framework")
                  << ",\"actors\":" << config.actors << ",\"cores\":" << config.cores << ",\"pinned\":false"
                  << ",\"profile\":\"park\",\"compiler\":" << quote(QVO_COMPILER)
                  << ",\"compiler_version\":" << quote(QVO_COMPILER_VERSION) << ",\"build_type\":" << quote(QVO_BUILD_TYPE)
                  << ",\"cxx_flags\":" << quote(QVO_CXX_FLAGS) << ",\"cxx_flags_scope\":" << quote(QVO_CXX_FLAGS_SCOPE)
                  << ",\"caveat\":" << quote(adapter.caveat)
                  << ",\"object_size_bytes\":";
        if (adapter.object_size) std::cout << *adapter.object_size; else std::cout << "null";
        std::cout << ",\"qb_thread_arenas\":";
        print_core_arenas(adapter.qb_thread_arenas);
        std::cout << ",\"subscription_storage_bytes\":null,\"oninit_frame_bytes\":null,\"samples\":[";
        print_sample("before", before); std::cout << ',';
        print_sample("live", live); std::cout << ',';
        print_sample("destroyed", destroyed); std::cout << "]}\n";
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "actor-memory: %s\n", error.what());
        std::cout << "{\"schema\":\"qvo-actor-memory/1\",\"verified\":false,\"framework\":"
                  << quote(adapter.framework) << ",\"error\":" << quote(error.what()) << ",\"samples\":[]}\n";
        return 1;
    }
}

} // namespace qvo::probe
