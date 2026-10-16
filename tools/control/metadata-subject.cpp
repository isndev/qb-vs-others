// Build-provenance control: synthetic work, no actor framework and no performance claim.
#include <qvo/harness.h>

#include <utility>

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark = "control/build-metadata";
    spec.framework = "control";
    spec.framework_version = "fixture";
    spec.params = {{"unavailable", 0}};
    spec.expected = [](const qvo::Params &) { return std::uint64_t{42}; };
    spec.caveats = {"SYNTHETIC METADATA CONTROL. Timings are not performance measurements."};
    return qvo::run(argc, argv, std::move(spec), [](const qvo::Params &params, qvo::Watch &watch) {
        if (params.get("unavailable"))
            qvo::not_applicable("synthetic metadata control of the unavailable-result path");
        watch.start();
        watch.stop();
        return qvo::Answer{42};
    });
}
