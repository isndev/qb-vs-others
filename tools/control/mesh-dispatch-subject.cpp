// Deterministic worker-dispatch ordering control. Never emits timing.
// Uses the actual Mesh ring capacity; 65,537 self messages force overflow.
#include <cstdint>
#include <iostream>
#include <string>
#include "../../frameworks/baseline/baseline_support.h"
int main(int argc, char **argv) {
    if (argc != 4 || (std::string(argv[1]) != "1" && std::string(argv[1]) != "2")
        || (std::string(argv[2]) != "spin" && std::string(argv[2]) != "park")
        || (std::string(argv[3]) != "0" && std::string(argv[3]) != "1" && std::string(argv[3]) != "cross")
        || (std::string(argv[1]) == "1" && std::string(argv[3]) != "0")) {
        std::cerr << "usage: mesh-dispatch-subject 1|2 spin|park owner:0|1|cross\n";
        return 2;
    }
    const unsigned workers = static_cast<unsigned>(std::stoul(argv[1]));
    const bool spin = std::string(argv[2]) == "spin";
    const bool cross = std::string(argv[3]) == "cross";
    const unsigned owner = cross ? 0u : static_cast<unsigned>(std::stoul(argv[3]));
    bool submitted = false;
    bool no_reentry = true;
    std::uint64_t received = 0;
    const std::uint64_t count = (std::uint64_t{1} << 16) + 1;
    qvobase::Mesh mesh(workers, spin, [&](auto &m, unsigned worker, qvobase::Msg const &msg) {
        if (msg.tag == 0) {
            for (std::uint64_t i = 0; i != count; ++i)
                m.send(worker, qvobase::Msg{owner, 1, i, 0});
            submitted = true;
        } else {
            no_reentry &= (cross || submitted) && msg.a == received;
            if (++received == count) m.stop();
        }
    });
    mesh.start(); mesh.send(0, qvobase::Msg{cross ? 1u : owner,0,0,0}); mesh.run();
    const bool valid = submitted && no_reentry && received == count;
    std::cout << "{\"valid\":" << (valid ? "true" : "false")
              << ",\"received\":" << received << ",\"submitted\":" << (submitted ? "true" : "false")
              << ",\"no_reentry_and_fifo\":" << (no_reentry ? "true" : "false") << "}\n";
    return valid ? 0 : 1;
}
