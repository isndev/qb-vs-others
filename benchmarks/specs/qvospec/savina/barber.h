// savina/barber — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum is plain arithmetic
// with no framework linked (FAIRNESS.md section 0). The rules the ping-pong spec states about the
// checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Sleeping Barber (Imam & Sarkar, AGERE 2014), one of the "concurrency"
// benchmarks -- SleepingBarberConfig.java and SleepingBarberAkkaActorBenchmark.scala. Deviations
// are recorded in benchmarks/savina/barber.md.

#ifndef QVOSPEC_SAVINA_BARBER_H
#define QVOSPEC_SAVINA_BARBER_H

#include <qvo/harness.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace qvospec::savina::barber {

inline constexpr const char *kId = "savina/barber";

// The shape, Savina's own: a FACTORY creates `haircuts` customers -- each one an actor created
// INSIDE the window -- and sends each to the waiting ROOM, busy-working between two customers
// (the production rate). The room holds at most `room` waiting customers: a customer arriving at
// a full room is told `Full`, goes back to the factory (`Returned`) and is sent again. A customer
// let in is told `Wait` when the BARBER is busy; when the barber is asleep the room wakes him by
// sending itself `Next`. On `Next` the room hands its oldest customer to the barber (`Enter`) or,
// empty, tells the barber `Wait` -- he sleeps. The barber tells the customer `Start`, busy-works
// the haircut, tells the customer `Done` and asks the room for the `Next`. A customer told `Done`
// reports `Done` to the factory and dies. After `haircuts` reports the factory sends `Exit` down
// the chain factory -> room -> barber.
//
// The coordination is "blocking-shaped": a bounded buffer, a consumer that sleeps when it is empty
// and is woken by the first arrival, a producer and a consumer that each do real work. It is the
// first shape here whose actors do work of their own between messages.
//
// WHICH customers find the barber asleep, and (with a small room) which are turned away and how
// often, depends on the interleaving and differs run to run and framework to framework. The
// checksum is therefore built from what every interleaving must deliver (see expected()); the
// interleaving-dependent counts are reported by every implementation, never asserted.
//
// `haircuts` -- customers, each served exactly once. Savina's own default, 5 000; no deviation.
// `room`     -- waiting-room capacity. Savina's default is 1 000; the default HERE is 5 000 (= the
//               customers), a DEVIATION with a recorded reason (barber.md): on one thread an actor
//               runtime orders the room's and the factory's turns by its queue, not by the wall
//               clock both busy-works are tuned against, and a 1 000-seat room then turns most of
//               the run into customers bouncing off it -- a retry storm whose size is decided by
//               each framework's scheduling order, not by the problem. So the table cell never
//               times a full room; Savina's 1 000 is a declared side experiment (`room=1000`), and
//               every implementation still implements the full branch.
// `apr`      -- average production rate: the factory busy-works `uniform[0, apr) + 10` iterations
//               after each customer. Savina's own default, 1 000; no deviation.
// `ahr`      -- average haircut rate: the barber busy-works `uniform[0, ahr) + 10` iterations per
//               haircut. Savina's own default, 1 000; no deviation.
// `pace`     -- the factory's SHAPE, a declared axis (barber.md, "The factory's shape"): 0 = the
//               reference's, every customer produced inside the one Start handler; 1 = paced, one
//               customer per self-addressed Start (n-1 more messages). Every adapter implements
//               both; each adapter's table cell runs the faster of its two forms as measured on the
//               quiet host, and its main() says which. The result document records the value.
// `cores`    -- 1: everything on one thread; 2: for the frameworks that place, the factory and the
//               customers it creates on one core, the room and the barber on the other -- the
//               barber's turn (Next -> Enter) stays on his core, and production overlaps haircuts.
// `wait`     -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"haircuts", 5000}, {"room", 5000}, {"apr", 1000}, {"ahr", 1000},
            {"pace", 0},        {"cores", 2},   {"wait", 1}};
}

// The busy work. Savina's busyWait(limit) calls Math.random() `limit` times with
// limit = random.nextInt(rate) + 10; here the iteration count is drawn from a deterministic stream
// (the i-th production, the k-th haircut) with the same distribution, and the work is
// qvo::spin_work -- the harness's one busy-work function, identical object code for every
// framework. The barber's stream is indexed by HIS haircut count, not by the customer, so the sum
// of all haircut results is the same whichever customer each haircut lands on.
inline constexpr std::uint64_t kProductionRate = 0x5ba4be4f00000001ULL;
inline constexpr std::uint64_t kProductionSeed = 0x5ba4be4f00000002ULL;
inline constexpr std::uint64_t kHaircutRate    = 0x5ba4be4f00000003ULL;
inline constexpr std::uint64_t kHaircutSeed    = 0x5ba4be4f00000004ULL;

// Every parameter of this benchmark is a count or a rate that must be at least 1.
inline std::uint64_t at_least_one(long long value, const char *name) {
    if (value < 1) {
        std::fprintf(stderr, "savina/barber: %s must be >= 1 (got %lld)\n", name, value);
        std::abort();
    }
    return static_cast<std::uint64_t>(value);
}

// `pace`, which is 0 or 1: true for the paced factory.
inline bool paced(const qvo::Params &p) {
    const long long v = p.get("pace");
    if (v != 0 && v != 1) {
        std::fprintf(stderr, "savina/barber: pace must be 0 or 1 (got %lld)\n", v);
        std::abort();
    }
    return v == 1;
}

// Busy work after the i-th customer (i from 0).
inline std::uint64_t production_work(std::uint64_t i, std::uint64_t apr) noexcept {
    const auto iterations = static_cast<int>(qvo::mix(kProductionRate + i) % apr + 10);
    return qvo::spin_work(qvo::mix(kProductionSeed + i), iterations);
}

// The barber's k-th haircut (k from 0), whichever customer it is.
inline std::uint64_t haircut_work(std::uint64_t k, std::uint64_t ahr) noexcept {
    const auto iterations = static_cast<int>(qvo::mix(kHaircutRate + k) % ahr + 10);
    return qvo::spin_work(qvo::mix(kHaircutSeed + k), iterations);
}

// A customer's IDENTITY in the checksum: every count a customer or the room keeps about a customer
// is weighted by it, so a message delivered to the WRONG customer moves the sum as surely as a
// message lost or doubled. Customers are numbered from 1 in production order, and the number
// travels with the customer in every message that names one (Enter, Returned, the barber's Next).
inline std::uint64_t identity(std::uint64_t number) noexcept {
    return qvo::mix(0x5ba4be4f1d000000ULL + number);
}

// The per-message weights of the checksum, one per kind of delivery. Each is the mix of a tag, so
// a single delivery missing or doubled moves the sum by a weight, never by zero.
enum Tag : std::uint64_t {
    kTagStart = 1,  // a customer told Start
    kTagWait,       // a customer told Wait, or the room waking the barber for that customer
    kTagEnter,      // a customer let into the room
    kTagNext,       // a Next from the barber, naming the customer just served
    kTagCut,        // an Enter at the barber
    kTagFull,       // a customer told Full (cancelled by the room's rejection of that customer)
    kTagReturned,   // a Returned at the factory (cancelled by the same rejection)
    kTagNap,        // a Wait at the barber (cancelled by the room's count of them)
    kTagWake,       // the room's own Next that wakes the barber (cancelled by its count of them)
};
inline std::uint64_t weight(Tag t) noexcept { return qvo::mix(0xba4be4000000ULL + t); }

// A run that breaks the protocol in a way the checksum cannot see in time -- the room receiving
// more Nexts from the barber than there are haircuts, which could let its Exit leave on a matching
// sum before the last real Next -- stops here, loudly, instead of reporting a number.
[[noreturn]] inline void fail(const char *what) {
    std::fprintf(stderr, "savina/barber: protocol violated -- %s\n", what);
    std::abort();
}

// The checksum. With id(i) = identity(i), customer i reports to the factory
//
//     mix(i) + h + id(i) * (w(Start)*starts + w(Wait)*waits + w(Full)*fulls)
//
// where h is the value of the haircut it received in Done and starts/waits/fulls count what it was
// told. The factory adds every report, its own production work and w(Returned)*id(i) for every
// Returned of customer i. The room adds, customer by customer as it handles them,
//
//     + w(Enter)*id(i)                 for every customer i it lets in,
//     - (w(Full) + w(Returned))*id(i)  for every customer i it turns away,
//     + w(Wait)*id(i)                  for every customer i whose arrival wakes the barber,
//     + w(Next)*id(i)                  for every Next of the barber naming customer i,
//     - w(Nap)                         for every Wait it sends the barber,
//
// and at Exit w(Wake) * (its own Nexts received - its wake-ups sent). The barber adds
// w(Cut)*id(i) for every customer i he serves and w(Nap) for every nap. Every interleaving then
// gives the same sum, because each term is paired with its counterpart FOR THE SAME CUSTOMER:
//
//   * every customer is let in exactly once and is then EITHER told Wait OR wakes the barber;
//   * every rejection of customer i is one Full at i and one Returned of i at the factory;
//   * the barber serves every customer exactly once and names it in the Next that follows;
//   * every Wait the room sends the barber is one nap he counts, every wake-up one own Next;
//   * the barber's k-th haircut is haircut_work(k) whoever gets it, and it reaches the factory
//     through that customer's report.
//
// So a dropped, duplicated OR MISROUTED delivery of any kind -- including the Waits, which change
// nothing else -- moves the sum, and a framework that skips a haircut or a production misses its
// work. The room leaves for Exit only once the barber's n-th Next is in, and fail()s on an
// (n+1)-th: the factory's Exit and the barber's last Next come from two senders, and nothing
// orders them.
inline std::uint64_t expected(const qvo::Params &p) {
    const auto n   = at_least_one(p.get("haircuts"), "haircuts");
    const auto apr = at_least_one(p.get("apr"), "apr");
    const auto ahr = at_least_one(p.get("ahr"), "ahr");
    (void)at_least_one(p.get("room"), "room");
    (void)paced(p);
    const std::uint64_t per_customer = weight(kTagStart) + weight(kTagWait) + weight(kTagEnter) +
                                       weight(kTagNext) + weight(kTagCut);
    std::uint64_t acc = 0;
    for (std::uint64_t i = 1; i <= n; ++i) acc += qvo::mix(i) + identity(i) * per_customer;
    for (std::uint64_t k = 0; k < n; ++k) acc += haircut_work(k, ahr);
    for (std::uint64_t i = 0; i < n; ++i) acc += production_work(i, apr);
    return acc;
}

// The report divides by this: one haircut -- a customer created, let in, served, reported, dead.
inline constexpr const char *kWorkUnit = "haircut";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("haircuts"));
}

// NO expected_messages. The count depends on the interleaving: every implementation reports the
// messages its actors received, which is 7n + 3r + a + 3 (n haircuts, r rejections, a wake-ups of
// the barber: the factory's Start, n+r Enters at the room and n at the barber, r Full, r Returned,
// n Waits in all, n+a Nexts, n Starts, 2n Dones, two Exits) -- plus n-1 with `pace=1`, whose
// factory sends itself one Start per customer. What a count would assert is asserted per kind by
// the checksum above.

}  // namespace qvospec::savina::barber

#endif  // QVOSPEC_SAVINA_BARBER_H
