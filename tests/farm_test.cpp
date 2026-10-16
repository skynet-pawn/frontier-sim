// Farms and new towns: farmers respond to the price they can net, a drought cuts the harvest, and a site with
// farmland and a road becomes a town only once a market justifies settling it.
#include <cmath>
#include <cstdio>
#include <string>

#include "frontier/date.h"
#include "frontier/simulation.h"

namespace {
int fails = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}
frontier::NodeId node_index(const frontier::World& w, const std::string& id) {
    for (std::size_t i = 0; i < w.nodes.size(); ++i)
        if (w.nodes[i].id == id) return static_cast<frontier::NodeId>(i);
    return -1;
}
}  // namespace

int main() {
    using namespace frontier;
    const auto world = load_world(FRONTIER_DATA_DIR);
    auto sc = load_scenario(FRONTIER_DATA_DIR, world);
    sc.seed = 4;
    Simulation sim(world, sc);
    const NodeId spearfish = node_index(sim.world(), "spearfish_valley");
    const NodeId wichita = node_index(sim.world(), "wichita");
    check(spearfish >= 0 && wichita >= 0, "test nodes exist");
    check(!sim.world().nodes[World::idx(spearfish)].founded, "Spearfish Valley starts as a site, not a town");
    check(sim.world().nodes[World::idx(spearfish)].population == 0, "an unfounded site has nobody in it");

    // Before the land opens (1876-03) and the road is built, nothing is settled there.
    sim.run(static_cast<int>(parse_date("1876-01-01") - sc.start));
    check(!sim.world().nodes[World::idx(spearfish)].founded, "still no town before the valley opens");

    // After a few years the valley has been settled because Deadwood pays for its grain.
    sim.run(static_cast<int>(parse_date("1878-01-01") - parse_date("1876-01-01")));
    const Node& sf = sim.world().nodes[World::idx(spearfish)];
    check(sf.founded && sf.farmers > 0, "Spearfish Valley is founded and has farmers by 1878");
    check(sim.log().count_kind("town_founded") >= 1, "the founding is in the event log with its cause");

    // The farming district around Wichita responds to its markets and stays within what the land holds.
    const Node& w = sim.world().nodes[World::idx(wichita)];
    check(w.farmers > 0 && w.farmers <= w.farm_capacity + 1e-6, "Wichita's farmers stay within the land's capacity");
    check(std::fabs(sim.money_audit().residual) < 1e-3, "money audit balances with farms and founding");

    std::puts(fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
