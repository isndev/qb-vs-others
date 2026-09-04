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
//   no-window     never marks the measured window, so a timing would be meaningless.
//
// It is deliberately NOT built by qvo_add_benchmark and its name does not start with "qvo-", so
// tools/run.py cannot discover it and no planted result can ever reach a published table.

#include <qvospec/savina/ping-pong.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto        rounds = static_cast<std::uint64_t>(p.get("messages"));
    const std::string mode   = plant();

    std::uint64_t acc       = 0;
    std::uint64_t delivered = 0;

    if (mode != "no-window") watch.start();

    for (std::uint64_t seq = rounds; seq-- > 0;) {
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

    if (mode == "wrong-answer") acc += 1;
    if (mode == "short-count") delivered -= 2;

    return qvo::Answer{acc, delivered};
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
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "plants a defect selected by QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
