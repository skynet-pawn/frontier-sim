// The money audit must balance exactly, and the usual invariants must hold, on the default scenario.
#include <cmath>
#include <cstdio>

#include "frontier/stats.h"

int main() {
    const auto world = frontier::load_world(FRONTIER_DATA_DIR);
    auto scenario = frontier::load_scenario(FRONTIER_DATA_DIR, world);
    scenario.days = 1500;
    int bad = 0;
    for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        const frontier::RunStats r = frontier::run_and_collect(world, scenario, seed);
        std::printf("seed %llu: residual %.6f, invariants %s%s%s\n", static_cast<unsigned long long>(seed), r.residual,
                    r.invariants_ok ? "ok" : "VIOLATED ", r.first_violation.c_str(), "");
        if (std::fabs(r.residual) > 1e-3 || !r.invariants_ok) ++bad;
    }
    std::puts(bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
