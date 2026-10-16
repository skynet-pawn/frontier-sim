// Merchant orders: a town that is running short of a staple posts an open order for it ahead, instead of
// waiting for a speculative trader to choose it.
//
// Storekeepers in Deadwood, Helena or a fort town did not hope a freighter would bring flour; they wrote
// to a wholesaler at the railhead and took delivery weeks later. Gold went out and flour came in because
// someone with money in the town wanted it. Here the order sits on the town's board with a delivered price,
// paid from the town's own cash on delivery. Any outfit that can profit from it, a freight house with spare
// wagons or a drover with a herd on a nearby range, takes the job, so what a town gets depends on what the
// freighters can actually carry. If nobody bites, the town raises its offer: scarcity pays, and the wagons
// turn toward the hill. A town that has waited too long does what storekeepers did: it hires a freight outfit of
// its own (`dispatch_convoy`), so a thin market never leaves it to starve.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

void Simulation::merchants_day() {
    const Params& p = world_.params;
    if (p.merchant_interval_days <= 0) return;
    orders_day();
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
    for (std::size_t g = 0; g < world_.goods.size(); ++g) inbound[g] += n.order_qty[g];  // already on the board
    double committed = 0;  // what the town's open orders would cost it
    for (std::size_t g = 0; g < world_.goods.size(); ++g) committed += n.order_qty[g] * n.order_price[g];

    // Most pressing shortage first, so scarce cash goes to food before whiskey.
    struct Want { GoodId g; double cover; double demand; };
    std::vector<Want> wants;
    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        const GoodDef& gd = world_.goods[g];
        if (gd.money) continue;  // cattle and horses are ordered too: a drover with a herd nearby fills them
        const double demand = world_.daily_demand(id, static_cast<GoodId>(g));
        if (demand <= 0) continue;
        const double cover = (n.stock[g] + inbound[g]) / demand;
        if (cover < p.merchant_reorder_days) wants.push_back({static_cast<GoodId>(g), cover, demand});
    }
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.cover < b.cover; });

    double budget = std::max(0.0, n.cash) * p.merchant_cash_share - committed;
    for (const Want& w : wants) {
        if (budget <= 0) break;
        const GoodDef& gd = world_.goods[World::idx(w.g)];
        const double need = w.demand * p.merchant_target_days - n.stock[World::idx(w.g)] - inbound[World::idx(w.g)];
        if (need <= 0) continue;
        const double load_units = p.trader_capacity_lb / gd.load_lb;
        const double local_price = world_.mid_price(id, w.g);

        NodeId src = -1;
        double best = 1e18;
        if (!cheapest_source(id, w.g, load_units, src, best)) continue;
        // Food is ordered for the people who must eat it, whatever the shelf price says: in a remote town the price only
        // climbs once the barrels are nearly empty, and a wagon train takes weeks. Other goods wait for a margin.
        if (gd.food_share <= 0 && best * (1.0 + p.merchant_margin) > local_price) continue;

        // Post the order at what the goods would cost landed, plus a margin for slippage and the road.
        // An order already on the board keeps its (possibly raised) price; the new quantity joins it.
        const std::size_t gi = World::idx(w.g);
        const double offer = n.order_qty[gi] > 0 ? n.order_price[gi] : best * 1.15;
        const double units = std::min({need, p.merchant_max_loads * load_units, budget / offer});
        if (units < 1.0) continue;
        if (n.order_qty[gi] <= 0) {
            n.order_price[gi] = n.order_first_price[gi] = offer;
            n.order_posted[gi] = n.order_raised[gi] = n.order_hired[gi] = world_.today;
        }
        n.order_qty[gi] += units;
        budget -= units * offer;
        ++orders_placed_;
        log_.with({field("town", n.name), field("good", gd.id), field("units", units), field("offer", offer), field("landed_cost", best), field("local_price", local_price), field("cover_days", w.cover)});
        log_.add(world_.today, EventType::Trade, "merchant_order", id,
                 strf("Merchants at %s post an order for %.0f %s of %s at $%.2f delivered", n.name.c_str(), units,
                      gd.unit.c_str(), gd.name.c_str(), offer),
                 strf("stock covers %.0f days; landed cost from %s is $%.2f against $%.2f on the shelf", w.cover,
                      nname(src).c_str(), best, local_price),
                 units * offer < 1500);
    }
}

// The cheapest delivered price among sources that can spare the goods. Eastern markets always can.
bool Simulation::cheapest_source(NodeId town, GoodId g, double load_units, NodeId& src, double& best) {
    const Params& p = world_.params;
    src = -1;
    best = 1e18;
    for (std::size_t s = 0; s < world_.nodes.size(); ++s) {
        const Node& sn = world_.nodes[s];
        const NodeId sid = static_cast<NodeId>(s);
        if (sid == town || !sn.founded || !world_.route(sid, town, scratch_route_)) continue;
        double spare = 1e12;
        if (!sn.fixed_market) {
            // A farm hub or neighbour sells only what it holds beyond its own month's cover.
            spare = sn.stock[World::idx(g)] - world_.daily_demand(sid, g) * p.merchant_reorder_days;
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
        const double unit = world_.quote_buy(sid, g, q) / q + world_.freight_unit_cost(g, miles);
        const double risked = unit / std::max(0.2, 1.0 - std::min(0.8, gang_risk));  // expected loss on the road raises the true price
        if (risked < best) { best = risked; src = sid; }
    }
    return src >= 0;
}

// Orders nobody has filled get dearer: an unfilled order is the town telling the freighters it is short, and the
// price is how they hear it. After a long wait the town gives up and the order is withdrawn.
void Simulation::orders_day() {
    const Params& p = world_.params;
    for (std::size_t ni = 0; ni < world_.nodes.size(); ++ni) {
        Node& n = world_.nodes[ni];
        for (std::size_t g = 0; g < world_.goods.size(); ++g) {
            if (n.order_qty[g] <= 0) continue;
            if (world_.today - n.order_posted[g] >= p.merchant_order_expire_days) {
                ++orders_expired_;
                log_.with({field("town", n.name), field("good", world_.goods[g].id), field("units", n.order_qty[g]), field("offer", n.order_price[g])});
                log_.add(world_.today, EventType::Trade, "order_expired", static_cast<NodeId>(ni),
                         strf("Merchants at %s withdraw their order for %.0f %s of %s", n.name.c_str(), n.order_qty[g],
                              world_.goods[g].unit.c_str(), world_.goods[g].name.c_str()),
                         strf("nobody filled it in %d days, even at $%.2f", p.merchant_order_expire_days, n.order_price[g]));
                n.order_qty[g] = n.order_price[g] = 0;
                continue;
            }
            // Still open after a fortnight: the town stops waiting for a freighter to come by and hires one.
            if (p.merchant_hire_after_days > 0 && world_.today - n.order_hired[g] >= p.merchant_hire_after_days)
                hire_for_order(static_cast<NodeId>(ni), static_cast<GoodId>(g));
            if (n.order_qty[g] <= 0) continue;
            const double cap = n.order_first_price[g] * p.merchant_order_cap_mult;
            if (world_.today - n.order_raised[g] >= p.merchant_order_raise_days && n.order_price[g] < cap) {
                n.order_price[g] = std::min(cap, n.order_price[g] * (1.0 + p.merchant_order_raise));
                n.order_raised[g] = world_.today;
            }
        }
    }
}

// The town's own freighters for part of an open order: what is bought and hauled comes off the order, so a
// freighter who turns up later finds less left to fill.
void Simulation::hire_for_order(NodeId id, GoodId g) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(id)];
    const std::size_t gi = World::idx(g);
    n.order_hired[gi] = world_.today;
    const double load_units = p.trader_capacity_lb / world_.goods[gi].load_lb;
    NodeId src = -1;
    double best = 0;
    if (!cheapest_source(id, g, load_units, src, best)) return;
    double remaining = std::min(n.order_qty[gi], p.merchant_max_loads * load_units);
    double budget = std::max(0.0, n.cash) * p.merchant_cash_share;
    int loads = 0;
    double hauled = 0;
    while (remaining > 1.0 && loads < p.merchant_max_loads && budget > 0) {
        const double units = std::min(remaining, load_units);
        const double cost = units * best * 1.15 + 30;  // a margin for price slippage and wages on the road
        if (cost > budget) break;
        const double ordered = dispatch_convoy(id, src, g, units, cost, best);
        if (ordered <= 0) break;
        budget -= cost;
        remaining -= ordered;
        hauled += ordered;
        ++loads;
    }
    if (loads == 0) return;
    n.order_qty[gi] -= hauled;
    if (n.order_qty[gi] < 1.0) n.order_qty[gi] = n.order_price[gi] = 0;
    ++convoys_hired_;
    log_.with({field("town", n.name), field("good", world_.goods[gi].id), field("from", nname(src)), field("loads", loads), field("units", hauled), field("landed_cost", best)});
    log_.add(world_.today, EventType::Trade, "merchant_convoy", id,
             strf("Merchants at %s hire %d load%s of %s from %s themselves", n.name.c_str(), loads, loads == 1 ? "" : "s",
                  world_.goods[gi].name.c_str(), nname(src).c_str()),
             strf("their order had gone %d days without a freighter taking it", p.merchant_hire_after_days),
             loads < 2);
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
