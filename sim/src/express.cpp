// Gold express: eastern agents who buy a camp's surplus dust and ship it to the mint.
//
// Why: a gold camp's output is its money, but a lone trader needs capital equal to the gold he carries and the
// frontier had few traders per camp, so most dust sat on the shelf and was written off at the stock cap (on the
// 4,694-node map 70% of all gold mined). Historically the camps' gold went east by Wells Fargo-style express
// companies and bank agents with eastern capital, guarded, and it was the biggest prize on the road for gangs.
//
// Model: every `express_interval_days` an agent calls at each camp that mines gold. Above `express_keep_days` of
// output (the dust the camp keeps to pay its suppliers with, see World::draw_purse) he buys the surplus at the
// camp's own shelf price, so the camp is paid in cash, and sends it to the nearest eastern market as a convoy
// with guards. Gangs meet it on the road like any other load and take it whole (crime.cpp). At the market it
// sells at the mint price; the agent's gain or loss goes back East (TraderExit). Capital comes from the East
// (NewTraderCapital), as for any eastern firm.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

void Simulation::express_day() {
    const Params& p = world_.params;
    if (p.express_stages <= 0 || markets_.empty() || world_.money_good < 0) return;
    const GoodId gold = world_.money_good;
    const std::size_t gi = World::idx(gold);
    const double interval = std::max(1.0, p.express_interval_days);
    const double face = world_.goods[gi].base_cost;
    for (std::size_t ni = 0; ni < world_.nodes.size(); ++ni) {
        Node& camp = world_.nodes[ni];
        if (camp.fixed_market || !camp.founded) continue;
        if (std::fmod(static_cast<double>(world_.today - static_cast<long>(ni)), interval) != 0.0) continue;  // agents stagger their rounds
        double rate = 0;
        for (const ProductionDef& pd : camp.production)
            if (pd.good == gold && world_.today >= pd.from && world_.today < pd.until) rate += pd.rate;
        if (rate <= 0) continue;
        const double surplus = camp.stock[gi] - rate * p.express_keep_days;
        if (surplus * face < p.express_min_value) continue;

        Trader c;
        c.id = next_trader_id_++;
        c.at = static_cast<NodeId>(ni);
        c.firm = true;
        c.express = true;
        c.capacity_lb = p.trader_capacity_lb;
        c.upkeep = p.trader_upkeep;
        c.base_risk_aversion = c.risk_aversion = 1.0;
        plan_paths(c, c.at, scratch_paths_);
        NodeId mint = -1;
        double best_days = 1e18;
        for (NodeId m : markets_) {
            const PathInfo& pi = scratch_paths_[World::idx(m)];
            if (pi.ok && pi.days < best_days) { best_days = pi.days; mint = m; }
        }
        if (mint < 0) continue;  // no road to a market: the dust waits

        const PathInfo pi = scratch_paths_[World::idx(mint)];
        const double units = std::min(surplus, c.capacity_lb / world_.goods[gi].load_lb);
        const double value = units * face;
        const double d0 = defense(0);
        int guards = 0;
        double best_u = -1e18;
        for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
            const double loss = std::min(0.95, pi.gang_risk * defense(k) / d0 + pi.hazard_risk);
            const double u = (1 - loss) * value - daily_cost(c, k) * pi.days;
            if (u > best_u) { best_u = u; guards = k; }
        }

        const double advance = value * 1.1 + daily_cost(c, guards) * pi.days + 30;
        c.cash = advance;
        c.start_capital = advance;
        world_.record(MoneyFlow::NewTraderCapital, advance);
        const Receipt r = act_buy(c, gold, units, advance - daily_cost(c, guards) * pi.days - world_.freight_unit_cost(gold, pi.miles) * units);
        c.agent_for = mint;  // the convoy code hands an arrived or robbed outfit to finish_convoy
        if (r.result != ActionResult::Ok || act_travel(c, mint, guards) != ActionResult::Ok) {
            if (r.result == ActionResult::Ok) {  // bought but cannot go: undo the sale
                camp.stock[gi] += c.qty;
                camp.cash -= r.money;
                c.cash += r.money;
                c.cargo = -1;
            }
            world_.record(MoneyFlow::TraderExit, -c.cash);
            continue;
        }
        ++express_sent_;
        express_oz_ += r.units;
        log_.with({field("camp", camp.name), field("mint", nname(mint)), field("oz", r.units), field("guards", guards)});
        log_.add(world_.today, EventType::Trade, "express_sent", c.at,
                 strf("An express agent buys %.0f oz of dust at %s and sends it to %s", r.units, camp.name.c_str(), nname(mint).c_str()),
                 strf("the camp holds more than %.0f days of output; paid %s at the camp's price", p.express_keep_days, money(r.money).c_str()), true);
        convoys_.push_back(std::move(c));
    }
}

// The shipment reached the mint or was robbed on the way; either way the agent's affairs are wound up.
void Simulation::finish_express(Trader& c) {
    if (!c.alive) return;
    Node& here = world_.nodes[World::idx(c.at)];
    if (here.fixed_market) world_.record(MoneyFlow::EastSpending, -c.pending_spend);
    else {
        here.cash += c.pending_spend;
        here.inflow_today += c.pending_spend;
        here.earned_visitors += c.pending_spend;
    }
    c.pending_spend = 0;
    if (c.cargo >= 0 && here.fixed_market) {
        sell_cargo(c, -1);
    } else if (c.cargo < 0) {
        ++express_robbed_;  // the dust was taken on the road
    }
    world_.record(MoneyFlow::TraderExit, -c.cash);  // what is left, or the debt, goes back East
    c.cash = 0;
    c.cargo = -1;
    c.qty = 0;
    c.alive = false;
}

}  // namespace frontier
