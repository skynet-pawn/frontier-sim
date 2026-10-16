// Merchant imports: a town that is running short of a staple orders it ahead instead of waiting for a
// speculative trader to choose it.
//
// Storekeepers in Deadwood, Helena or a fort town did not hope a freighter would bring flour; they wrote
// to a wholesaler at the railhead, paid a freight contractor, and took delivery weeks later. Gold went out
// and flour came in because someone with money in the town ordered it. Here the town's own cash pays for
// the goods and the freight (so the money leaves the town exactly as it would to a trader), the load rides
// the same roads and meets the same gangs as any other freight, and a robbed load is the town's loss.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

void Simulation::merchants_day() {
    const Params& p = world_.params;
    if (p.merchant_interval_days <= 0) return;
    for (std::size_t ni = 0; ni < world_.nodes.size(); ++ni) {
        Node& n = world_.nodes[ni];
        const NodeId id = static_cast<NodeId>(ni);
        if (n.fixed_market || !n.founded || n.ghost || n.population < p.merchant_min_pop) continue;
        if ((world_.today + id) % p.merchant_interval_days != 0) continue;  // towns order on staggered days
        order_for_town(id);
    }
}

void Simulation::order_for_town(NodeId id) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(id)];

    // Goods on the way to this town already count as stock: a merchant does not order twice for one shortage.
    std::vector<double> inbound(world_.goods.size(), 0.0);
    for (const Trader& c : convoys_)
        if (c.alive && c.agent_for == id && c.cargo >= 0) inbound[World::idx(c.cargo)] += c.qty;

    // Most pressing shortage first, so scarce cash goes to food before whiskey.
    struct Want { GoodId g; double cover; double demand; };
    std::vector<Want> wants;
    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        const GoodDef& gd = world_.goods[g];
        if (gd.self_propelled || gd.money) continue;
        const double demand = world_.daily_demand(id, static_cast<GoodId>(g));
        if (demand <= 0) continue;
        const double cover = (n.stock[g] + inbound[g]) / demand;
        if (cover < p.merchant_reorder_days) wants.push_back({static_cast<GoodId>(g), cover, demand});
    }
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.cover < b.cover; });

    double budget = std::max(0.0, n.cash) * p.merchant_cash_share;
    for (const Want& w : wants) {
        if (budget <= 0) break;
        const GoodDef& gd = world_.goods[World::idx(w.g)];
        const double need = w.demand * p.merchant_target_days - n.stock[World::idx(w.g)] - inbound[World::idx(w.g)];
        if (need <= 0) continue;
        const double load_units = p.trader_capacity_lb / gd.load_lb;
        const double local_price = world_.mid_price(id, w.g);

        // The cheapest delivered price among sources that can spare the goods. Eastern markets always can.
        NodeId src = -1;
        double best = 1e18;
        for (std::size_t s = 0; s < world_.nodes.size(); ++s) {
            const Node& sn = world_.nodes[s];
            const NodeId sid = static_cast<NodeId>(s);
            if (sid == id || !sn.founded || !world_.route(sid, id, scratch_route_)) continue;
            double spare = 1e12;
            if (!sn.fixed_market) {
                // A farm hub or neighbour sells only what it holds beyond its own month's cover.
                spare = sn.stock[World::idx(w.g)] - world_.daily_demand(sid, w.g) * p.merchant_reorder_days;
                if (spare < load_units * 0.25) continue;
            }
            double miles[kModeCount] = {0, 0, 0};
            double gang_risk = 0;
            for (EdgeId eid : scratch_route_) {
                const Edge& e = world_.edges[World::idx(eid)];
                miles[static_cast<int>(e.mode)] += e.miles;
                gang_risk += (e.robbed_ema + p.risk_prior * p.risk_prior_weight) / (e.transits_ema + p.risk_prior_weight) * e.travel_days;
            }
            const double q = std::min(load_units, spare);
            const double unit = world_.quote_buy(sid, w.g, q) / q + world_.freight_unit_cost(w.g, miles);
            const double risked = unit / std::max(0.2, 1.0 - std::min(0.8, gang_risk));  // expected loss on the road raises the true price
            if (risked < best) { best = risked; src = sid; }
        }
        if (src < 0) continue;
        // Food is ordered for the people who must eat it, whatever the shelf price says: in a remote town the price only
        // climbs once the barrels are nearly empty, and a wagon train takes weeks. Other goods wait for a margin.
        if (gd.food_share <= 0 && best * (1.0 + p.merchant_margin) > local_price) continue;

        double remaining = need;
        int loads = 0;
        while (remaining > 1.0 && loads < p.merchant_max_loads && budget > 0) {
            const double units = std::min(remaining, load_units);
            const double cost = units * best * 1.15 + 30;  // a margin for price slippage and wages on the road
            if (cost > budget) break;
            const double ordered = dispatch_convoy(id, src, w.g, units, cost, best);
            if (ordered <= 0) break;
            budget -= cost;
            remaining -= ordered;
            ++loads;
        }
        if (loads > 0) {
            ++orders_placed_;
            log_.with({field("town", n.name), field("good", gd.id), field("from", nname(src)), field("loads", loads), field("landed_cost", best), field("local_price", local_price)});
            log_.add(world_.today, EventType::Trade, "merchant_order", id,
                     strf("Merchants at %s order %d load%s of %s from %s", n.name.c_str(), loads, loads == 1 ? "" : "s",
                          gd.name.c_str(), nname(src).c_str()),
                     strf("stock covers %.0f days; landed cost $%.2f against $%.2f on the shelf", w.cover, best, local_price),
                     loads < 2);
        }
    }
}

// Hires a freight outfit at `src` with an advance from the town. Returns the units actually bought.
double Simulation::dispatch_convoy(NodeId town, NodeId src, GoodId g, double units, double advance, double landed) {
    const Params& p = world_.params;
    Node& tn = world_.nodes[World::idx(town)];
    Trader c;
    c.id = next_trader_id_++;
    c.at = src;
    c.agent_for = town;
    c.capacity_lb = p.trader_capacity_lb;
    c.upkeep = p.trader_upkeep;
    c.base_risk_aversion = c.risk_aversion = 1.0;
    c.cash = advance;
    tn.cash -= advance;  // a transfer inside the economy: the town's money rides with the wagons

    // He hires as many guards as the road's reputation warrants, judged on what the load is worth to the town.
    plan_paths(c, src, scratch_paths_);
    const PathInfo pi = scratch_paths_[World::idx(town)];
    const double value = units * world_.mid_price(town, g);
    const double d0 = defense(0);
    int guards = 0;
    double best_u = -1e18;
    for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
        const double loss = std::min(0.95, pi.gang_risk * defense(k) / d0 + pi.hazard_risk);
        const double u = (1 - loss) * value - daily_cost(c, k) * pi.days;
        if (u > best_u) { best_u = u; guards = k; }
    }
    (void)landed;

    const Receipt r = act_buy(c, g, units, advance - daily_cost(c, guards) * pi.days - world_.freight_unit_cost(g, pi.miles) * units);
    if (r.result != ActionResult::Ok || act_travel(c, town, guards) != ActionResult::Ok) {
        // Nothing bought or no road: the advance goes back to the town (and any goods stay with the seller).
        if (r.result == ActionResult::Ok) {
            tn.stock[World::idx(g)] += c.qty;  // cannot happen with a route; keep the books whole if it does
            c.cargo = -1;
        }
        tn.cash += c.cash;
        return 0;
    }
    imported_value_[World::idx(town)] += r.money;
    convoys_.push_back(std::move(c));
    return r.units;
}

// A convoy's journey is over: it arrived, or was robbed on the way. Either way the outfit disbands and
// whatever cash is left goes back to the town that sent it.
void Simulation::finish_convoy(Trader& c) {
    if (!c.alive) return;
    Node& home = world_.nodes[World::idx(c.agent_for)];
    Node& here = world_.nodes[World::idx(c.at)];
    // Wages accrued on the road are spent where the outfit stops.
    if (!here.fixed_market) {
        here.cash += c.pending_spend;
        here.inflow_today += c.pending_spend;
        here.earned_visitors += c.pending_spend;
    } else {
        world_.record(MoneyFlow::EastSpending, -c.pending_spend);
    }
    c.pending_spend = 0;
    if (c.cargo >= 0 && c.at == c.agent_for) {
        const double value = c.qty * world_.mid_price(c.at, c.cargo);
        home.stock[World::idx(c.cargo)] += c.qty;
        delivered_value_ += value;
        log_.with({field("town", home.name), field("good", good(c.cargo).id), field("units", c.qty), field("landed_cost", c.unit_cost)});
        log_.add(world_.today, EventType::Trade, "merchant_delivery", c.at,
                 strf("%.0f %s of merchants' freight reach %s", c.qty, good(c.cargo).unit.c_str(), home.name.c_str()),
                 strf("ordered by the town; landed at $%.2f a unit", c.unit_cost), true);
    }
    // Unspent advance goes home. An outfit that ran into debt on the road is written off, as a retiring trader's is.
    if (c.cash >= 0) home.cash += c.cash;
    else world_.record(MoneyFlow::TraderExit, -c.cash);
    c.cash = 0;
    c.cargo = -1;
    c.qty = 0;
    c.alive = false;
}

}  // namespace frontier
