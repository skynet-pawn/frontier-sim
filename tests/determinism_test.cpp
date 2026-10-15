// Same seed must give the same history; different seeds must not.
#include <cstdint>
#include <cstdio>

#include "frontier/simulation.h"

namespace {
std::uint64_t run(std::uint64_t seed, int days) {
    const auto world = frontier::load_world(FRONTIER_DATA_DIR);
    auto scenario = frontier::load_scenario(FRONTIER_DATA_DIR, world);
    scenario.seed = seed;
    frontier::Simulation sim(world, scenario);
    sim.run(days);
    return sim.log().hash();
}
}  // namespace

int main() {
    const std::uint64_t a = run(7, 600), b = run(7, 600), c = run(8, 600);
    std::printf("seed 7: %016llx / %016llx, seed 8: %016llx\n", static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
    if (a != b) {
        std::puts("FAIL: same seed produced different histories");
        return 1;
    }
    if (a == c) {
        std::puts("FAIL: different seeds produced identical histories");
        return 1;
    }
    std::puts("ok");
    return 0;
}
