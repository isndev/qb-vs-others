// Parser/validity control. No actor runtime, no benchmark, no performance assertion.
#include <qvo/process-memory.h>

#include <cstdio>
#include <sstream>

int main() {
    struct Fixture { const char *name; const char *text; bool valid; std::uint64_t rss; std::uint64_t hwm; };
    const Fixture cases[] = {
        {"complete", "Name: process\nVmRSS: 123 kB\nVmHWM: 456 kB\n", true, 123, 456},
        {"zero", "VmRSS: 0 kB\nVmHWM: 0 kB\n", true, 0, 0},
        {"missing-rss", "VmHWM: 456 kB\n", false, 0, 0},
        {"missing-hwm", "VmRSS: 123 kB\n", false, 0, 0},
        {"absent", "", false, 0, 0},
        {"wrong-unit", "VmRSS: 123 MB\nVmHWM: 456 kB\n", false, 0, 0},
        {"negative", "VmRSS: -1 kB\nVmHWM: 456 kB\n", false, 0, 0},
        {"overflow", "VmRSS: 18446744073709551616 kB\nVmHWM: 456 kB\n", false, 0, 0},
        {"duplicate", "VmRSS: 1 kB\nVmRSS: 2 kB\nVmHWM: 3 kB\n", false, 0, 0},
    };
    for (const auto &test : cases) {
        std::istringstream input(test.text);
        const auto reading = qvo::probe::parse_linux_memory_status(input);
        if (reading.value.has_value() != test.valid || (!test.valid && !reading.error)
            || (test.valid && (reading.value->rss_kb != test.rss || reading.value->secondary_kb != test.hwm
                              || reading.value->secondary_metric != qvo::probe::SecondaryMemoryMetric::resident_high_water))) {
            std::fprintf(stderr, "process-memory control failed: %s\n", test.name);
            return 1;
        }
    }
    std::istringstream failed("VmRSS: 1 kB\nVmHWM: 2 kB\n");
    failed.setstate(std::ios::badbit);
    if (qvo::probe::parse_linux_memory_status(failed).value)
        return 1;
    std::istringstream failed_without_eof("VmRSS: 1 kB\nVmHWM: 2 kB\n");
    failed_without_eof.setstate(std::ios::failbit);
    if (qvo::probe::parse_linux_memory_status(failed_without_eof).value)
        return 1;
#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
    const auto unsupported = qvo::probe::read_process_memory();
    if (unsupported.value || !unsupported.error)
        return 1;
#else
    const auto native = qvo::probe::read_process_memory();
    if (!native.value || native.error) {
        std::fprintf(stderr, "native process-memory read failed: %s\n",
                     native.error ? native.error : "missing value");
        return 1;
    }
#ifdef _WIN32
    if (native.value->secondary_metric != qvo::probe::SecondaryMemoryMetric::private_commit)
        return 1;
#else
    if (native.value->secondary_metric != qvo::probe::SecondaryMemoryMetric::resident_high_water)
        return 1;
#endif
#endif
    std::puts("process-memory: all fixture expectations satisfied; no actors created");
    return 0;
}
