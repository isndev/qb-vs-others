// savina/fork-join-create — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Fork Join (actor creation) (Imam & Sarkar, AGERE 2014), benchmark 4 of the
// "micro" group -- `fjcreate` in the suite's sources, the actor-creation twin of benchmark 5
// (savina/fork-join, the throughput one). Deviations are recorded in
// benchmarks/savina/fork-join-create.md.

#ifndef QVOSPEC_SAVINA_FORK_JOIN_CREATE_H
#define QVOSPEC_SAVINA_FORK_JOIN_CREATE_H

#include <qvo/harness.h>

#include <cstdint>

namespace qvospec::savina::fork_join_create {

inline constexpr const char *kId = "savina/fork-join-create";

// The shape: a creator FORKS `actors` short-lived actors in one tight loop -- create one, send it
// its single job, create the next -- and every created actor does its job, answers its creator
// once and TERMINATES. Savina's loop is exactly that (ForkJoinAkkaActorBenchmark.runIteration:
// actorOf, start, `!`, `actors` times), with the actor ending through `exit()` after one message.
// Unlike savina/fib the actors here are born from a FLAT loop rather than a recursion, so the
// creator's spawn rate and a whole burst of actors alive at once are what is measured, not a
// tree's depth.
//
// The JOIN: Savina's actors answer nobody and its driver waits for the actor system to
// terminate. Here every actor answers its creator with its result (a `done`), the creator folds
// them, and the window closes when the driver has every creator's summary -- the answer has to be
// asserted (FAIRNESS.md section 0), and a reply to the creator is the join the benchmark's name
// promises.
//
// `actors`   -- actors created per repetition. Savina's own default, N = 40 000. Savina's `C`
//               ("num channels") is parsed by its config and read by no actor implementation;
//               it has no counterpart here.
// `work`     -- iterations of qvo::spin_work an actor performs on its job. DEVIATION FROM SAVINA,
//               the same one savina/fork-join records: Savina's actor computes sin(37.2)^2, a few
//               nanoseconds that exist to keep a JIT from deleting it; the checksum here already
//               makes every job observable, so the default is 0 and the cell measures an actor's
//               life alone. A non-zero value puts a deterministic computation back, folded into
//               the checksum.
// `cores`    -- worker budget, AND the number of creators. DEVIATION FROM SAVINA: Savina's
//               creator is the JVM's main thread, outside the actor system; here creation happens
//               inside the measured system, from actors, so there is one creator per core of the
//               cell -- creator s is placed by the adapter on core s % cores and forks the actors
//               whose index i satisfies i % cores == s. Every framework gets the same structure:
//               with cores=1 one creator forks them all (Savina's shape exactly), with cores=2 two
//               creators fork half each, in parallel.
// `wait`     -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"actors", 40000}, {"work", 0}, {"cores", 2}, {"wait", 1}};
}

// The number of creators of a cell: one per core, at least one.
inline std::uint64_t creators(const qvo::Params &p) {
    const long long cores = p.get("cores");
    return cores < 1 ? 1u : static_cast<std::uint64_t>(cores);
}

// How many actors creator `s` forks: the indices i in [0, actors) with i % creators == s.
inline std::uint64_t share(std::uint64_t actors, std::uint64_t creators, std::uint64_t s) {
    return s < actors % creators ? actors / creators + 1 : actors / creators;
}

// The value an actor folds for its job. `self` is the index the actor was CREATED with (a
// constructor argument), `job` the index its one message CARRIES: a job delivered to the right
// actor has self == job, and a job delivered to any other actor folds a different key. Declared
// once here so that every framework AND expected() compute the same thing, the optional work
// included.
inline std::uint64_t job_value(std::uint64_t self, std::uint64_t job, int work) noexcept {
    const std::uint64_t key = (self << 32) | job;
    return work > 0 ? qvo::mix(key) + qvo::spin_work(key, work) : qvo::mix(key);
}

// Every actor answers its creator with job_value(i, i, work); a creator folds its answers, as a
// wrapping sum, into one summary; the driver adds the summaries. A job dropped, delivered twice,
// delivered to the wrong actor, an answer dropped or duplicated, an actor that skipped its work,
// or a summary lost or repeated all change the total.
inline std::uint64_t expected(const qvo::Params &p) {
    const auto    actors = static_cast<std::uint64_t>(p.get("actors"));
    const auto    work   = static_cast<int>(p.get("work"));
    std::uint64_t acc    = 0;
    for (std::uint64_t i = 0; i < actors; ++i) acc += job_value(i, i, work);
    return acc;
}

// The report divides by this: one actor's whole life -- created, sent its job, answered,
// terminated.
inline constexpr const char *kWorkUnit = "actor";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("actors"));
}

// Inside the window: per creator one `fork` order from the driver and one summary back; per actor
// one job and one done. Counted at the receivers: an actor counts its job and reports it in its
// done, a creator counts its order and every done (plus the job it carries), the driver counts
// every summary (plus what it carries).
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return 2 * static_cast<std::uint64_t>(p.get("actors")) + 2 * creators(p);
}

}  // namespace qvospec::savina::fork_join_create

#endif  // QVOSPEC_SAVINA_FORK_JOIN_CREATE_H
