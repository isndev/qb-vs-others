// The negative control's piprecision subject: savina/piprecision's verification, watched rejecting
// each fault class the shape can have and accepting the arrival orders no framework controls
// (Huly QB-246).
//
// Savina's master and `workers` workers simulated on one thread -- one FIFO holds the work
// messages and the answers -- computing every term through the spec's own bbp_term(), folding
// through its Fold, and reducing through checksum() and works(), like every adapter.
// `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none              correct -- the POSITIVE control: must verify.
//   shuffled          correct, every next delivery drawn at random from the FIFO (a fixed seed):
//                     must verify -- the sum is an integer sum, and every term past the first one
//                     at the tolerance is 0, so the number of terms the race hands out (reported as
//                     `terms`) never reaches the answer.
//   duplicate-answer  the answer of term 3 delivered twice.
//   duplicate-zero    the first answer of value 0 -- a term past the first one at the tolerance --
//                     delivered twice: the SUM is unchanged, and only the per-index count sees it.
//                     The subject says so on stderr ("the duplicated answer is 0: yes").
//   untouched         term 2 answered without being computed: no limbs.
//   wrong-term        term 4 answered with term 5's value.
//   skip-zero         the first term past the first one at the tolerance answered with 0 WITHOUT
//                     being computed: its value is right, only its proof of work is not. The
//                     subject says so on stderr ("the skipped term's value is 0: yes").
//   phantom           an answer for a term never handed out: a protocol violation, qvo::fail().
//
// Each plant must be REJECTED: the first six by the checksum, the last by the declared failure.
// Run with --param precision=0, the spec must refuse the run before anything is built (exit 2).
//
// Like the subjects beside it, it is NOT built by qvo_add_benchmark and its name does not start
// with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/piprecision.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace qvospec::savina::piprecision;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

struct Message {
    bool              answer{false};
    std::uint32_t     worker{0};
    std::uint32_t     term{0};
    Limb              proof{0};
    std::vector<Limb> limbs;
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config      c    = config(p);
    const std::string mode = plant();
    const Context     ctx  = make_context(c.precision);

    Fold                 fold(c, ctx);
    std::vector<Tally>   tallies(c.workers);
    Scratch              scratch;
    std::deque<Message>  fifo;
    std::mt19937_64      rng(0x51a7c0de);
    bool                 stop = false, planted = false, zero_duplicated = false;
    bool                 skipped_zero = false;
    std::uint32_t        k1 = 0;  // the first term at the tolerance, as this run's answers show it
    bool                 seen_k1 = false;

    watch.start();
    for (std::uint32_t t = 0; t < c.batch; ++t)  // Savina's StartMessage
        fifo.push_back({false, t % c.workers, fold.next(), 0, {}});
    while (!fifo.empty()) {
        std::size_t at = 0;
        if (mode == "shuffled") at = static_cast<std::size_t>(rng() % fifo.size());
        Message m = std::move(fifo[at]);
        fifo.erase(fifo.begin() + static_cast<std::ptrdiff_t>(at));
        if (!m.answer) {  // a worker
            ++tallies[m.worker].works;
            std::uint32_t k = m.term;
            if (mode == "wrong-term" && k == 4) k = 5;
            if (mode == "skip-zero" && !planted && seen_k1 && m.term > k1) {
                planted = true;  // answered without the four divisions: the value of every term
                m.limbs.clear(); // past k1, and no proof
                m.proof      = 0;
                skipped_zero = true;
                m.answer     = true;
                fifo.push_back(std::move(m));
                continue;
            }
            bbp_term(ctx, k, scratch, m.limbs);
            m.proof = bbp_proof(scratch);
            if (mode == "untouched" && m.term == 2) m.limbs.clear();
            m.answer = true;
            if (mode == "phantom" && !planted) {
                planted = true;
                m.term  = fold.requested;  // the next index, never handed out
            }
            const bool twice = (mode == "duplicate-answer" && m.term == 3)
                               || (mode == "duplicate-zero" && m.limbs.empty() && !planted);
            if (twice) {
                planted         = true;
                zero_duplicated = m.limbs.empty();
                fifo.push_back(m);
            }
            fifo.push_back(std::move(m));
            continue;
        }
        // The master: Savina's Master.process(ResultMessage).
        if (fold.add(m.term, m.limbs.data(), m.limbs.size(), m.proof)) {
            stop = true;
            if (!seen_k1) {
                seen_k1 = true;
                k1      = m.term;
            }
        }
        if (!stop) fifo.push_back({false, m.worker, fold.next(), 0, {}});
        if (fold.complete()) {
            watch.stop();
            break;
        }
    }
    if (mode == "duplicate-zero")
        std::fprintf(stderr, "piprecision subject: the duplicated answer is 0: %s\n",
                     zero_duplicated ? "yes" : "no");
    if (mode == "skip-zero")
        std::fprintf(stderr, "piprecision subject: the skipped term's value is 0: %s\n",
                     skipped_zero ? "yes" : "no");
    std::fprintf(stderr, "piprecision subject: terms handed out %u\n", fold.requested);

    qvo::Answer answer{checksum(fold, works(tallies), reference(p).proofs), 0};
    answer.observed[kObservedTerms] = fold.requested;
    return answer;
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/piprecision";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::piprecision::params();
    spec.expected          = qvospec::savina::piprecision::expected;
    spec.work_unit         = qvospec::savina::piprecision::kWorkUnit;
    spec.work_units        = qvospec::savina::piprecision::work_units;
    spec.observed_at_least = {{qvospec::savina::piprecision::kObservedTerms,
                               qvospec::savina::piprecision::min_terms}};
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "Savina's master and its workers simulated on one thread; plants a "
                             "defect selected by QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
