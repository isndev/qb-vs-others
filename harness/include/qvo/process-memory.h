#ifndef QVO_PROCESS_MEMORY_H
#define QVO_PROCESS_MEMORY_H

#include <cstdint>
#include <iosfwd>
#include <optional>

namespace qvo::probe {

enum class SecondaryMemoryMetric { private_commit, resident_high_water };

struct ProcessMemory {
    std::uint64_t rss_kb;
    std::uint64_t secondary_kb;
    SecondaryMemoryMetric secondary_metric;
};

// A failed read has no numeric value. Zero remains a valid reading when the source says zero.
struct MemoryReading {
    std::optional<ProcessMemory> value;
    const char *error = nullptr;
};

MemoryReading parse_linux_memory_status(std::istream &input);
MemoryReading read_process_memory();
const char *secondary_memory_metric_name(SecondaryMemoryMetric metric) noexcept;

} // namespace qvo::probe
#endif
