// Dev tool: runs day by day and prints each day the money residual changes, with that day's events,
// to find where money is created or destroyed outside the ledger.
#include <cmath>
#include <cstdio>

#include "frontier/date.h"
#include "frontier/simulation.h"

int main(int argc, char** argv) {
    const int days = argc > 1 ? std::atoi(argv[1]) : 600;
    const auto world = frontier::load_world(FRONTIER_DATA_DIR);
    auto scenario = frontier::load_scenario(FRONTIER_DATA_DIR, world);
    frontier::Simulation sim(world, scenario);
    double last = 0;
    int shown = 0;
    for (int d = 0; d < days && shown < 25; ++d) {
        const std::size_t before = sim.log().events().size();
        sim.step();
        const double r = sim.money_audit().residual;
        if (std::fabs(r - last) > 0.01) {
            std::printf("%s residual %+.2f (change %+.2f)\n", frontier::format_date(sim.world().today - 1).c_str(), r, r - last);
            for (std::size_t i = before; i < sim.log().events().size(); ++i)
                std::printf("    [%s] %s\n", sim.log().events()[i].kind.c_str(), sim.log().events()[i].text.c_str());
            last = r;
            ++shown;
        }
    }
    return 0;
}
