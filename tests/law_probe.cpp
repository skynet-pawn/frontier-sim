// Dev tool: prints each town's law level, region and civilians on the first day and every N days after.
#include <cstdio>
#include <cstdlib>

#include "frontier/date.h"
#include "frontier/simulation.h"

int main(int argc, char** argv) {
    const int days = argc > 1 ? std::atoi(argv[1]) : 1500;
    const int every = argc > 2 ? std::atoi(argv[2]) : 500;
    const auto world = frontier::load_world(FRONTIER_DATA_DIR);
    auto scenario = frontier::load_scenario(FRONTIER_DATA_DIR, world);
    frontier::Simulation sim(world, scenario);
    for (int d = 0; d <= days; ++d) {
        if (d % every == 0) {
            std::printf("%s\n", frontier::format_date(sim.world().today).c_str());
            for (const auto& n : sim.world().nodes)
                if (!n.fixed_market)
                    std::printf("  %-18s region %-16s law %d paper %d civ %7.0f outrage %.1f fund %.0f\n", n.name.c_str(),
                                n.region >= 0 ? sim.world().region_names[static_cast<std::size_t>(n.region)].c_str() : "-", n.law, n.paper,
                                n.market_population(), n.outrage, n.law_fund);
        }
        sim.step();
    }
    return 0;
}
