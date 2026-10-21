// Dev tool: from a saved state, follows every freelancer's cash for N days and prints how rich they get.
// Usage: wealth_probe DATA_DIR SNAPSHOT DAYS
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "frontier/simulation.h"

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: wealth_probe DATA SNAPSHOT DAYS\n"); return 1; }
    const auto world = frontier::load_world(argv[1]);
    auto scenario = frontier::load_scenario(argv[1], world);
    frontier::Simulation sim(world, scenario);
    sim.load_state(argv[2]);
    const int days = std::atoi(argv[3]);
    std::map<int, double> peak;  // trader id -> highest cash seen while a freelancer
    long rich_days = 0, eligible_days = 0, travelling_days = 0, loaded_days = 0, controlled_days = 0, market_days = 0;
    const int houses0 = sim.houses_founded();
    const std::size_t settled0 = sim.log().count_kind("trader_settled");
    for (int d = 0; d < days; ++d) {
        sim.step();
        for (const auto& t : sim.traders())
            if (t.alive && !t.firm && t.house < 0 && t.agent_for < 0) {
                double& p = peak[t.id]; p = std::max(p, t.cash);
                if (t.cash > 20000) {
                    ++rich_days;
                    if (t.travelling) ++travelling_days;
                    else if (t.controlled) ++controlled_days;
                    else if (t.cargo >= 0) ++loaded_days;
                    else if (sim.world().nodes[static_cast<std::size_t>(t.at)].fixed_market) ++market_days;
                    else ++eligible_days;
                }
            }
    }
    std::printf("rich (>20k) freelancer-days %ld: travelling %ld, controlled %ld, loaded %ld, at eastern market %ld, eligible to settle %ld\n",
                rich_days, travelling_days, controlled_days, loaded_days, market_days, eligible_days);
    std::printf("houses founded %d, traders settled %zu\n", sim.houses_founded() - houses0,
                sim.log().count_kind("trader_settled") - settled0);
    std::vector<double> v;
    for (auto& kv : peak) v.push_back(kv.second);
    std::sort(v.begin(), v.end());
    auto q = [&](double f) { return v.empty() ? 0.0 : v[static_cast<std::size_t>(f * (v.size() - 1))]; };
    std::printf("freelancers seen %zu over %d days\n", v.size(), days);
    std::printf("peak cash: p50 %.0f p90 %.0f p99 %.0f max %.0f\n", q(0.5), q(0.9), q(0.99), q(1.0));
    for (double th : {5000.0, 10000.0, 20000.0, 24000.0})
        std::printf("  reached $%.0f: %zu\n", th, static_cast<std::size_t>(std::count_if(v.begin(), v.end(), [&](double x) { return x >= th; })));
    return 0;
}
