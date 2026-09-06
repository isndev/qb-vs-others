// savina/fib — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Recursive Fibonacci (Imam & Sarkar, AGERE 2014), benchmark 18, the first of
// the "parallel" group. Deviations are recorded in benchmarks/savina/fib.md.

#ifndef QVOSPEC_SAVINA_FIB_H
#define QVOSPEC_SAVINA_FIB_H

#include <qvo/harness.h>

#include <cstdint>
#include <vector>

namespace qvospec::savina::fib {

inline constexpr const char *kId = "savina/fib";

// The shape: an actor asked for fib(n) with n > 2 CREATES two children, asks them for fib(n-1)
// and fib(n-2), sums their two responses, responds to its own parent and DIES. A leaf (n <= 2)
// responds 1 and dies. Every actor therefore lives for exactly one request and one response,
// and the benchmark is the cost of an actor's whole life -- creation, one inbound message, its
// own two sends, and destruction -- repeated nodes(n) times. It is the only Savina shape in
// this repository whose actors are born and die INSIDE the window; every other one builds its
// field before the watch starts. ROADMAP.md lists actor creation cost as unmeasured until it.
//
// Two seeds are created before the window, one for each of the root's two sub-problems, and
// placed by the adapter on the cores the cell allows (seed s on core s % cores); everything
// below a seed is created by its parent, on whatever core the framework's spawn primitive puts
// it. The driver plays the root: it sends fib(n-1) to seed 0 and fib(n-2) to seed 1, and closes
// the window on the second response.
//
// `n`     -- the argument. Savina's own default is 25. The default HERE is 23, a DEVIATION with
//            a recorded reason (fib.md): a FIFO mailbox expands this tree breadth-first, so at the
//            peak nearly every node is alive at once, and qb's actor id is a 16-bit slot per
//            VirtualCore -- 65 534 live actors on one core, of which n=23 needs 57 312 and n=24
//            would need 92 734. The cap is qb's, is reported as qb's, and the parameter is the
//            largest Savina-shaped tree every framework in the field can hold on one core.
// `cores` -- 1: everything on one thread; 2: the seeds on two pinned threads, so the two
//            sub-trees run in parallel where the framework's spawn keeps a child near its parent
//            and are balanced where a pool decides.
// `wait`  -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"n", 23}, {"cores", 2}, {"wait", 1}};
}

// fib(1) = fib(2) = 1.
inline std::uint64_t fib(long long n) {
    std::uint64_t a = 1, b = 1;
    for (long long i = 2; i < n; ++i) {
        const std::uint64_t c = a + b;
        a                     = b;
        b                     = c;
    }
    return n < 1 ? 0 : b;
}

// Nodes of the call tree rooted at fib(n): T(n) = 1 + T(n-1) + T(n-2), T(1) = T(2) = 1, which
// closes to 2 fib(n) - 1.
inline std::uint64_t nodes(long long n) { return 2 * fib(n) - 1; }

// Every response carries the value AND a fold of the sub-tree that produced it:
//
//     chk(leaf)   = mix(1)
//     chk(node n) = mix(fib(n)) + chk(n-1) + chk(n-2)
//
// The root's checksum is chk(n-1) + chk(n-2), computed here by the same recurrence with a memo.
// A response dropped or duplicated anywhere in the tree changes the sum its ancestors carry; a
// framework that answered a request without creating the actor would have to reproduce the
// whole fold, which is the computation itself.
inline std::uint64_t chk(long long n, std::vector<std::uint64_t> &memo) {
    if (n <= 2) return qvo::mix(1);
    if (memo[static_cast<std::size_t>(n)]) return memo[static_cast<std::size_t>(n)];
    const std::uint64_t v = qvo::mix(fib(n)) + chk(n - 1, memo) + chk(n - 2, memo);
    return memo[static_cast<std::size_t>(n)] = v;
}

inline std::uint64_t expected(const qvo::Params &p) {
    const long long            n = p.get("n");
    std::vector<std::uint64_t> memo(static_cast<std::size_t>(n) + 1, 0);
    return chk(n - 1, memo) + chk(n - 2, memo);
}

// The report divides by this: one actor's whole life -- created, asked, answered, destroyed.
// The two seeds are counted: they are asked and answer and die exactly like the rest, only their
// creation happens before the window.
inline constexpr const char *kWorkUnit = "actor";
inline std::uint64_t work_units(const qvo::Params &p) { return nodes(p.get("n")) - 1; }

// One request and one response per node of the tree below the root (the root is the driver).
// Counted at the receivers: every node counts its request and its children's responses, the
// driver counts the two seeds' responses; the fold carries the count up with the checksum.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return 2 * (nodes(p.get("n")) - 1);
}

}  // namespace qvospec::savina::fib

#endif  // QVOSPEC_SAVINA_FIB_H
