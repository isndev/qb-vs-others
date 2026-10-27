// Ask-any scatter probe: what does one `co_await qb::ask_any()` over N responders cost the asker, on a
// long-lived engine -- the instrument behind Huly QB-1153.
//
// A qb-only probe like ask-cost.cpp (docs/TUNING.md section 24): `qb::ask_any` scatters one request to
// N responders, resumes the asker on the first reply and tears the N-1 losing exchanges down; nothing in
// CAF or SObjectizer is that operation. NOT declared through qvo_add_benchmark and not `qvo-` prefixed,
// so tools/run.py cannot discover it.
//
// The dev/bench cells `Ask_Scatter<Any>/N` time an engine's whole life (start, scatter, stop), so a
// per-responder cost of a few nanoseconds is buried under the lifecycle there. This probe starts ONE
// engine, runs `asks` scatters back to back from one coroutine, and divides: the per-scatter cost of a
// warm engine, which is what QB-1153 measured on the M4 (+8-12 ns per cross-core responder from N = 6,
// +4.7 % at N = 8 against 3.2.1). Two placements:
//
//   cross  the asker on core 0, responder i on core 1 + i: every leg crosses a core (the QB-1153 shape).
//   same   the asker and the N responders on core 0: the same machinery with no mailbox crossing.
//
// The responders are qb's own `qb::test::MarketResponder` (qb/tests/core/shared/AskResponders.h, the
// header the ask and resilience suites share; it exists unchanged since 3.2), so the probe builds
// against any qb tree that carries it -- shipped 3.2.1 and the branch tip alike. One line on stdout:
//
//   responders=N asks=M  X ns/ask_any  (checksum C)
//
// The checksum is the sum of the winning replies (2 * seq): a scatter that lost its answer would show.
//
//   qvoprobe-ask-any-scatter <cross|same> [responders=8] [asks=200000]
//
// The cores are VirtualCores, not pinned CPUs (qb places core i on the i-th CPU it may run on, and pins
// it where the platform can); a cross run needs responders + 1 CPUs that nothing else uses.
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

#include "AskResponders.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace std::chrono_literals;

// A NAMED namespace: the spawned frame is a class of a qb header, so an anonymous one would make
// g++ 14 report -Wsubobject-linkage (the rule qb's own coroutine fixtures follow).
namespace ask_any_scatter_probe {

struct Done : qb::Event {};

struct Out {
    double        ns_per_ask{0};
    std::uint64_t checksum{0};
};

class Asker final : public qb::Actor {
    std::vector<qb::ActorId> _targets;
    std::uint64_t            _asks;
    Out                     &_out;

public:
    Asker(std::vector<qb::ActorId> t, std::uint64_t asks, Out &out)
        : _targets(std::move(t))
        , _asks(asks)
        , _out(out) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::test::Ping>(*this);
        registerEvent<Done>(*this);
        auto targets = _targets;
        auto asks    = _asks;
        Out *out     = &_out;
        spawn([targets, asks, out](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            std::uint64_t sum = 0;
            const auto    t0  = std::chrono::steady_clock::now();
            for (std::uint64_t i = 0; i < asks; ++i) {
                auto r = co_await qb::ask_any(ctx, targets, qb::test::Ping{static_cast<int>(i)}, 2s);
                sum += static_cast<std::uint64_t>(r.response);
            }
            const auto dt   = std::chrono::steady_clock::now() - t0;
            out->ns_per_ask = double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()) / double(asks);
            out->checksum   = sum;
            ctx.push<Done>();
        });
        co_return true;
    }

    void
    on(qb::test::Ping &e) {
        resolve_ask(e);
    }

    void
    on(Done const &) {
        for (auto t : _targets)
            push<qb::KillEvent>(t);
        kill();
    }
};

} // namespace ask_any_scatter_probe

using ask_any_scatter_probe::Asker;
using ask_any_scatter_probe::Out;

int
main(int argc, char **argv) {
    const bool cross = argc > 1 && std::strcmp(argv[1], "cross") == 0;
    const bool same  = argc > 1 && std::strcmp(argv[1], "same") == 0;
    if (!cross && !same) {
        std::fprintf(stderr, "usage: %s <cross|same> [responders=8] [asks=200000]\n", argv[0]);
        return 2;
    }
    const unsigned      n    = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 8u;
    const std::uint64_t asks = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 200000u;
    if (n == 0 || asks == 0 || (cross && n + 1 > qb::MaxCores)) {
        std::fprintf(stderr, "responders and asks must be positive (and responders + 1 <= %u cores)\n",
                     static_cast<unsigned>(qb::MaxCores));
        return 2;
    }
    Out out;
    {
        qb::Main                 engine;
        std::vector<qb::ActorId> targets;
        for (unsigned i = 0; i < n; ++i)
            targets.push_back(engine.addActor<qb::test::MarketResponder<qb::Actor>>(
                static_cast<qb::CoreId>(cross ? 1 + i : 0)));
        engine.addActor<Asker>(0, targets, asks, std::ref(out));
        engine.start();
        engine.join();
        if (engine.hasError()) {
            std::fprintf(stderr, "the engine reported an error\n");
            return 1;
        }
    }
    // Every reply is 2 * seq, so the winners sum to asks * (asks - 1): anything else lost an answer.
    const std::uint64_t expected = asks * (asks - 1);
    std::printf("responders=%u asks=%llu  %.1f ns/ask_any  (checksum %llu)\n", n, static_cast<unsigned long long>(asks),
                out.ns_per_ask, static_cast<unsigned long long>(out.checksum));
    return out.checksum == expected ? 0 : 1;
}
