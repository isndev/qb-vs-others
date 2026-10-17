#include <qvo/process-memory.h>

#include <charconv>
#include <fstream>
#include <istream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace qvo::probe {
namespace {

bool parse_kb(std::string_view text, std::uint64_t &value) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return false;
    text.remove_prefix(first);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == text.data())
        return false;
    text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
    const auto unit = text.find_first_not_of(" \t");
    if (unit == std::string_view::npos)
        return false;
    text.remove_prefix(unit);
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(0, last + 1) == "kB";
}

} // namespace

MemoryReading parse_linux_memory_status(std::istream &input) {
    if (!input.good())
        return {std::nullopt, "memory status could not be read"};
    ProcessMemory value{0, 0, SecondaryMemoryMetric::resident_high_water};
    bool rss = false;
    bool hwm = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            if (rss || !parse_kb(std::string_view(line).substr(6), value.rss_kb))
                return {std::nullopt, "invalid or duplicate VmRSS"};
            rss = true;
        } else if (line.rfind("VmHWM:", 0) == 0) {
            if (hwm || !parse_kb(std::string_view(line).substr(6), value.secondary_kb))
                return {std::nullopt, "invalid or duplicate VmHWM"};
            hwm = true;
        }
    }
    if (input.bad() || (input.fail() && !input.eof()))
        return {std::nullopt, "memory status read failed"};
    if (!rss || !hwm)
        return {std::nullopt, "memory status lacks VmRSS or VmHWM"};
    return {value, nullptr};
}

MemoryReading read_process_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc)))
        return {std::nullopt, "GetProcessMemoryInfo failed"};
    return {ProcessMemory{pmc.WorkingSetSize / 1024u, pmc.PrivateUsage / 1024u,
                          SecondaryMemoryMetric::private_commit}, nullptr};
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS
        || count < MACH_TASK_BASIC_INFO_COUNT)
        return {std::nullopt, "task_info(MACH_TASK_BASIC_INFO) failed"};
    return {ProcessMemory{info.resident_size / 1024u, info.resident_size_max / 1024u,
                          SecondaryMemoryMetric::resident_high_water}, nullptr};
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    return parse_linux_memory_status(status);
#else
    return {std::nullopt, "process memory accounting is unsupported on this platform"};
#endif
}

const char *secondary_memory_metric_name(SecondaryMemoryMetric metric) noexcept {
    return metric == SecondaryMemoryMetric::private_commit ? "private_commit" : "rss_high_water";
}

} // namespace qvo::probe
