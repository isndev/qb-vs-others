// savina/filterbank — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0). The rules the ping-pong spec
// states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Filter Bank (Imam & Sarkar, AGERE 2014), one of the "parallelism" benchmarks
// -- FilterBankConfig.java and FilterBankAkkaActorBenchmark.scala (StreamIt's FilterBank, ported
// to actors). Deviations are recorded in benchmarks/savina/filterbank.md.

#ifndef QVOSPEC_SAVINA_FILTERBANK_H
#define QVOSPEC_SAVINA_FILTERBANK_H

#include <qvo/harness.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The FIR's dot product is the benchmark's work: kept out of line so that each binary carries it
// as ONE function -- expected() and every adapter's filters call the same instructions, and no
// framework's handler can inline it into a different schedule (concsll.h's rule for its walks).
#if defined(_MSC_VER)
#define QVOSPEC_FILTERBANK_NOINLINE __declspec(noinline)
#else
#define QVOSPEC_FILTERBANK_NOINLINE __attribute__((noinline))
#endif

namespace qvospec::savina::filterbank {

inline constexpr const char *kId = "savina/filterbank";

// The shape, Savina's own: a BRANCHING pipeline. A PRODUCER and a SOURCE trade one message each
// way per sample (the producer's Boot, the source's Next: the producer paces the source), and the
// source sends each sample -- 0, 1, ..., 999, 0, ... -- to BRANCHES, which FANS IT OUT to
// `channels` banks. Each bank is a chain of seven actors: the bank's own (it forwards), a delay of
// columns - 1, an FIR filter over the last `columns` samples with the bank's H coefficients, a
// sampler that keeps one sample in `columns` and zeroes the rest, a second delay, a second FIR
// with the bank's F coefficients, and a tagger that names the channel. An INTEGRATOR FANS IN the
// `channels` streams -- the k-th value of every channel makes round k -- a COMBINE sums each round
// and a SINK receives the sums. When the producer has paced `simulations` samples it sends Exit,
// which every actor passes on behind its last value; the sink's Exit ends the run.
//
// An FIR outputs nothing until it has seen `columns` values, then one value per input, each a
// `columns`-term dot product: at Savina's defaults the 16 FIRs evaluate 2.68 G multiply-adds per
// run, which is most of the cell -- the frameworks are compared on how they spread eight banks of
// work over the cores while ~1.4 M messages move.
//
// `simulations` -- the samples the producer paces. Savina's own default: NUM_SIMULATIONS =
//                  2048 + max(2048, NUM_COLUMNS) * 2 = 34 816, computed before its arguments are
//                  read (so its -col does not move it); no deviation.
// `columns`     -- the FIR length, the delays' length + 1 and the sampling rate. Savina's own
//                  default, 8192 * 2 = 16 384; no deviation.
// `channels`    -- the banks. Savina's own default, 8; Savina clamps its argument into [2, 33]
//                  (its MessageChannel enum), here a value outside is refused.
// `cores`       -- the worker budget. For the frameworks that place (qb and the floor): the
//                  producer, the source and the branches on core 0, bank j's seven actors on core
//                  j % cores, the integrator, the combine and the sink on the last core (core_of).
// `wait`        -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"simulations", 34816}, {"columns", 16384}, {"channels", 8}, {"cores", 2}, {"wait", 1}};
}

inline constexpr long long kMaxSimulations = 1LL << 26;
inline constexpr long long kMaxColumns     = 1LL << 20;
inline constexpr long long kMinChannels    = 2;
inline constexpr long long kMaxChannels    = 33;
inline void check(const qvo::Params &p) {
    const long long s  = p.get("simulations");
    const long long c  = p.get("columns");
    const long long ch = p.get("channels");
    if (s < 1 || s > kMaxSimulations || c < 2 || c > kMaxColumns || ch < kMinChannels
        || ch > kMaxChannels)
        qvo::refuse("savina/filterbank: need 1 <= simulations <= 2^26, 2 <= columns <= 2^20 and "
                    "2 <= channels <= 33 (got simulations=" + std::to_string(s) + " columns="
                    + std::to_string(c) + " channels=" + std::to_string(ch) + ")");
}

// The source's k-th sample: Savina's SourceActor counts `current` from 0 modulo maxValue = 1000.
inline constexpr std::uint64_t kMaxValue = 1000;
inline double sample(std::uint64_t k) noexcept { return static_cast<double>(k % kMaxValue); }

// FilterBankConfig.parseArgs's coefficient tables, built once before the window and read by every
// bank: H[j][i] = i*columns + j*channels + j + i + j + 1 and F[j][i] = i*j + j*j + j + i, in
// doubles, evaluated left to right as the Java does (every term an integer below 2^53: exact).
class Coefficients {
public:
    Coefficients(std::uint32_t channels, std::uint32_t columns)
        : _columns(columns), _h(std::size_t{channels} * columns), _f(std::size_t{channels} * columns) {
        for (std::uint32_t j = 0; j < channels; ++j)
            for (std::uint32_t i = 0; i < columns; ++i) {
                const double di = i, dj = j;
                _h[std::size_t{j} * columns + i] =
                    1.0 * di * columns + 1.0 * dj * channels + dj + di + dj + 1;
                _f[std::size_t{j} * columns + i] = 1.0 * di * dj + 1.0 * dj * dj + dj + di;
            }
    }
    const double *h(std::uint32_t channel) const noexcept { return &_h[std::size_t{channel} * _columns]; }
    const double *f(std::uint32_t channel) const noexcept { return &_f[std::size_t{channel} * _columns]; }

private:
    std::uint32_t       _columns;
    std::vector<double> _h;
    std::vector<double> _f;
};

// Savina's FirFilterActor sum, term for term: data[i] * coefficients[n - i - 1], accumulated from
// i = 0 in one double. The data array is the filter's circular buffer read from index 0, not from
// its write position -- the reference's arithmetic, kept.
QVOSPEC_FILTERBANK_NOINLINE inline double fir_dot(const double *data, const double *coefficients,
                                                  std::size_t n) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += data[i] * coefficients[n - i - 1];
    return sum;
}

// The four filters of a bank, Savina's actors' state step for step. Each step() takes the value a
// filter received and gives what it sends on; the FIR sends nothing until it is full.
class Delay {
public:
    Delay() = default;
    explicit Delay(std::uint32_t length) : _state(length, 0.0) {}
    double step(double value) noexcept {
        const double out  = _state[_at];
        _state[_at]       = value;
        if (++_at == _state.size()) _at = 0;
        return out;
    }

private:
    std::vector<double> _state;
    std::size_t         _at{0};
};

class Fir {
public:
    Fir() = default;
    Fir(std::uint32_t peek, const double *coefficients)
        : _data(peek, 0.0), _coefficients(coefficients) {}
    bool step(double value, double &out) noexcept {
        _data[_at] = value;
        if (++_at == _data.size()) {
            _full = true;
            _at   = 0;
        }
        if (!_full) return false;
        out = fir_dot(_data.data(), _coefficients, _data.size());
        return true;
    }

private:
    std::vector<double> _data;
    const double       *_coefficients{nullptr};
    std::size_t         _at{0};
    bool                _full{false};
};

class Sampler {
public:
    Sampler() = default;
    explicit Sampler(std::uint32_t rate) noexcept : _rate(rate) {}
    // Whether the value received now is the one in `rate` that is kept (the rest are zeroed).
    bool keeps() noexcept {
        const bool keep = _received == 0;
        if (++_received == _rate) _received = 0;
        return keep;
    }
    double step(double value) noexcept { return keeps() ? value : 0.0; }

private:
    std::uint32_t _rate{1};
    std::uint32_t _received{0};
};

// The integrator's join. Savina's IntegratorActor files channel j's value into the oldest round
// that has none from j and sends the oldest round once it holds every channel; channels deliver in
// order, so round k is the k-th value of each channel, whatever the interleaving. put() files a
// value and answers whether the oldest round is now complete; take() copies it out IN CHANNEL ORDER
// (the order Savina's HashMap<Integer, Double>.values() yields for keys 0..32) and answers whether
// the next one is complete too. Each channel's queue is reserved for the run before the window.
class Join {
public:
    Join() = default;
    Join(std::uint32_t channels, std::uint64_t per_channel) : _queues(channels), _heads(channels, 0) {
        for (auto &q : _queues) q.reserve(static_cast<std::size_t>(per_channel));
    }
    bool put(std::uint32_t channel, double value) {
        auto &q = _queues[channel];
        if (q.size() == _heads[channel]) ++_pending;
        q.push_back(value);
        return _pending == _queues.size();
    }
    bool take(double *round) noexcept {
        for (std::size_t j = 0; j < _queues.size(); ++j) {
            round[j] = _queues[j][_heads[j]++];
            if (_heads[j] == _queues[j].size()) --_pending;
        }
        return _pending == _queues.size();
    }

private:
    std::vector<std::vector<double>> _queues;
    std::vector<std::size_t>         _heads;
    std::size_t                      _pending{0};  // channels with a value not yet in a round
};

// Savina's CombineActor: the round's values summed in channel order from 0.
inline double combine(const double *round, std::uint32_t channels) noexcept {
    double sum = 0.0;
    for (std::uint32_t j = 0; j < channels; ++j) sum += round[j];
    return sum;
}

// The actors, numbered: the producer, the source, the branches, bank j's seven, then the
// integrator, the combine and the sink. Every actor has a tally slot at its number.
enum Role : std::uint32_t { kBank = 0, kDelay1, kFir1, kSampler, kDelay2, kFir2, kTagger, kRoles };
inline constexpr std::uint32_t kProducer = 0;
inline constexpr std::uint32_t kSource   = 1;
inline constexpr std::uint32_t kBranches = 2;
inline std::uint32_t bank_actor(std::uint32_t channel, Role role) noexcept {
    return 3 + channel * kRoles + role;
}
inline std::uint32_t integrator(std::uint32_t channels) noexcept { return 3 + channels * kRoles; }
inline std::uint32_t combiner(std::uint32_t channels) noexcept { return integrator(channels) + 1; }
inline std::uint32_t sink(std::uint32_t channels) noexcept { return integrator(channels) + 2; }
inline std::uint32_t actors(std::uint32_t channels) noexcept { return integrator(channels) + 3; }

// The placement of the frameworks that place (qb and the floor).
inline int core_of(std::uint32_t actor, std::uint32_t channels, int cores) noexcept {
    const int c = cores < 1 ? 1 : cores;
    if (actor < 3) return 0;
    if (actor >= integrator(channels)) return c - 1;
    return static_cast<int>(((actor - 3) / kRoles) % static_cast<std::uint32_t>(c));
}

// What an actor keeps of what it received: the values (or the producer's Nexts, the source's
// Boots, the combine's rounds), the wrapping sum of their IEEE-754 bit patterns, and the Exits.
// Adds only, nothing riding on a message. One 128-byte slot per actor (thread-ring.h says why
// 128); qb keeps the Counts in the actor and hands them to the slot at teardown.
struct Counts {
    std::uint64_t count{0};
    std::uint64_t sum{0};
    std::uint64_t exits{0};
    void add(double value) noexcept {
        ++count;
        sum += std::bit_cast<std::uint64_t>(value);
    }
    void add_round(const double *round, std::uint32_t channels) noexcept {
        ++count;
        for (std::uint32_t j = 0; j < channels; ++j) sum += std::bit_cast<std::uint64_t>(round[j]);
    }
};
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally : Counts {};

// What every FIR sends: nothing for the first columns - 1 values, then one per value.
inline std::uint64_t fir_outputs(std::uint64_t inputs, std::uint64_t columns) noexcept {
    return inputs >= columns ? inputs - (columns - 1) : 0;
}
inline std::uint64_t first_outputs(const qvo::Params &p) {  // FIR 1 -> sampler, delay 2, FIR 2
    return fir_outputs(static_cast<std::uint64_t>(p.get("simulations")),
                       static_cast<std::uint64_t>(p.get("columns")));
}
inline std::uint64_t rounds(const qvo::Params &p) {  // FIR 2 -> tagger, and the sink's values
    return fir_outputs(first_outputs(p), static_cast<std::uint64_t>(p.get("columns")));
}

// The sink's whole work inside the window: the sum goes to the next position of a sequence the
// body owns, as IEEE-754 bits; checked against the reference AFTER the window. A value past the
// last round breaks the protocol and fails the run (qvo::fail, exit 1 and a document).
class Sequence {
public:
    Sequence() noexcept = default;
    Sequence(std::uint64_t *data, std::uint64_t rounds) noexcept : _data(data), _rounds(rounds) {}
    void take(double value) noexcept {
        if (_count >= _rounds)
            qvo::fail("savina/filterbank: protocol violated -- the sink received more values than "
                      "rounds");
        _data[_count++] = std::bit_cast<std::uint64_t>(value);
    }
    std::uint64_t count() const noexcept { return _count; }

private:
    std::uint64_t *_data{nullptr};
    std::uint64_t  _rounds{0};
    std::uint64_t  _count{0};
};
struct alignas(kTallyLine) SequenceSlot {
    Sequence sequence;
};

// THE VERIFIED ANSWER. The whole run is fixed by the parameters -- every filter is deterministic,
// every channel delivers in order and the join rounds by position -- so the reference below
// replays it on one thread, and the checksum is:
//
//   * every actor's tally, digested with its number: how many values it received, the sum of their
//     bits and its Exits -- a value lost, duplicated, zeroed, sent to the wrong filter or the wrong
//     channel moves the count or the sum of the actor that should have had it, and of every actor
//     downstream;
//   * the sink's sequence, position by position: the sums the run produced, bit for bit.
//
// Nothing blocks on a lost value: Exit follows every actor's last value through the same FIFO, so
// a lost or extra value ends the run on a wrong sum. What blocks is an Exit lost (the integrator
// waits for one per channel): the harness's deadline fails it (benchmarks/savina/filterbank.md).
inline std::uint64_t tally_digest(std::uint64_t actor, const Counts &c) noexcept {
    return qvo::mix(qvo::mix(qvo::mix(0xf17e4ba000000000ULL ^ (actor << 40) ^ c.count) ^ c.sum)
                    ^ c.exits);
}
inline std::uint64_t position_digest(std::uint64_t position, std::uint64_t bits) noexcept {
    return qvo::mix(qvo::mix(0xf17e4ba500000000ULL + position) ^ bits);
}

inline std::uint64_t checksum(const std::vector<Tally> &tallies,
                              const std::vector<std::uint64_t> &sequence) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t a = 0; a < tallies.size(); ++a) acc += tally_digest(a, tallies[a]);
    for (std::size_t i = 0; i < sequence.size(); ++i) acc += position_digest(i, sequence[i]);
    return acc;
}

// Every delivery the handlers ran: each actor's count and its Exits.
inline std::uint64_t delivered(const std::vector<Tally> &tallies) noexcept {
    std::uint64_t n = 0;
    for (const Tally &t : tallies) n += t.count + t.exits;
    return n;
}

// The reference: the run replayed on one thread with the same filters, in the order one sample at
// a time, bank after bank -- the join makes the rounds the same for every interleaving.
inline std::uint64_t expected(const qvo::Params &p) {
    check(p);
    const auto   s   = static_cast<std::uint64_t>(p.get("simulations"));
    const auto   c   = static_cast<std::uint32_t>(p.get("columns"));
    const auto   ch  = static_cast<std::uint32_t>(p.get("channels"));
    const auto   r   = rounds(p);
    Coefficients k(ch, c);
    std::vector<Tally>   t(actors(ch));
    std::vector<Delay>   d1, d2;
    std::vector<Fir>     f1, f2;
    std::vector<Sampler> sm;
    for (std::uint32_t j = 0; j < ch; ++j) {
        d1.emplace_back(c - 1);
        f1.emplace_back(c, k.h(j));
        sm.emplace_back(c);
        d2.emplace_back(c - 1);
        f2.emplace_back(c, k.f(j));
    }
    Join                       join(ch, r);
    std::vector<double>        round(ch);
    std::vector<std::uint64_t> sequence;
    sequence.reserve(static_cast<std::size_t>(r));

    t[kProducer].count = s + 1;  // the opening Next and one per sample
    t[kSource].count   = s;      // one Boot per sample
    for (std::uint64_t n = 0; n < s; ++n) {
        const double v = sample(n);
        t[kBranches].add(v);
        for (std::uint32_t j = 0; j < ch; ++j) {
            t[bank_actor(j, kBank)].add(v);
            t[bank_actor(j, kDelay1)].add(v);
            const double x = d1[j].step(v);
            t[bank_actor(j, kFir1)].add(x);
            double y = 0;
            if (!f1[j].step(x, y)) continue;
            t[bank_actor(j, kSampler)].add(y);
            const double z = sm[j].step(y);
            t[bank_actor(j, kDelay2)].add(z);
            const double w = d2[j].step(z);
            t[bank_actor(j, kFir2)].add(w);
            double u = 0;
            if (!f2[j].step(w, u)) continue;
            t[bank_actor(j, kTagger)].add(u);
            t[integrator(ch)].add(u);
            if (!join.put(j, u)) continue;
            bool more = true;
            while (more) {
                more = join.take(round.data());
                t[combiner(ch)].add_round(round.data(), ch);
                const double total = combine(round.data(), ch);
                t[sink(ch)].add(total);
                sequence.push_back(std::bit_cast<std::uint64_t>(total));
            }
        }
    }
    for (std::uint32_t a = kSource; a < actors(ch); ++a) t[a].exits = 1;
    t[integrator(ch)].exits = ch;
    return checksum(t, sequence);
}

// The report divides by this: one sample through the bank -- paced, fanned out to every channel,
// filtered, joined.
inline constexpr const char *kWorkUnit = "sample";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("simulations"));
}

// Every delivery, closed form, COUNTED by the adapters (delivered() above): the producer's s + 1
// Nexts; s values and an Exit at the source (its Boots), the branches, and each bank's own, first
// delay and first FIR; o1 + 1 at each sampler, second delay and second FIR; o2 + 1 at each tagger;
// channels * (o2 + 1) at the integrator; o2 + 1 at the combine and at the sink.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const auto s  = static_cast<std::uint64_t>(p.get("simulations"));
    const auto ch = static_cast<std::uint64_t>(p.get("channels"));
    const auto o1 = first_outputs(p);
    const auto o2 = rounds(p);
    return (s + 1) + 2 * (s + 1) + ch * (3 * (s + 1) + 3 * (o1 + 1) + (o2 + 1)) + ch * (o2 + 1)
           + 2 * (o2 + 1);
}

}  // namespace qvospec::savina::filterbank

#endif  // QVOSPEC_SAVINA_FILTERBANK_H
