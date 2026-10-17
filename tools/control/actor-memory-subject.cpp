// Synthetic population state transitions only: no framework or performance work.
#include <qvo/actor-memory.h>
#include <cstdio>

using qvo::probe::PopulationLedger;

int main() {
    auto require = [](bool condition, const char *name) {
        if (!condition) { std::fprintf(stderr, "actor-memory ledger control failed: %s\n", name); return false; }
        return true;
    };
    PopulationLedger complete(2, 1);
    complete.infrastructure_ready_signal(0);
    complete.born(0); complete.born(1); complete.ready(0); complete.ready(1);
    if (!require(complete.is_live(), "two distinct live identities accepted")) return 1;
    auto count = complete.snapshot();
    if (!require(count.born == 2 && count.ready == 2 && count.alive == 2 && count.destroyed == 0,
                 "accepted population has exact counts")) return 1;
    complete.destroyed(0); complete.destroyed(1);
    if (!require(complete.is_destroyed() && !complete.is_live(), "complete destruction accepted")) return 1;

    PopulationLedger zero(0, 1);
    if (!require(!zero.is_live(), "zero population still needs infrastructure readiness")) return 1;
    zero.infrastructure_ready_signal(0);
    if (!require(zero.is_live() && zero.is_destroyed(), "zero control accepted with infrastructure readiness")) return 1;

    PopulationLedger missing(2, 1);
    missing.infrastructure_ready_signal(0); missing.born(0); missing.ready(0);
    if (!require(missing.snapshot().born == 1 && !missing.is_live(), "missing actor rejected")) return 1;

    PopulationLedger unready(1, 1);
    unready.infrastructure_ready_signal(0); unready.born(0);
    if (!require(unready.snapshot().alive == 1 && !unready.is_live(), "construction is not readiness")) return 1;

    PopulationLedger early(1, 1);
    early.infrastructure_ready_signal(0); early.born(0); early.ready(0); early.destroyed(0);
    if (!require(early.snapshot().destroyed == 1 && !early.is_live(), "early destruction rejected")) return 1;

    PopulationLedger duplicate(1, 1);
    duplicate.infrastructure_ready_signal(0); duplicate.born(0); duplicate.ready(0); duplicate.ready(0);
    if (!require(duplicate.snapshot().error && !duplicate.is_live(), "duplicate readiness rejected")) return 1;

    PopulationLedger out_of_range(1, 1);
    out_of_range.infrastructure_ready_signal(0); out_of_range.born(1);
    if (!require(out_of_range.snapshot().error && !out_of_range.is_live(), "invalid identity rejected")) return 1;

    PopulationLedger duplicate_birth(1, 1);
    duplicate_birth.infrastructure_ready_signal(0); duplicate_birth.born(0); duplicate_birth.born(0); duplicate_birth.ready(0);
    if (!require(duplicate_birth.snapshot().error && !duplicate_birth.is_live(), "duplicate birth rejected")) return 1;
    std::puts("actor-memory ledger: exact population and refusal fixtures passed; no actors created");
    return 0;
}
