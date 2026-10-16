// A scripted outfit drives a full cattle trade through the public action API, the same one the AI uses.
// Checks the rules (busy, wrong kind, one cargo at a time) and that money stays balanced.
#include <cmath>
#include <cstdio>
#include <string>

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
frontier::GoodId good_index(const frontier::World& w, const std::string& id) {
    for (std::size_t i = 0; i < w.goods.size(); ++i)
        if (w.goods[i].id == id) return static_cast<frontier::GoodId>(i);
    return -1;
}
}  // namespace

int main() {
    using namespace frontier;
    const auto world = load_world(FRONTIER_DATA_DIR);
    auto sc = load_scenario(FRONTIER_DATA_DIR, world);
    // A quiet frontier: nobody else trades, no bandits, no accidents, so the outcome is only the outfit's own.
    sc.traders = 0;
    sc.gangs.clear();
    sc.params.freelancer_entry_rate = 0;
    sc.params.firm_entry_rate = 0;
    sc.params.gang_form_chance = 0;
    for (double& h : sc.params.hazard) h = 0;
    Simulation sim(world, sc);

    const NodeId nueces = node_index(sim.world(), "nueces_range"), kc = node_index(sim.world(), "kansas_city");
    const GoodId cattle = good_index(sim.world(), "beef_cattle"), flour = good_index(sim.world(), "flour");
    const int id = sim.add_trader(nueces, 6000, /*drover=*/true, /*controlled=*/true);
    Trader* t = sim.find_trader(id);

    check(sim.act_sell(*t).result == ActionResult::NoCargo, "cannot sell with no cargo");
    check(sim.act_buy(*t, flour, 10).result == ActionResult::WrongKind, "a drover cannot buy flour");
    const Receipt bought = sim.act_buy(*t, cattle, 1000);
    check(bought.result == ActionResult::Ok && bought.units > 100, "buys cattle at the range");
    check(sim.act_buy(*t, cattle, 10).result == ActionResult::HasCargo, "cannot buy a second cargo");
    check(sim.act_travel(*t, nueces, 0) == ActionResult::WrongPlace, "cannot travel to where he already is");
    check(sim.act_travel(*t, kc, 0) == ActionResult::Ok, "sets off for Kansas City");
    check(sim.act_travel(*t, kc, 0) == ActionResult::Busy, "cannot give new orders mid-journey");
    check(sim.act_buy(*t, cattle, 10).result == ActionResult::Busy, "cannot trade mid-journey");

    int days = 0;
    while (days < 600 && (t = sim.find_trader(id)) && t->travelling) {
        sim.step();
        ++days;
    }
    t = sim.find_trader(id);
    check(t != nullptr && !t->travelling, "arrives");
    if (t) {
        const double before = t->cash;
        const Receipt sold = sim.act_sell(*t);
        std::printf("     sold %.0f head for %.0f after %d days; cash %.0f -> %.0f\n", sold.units, sold.money, days, before, t->cash);
        check(sold.result == ActionResult::Ok && sold.money > bought.money, "sells for more than he paid (Texas to Kansas City)");
        check(t->cargo < 0, "cargo is gone after selling");
    }
    const double residual = sim.money_audit().residual;
    check(std::fabs(residual) < 1e-6, "money audit balances after scripted trades");
    std::printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
