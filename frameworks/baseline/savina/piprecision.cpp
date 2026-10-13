// @benchmark     savina/piprecision
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the master
//                is actor 0 and worker w is actor cores + w, each owned by thread id % cores.
// @idiom-note    The floor for this shape is the shared term kernel plus the cheapest way for a
//                payload to move from one thread to another: a worker computes the term into ONE
//                heap block (its length, then its limbs), sends the block's pointer in a ring
//                message, and the master frees it once the term is in its sum -- the form=0 data
//                movement every framework has, with no message object around it. form=1 is not
//                applicable: the floor's ring carries a fixed three-word message, and a payload
//                inline in it would need a second ring the floor does not have. Placement is
//                static and is qb's exactly: the master on thread 0, worker w on thread w % cores.

#include <qvospec/savina/piprecision.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_piprecision_baseline {

using namespace qvospec::savina::piprecision;

// kWork: a = the term.  kResult: a = term | worker << 32, b = the block: [0] its limb count, [1]
// the term's proof, then the limbs.
enum Tag : std::uint32_t { kWork = 1, kResult = 2 };

constexpr std::uint32_t kMaster = 0;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config   c     = config(p);
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;
    if (inline_payload(p))
        qvo::not_applicable("form=1 carries the payload in the message's own storage; the floor's "
                            "ring carries a fixed three-word message, so its form=0 cell -- one "
                            "heap block per term, its pointer in the message -- is the floor for "
                            "both forms");
    const Context context = make_context(c.precision);

    // The master's state. Actor 0 is owned by thread 0, so only thread 0 ever touches it (the
    // inline drain of a full ring runs a handler re-entrantly, but on the same thread).
    Fold fold(c, context);
    bool stop = false;
    // Each worker's count, one 128-byte slot of its own, written only by the thread that owns it;
    // each thread's scratch, used only by that thread.
    std::vector<Tally>   tallies(c.workers);
    std::vector<Scratch> scratch(W);

    auto hand_out = [&](auto &m, unsigned from, std::uint32_t worker) {
        m.send(from, qvobase::Msg{W + worker, kWork, fold.next(), 0});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned thread, const qvobase::Msg &msg) {
        if (msg.dst != kMaster) {  // a worker actor: only ever sent work
            const std::uint32_t index = msg.dst - W;
            const auto          term  = static_cast<std::uint32_t>(msg.a);
            ++tallies[index].works;
            Scratch &s = scratch[thread];
            bbp_numerator(context, term, s);
            auto *block = new Limb[bbp_room(term, s) + 2];  // the term computed straight into it
            block[1]    = bbp_proof(s);
            block[0]    = bbp_shift(term, s, block + 2);
            m.send(thread, qvobase::Msg{kMaster, kResult,
                                        term | (static_cast<std::uint64_t>(index) << 32),
                                        reinterpret_cast<std::uint64_t>(block)});
            return;
        }
        const auto  term   = static_cast<std::uint32_t>(msg.a & 0xffffffffULL);
        const auto  worker = static_cast<std::uint32_t>(msg.a >> 32);
        const Limb *block  = reinterpret_cast<const Limb *>(msg.b);
        if (worker >= c.workers)
            qvo::fail("savina/piprecision: an answer names worker " + std::to_string(worker)
                      + ", which does not exist");
        if (fold.add(term, block + 2, static_cast<std::size_t>(block[0]), block[1])) stop = true;
        delete[] block;
        if (!stop) hand_out(m, thread, worker);
        if (fold.complete()) {
            watch.stop();
            m.stop();
        }
    });
    // A full ring the sending thread owns is drained inline, a handler running inside the send.
    // At most `batch` work messages and `batch` answers are in flight, so below the bound no ring
    // ever fills -- 400 against 65 536 at the defaults; above it the floor declares the cell not
    // applicable rather than run a configuration that can nest those drains.
    if (2 * static_cast<std::size_t>(c.batch) > mesh.kRingCapacity)
        qvo::not_applicable("the run keeps up to " + std::to_string(2 * c.batch)
                            + " messages in flight, more than one ring of the floor holds ("
                            + std::to_string(mesh.kRingCapacity) + ")");
    mesh.start();

    watch.start();
    // Savina's StartMessage: the first `batch` terms, round-robin. The calling thread is worker 0,
    // the master's owner.
    for (std::uint32_t t = 0; t < c.batch; ++t) hand_out(mesh, 0, t % c.workers);
    mesh.run();

    // run() joined every worker: the fold and every tally are final, and reduced here, outside the
    // window.
    qvo::Answer answer{checksum(fold, works(tallies), reference(p).proofs), 0};
    answer.observed[kObservedTerms] = fold.requested;
    return answer;
}

}  // namespace savina_piprecision_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::piprecision::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::piprecision::expected;
    spec.work_unit         = qvospec::savina::piprecision::kWorkUnit;
    spec.work_units        = qvospec::savina::piprecision::work_units;
    spec.observed_at_least = {{qvospec::savina::piprecision::kObservedTerms,
                               qvospec::savina::piprecision::min_terms}};
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; an answer is one heap block whose pointer rides a ring message, "
                             "freed by the master; master on thread 0, worker w (id cores + w) on "
                             "thread w % cores -- qb's placement; not an actor framework and not "
                             "ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A work message is one ring message carrying the term's index, an "
        "answer one ring message carrying the pointer of a heap block (length + limbs) the worker "
        "filled and the master frees, and the term is the same shared kernel every framework "
        "runs: the floor for moving a heavy payload from a worker to a master",
        "worker w is owned by thread w % cores and the master by thread 0, fixed before the "
        "window -- qb's placement exactly: with cores=2 half the answers cross a thread, and the "
        "block they point at is read by the master's thread through the cache",
        "cores=1 is one thread with every actor's messages in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_piprecision_baseline::body);
}
