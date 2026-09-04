#include "qvo/harness.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <pthread.h>
#    include <sched.h>
#    include <unistd.h>
#endif

// Build-time facts the harness records so a result file is self-describing. Supplied by
// cmake/qvoHarness.cmake; the defaults exist only so the header compiles standalone.
#ifndef QVO_COMPILER
#    define QVO_COMPILER "unknown"
#endif
#ifndef QVO_COMPILER_VERSION
#    define QVO_COMPILER_VERSION "unknown"
#endif
#ifndef QVO_BUILD_TYPE
#    define QVO_BUILD_TYPE "unknown"
#endif
#ifndef QVO_CXX_FLAGS
#    define QVO_CXX_FLAGS ""
#endif

namespace qvo {
namespace {

[[noreturn]] void fatal(const std::string &msg) {
    std::fprintf(stderr, "qvo: FATAL: %s\n", msg.c_str());
    std::exit(2);
}

// -------------------------------------------------------------------------------------------
// JSON emission
//
// Hand-written on purpose. A JSON library inside the measured process is one more allocator
// customer and one more thing that differs between frameworks if any of them vendors its own.
// The document is flat; this is fifty lines and buys the harness zero dependencies.
// -------------------------------------------------------------------------------------------

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string quoted(const std::string &s) { return "\"" + json_escape(s) + "\""; }

// -------------------------------------------------------------------------------------------
// CPU affinity
//
// FAIRNESS.md 1.4: on a hybrid CPU an unpinned run is not a measurement. The harness therefore
// applies the mask AND reads it back. A refused pin is a hard failure, never a warning -- the
// failure mode this guards against is precisely a pin that reports success and does nothing.
// -------------------------------------------------------------------------------------------

struct Pin {
    bool             requested{false};
    bool             applied{false};
    std::vector<int> cpus;
};

std::vector<int> parse_cpu_list(const std::string &s) {
    std::vector<int>  out;
    std::stringstream ss(s);
    std::string       tok;
    while (std::getline(ss, tok, ',')) {
        auto dash = tok.find('-');
        if (dash != std::string::npos) {
            int lo = std::atoi(tok.substr(0, dash).c_str());
            int hi = std::atoi(tok.substr(dash + 1).c_str());
            if (hi < lo) fatal("bad --cpus range: " + tok);
            for (int i = lo; i <= hi; ++i) out.push_back(i);
        } else if (!tok.empty()) {
            out.push_back(std::atoi(tok.c_str()));
        }
    }
    return out;
}

// Applies the mask to the whole process, so every thread a framework spawns afterwards inherits
// it. This is the only way to constrain a framework whose scheduler threads we do not control.
bool apply_and_verify_pin(const std::vector<int> &cpus, std::string &why_not) {
#if defined(_WIN32)
    DWORD_PTR mask = 0;
    for (int c : cpus) {
        if (c < 0 || c >= 64) { why_not = "cpu index out of range for a Windows affinity mask"; return false; }
        mask |= (DWORD_PTR{1} << c);
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), mask)) {
        why_not = "SetProcessAffinityMask failed, GetLastError=" + std::to_string(GetLastError());
        return false;
    }
    DWORD_PTR got_proc = 0, got_sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &got_proc, &got_sys)) {
        why_not = "GetProcessAffinityMask failed";
        return false;
    }
    if (got_proc != mask) {
        why_not = "affinity read back as 0x" + std::to_string(got_proc) + " but 0x"
                  + std::to_string(mask) + " was requested";
        return false;
    }
    return true;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        why_not = std::string("sched_setaffinity failed: ") + std::strerror(errno);
        return false;
    }
    cpu_set_t got;
    CPU_ZERO(&got);
    if (sched_getaffinity(0, sizeof(got), &got) != 0) {
        why_not = "sched_getaffinity failed";
        return false;
    }
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        bool want = std::find(cpus.begin(), cpus.end(), c) != cpus.end();
        if (want != (CPU_ISSET(c, &got) != 0)) {
            why_not = "affinity read back differs from the requested set at cpu " + std::to_string(c);
            return false;
        }
    }
    return true;
#else
    // Deliberately refuses rather than pretending. qb's own docs record a platform where the pin
    // silently does nothing; a harness that mirrored that would invalidate every number it prints.
    (void) cpus;
    why_not = "this platform has no verified CPU pinning; run with --no-pin and accept the caveat";
    return false;
#endif
}

int logical_cpu_count() {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<int>(si.dwNumberOfProcessors);
#else
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
#endif
}

std::string host_name() {
#if defined(_WIN32)
    char  buf[256];
    DWORD n = sizeof buf;
    if (GetComputerNameA(buf, &n)) return std::string(buf, n);
    return "unknown";
#else
    char buf[256];
    if (gethostname(buf, sizeof buf) == 0) return buf;
    return "unknown";
#endif
}

std::string os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

std::string iso_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// -------------------------------------------------------------------------------------------
// Statistics
//
// The report renders the distribution, not a mean (FAIRNESS.md 1.5). The harness ships every
// sample so nothing downstream has to trust a summary it cannot recompute.
// -------------------------------------------------------------------------------------------

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    if (v.size() == 1) return v[0];
    double idx = p * (static_cast<double>(v.size()) - 1.0);
    auto   lo  = static_cast<std::size_t>(idx);
    auto   hi  = lo + 1 < v.size() ? lo + 1 : lo;
    double f   = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - f) + v[hi] * f;
}

}  // namespace

// -------------------------------------------------------------------------------------------
// Params
// -------------------------------------------------------------------------------------------

void Params::set(const std::string &name, long long value) { values_[name] = value; }

bool Params::has(const std::string &name) const { return values_.count(name) != 0; }

long long Params::get(const std::string &name) const {
    auto it = values_.find(name);
    if (it == values_.end())
        fatal("benchmark asked for parameter '" + name + "' which its Spec does not declare");
    return it->second;
}

// -------------------------------------------------------------------------------------------
// Watch
// -------------------------------------------------------------------------------------------

void Watch::start() noexcept {
    t0_      = clock::now();
    started_ = true;
}

void Watch::stop() noexcept {
    t1_      = clock::now();
    stopped_ = true;
}

double Watch::work_ns() const noexcept {
    if (!started_ || !stopped_) return -1.0;
    return std::chrono::duration<double, std::nano>(t1_ - t0_).count();
}

// -------------------------------------------------------------------------------------------
// spin_work / sink
// -------------------------------------------------------------------------------------------

namespace {
std::atomic<std::uint64_t> g_sink{0};
}

void sink(std::uint64_t value) noexcept { g_sink.fetch_add(value, std::memory_order_relaxed); }

std::uint64_t spin_work(std::uint64_t seed, int iterations) noexcept {
    std::uint64_t x = seed | 1ULL;
    for (int i = 0; i < iterations; ++i) x = mix(x + static_cast<std::uint64_t>(i));
    return x;
}

// -------------------------------------------------------------------------------------------
// run
// -------------------------------------------------------------------------------------------

int run(int argc, char **argv, Spec spec, Body body) {
    int         repetitions = 5;
    int         warmup      = 1;
    std::string out_path;
    bool        describe = false;
    bool        no_pin   = false;
    Pin         pin;
    Params      params;

    for (const auto &kv : spec.params) params.set(kv.first, kv.second);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto        next = [&]() -> std::string {
            if (i + 1 >= argc) fatal("missing value for " + a);
            return argv[++i];
        };
        if (a == "--repetitions") {
            repetitions = std::atoi(next().c_str());
        } else if (a == "--warmup") {
            warmup = std::atoi(next().c_str());
        } else if (a == "--out") {
            out_path = next();
        } else if (a == "--cpus") {
            pin.cpus      = parse_cpu_list(next());
            pin.requested = true;
        } else if (a == "--no-pin") {
            no_pin = true;
        } else if (a == "--describe") {
            describe = true;
        } else if (a == "--param") {
            std::string kv  = next();
            auto        eq  = kv.find('=');
            if (eq == std::string::npos) fatal("--param expects name=value, got " + kv);
            std::string key = kv.substr(0, eq);
            if (!params.has(key))
                fatal("--param '" + key + "' is not declared by benchmark " + spec.benchmark);
            params.set(key, std::atoll(kv.substr(eq + 1).c_str()));
        } else {
            fatal("unknown argument: " + a);
        }
    }

    if (repetitions < 1) fatal("--repetitions must be >= 1");
    if (warmup < 0) fatal("--warmup must be >= 0");

    // Pinning happens BEFORE any framework object is constructed, so every thread the framework
    // creates inherits the mask. A framework that spawns its scheduler in a static initializer
    // would defeat this; none of the ones measured here does, and the report records the mask
    // that was actually read back rather than the one requested.
    if (pin.requested && !no_pin) {
        std::string why;
        if (!apply_and_verify_pin(pin.cpus, why))
            fatal("CPU pinning was requested and could not be verified (" + why
                  + "). Refusing to produce a number that a hybrid CPU would make meaningless. "
                    "Pass --no-pin to measure anyway; the result is then marked pinned:false.");
        pin.applied = true;
    }

    std::string pin_repr;
    for (std::size_t i = 0; i < pin.cpus.size(); ++i)
        pin_repr += (i ? "," : "") + std::to_string(pin.cpus[i]);

    if (describe) {
        std::cout << "{\"benchmark\":" << quoted(spec.benchmark)
                  << ",\"framework\":" << quoted(spec.framework)
                  << ",\"framework_version\":" << quoted(spec.framework_version) << ",\"params\":{";
        bool first = true;
        for (const auto &kv : spec.params) {
            std::cout << (first ? "" : ",") << quoted(kv.first) << ":" << kv.second;
            first = false;
        }
        std::cout << "}}\n";
        return 0;
    }

    if (!spec.expected)
        fatal("benchmark " + spec.benchmark + " declares no expected() -- see FAIRNESS.md section 0");

    const std::uint64_t want          = spec.expected(params);
    const std::uint64_t want_messages = spec.expected_messages ? spec.expected_messages(params) : 0;

    std::vector<double>        work_ns, total_ns, setup_ns, teardown_ns;
    std::vector<std::string>   failures;

    const int all_reps = warmup + repetitions;
    for (int rep = 0; rep < all_reps; ++rep) {
        Watch w;
        auto  t_begin = clock::now();
        Answer got    = body(params, w);
        auto  t_end   = clock::now();

        if (got.checksum != want) {
            failures.push_back("repetition " + std::to_string(rep) + ": checksum "
                               + std::to_string(got.checksum) + " != expected "
                               + std::to_string(want));
            continue;
        }
        if (want_messages != 0 && got.messages != 0 && got.messages != want_messages) {
            failures.push_back("repetition " + std::to_string(rep) + ": delivered "
                               + std::to_string(got.messages) + " messages, expected "
                               + std::to_string(want_messages));
            continue;
        }
        if (!w.started() || !w.stopped()) {
            failures.push_back("repetition " + std::to_string(rep)
                               + ": the body never marked its workload window (Watch::start/stop)");
            continue;
        }

        if (rep < warmup) continue;

        const double total = std::chrono::duration<double, std::nano>(t_end - t_begin).count();
        const double work  = w.work_ns();
        work_ns.push_back(work);
        total_ns.push_back(total);
        // Everything outside the marked window: framework construction, actor creation, shutdown.
        // Reported so a fast workload window bought by an expensive startup is visible.
        setup_ns.push_back(total - work);
    }

    const bool ok = failures.empty() && work_ns.size() == static_cast<std::size_t>(repetitions);

    std::ostringstream j;
    j << "{\n";
    j << "  \"schema\": \"qvo/result/1\",\n";
    j << "  \"benchmark\": " << quoted(spec.benchmark) << ",\n";
    j << "  \"framework\": " << quoted(spec.framework) << ",\n";
    j << "  \"framework_version\": " << quoted(spec.framework_version) << ",\n";
    j << "  \"verified\": " << (ok ? "true" : "false") << ",\n";
    j << "  \"expected_checksum\": " << want << ",\n";
    j << "  \"expected_messages\": " << want_messages << ",\n";
    j << "  \"repetitions\": " << repetitions << ",\n";
    j << "  \"warmup\": " << warmup << ",\n";

    j << "  \"params\": {";
    {
        bool first = true;
        for (const auto &kv : params.all()) {
            j << (first ? "" : ", ") << quoted(kv.first) << ": " << kv.second;
            first = false;
        }
    }
    j << "},\n";

    j << "  \"pinned\": " << (pin.applied ? "true" : "false") << ",\n";
    j << "  \"cpus\": " << quoted(pin_repr) << ",\n";

    j << "  \"env\": {\n";
    j << "    \"host\": " << quoted(host_name()) << ",\n";
    j << "    \"os\": " << quoted(os_name()) << ",\n";
    j << "    \"logical_cpus\": " << logical_cpu_count() << ",\n";
    j << "    \"compiler\": " << quoted(QVO_COMPILER) << ",\n";
    j << "    \"compiler_version\": " << quoted(QVO_COMPILER_VERSION) << ",\n";
    j << "    \"build_type\": " << quoted(QVO_BUILD_TYPE) << ",\n";
    j << "    \"cxx_flags\": " << quoted(QVO_CXX_FLAGS) << ",\n";
    j << "    \"utc\": " << quoted(iso_utc_now()) << "\n";
    j << "  },\n";

    j << "  \"idiom\": {\n";
    j << "    \"source\": " << quoted(spec.idiom_source) << ",\n";
    j << "    \"note\": " << quoted(spec.idiom_note) << "\n";
    j << "  },\n";

    j << "  \"caveats\": [";
    for (std::size_t i = 0; i < spec.caveats.size(); ++i)
        j << (i ? ", " : "") << quoted(spec.caveats[i]);
    j << "],\n";

    j << "  \"failures\": [";
    for (std::size_t i = 0; i < failures.size(); ++i) j << (i ? ", " : "") << quoted(failures[i]);
    j << "],\n";

    auto array = [&](const char *name, const std::vector<double> &v, bool last) {
        j << "  \"" << name << "\": [";
        for (std::size_t i = 0; i < v.size(); ++i) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.1f", v[i]);
            j << (i ? ", " : "") << buf;
        }
        j << "]" << (last ? "\n" : ",\n");
    };
    array("work_ns", work_ns, false);
    array("total_ns", total_ns, false);
    array("outside_window_ns", setup_ns, false);

    auto stat = [&](const char *name, double v, bool last) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f", v);
        j << "    \"" << name << "\": " << buf << (last ? "\n" : ",\n");
    };
    j << "  \"summary\": {\n";
    stat("work_min", work_ns.empty() ? 0.0 : *std::min_element(work_ns.begin(), work_ns.end()), false);
    stat("work_p50", percentile(work_ns, 0.50), false);
    stat("work_p99", percentile(work_ns, 0.99), false);
    stat("work_iqr", percentile(work_ns, 0.75) - percentile(work_ns, 0.25), true);
    j << "  }\n";
    j << "}\n";

    const std::string doc = j.str();
    if (out_path.empty()) {
        std::cout << doc;
    } else {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) fatal("cannot write " + out_path);
        f << doc;
    }

    if (!ok) {
        std::fprintf(stderr, "qvo: %s/%s FAILED VERIFICATION\n", spec.framework.c_str(),
                     spec.benchmark.c_str());
        for (const auto &f : failures) std::fprintf(stderr, "qvo:   %s\n", f.c_str());
        return 1;
    }
    return 0;
}

}  // namespace qvo
