#include "qvo/harness.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <shellapi.h>
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

// Carries the reason from qvo::not_applicable() back to run(), which owns the output document.
// An exception rather than a global: the body is the only place it can be raised from, run() is
// the only place it is caught, and nothing between the two has to know it exists.
struct NotApplicable {
    std::string reason;
};

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

// Writes the document and says why not when it cannot. Flushes stdout itself: the deadline's
// watchdog leaves through std::_Exit, which flushes nothing.
bool try_write_document(const std::filesystem::path &out_path, const std::string &doc,
                        std::string &why_not) {
    if (out_path.empty()) {
        std::cout << doc;
        std::cout.flush();
        if (!std::cout) why_not = "cannot write the document to stdout";
        return static_cast<bool>(std::cout);
    }

    // Keep the temporary file beside --out: the replacement is atomic on the same filesystem.
    // The runner also stages each process attempt, but direct harness callers need this guarantee.
    static std::atomic<unsigned long long> sequence{0};
#if defined(_WIN32)
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    std::filesystem::path tmp = out_path;
    tmp += ".qvo-tmp-" + std::to_string(pid) + "-"
           + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            why_not = "cannot write output temporary file";
            return false;
        }
        f.write(doc.data(), static_cast<std::streamsize>(doc.size()));
        f.close();
        if (!f) {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            why_not = "cannot finish writing output temporary file";
            return false;
        }
    }
#if defined(_WIN32)
    const bool replaced = MoveFileExW(tmp.c_str(), out_path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool replaced = std::rename(tmp.c_str(), out_path.c_str()) == 0;
#endif
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        why_not = "cannot replace output document";
        return false;
    }
    return true;
}

void write_document(const std::filesystem::path &out_path, const std::string &doc) {
    std::string why;
    if (!try_write_document(out_path, doc, why)) fatal(why);
}

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
    int              threads{0};  // Linux: the threads pinned and read back; 0 where not enumerated
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

#if defined(__linux__)
// The threads of this process, from /proc/self/task.
bool list_threads(std::vector<pid_t> &tids, std::string &why_not) {
    tids.clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        char             *last = nullptr;
        const long        tid  = std::strtol(name.c_str(), &last, 10);
        if (!name.empty() && *last == '\0' && tid > 0) tids.push_back(static_cast<pid_t>(tid));
    }
    if (ec || tids.empty()) {
        why_not = "cannot list the threads of this process in /proc/self/task";
        return false;
    }
    return true;
}
#endif

// Applies the mask to the whole process, so every thread a framework spawns afterwards inherits
// it. This is the only way to constrain a framework whose scheduler threads we do not control.
// `threads` receives the number of threads pinned and read back where they are enumerated (Linux).
bool apply_and_verify_pin(const std::vector<int> &cpus, int &threads, std::string &why_not) {
    threads = 0;
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
    // On Linux the mask belongs to each THREAD, and sched_setaffinity(0, ...) sets the caller's
    // alone. A thread started before main() -- qb's log writer, from a static initialiser when qb
    // is built with QB_WITH_LOGGING, its default -- kept the mask the process was launched with
    // while main() alone was pinned and read back (Huly QB-841). So every thread of the process is
    // pinned, listing /proc/self/task again until a listing holds no thread left to pin: a thread
    // started later inherits the mask of its creator, which by then is pinned. Then every thread
    // of that last listing is read back.
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) {
        if (c < 0 || c >= CPU_SETSIZE) {
            why_not = "cpu index out of range for a Linux cpu_set_t";
            return false;
        }
        CPU_SET(c, &set);
    }
    std::vector<pid_t> pinned, tids;
    for (int listing = 0;; ++listing) {
        if (!list_threads(tids, why_not)) return false;
        bool fresh = false;
        for (const pid_t tid : tids) {
            if (std::find(pinned.begin(), pinned.end(), tid) != pinned.end()) continue;
            fresh = true;
            if (sched_setaffinity(tid, sizeof(set), &set) != 0) {
                if (errno == ESRCH) continue;  // it ended after the listing: nothing left to pin
                why_not = "sched_setaffinity failed for thread " + std::to_string(tid) + ": "
                          + std::strerror(errno);
                return false;
            }
            pinned.push_back(tid);
        }
        if (!fresh) break;
        if (listing == 16) {
            why_not = "threads kept appearing while the process was being pinned";
            return false;
        }
    }
    for (const pid_t tid : tids) {
        cpu_set_t got;
        CPU_ZERO(&got);
        if (sched_getaffinity(tid, sizeof(got), &got) != 0) {
            if (errno == ESRCH) continue;  // it ended: it runs nowhere
            why_not = "sched_getaffinity failed for thread " + std::to_string(tid);
            return false;
        }
        for (int c = 0; c < CPU_SETSIZE; ++c) {
            if ((CPU_ISSET(c, &set) != 0) != (CPU_ISSET(c, &got) != 0)) {
                why_not = "affinity of thread " + std::to_string(tid)
                          + " read back differs from the requested set at cpu " + std::to_string(c);
                return false;
            }
        }
        ++threads;
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

// The `env` object of a result document: which binary, on which host, produced it. Shared by the
// measured document and the not-applicable one, so a cell that declined still says what toolchain
// declined it -- report.py groups the field by this object, and a document without it shows up as
// a second, nameless toolchain.
void write_env(std::ostream &j) {
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

// -------------------------------------------------------------------------------------------
// The deadline (Huly QB-991)
//
// A benchmark waits for the end of a protocol -- N "done" messages, a report, the end of a chain
// -- so a message the protocol waits for, lost (or, in many shapes, misrouted or carrying a
// corrupted control field), does not move the checksum: the body never returns; each shape's page
// says which of its defects block. tools/run.py's --timeout bounds the whole process from
// outside; a binary run directly (a correctness matrix, a negative proof, a reproduction) had no
// bound at all, and a planted defect that blocks read as "still running" instead of "failed".
//
// The watchdog bounds each repetition: armed before the repetition's first clock read, disarmed
// after its last, so nothing of it is inside a timed span. Arming is a store under an uncontended
// mutex and notifies nobody. The thread sleeps until the earliest instant a deadline could fall
// due and, when it wakes, judges the repetition armed at that moment: a run whose repetitions are
// short against the deadline never wakes it, a long run wakes it once per deadline at most. When
// a repetition outlives its deadline, `fire` -- called with the lock held, so the body's thread
// cannot disarm underneath it -- writes the failure document and ends the process.
//
// qvo::fail() (Huly QB-1018) ends a repetition the same way, from whichever thread detected the
// failure: `fail_now` takes the same lock and calls the same `fire`, so the document is written
// once, by the first of the two, and a failure the body declared reads exactly like one the
// deadline found. arm() records the repetition even with the watchdog off (--deadline 0), so a
// declared failure is named and documented with or without a deadline.
// -------------------------------------------------------------------------------------------

// Ten times the slowest repetition any result document of this repository records: 30.0 s,
// savina/ping-pong 2c-park at its default parameters, shipped qb 3.1.0's parked cross-core hop on
// the arm64 Linux guest (results/utm-debian13-arm64-g++14/qb-branch-develop/). The slowest
// published cell is 26.3 s (SObjectizer, the same cell on WSL2).
constexpr double kDefaultDeadlineSeconds = 300.0;
// Far beyond any run, and far inside steady_clock's range once added to its epoch.
constexpr double kMaxDeadlineSeconds = 1e7;

class Deadline {
public:
    // Called with the lock held -- by the watchdog thread, or by fail_now()'s caller -- with the
    // verdict ("DEADLINE", "FAILED") and the failure ("repetition k: ..."); must not return.
    using Fire = std::function<void(const char *verdict, const std::string &failure)>;

    // `seconds` <= 0 disables the watchdog: no thread is started, and arm()/disarm() only record
    // which repetition runs, for fail_now().
    Deadline(double seconds, Fire fire) : fire_(std::move(fire)), seconds_(seconds) {
        if (seconds_ <= 0) return;
        budget_ = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds_));
        thread_ = std::thread([this] { watch(); });
    }

    ~Deadline() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    Deadline(const Deadline &)            = delete;
    Deadline &operator=(const Deadline &) = delete;

    void arm(int rep) {
        std::lock_guard<std::mutex> lock(m_);
        rep_   = rep;
        start_ = thread_.joinable() ? clock::now() : clock::time_point{};
        armed_ = true;
    }

    void disarm() {
        std::lock_guard<std::mutex> lock(m_);
        armed_ = false;
    }

    // qvo::fail(), from any thread. While a repetition is armed, `fire` ends the process; between
    // two of them the run's own document may be in the writing, so none is written here.
    [[noreturn]] void fail_now(const std::string &reason) {
        std::lock_guard<std::mutex> lock(m_);
        if (armed_) fire_("FAILED", "repetition " + std::to_string(rep_) + ": " + reason);
        std::fprintf(stderr, "qvo: FAILED outside a repetition, no document written: %s\n",
                     reason.c_str());
        std::fflush(nullptr);
        std::_Exit(1);
    }

private:
    void watch() {
        std::unique_lock<std::mutex> lock(m_);
        // No repetition armed yet: none can fall due before a full budget from now.
        auto wake = clock::now() + budget_;
        for (;;) {
            if (cv_.wait_until(lock, wake, [this] { return stop_; })) return;
            const auto now = clock::now();
            if (!armed_) {
                // Between two repetitions: the next one starts no earlier than now.
                wake = now + budget_;
                continue;
            }
            const auto due = start_ + budget_;
            if (now >= due) {  // fire_ never returns
                char s[32];
                std::snprintf(s, sizeof s, "%g", seconds_);
                fire_("DEADLINE", "repetition " + std::to_string(rep_) + ": deadline exceeded "
                                      + "after " + s + " s -- the body did not return (--deadline)");
            }
            wake = due;
        }
    }

    Fire                    fire_;
    double                  seconds_{0};
    clock::duration         budget_{};
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    stop_{false};
    bool                    armed_{false};
    int                     rep_{-1};
    clock::time_point       start_{};
    std::thread             thread_;  // last: started once every member above exists
};

// The run in progress, for qvo::fail(): published by run() once its Deadline exists, withdrawn
// before the Deadline is destroyed. Every body joins its threads before it returns, so no caller
// of fail() outlives the object it reaches through this.
std::atomic<Deadline *> g_running{nullptr};

struct PublishRun {
    explicit PublishRun(Deadline &d) noexcept { g_running.store(&d, std::memory_order_release); }
    ~PublishRun() { g_running.store(nullptr, std::memory_order_release); }
    PublishRun(const PublishRun &)            = delete;
    PublishRun &operator=(const PublishRun &) = delete;
};

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
std::vector<int>           g_pinned_cpus;
}  // namespace

const std::vector<int> &pinned_cpus() noexcept { return g_pinned_cpus; }

bool thread_pinning_supported() noexcept {
#if defined(_WIN32) || defined(__linux__)
    return true;
#else
    // macOS has no pthread_setaffinity_np, and the THREAD_AFFINITY_POLICY shim qb documents is a
    // grouping hint that arm64 does not implement at all. Reporting true here would commit the
    // exact defect this harness refuses: a pin that reports success and does not happen.
    return false;
#endif
}

bool pin_this_thread(int cpu) noexcept {
#if defined(_WIN32)
    if (cpu < 0 || cpu >= 64) return false;
    const DWORD_PTR mask = DWORD_PTR{1} << cpu;
    // Returns the PREVIOUS mask; 0 means the call failed.
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) return false;
    cpu_set_t got;
    CPU_ZERO(&got);
    if (pthread_getaffinity_np(pthread_self(), sizeof(got), &got) != 0) return false;
    return CPU_ISSET(cpu, &got) && CPU_COUNT(&got) == 1;
#else
    (void) cpu;
    return false;
#endif
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
    std::filesystem::path out_path;
    bool        describe = false;
    bool        no_pin   = false;
    double      deadline_arg = -1;  // --deadline; negative = not given, derive it from the spec
    Pin         pin;
    Params      params;

#if defined(_WIN32)
    // The C runtime's narrow argv can lose characters outside the current ANSI code page.
    // Keep the native path for --out, including a Unicode results directory from Python.
    int      wide_argc = 0;
    LPWSTR * wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
#endif

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
#if defined(_WIN32)
            next();  // Advance the narrow parser; the native path comes from the wide argument.
            if (!wide_argv || wide_argc != argc) fatal("cannot decode native Windows --out path");
            out_path = std::filesystem::path(wide_argv[i]);
#else
            out_path = next();
#endif
        } else if (a == "--cpus") {
            pin.cpus      = parse_cpu_list(next());
            pin.requested = true;
        } else if (a == "--no-pin") {
            no_pin = true;
        } else if (a == "--deadline") {
            // Parsed whole: 0 disables the watchdog, so a typo read as 0 would disable it silently.
            const std::string v   = next();
            char             *end = nullptr;
            const double      s   = std::strtod(v.c_str(), &end);
            if (v.empty() || end != v.c_str() + v.size() || !std::isfinite(s) || s < 0)
                fatal("--deadline expects a number of seconds >= 0 (0: no deadline), got '" + v + "'");
            deadline_arg = s;
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
#if defined(_WIN32)
    if (wide_argv) LocalFree(wide_argv);
#endif

    if (repetitions < 1) fatal("--repetitions must be >= 1");
    if (warmup < 0) fatal("--warmup must be >= 0");

    // Pinning happens BEFORE any framework object is constructed, so every thread the framework
    // creates inherits the mask. A thread a static initialiser started earlier -- qb's log writer
    // -- is pinned with it: on Windows by the process mask, on Linux one thread at a time (above),
    // and the document records how many threads were read back there (`pinned_threads`).
    if (pin.requested && !no_pin) {
        std::string why;
        if (!apply_and_verify_pin(pin.cpus, pin.threads, why))
            fatal("CPU pinning was requested and could not be verified (" + why
                  + "). Refusing to produce a number that a hybrid CPU would make meaningless. "
                    "Pass --no-pin to measure anyway; the result is then marked pinned:false.");
        pin.applied   = true;
        g_pinned_cpus = pin.cpus;
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
    if (spec.work_unit.empty() != !spec.work_units)
        fatal("benchmark " + spec.benchmark +
              " declares work_unit without work_units or the reverse -- both or neither");

    const std::uint64_t want          = spec.expected(params);
    const std::uint64_t want_messages = spec.expected_messages ? spec.expected_messages(params) : 0;

    // Lower bounds on observations, computed once like the expected values above.
    std::map<std::string, std::uint64_t> at_least;
    for (const auto &kv : spec.observed_at_least) {
        if (!kv.second)
            fatal("benchmark " + spec.benchmark + " declares a lower bound for observation '" + kv.first
                  + "' with no function");
        at_least[kv.first] = kv.second(params);
    }

    std::vector<double>        work_ns, total_ns, setup_ns, teardown_ns;
    std::vector<std::string>   failures;
    // Every measured repetition's observations, by name, in repetition order.
    std::map<std::string, std::vector<std::uint64_t>> observed;

    // The result document: one builder for the end of the run and for the deadline's watchdog, so
    // a blocked run's document has the shape of any other failed run. A run with a failed
    // repetition has no publishable timing, even when earlier repetitions passed: the reasons are
    // kept, and no partial sample or summary is written unless `ok`.
    auto document = [&](bool ok, const std::vector<std::string> &why) -> std::string {
        const std::vector<double>  none;
        const std::vector<double> &work  = ok ? work_ns : none;
        const std::vector<double> &total = ok ? total_ns : none;
        const std::vector<double> &setup = ok ? setup_ns : none;

        std::ostringstream j;
        j << "{\n";
        j << "  \"schema\": \"qvo/result/1\",\n";
        j << "  \"benchmark\": " << quoted(spec.benchmark) << ",\n";
        j << "  \"framework\": " << quoted(spec.framework) << ",\n";
        j << "  \"framework_version\": " << quoted(spec.framework_version) << ",\n";
        j << "  \"verified\": " << (ok ? "true" : "false") << ",\n";
        j << "  \"expected_checksum\": " << want << ",\n";
        j << "  \"expected_messages\": " << want_messages << ",\n";
        if (spec.work_units) {
            j << "  \"work_unit\": " << quoted(spec.work_unit) << ",\n";
            j << "  \"work_units\": " << spec.work_units(params) << ",\n";
        }
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
        // Linux only: every thread present when the mask was applied, each read back (Huly QB-841).
        if (pin.threads > 0) j << "  \"pinned_threads\": " << pin.threads << ",\n";
        j << "  \"thread_pinning_supported\": " << (thread_pinning_supported() ? "true" : "false")
          << ",\n";

        write_env(j);

        j << "  \"idiom\": {\n";
        j << "    \"source\": " << quoted(spec.idiom_source) << ",\n";
        j << "    \"note\": " << quoted(spec.idiom_note) << "\n";
        j << "  },\n";

        j << "  \"caveats\": [";
        for (std::size_t i = 0; i < spec.caveats.size(); ++i)
            j << (i ? ", " : "") << quoted(spec.caveats[i]);
        j << "],\n";

        j << "  \"failures\": [";
        for (std::size_t i = 0; i < why.size(); ++i) j << (i ? ", " : "") << quoted(why[i]);
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
        array("work_ns", work, false);
        array("total_ns", total, false);
        array("outside_window_ns", setup, false);

        // Written only when there is something to write, so the document of a benchmark that
        // observes nothing is byte-for-byte the shape it always had.
        if (!observed.empty()) {
            j << "  \"observed\": {";
            bool first = true;
            for (const auto &kv : observed) {
                const auto &s = kv.second;
                j << (first ? "\n" : ",\n") << "    " << quoted(kv.first) << ": {\"samples\": [";
                for (std::size_t i = 0; i < s.size(); ++i) j << (i ? ", " : "") << s[i];
                std::vector<double> d;
                d.reserve(s.size());
                for (const std::uint64_t v : s) d.push_back(static_cast<double>(v));
                char p50[64];
                std::snprintf(p50, sizeof p50, "%.1f", percentile(d, 0.50));
                j << "], \"min\": " << *std::min_element(s.begin(), s.end()) << ", \"p50\": " << p50
                  << ", \"max\": " << *std::max_element(s.begin(), s.end()) << "}";
                first = false;
            }
            j << "\n  },\n";
        }
        if (!at_least.empty()) {
            j << "  \"observed_at_least\": {";
            bool first = true;
            for (const auto &kv : at_least) {
                j << (first ? "" : ", ") << quoted(kv.first) << ": " << kv.second;
                first = false;
            }
            j << "},\n";
        }

        auto stat = [&](const char *name, double v, bool last) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.1f", v);
            j << "    \"" << name << "\": " << buf << (last ? "\n" : ",\n");
        };
        if (ok) {
            j << "  \"summary\": {\n";
            stat("work_min", *std::min_element(work_ns.begin(), work_ns.end()), false);
            stat("work_p50", percentile(work_ns, 0.50), false);
            stat("work_p99", percentile(work_ns, 0.99), false);
            stat("work_iqr", percentile(work_ns, 0.75) - percentile(work_ns, 0.25), true);
            j << "  }\n";
        } else {
            j << "  \"summary\": {}\n";
        }
        j << "}\n";
        return j.str();
    };

    // The deadline of one repetition (`Deadline` above; the contract is under run() in harness.h).
    // --deadline is taken as given. Otherwise the spec's own, else the harness default -- both
    // stated for the spec's DEFAULT parameters -- grows with the work the requested parameters
    // declare, and never shrinks: a smaller run keeps the full deadline.
    if (!std::isfinite(spec.deadline_s) || spec.deadline_s < 0)
        fatal("benchmark " + spec.benchmark + " declares a negative or non-finite deadline_s");
    double deadline_s = deadline_arg;
    if (deadline_s < 0) {
        deadline_s = spec.deadline_s > 0 ? spec.deadline_s : kDefaultDeadlineSeconds;
        if (spec.work_units) {
            Params defaults;
            for (const auto &kv : spec.params) defaults.set(kv.first, kv.second);
            const std::uint64_t at_defaults = spec.work_units(defaults);
            const std::uint64_t requested   = spec.work_units(params);
            if (at_defaults > 0 && requested > at_defaults)
                deadline_s *= static_cast<double>(requested) / static_cast<double>(at_defaults);
        }
    }
    deadline_s = (std::min)(deadline_s, kMaxDeadlineSeconds);  // parenthesised: <windows.h>'s min macro

    // Runs while a repetition is armed, on the watchdog's thread (the deadline) or on the thread
    // that called qvo::fail(): the body is inside its call, and its thread writes nothing the
    // document reads until the body has returned.
    Deadline deadline(deadline_s, [&](const char *verdict, const std::string &failure) {
        std::vector<std::string> why = failures;
        why.push_back(failure);
        std::string err;
        if (!try_write_document(out_path, document(false, why), err)) {
            std::fprintf(stderr, "qvo: FATAL: %s\n", err.c_str());
            std::fflush(stderr);
            std::_Exit(2);
        }
        std::fprintf(stderr, "qvo: %s/%s %s: %s\n", spec.framework.c_str(), spec.benchmark.c_str(),
                     verdict, failure.c_str());
        std::fflush(stderr);
        // Never exit(): the body's threads are still running, and static destructors run under
        // them are a crash or a hang. The document is written; nothing else needs flushing.
        std::_Exit(1);
    });
    const PublishRun published(deadline);  // qvo::fail() reaches this run's document through it

    const int all_reps = warmup + repetitions;
    for (int rep = 0; rep < all_reps; ++rep) {
        Watch w;
        deadline.arm(rep);  // outside the repetition's timed span, like disarm() below
        auto  t_begin = clock::now();
        Answer got;
        try {
            got = body(params, w);
        } catch (const NotApplicable &na) {
            deadline.disarm();
            // The body declined the configuration. Emit a document with the reason and no
            // timings, so the cell exists in the results and the report can say why it is
            // empty; exit 3 so run.py tells it apart from a defect (1) and a harness error (2).
            std::ostringstream j;
            j << "{\n";
            j << "  \"schema\": \"qvo/result/1\",\n";
            j << "  \"benchmark\": " << quoted(spec.benchmark) << ",\n";
            j << "  \"framework\": " << quoted(spec.framework) << ",\n";
            j << "  \"framework_version\": " << quoted(spec.framework_version) << ",\n";
            j << "  \"verified\": false,\n";
            j << "  \"not_applicable\": " << quoted(na.reason) << ",\n";
            j << "  \"params\": {";
            {
                bool first = true;
                for (const auto &kv : params.all()) {
                    j << (first ? "" : ", ") << quoted(kv.first) << ": " << kv.second;
                    first = false;
                }
            }
            j << "},\n";
            write_env(j);
            j << "  \"caveats\": [";
            for (std::size_t i = 0; i < spec.caveats.size(); ++i)
                j << (i ? ", " : "") << quoted(spec.caveats[i]);
            j << "],\n";
            j << "  \"work_ns\": [],\n";
            j << "  \"summary\": {}\n";
            j << "}\n";
            write_document(out_path, j.str());
            std::fprintf(stderr, "qvo: %s/%s NOT APPLICABLE: %s\n", spec.framework.c_str(),
                         spec.benchmark.c_str(), na.reason.c_str());
            return 3;
        }
        auto  t_end   = clock::now();
        deadline.disarm();

        if (got.checksum != want) {
            failures.push_back("repetition " + std::to_string(rep) + ": checksum "
                               + std::to_string(got.checksum) + " != expected "
                               + std::to_string(want));
            continue;
        }
        // A spec that declares a message count asserts it: a body that reports none fails like
        // one that reports the wrong one -- an assertion an adapter can skip by reporting 0 asserts
        // nothing (Huly QB-1017).
        if (want_messages != 0 && got.messages != want_messages) {
            failures.push_back(
                "repetition " + std::to_string(rep) + ": "
                + (got.messages == 0 ? std::string("reported no message count")
                                     : "delivered " + std::to_string(got.messages) + " messages")
                + ", expected " + std::to_string(want_messages));
            continue;
        }
        // A declared lower bound is asserted like the checksum: a missing or short observation is
        // a defect (the adapter skipped work the semantics requires), never a timing.
        std::string short_of;
        for (const auto &kv : at_least) {
            const auto it = got.observed.find(kv.first);
            if (it == got.observed.end()) {
                short_of = "observation '" + kv.first + "' is missing -- the spec asserts it is at least "
                           + std::to_string(kv.second);
                break;
            }
            if (it->second < kv.second) {
                short_of = "observed " + kv.first + "=" + std::to_string(it->second)
                           + ", below its asserted lower bound " + std::to_string(kv.second);
                break;
            }
        }
        if (!short_of.empty()) {
            failures.push_back("repetition " + std::to_string(rep) + ": " + short_of);
            continue;
        }
        if (!w.started() || !w.stopped()) {
            failures.push_back("repetition " + std::to_string(rep)
                               + ": the body never marked its workload window (Watch::start/stop)");
            continue;
        }

        if (rep < warmup) continue;

        for (const auto &kv : got.observed) observed[kv.first].push_back(kv.second);

        const double total = std::chrono::duration<double, std::nano>(t_end - t_begin).count();
        const double work  = w.work_ns();
        work_ns.push_back(work);
        total_ns.push_back(total);
        // Everything outside the marked window: framework construction, actor creation, shutdown.
        // Reported so a fast workload window bought by an expensive startup is visible.
        setup_ns.push_back(total - work);
    }

    const bool ok = failures.empty() && work_ns.size() == static_cast<std::size_t>(repetitions);
    write_document(out_path, document(ok, failures));

    if (!ok) {
        std::fprintf(stderr, "qvo: %s/%s FAILED VERIFICATION\n", spec.framework.c_str(),
                     spec.benchmark.c_str());
        for (const auto &f : failures) std::fprintf(stderr, "qvo:   %s\n", f.c_str());
        return 1;
    }
    return 0;
}

void not_applicable(const std::string &reason) {
    throw NotApplicable{reason};
}

void fail(const std::string &reason) {
    if (Deadline *const run = g_running.load(std::memory_order_acquire)) run->fail_now(reason);
    std::fprintf(stderr, "qvo: FAILED outside a run, no document written: %s\n", reason.c_str());
    std::fflush(nullptr);
    std::_Exit(1);
}

void refuse(const std::string &reason) {
    std::fprintf(stderr, "qvo: FATAL: %s\n", reason.c_str());
    std::fflush(nullptr);
    std::_Exit(2);
}

}  // namespace qvo
