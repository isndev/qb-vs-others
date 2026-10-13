// savina/sieve — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0). The rules the ping-pong spec
// states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Sieve of Eratosthenes (Imam & Sarkar, AGERE 2014), one of the "parallelism"
// benchmarks -- SieveConfig.java and SieveAkkaActorBenchmark.scala. Deviations are recorded in
// benchmarks/savina/sieve.md.

#ifndef QVOSPEC_SAVINA_SIEVE_H
#define QVOSPEC_SAVINA_SIEVE_H

#include <qvo/harness.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The trial division is the benchmark's work: kept out of line so that each binary carries it as
// ONE function, called by every adapter's filters and by nobody's inlined variant (concsll.h's
// rule for its walks).
#if defined(_MSC_VER)
#define QVOSPEC_SIEVE_NOINLINE __declspec(noinline)
#else
#define QVOSPEC_SIEVE_NOINLINE __attribute__((noinline))
#endif

namespace qvospec::savina::sieve {

inline constexpr const char *kId = "savina/sieve";

// The shape, Savina's own: a pipeline that GROWS while it runs. A PRODUCER sends every odd
// candidate 3, 5, ..., below `limit` -- one message each, from one handler -- and then EXIT, to
// the first FILTER, which holds the prime 2. A filter holds up to `local` primes; a candidate none
// of them divides is passed to the next filter, or, at the last filter, is a NEW PRIME: kept there
// while it has room, else the filter CREATES the next filter -- a new actor, inside the window --
// with that prime as its first. A composite dies at the filter holding its smallest prime factor.
// EXIT runs down the chain behind the last candidate; the last filter's EXIT ends the run.
// Messages along the chain must keep their order: a composite that overtook its smallest factor
// would reach the end of the chain and be kept as a prime.
//
// `limit` -- N, the candidates are the odd numbers from 3 below it. Savina's own default,
//            100 000; no deviation (9 592 primes, 49 999 candidates).
// `local` -- M, the primes one filter holds. Savina's own default, 1 000: ten filters, nine of
//            them created inside the window. `local=1` is the one-actor-per-prime sieve (9 591
//            filters created, every prime passing through every filter before it).
// `place` -- where the frameworks that place (qb and the floor) put the filters, a declared axis
//            (benchmarks/savina/sieve.md, "Placement"): 0 = the reference's -- a filter creates its
//            successor itself, so every filter lives on the first filter's core; 1 = filter f on
//            core (f + 1) % cores, created on request by that core. CAF and SObjectizer place by
//            their pools and declare place=1 not applicable. The result document records it.
// `cores` -- the worker budget; the producer on core 0, the first filter on core 1 % cores.
// `wait`  -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"limit", 100000}, {"local", 1000}, {"place", 0}, {"cores", 2}, {"wait", 1}};
}

inline constexpr long long kMaxLimit = 1LL << 25;
inline constexpr long long kMaxLocal = 1LL << 20;
inline void check(const qvo::Params &p) {
    const long long limit = p.get("limit");
    const long long local = p.get("local");
    const long long place = p.get("place");
    if (limit < 4 || limit > kMaxLimit || local < 1 || local > kMaxLocal || (place != 0 && place != 1))
        qvo::refuse("savina/sieve: need 4 <= limit <= 2^25, 1 <= local <= 2^20 and place 0 or 1 "
                    "(got limit=" + std::to_string(limit) + " local=" + std::to_string(local)
                    + " place=" + std::to_string(place) + ")");
}

// The odd candidates from 3 below `limit`.
inline std::uint64_t candidates(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("limit")) / 2 - 1;
}

// SieveConfig.isLocallyPrime: the candidate against every prime the filter holds, in the order it
// found them, until one divides it.
QVOSPEC_SIEVE_NOINLINE inline bool locally_prime(std::uint64_t candidate, const std::uint64_t *primes,
                                                 std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (candidate % primes[i] == 0) return false;
    return true;
}

// What a filter keeps of what it received: the candidates (how many, their wrapping sum), how many
// it passed on, and its Exits -- adds only. One 128-byte slot per filter (thread-ring.h says why
// 128); qb keeps the Counts in the actor and hands them to the slot at teardown. The primes a filter
// holds are a vector the body owns, one per filter, read after the window.
struct Counts {
    std::uint64_t received{0};
    std::uint64_t sum{0};
    std::uint64_t forwarded{0};
    std::uint64_t exits{0};
};
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally : Counts {};

// One filter's decision, Savina's PrimeFilterActor step for step, written once so the work in every
// handler is the same source. take() answers what to do with a candidate: drop it (a prime the
// filter holds divides it), pass it on (the filter has a successor), keep it (a new prime, room
// left), or grow (a new prime, no room: the adapter creates the successor with it). The Counts and
// the primes are wherever the adapter put them (above).
class Filter {
public:
    enum class Verdict { drop, pass, keep, grow };

    Filter() = default;
    Filter(std::uint64_t initial, std::uint64_t local, Counts &counts,
           std::vector<std::uint64_t> &primes)
        : _local(local), _counts(&counts), _primes(&primes) {
        _primes->reserve(static_cast<std::size_t>(local));  // Savina's localPrimes(numMaxLocalPrimes)
        _primes->push_back(initial);
    }

    Verdict take(std::uint64_t candidate) noexcept {
        Counts &c = *_counts;
        ++c.received;
        c.sum += candidate;
        if (!locally_prime(candidate, _primes->data(), _primes->size())) return Verdict::drop;
        if (_linked) {
            ++c.forwarded;
            return Verdict::pass;
        }
        if (_primes->size() < _local) {
            _primes->push_back(candidate);
            return Verdict::keep;
        }
        _linked = true;
        return Verdict::grow;
    }
    bool linked() const noexcept { return _linked; }

private:
    std::uint64_t               _local{1};
    Counts                     *_counts{nullptr};
    std::vector<std::uint64_t> *_primes{nullptr};
    bool                        _linked{false};
};

// A run that creates more filters than the primes below `limit` need breaks the protocol in a way
// no tally can carry (there is no slot for it): it stops, qvo::fail(), exit 1 and a document.
[[noreturn]] inline void too_many_filters() {
    qvo::fail("savina/sieve: protocol violated -- a filter was created past the last one the primes "
              "below `limit` need");
}

// The placement of the frameworks that place: the producer on core 0, filter f on the first
// filter's core (place=0) or on core (f + 1) % cores (place=1).
inline int core_of_filter(std::uint64_t filter, long long place, int cores) noexcept {
    const auto c = static_cast<std::uint64_t>(cores < 1 ? 1 : cores);
    return static_cast<int>(place == 0 ? 1 % c : (filter + 1) % c);
}

// THE REFERENCE: the whole run is fixed by the parameters (the candidates arrive in order, so every
// filter holds the same primes in the same order in every run), and it is computed here from a
// sieve of each odd number's smallest prime factor rather than by trial division: a prime p_i (i
// from 0, p_0 = 2) reaches filters 0 .. (i - 1) / local -- passed on by all but the last, which
// keeps it or creates filter i / local with it -- and a composite reaches filters 0 .. g, g the
// filter holding its smallest prime factor, and dies there. Filter f holds p_(f * local) onwards.
struct Reference {
    std::vector<Counts>                     tallies;  // one per filter
    std::vector<std::vector<std::uint64_t>> primes;   // what each filter holds, in order
};

inline Reference reference(const qvo::Params &p) {
    check(p);
    const auto     limit = static_cast<std::uint64_t>(p.get("limit"));
    const auto     local = static_cast<std::uint64_t>(p.get("local"));
    constexpr auto unset = std::uint32_t{0xffffffff};

    // spf[n / 2] for odd n: the index of n's smallest prime factor among the primes, or unset (n is
    // prime). Every candidate is visited in order and filed under the LAST filter it reaches.
    std::vector<std::uint32_t> spf(static_cast<std::size_t>(limit / 2 + 1), unset);
    std::vector<std::uint64_t> primes{2};
    std::vector<std::uint64_t> last_count, last_sum;  // by the last filter a candidate reaches
    const auto file = [&](std::uint64_t n, std::uint64_t last) {
        if (last_count.size() <= last) {
            last_count.resize(static_cast<std::size_t>(last + 1));
            last_sum.resize(static_cast<std::size_t>(last + 1));
        }
        ++last_count[last];
        last_sum[last] += n;
    };
    for (std::uint64_t n = 3; n < limit; n += 2) {
        const std::uint32_t f = spf[n / 2];
        if (f != unset) {  // a composite dies at the filter holding its smallest prime factor
            file(n, f / local);
            continue;
        }
        const std::uint64_t i = primes.size();  // a prime: kept by, or creating, the last filter
        primes.push_back(n);
        if (n <= (limit - 1) / n)
            for (std::uint64_t m = n * n; m < limit; m += 2 * n)
                if (spf[m / 2] == unset) spf[m / 2] = static_cast<std::uint32_t>(i);
        file(n, (i - 1) / local);
    }

    // Filter f receives every candidate whose last filter is f or beyond, and passes on those whose
    // last is beyond it: what filter f + 1 receives.
    const std::uint64_t filters = (primes.size() + local - 1) / local;
    Reference           r;
    r.tallies.resize(static_cast<std::size_t>(filters));
    r.primes.resize(static_cast<std::size_t>(filters));
    std::uint64_t received = 0, sum = 0;
    for (std::uint64_t f = filters; f-- > 0;) {
        Counts &c   = r.tallies[f];
        c.forwarded = received;  // filter f + 1's
        if (f < last_count.size()) {
            received += last_count[f];
            sum += last_sum[f];
        }
        c.received = received;
        c.sum      = sum;
        c.exits    = 1;
        for (std::uint64_t k = f * local; k < primes.size() && k < (f + 1) * local; ++k)
            r.primes[f].push_back(primes[k]);
    }
    return r;
}

// THE VERIFIED ANSWER: every filter's tally and the primes it holds, position by position,
// digested with the filter's number -- so a candidate lost, duplicated, sent past a filter or
// arriving out of order moves the count or the sum of a filter that should have seen it, or keeps
// a composite as a prime, or files a prime in another filter or at another position; and a filter
// the chain never created leaves its slot empty.
inline std::uint64_t tally_digest(std::uint64_t filter, const Counts &c) noexcept {
    return qvo::mix(qvo::mix(qvo::mix(qvo::mix(0x51e7e00000000000ULL ^ (filter << 40) ^ c.received)
                                      ^ c.sum)
                             ^ c.forwarded)
                    ^ c.exits);
}
inline std::uint64_t prime_digest(std::uint64_t filter, std::uint64_t position,
                                  std::uint64_t prime) noexcept {
    return qvo::mix(qvo::mix(0x51e7e50000000000ULL ^ (filter << 40) ^ position) ^ prime);
}

inline std::uint64_t checksum(const std::vector<Tally> &tallies,
                              const std::vector<std::vector<std::uint64_t>> &primes) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t f = 0; f < tallies.size(); ++f) acc += tally_digest(f, tallies[f]);
    for (std::size_t f = 0; f < primes.size(); ++f)
        for (std::size_t k = 0; k < primes[f].size(); ++k) acc += prime_digest(f, k, primes[f][k]);
    return acc;
}

// Every delivery the handlers ran: each filter's candidates and Exits, and the producer's Start.
inline std::uint64_t delivered(const std::vector<Tally> &tallies, std::uint64_t starts) noexcept {
    std::uint64_t n = starts;
    for (const Tally &t : tallies) n += t.received + t.exits;
    return n;
}

// The number of filters a run creates (the first one included): what the body sizes its slots by.
inline std::uint64_t filters(const qvo::Params &p) { return reference(p).tallies.size(); }

inline std::uint64_t expected(const qvo::Params &p) {
    const Reference r = reference(p);
    std::uint64_t   acc = 0;
    for (std::size_t f = 0; f < r.tallies.size(); ++f) acc += tally_digest(f, r.tallies[f]);
    for (std::size_t f = 0; f < r.primes.size(); ++f)
        for (std::size_t k = 0; k < r.primes[f].size(); ++k) acc += prime_digest(f, k, r.primes[f][k]);
    return acc;
}

// The report divides by this: one candidate tested -- sent down the chain until it dies, is kept,
// or starts a filter.
inline constexpr const char *kWorkUnit = "candidate";
inline std::uint64_t work_units(const qvo::Params &p) { return candidates(p); }

// Every candidate's visits to the filters, one Exit per filter, and the producer's one Start --
// COUNTED by the adapters (delivered() above).
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Reference r = reference(p);
    std::uint64_t   n = 1;
    for (const Counts &c : r.tallies) n += c.received + c.exits;
    return n;
}

}  // namespace qvospec::savina::sieve

#endif  // QVOSPEC_SAVINA_SIEVE_H
