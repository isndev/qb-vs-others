// @benchmark     savina/logmap
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the
//                master is actor 0 on worker 0, and series i is ONE slot, 1 + i, owned by worker
//                (1 + i) % cores -- its worker role and its computer role together, the placement
//                qb's cell fixes for the pair.
// @idiom-note    There is no actor per role and no mailbox: a slot is a struct, a NextTerm is a
//                ring push, the request to the computer a push into the owner's own ring and the
//                answer a push back, the held requests a count -- the state every framework keeps
//                for the worker, kept by hand. The request still carries its sender's slot and the
//                answer goes there, as Savina's ComputeMessage carries its sender. What the floor
//                lacks, and the frameworks pay for, is a mailbox per actor and a dispatch per
//                role: that is the point of the floor.

#include <qvospec/savina/logmap.h>

#include "../baseline_support.h"

#include <bit>
#include <vector>

namespace savina_logmap_baseline {

using namespace qvospec::savina::logmap;

// kNext / kGet: no payload.   kCompute: a = the term's bits, b = the sender's slot.
// kReply: a = the next term's bits.   kResult: a = received << 32 | series, b = chain.
// kExit: none.   kReport: a = received << 32 | series, b = served.
enum Tag : std::uint32_t {
    kNext    = 1,
    kGet     = 2,
    kCompute = 3,
    kReply   = 4,
    kResult  = 5,
    kExit    = 6,
    kReport  = 7
};

// One series: its worker's state and its computer's, owned by one worker thread. A line of its
// own: neighbouring series belong to different threads at cores=2 ((1 + i) % 2), and 72 bytes
// packed in a vector would share cache lines with the neighbours' -- false sharing between the
// two threads on every term, a cost of the floor's layout and not of the workload.
struct alignas(qvobase::kCacheLine) Series {
    double        rate{0};
    double        term{0};
    std::uint64_t chain{0};
    std::uint64_t owed{0};  // NextTerm requests held while an answer is awaited
    bool          awaiting{false};
    bool          get_pending{false};
    std::uint64_t received{0};  // by the worker role
    std::uint64_t held{0};
    std::uint64_t served{0};             // by the computer role
    std::uint64_t computer_received{0};  // by the computer role
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     terms  = static_cast<std::uint64_t>(p.get("terms"));
    const auto     series = static_cast<std::uint32_t>(p.get("series"));
    const auto     cores  = static_cast<unsigned>(p.get("cores"));
    const bool     spin   = p.get("wait") != 0;
    const unsigned W      = cores < 1 ? 1u : cores;

    // A message count travels in the upper 32 bits of a payload word.
    if (2 * terms + 1 >= (std::uint64_t{1} << 32))
        qvo::not_applicable("the floor packs a per-series message count into 32 bits; terms must "
                            "stay below 2^31");

    const std::uint32_t master  = 0;
    auto                slot_of = [](std::uint32_t i) { return 1 + i; };
    auto                index_of = [](std::uint32_t slot) { return slot - 1; };

    std::vector<Series> state(series);
    for (std::uint32_t i = 0; i < series; ++i) {
        state[i].rate  = rate_of(i);
        state[i].term  = start_of(i);
        state[i].chain = chain_seed(i);
    }
    std::uint64_t master_received = 0;
    std::uint32_t results         = 0;
    std::uint32_t reports         = 0;
    std::uint64_t checksum        = 0;
    std::uint64_t delivered       = 0;

    auto answer = [&](auto &m, unsigned worker, std::uint32_t i) {
        const Series &s = state[i];
        m.send(worker, qvobase::Msg{master, kResult, (s.received << 32) | i, s.chain});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kNext: {
            const std::uint32_t i = index_of(msg.dst);
            Series             &s = state[i];
            ++s.received;
            if (s.awaiting) {
                ++s.owed;
                ++s.held;
                break;
            }
            s.awaiting = true;
            m.send(worker, qvobase::Msg{msg.dst, kCompute, std::bit_cast<std::uint64_t>(s.term),
                                        msg.dst});
            break;
        }
        case kCompute: {  // the computer role of the slot: the next term, back to the sender
            Series &s = state[index_of(msg.dst)];
            ++s.computer_received;
            ++s.served;
            const double next = next_term(std::bit_cast<double>(msg.a), s.rate);
            m.send(worker, qvobase::Msg{static_cast<std::uint32_t>(msg.b), kReply,
                                        std::bit_cast<std::uint64_t>(next), 0});
            break;
        }
        case kReply: {  // the worker role: the computer's answer
            const std::uint32_t i = index_of(msg.dst);
            Series             &s = state[i];
            ++s.received;
            s.term  = std::bit_cast<double>(msg.a);
            s.chain = chain_step(s.chain, s.term);
            if (s.owed) {
                --s.owed;
                m.send(worker, qvobase::Msg{msg.dst, kCompute, msg.a, msg.dst});
                break;
            }
            s.awaiting = false;
            if (s.get_pending) answer(m, worker, i);
            break;
        }
        case kGet: {
            const std::uint32_t i = index_of(msg.dst);
            Series             &s = state[i];
            ++s.received;
            if (s.awaiting)
                s.get_pending = true;  // answered when the last answer lands
            else
                answer(m, worker, i);
            break;
        }
        case kResult: {  // the master
            ++master_received;
            const auto i = static_cast<std::uint32_t>(msg.a & 0xffffffffu);
            checksum += series_key(i, msg.b);
            delivered += msg.a >> 32;
            if (++results == series)
                for (std::uint32_t c = 0; c < series; ++c)
                    m.send(worker, qvobase::Msg{slot_of(c), kExit, 0, 0});
            break;
        }
        case kExit: {  // the computer role
            const std::uint32_t i = index_of(msg.dst);
            Series             &s = state[i];
            ++s.computer_received;
            m.send(worker,
                   qvobase::Msg{master, kReport, (s.computer_received << 32) | i, s.served});
            break;
        }
        case kReport: {  // the master
            ++master_received;
            const auto i = static_cast<std::uint32_t>(msg.a & 0xffffffffu);
            checksum += computer_key(i, msg.b);
            delivered += msg.a >> 32;
            if (++reports == series) {
                delivered += master_received;
                watch.stop();
                m.stop();
            }
            break;
        }
        }
    });

    mesh.start();
    watch.start();
    // The reference's loop: term by term, worker by worker, then one GetTerm each.
    for (std::uint64_t k = 0; k < terms; ++k)
        for (std::uint32_t i = 0; i < series; ++i)
            mesh.send(0, qvobase::Msg{slot_of(i), kNext, 0, 0});
    for (std::uint32_t i = 0; i < series; ++i) mesh.send(0, qvobase::Msg{slot_of(i), kGet, 0, 0});
    mesh.run();

    // Read after the workers are joined: the held counts are observations, not asserted.
    std::uint64_t held = 0;
    for (const auto &s : state) held += s.held;
    qvo::Answer answer_doc{checksum, delivered};
    answer_doc.observed[kHeld] = held;
    return answer_doc;
}

}  // namespace savina_logmap_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::logmap::params();
    spec.expected          = qvospec::savina::logmap::expected;
    spec.expected_messages = qvospec::savina::logmap::expected_messages;
    spec.work_unit         = qvospec::savina::logmap::kWorkUnit;
    spec.work_units        = qvospec::savina::logmap::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the master is owned by thread 0 and series i -- its worker "
                             "and computer roles in one slot -- by thread (1 + i) % cores; a "
                             "request is a push into the owner's own ring and the answer a push "
                             "back; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A series is one slot of a vector holding its worker's and its "
        "computer's state, with no mailbox and no per-actor dispatch: the request and the answer "
        "are two pushes into the owner thread's own ring, the held requests a count",
        "the master lives on thread 0 and series i on thread (1 + i) % cores -- the placement "
        "qb's cell fixes -- so no round trip crosses a core; with cores=2 the burst to the even "
        "series (i = 0, 2, 4, ...: thread (1 + i) % 2 = 1) and their answers to the master do",
        "the terms x series NextTerm burst is pushed from the caller's thread, which IS worker 0: "
        "a full ring of its own is drained inline (Mesh::send), so at cores=1 the chains run in "
        "slices of a ring (65 536 messages) while the burst is still being sent, where a "
        "framework's master finishes its handler first; at cores=2 the ring to thread 1 holds "
        "65 536 of its 125 000 NextTerm and the caller spins until thread 1 makes room",
        "cores=1 is one thread with the master and every series in its own ring -- the floor "
        "for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_logmap_baseline::body);
}
