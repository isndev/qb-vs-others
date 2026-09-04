// savina/fork-join — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0).
//
// Savina reference: ForkJoin (throughput) (Imam & Sarkar, AGERE 2014), benchmark 5 of the
// "micro" group -- the one that fans messages OUT to a fixed set of workers, not the
// actor-creation one (benchmark 4). Deviations are recorded in benchmarks/savina/fork-join.md.

#ifndef QVOSPEC_SAVINA_FORK_JOIN_H
#define QVOSPEC_SAVINA_FORK_JOIN_H

#include <qvo/harness.h>

namespace qvospec::savina::fork_join {

inline constexpr const char *kId = "savina/fork-join";

// The shape: ONE master sends `messages` jobs to EACH of `actors` workers, round-robin, one way,
// as fast as it can; a worker that has received its last job sends one `done` back. Nothing
// replies per job, so this is the cheapest dispatch a framework has -- fire-and-forget into many
// mailboxes -- with the scheduler deciding how `actors` runnable workers share `cores` threads.
//
// `actors`   -- workers. Savina's own default, 60.
// `messages` -- jobs PER WORKER. Savina's own default, 10 000 (600 000 jobs per repetition).
// `work`     -- iterations of qvo::spin_work a worker performs per job. DEVIATION FROM SAVINA:
//               Savina's worker computes one sin() per job, a few nanoseconds that exist to keep
//               a JIT from deleting the actor; the checksum here already makes every job
//               observable, so the default is 0 and the cell measures dispatch alone. A non-zero
//               value turns this into a parallel-speed-up measurement; a document measured under
//               one carries the value in its params.
// `cores`    -- worker budget. The master lives on core 0; worker w lives on core w % cores, so
//               with cores=2 half the jobs stay on the master's core and half cross.
// `wait`     -- 1 = spin, 0 = park.
inline std::map<std::string, long long> params() {
    return {{"actors", 60}, {"messages", 10000}, {"work", 0}, {"cores", 2}, {"wait", 1}};
}

// The value worker `a` folds into its accumulator for its `i`-th job. Declared once here so that
// every framework AND expected() compute the same thing, the optional work included.
inline std::uint64_t job_value(std::uint64_t a, std::uint64_t i, int work) noexcept {
    const std::uint64_t key = (a << 32) | i;
    return work > 0 ? qvo::mix(key) + qvo::spin_work(key, work) : qvo::mix(key);
}

inline std::uint64_t expected(const qvo::Params &p) {
    const auto    actors   = static_cast<std::uint64_t>(p.get("actors"));
    const auto    messages = static_cast<std::uint64_t>(p.get("messages"));
    const auto    work     = static_cast<int>(p.get("work"));
    std::uint64_t acc      = 0;
    for (std::uint64_t a = 0; a < actors; ++a)
        for (std::uint64_t i = 0; i < messages; ++i) acc += job_value(a, i, work);
    return acc;
}

// The report divides by this: one job delivered to a worker.
inline constexpr const char *kWorkUnit = "message";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("actors")) *
           static_cast<std::uint64_t>(p.get("messages"));
}

// actors x messages jobs + one done per worker.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return work_units(p) + static_cast<std::uint64_t>(p.get("actors"));
}

}  // namespace qvospec::savina::fork_join

#endif  // QVOSPEC_SAVINA_FORK_JOIN_H
