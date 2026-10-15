// The negative control's subject: a benchmark that can be told to be defective.
//
// FAIRNESS.md section 0 rests entirely on the claim that the harness REJECTS a framework which
// drops, duplicates or miscounts messages. That claim is worth nothing until something has been
// watched being rejected -- a verifier nobody has seen fire is not known to work.
//
// This binary is that something. It implements savina/ping-pong correctly, and `QVO_CONTROL_PLANT`
// makes it wrong in one specific, realistic way at a time:
//
//   none          correct -- the POSITIVE control. The battery must see this PASS, or a battery
//                 that rejects everything would look like a working one.
//   drop-rare     loses one message in 10^7. The headline defect: a runtime that silently drops
//                 under load wins any wall-clock benchmark, and this is the shape the checksum
//                 exists to catch. Chosen to be rarer than anything a spot check would notice.
//   drop-one      loses exactly one message in the whole run.
//   duplicate     delivers one message twice. This is why the reduction is a wrapping SUM and not
//                 an XOR -- an XOR would be unchanged by a duplicate and this control would MISS.
//   wrong-answer  returns a plausible but incorrect checksum.
//   short-count   right checksum, wrong message count. Catches a framework reaching the correct
//                 answer by doing a different amount of work.
//   no-count      right checksum, NO message count (Answer::messages = 0) against a spec that
//                 declares one. Until Huly QB-1017 the harness skipped the comparison for 0.
//   no-window     never marks the measured window, so a timing would be meaningless.
//   fail-second   correct first call, wrong checksum on the second (partial measured run).
//   fail-first    wrong first call, correct later calls (also a defective warmup).
//   hang          the first call opens its window and never returns: a lost message, which in
//                 a shape that waits for the end of its protocol blocks the run instead of
//                 moving the checksum. Run with a short --deadline: must be REJECTED by the
//                 harness's deadline, with a failure document, inside it.
//   hang-teardown the first two calls are correct; the third closes its window and never
//                 returns: a shutdown that never joins. The same verdict, naming repetition 2.
//   fail-then-hang a wrong checksum on the first call, then the second never returns: the
//                 deadline's document must keep the earlier failure beside its own.
//
// A failure the body DECLARES -- a protocol violation a spec or an adapter detects, which no
// checksum can carry in time -- goes through qvo::fail(): exit 1 and a failure document, never
// std::abort, whose code on Windows is 0xC0000409 or the not-applicable 3 (Huly QB-1018):
//
//   fail-body          the body's own thread calls qvo::fail() inside its window.
//   fail-thread        another thread calls it while the body waits for the end of its protocol --
//                      an actor's handler on a framework thread, the body blocked in its join.
//   fail-after-failure a wrong checksum on the first call, then the second's thread calls it: the
//                      document keeps the earlier failure before the declared one.
//   refuse-thread      another thread calls qvo::refuse(): exit 2, never 3, from any thread.
//
// The subject also reports the rounds it actually played as an OBSERVATION and declares the
// parameter as its asserted lower bound (Answer::observed, Spec::observed_at_least) -- the shape a
// benchmark uses for a minimum amount of work its checksum alone cannot prove:
//
//   observe-below    reports one round fewer than the bound: must be REJECTED.
//   observe-missing  reports no observation at all: must be REJECTED.
//   observe-extra    reports above the bound plus an unbounded observation: must be ACCEPTED,
//                    with both written into the result document.
//
// And one about placement (Linux, where the affinity mask belongs to each thread):
//
//   pre-main-thread  a static initialiser starts a thread before main() -- the shape of qb's log
//                    writer -- and the body declares a failure (qvo::fail) unless that thread
//                    runs on exactly the pinned set. Run pinned: must be ACCEPTED, the document
//                    counting both threads in `pinned_threads` (Huly QB-841).
//
// It is deliberately NOT built by qvo_add_benchmark and its name does not start with "qvo-", so
// tools/run.py cannot discover it and no planted result can ever reach a published table.

#include <qvospec/savina/ping-pong.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__)
#    include <sched.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

namespace {

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

#if defined(__linux__)
// The `pre-main-thread` plant's thread: started during static initialisation, before main() and so
// before the harness pins anything; it idles until the process ends normally.
struct PreMainThread {
    std::atomic<pid_t> tid{0};
    std::atomic<bool>  stop{false};
    std::thread        thread;

    PreMainThread() {
        if (plant() != "pre-main-thread") return;
        thread = std::thread([this] {
            tid.store(static_cast<pid_t>(::syscall(SYS_gettid)));
            while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        while (tid.load() == 0) std::this_thread::yield();
    }
    ~PreMainThread() {
        if (!thread.joinable()) return;
        stop.store(true);
        thread.join();
    }
    PreMainThread(const PreMainThread &)            = delete;
    PreMainThread &operator=(const PreMainThread &) = delete;
};
PreMainThread g_pre_main;

// Declares a failure unless the thread started before main() runs on exactly the pinned set.
void check_pre_main_thread() {
    const auto &cpus = qvo::pinned_cpus();
    if (cpus.empty()) qvo::fail("pre-main-thread: run it pinned (--cpus), its check reads the pinned set");
    cpu_set_t want, got;
    CPU_ZERO(&want);
    CPU_ZERO(&got);
    for (const int c : cpus) CPU_SET(c, &want);
    if (sched_getaffinity(g_pre_main.tid.load(), sizeof got, &got) != 0)
        qvo::fail("pre-main-thread: cannot read the mask of the thread started before main()");
    if (!CPU_EQUAL(&got, &want))
        qvo::fail("a thread started before main() runs outside the pinned CPU set ("
                  + std::to_string(CPU_COUNT(&got)) + " CPUs allowed, "
                  + std::to_string(CPU_COUNT(&want)) + " pinned)");
}
#endif

// What a benchmark does when the message that ends its protocol never comes: it waits.
[[noreturn]] void wait_forever() {
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    static unsigned   calls = 0;  // body calls are sequential within one harness process
    const unsigned    call  = calls++;
    const auto        rounds = static_cast<std::uint64_t>(p.get("messages"));
    const std::string mode   = plant();

    std::uint64_t acc       = 0;
    std::uint64_t delivered = 0;
    std::uint64_t played    = 0;

#if defined(__linux__)
    if (mode == "pre-main-thread") check_pre_main_thread();
#endif
    if (mode != "no-window") watch.start();
    if ((mode == "hang" && call == 0) || (mode == "fail-then-hang" && call == 1)) wait_forever();
    if (mode == "fail-body") qvo::fail("planted protocol violation on the body's thread");
    if (mode == "fail-thread" || (mode == "fail-after-failure" && call == 1)) {
        std::thread([] { qvo::fail("planted protocol violation on another thread"); }).detach();
        wait_forever();
    }
    if (mode == "refuse-thread") {
        std::thread([] { qvo::refuse("planted refusal on another thread"); }).detach();
        wait_forever();
    }

    for (std::uint64_t seq = rounds; seq-- > 0;) {
        ++played;
        // A dropped message is one whose contribution never reaches the accumulator. Modelled
        // exactly that way rather than by fiddling the total, so the control exercises the same
        // arithmetic a real loss would.
        if (mode == "drop-rare" && (seq % 10000000ull) == 7ull) continue;
        if (mode == "drop-one" && seq == rounds / 2) continue;

        acc += qvo::mix(seq);
        delivered += 2;

        if (mode == "duplicate" && seq == rounds / 3) {
            acc += qvo::mix(seq);
            delivered += 2;
        }
    }

    if (mode != "no-window") watch.stop();
    if (mode == "hang-teardown" && call == 2) wait_forever();

    if (mode == "wrong-answer" || (mode == "fail-second" && call == 1) ||
        ((mode == "fail-first" || mode == "fail-then-hang" || mode == "fail-after-failure") &&
         call == 0))
        acc += 1;
    if (mode == "short-count") delivered -= 2;
    if (mode == "no-count") delivered = 0;

    qvo::Answer answer{acc, delivered};
    if (mode == "observe-below") answer.observed["rounds"] = played - 1;
    else if (mode == "observe-extra") {
        answer.observed["rounds"] = played + 5;
        answer.observed["extra"]  = 42;
    } else if (mode != "observe-missing")
        answer.observed["rounds"] = played;
    return answer;
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/ping-pong";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::ping_pong::params();
    spec.expected          = qvospec::savina::ping_pong::expected;
    spec.expected_messages = qvospec::savina::ping_pong::expected_messages;
    spec.observed_at_least["rounds"] = [](const qvo::Params &p) {
        return static_cast<std::uint64_t>(p.get("messages"));
    };
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "plants a defect selected by QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
