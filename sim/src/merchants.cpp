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

#include "frontier/date.h"
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
        // Below merchant_min_pop there used to be no merchant class at all, so a 60-man camp never ordered a sack of flour and
        // starved once its opening provisions ran out. With lead_stages any town orders what is worth a wagon (see below).
        if (n.fixed_market || !n.founded || n.ghost || n.population < (p.lead_stages > 0 ? 1.0 : p.merchant_min_pop)) continue;
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
    // (An offer nobody has taken is not stock. Counting it made a town believe it was covered while its shelf was bare,
    // and the bigger the order the bigger the illusion.)
    double committed = 0;  // what the town's open orders would cost it
    for (std::size_t g = 0; g < world_.goods.size(); ++g) committed += n.order_qty[g] * n.order_price[g];

    // A town far from its supply must keep more on the shelf: what it orders today arrives after the whole journey,
    // and a storekeeper who reorders at forty days of stock with a fifty-day haul will run dry every time. So the
    // reorder point is the lead time (days from the nearest eastern market), plus a safety margin, and the order
    // brings the shelf up to that point plus a cycle's worth. Deadwood lays in a season's stock; a railhead does not.
    double lead = lead_days(id), safety = p.merchant_safety_days, growth = 0.0;
    if (p.lead_stages > 0) {
        // The lead time is what this town's orders have really taken, and the safety stock is how much that wanders;
        // both come from its own deliveries (see lead_estimate), not from the map and a fixed margin.
        const LeadEstimate est = lead_estimate(id);
        lead = est.lead;
        safety = p.lead_safety_k * est.spread;
        // Growth: people arrive daily, so the order is sized for the town it will meet, not the one that posted it.
        if (n.pop_mark_day == 0 || n.pop_mark <= 0) { n.pop_mark = n.population; n.pop_mark_day = world_.today; }
        else if (world_.today - n.pop_mark_day >= 28) {
            const double days = static_cast<double>(world_.today - n.pop_mark_day);
            const double r = (n.population - n.pop_mark) / n.pop_mark / days;
            if (n.population < n.pop_mark * 1.2)  // a jump is a rush (it brings its own provisions), not growth
                n.growth_rate = 0.5 * n.growth_rate + 0.5 * std::clamp(r, 0.0, 0.01);
            n.pop_mark = n.population;
            n.pop_mark_day = world_.today;
        }
        growth = n.growth_rate;
    }
    // The winter store: from autumn the northern towns hold a month more, so a closed road finds the shelves full.
    double winter = 0;
    if (p.winter_cover_days > 0 && n.y >= p.winter_min_y) {
        int yr;
        unsigned month, day;
        civil_from_days(world_.today, yr, month, day);
        if (static_cast<int>(month) >= p.winter_lay_in_month || month <= 2) winter = p.winter_cover_days;
    }
    const double reorder_days = lead + safety + winter;
    // What the town will eat on the day the wagons land: today's demand grown by the town's own pace, as far as its
    // capacity lets it grow.
    const double grown = p.lead_stages > 0
        ? std::min({1.0 + growth * (reorder_days + p.merchant_interval_days) / 2, p.growth_cap_mult, std::max(1.0, n.capacity / std::max(1.0, n.population))})
        : 1.0;

    // Most pressing shortage first, so scarce cash goes to food before whiskey.
    struct Want { GoodId g; double cover; double demand; };
    std::vector<Want> wants;
    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        const GoodDef& gd = world_.goods[g];
        if (gd.money) continue;  // cattle and horses are ordered too: a drover with a herd nearby fills them
        const double demand = world_.daily_demand(id, static_cast<GoodId>(g)) * grown;
        if (demand <= 0) continue;
        const double cover = (n.stock[g] + inbound[g]) / demand;
        if (cover < reorder_days) wants.push_back({static_cast<GoodId>(g), cover, demand});
    }
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.cover < b.cover; });

    double budget = world_.purse(id) * p.merchant_cash_share - committed;
    for (const Want& w : wants) {
        if (budget <= 0) break;
        const GoodDef& gd = world_.goods[World::idx(w.g)];
        const double load_units = p.trader_capacity_lb / gd.load_lb;
        // The cycle beyond the reorder point is one wagon load of the town's demand (a wagon costs the same half empty),
        // and never less than the review interval.
        const double cycle = p.lead_stages > 0 ? std::max<double>(p.merchant_interval_days, load_units / w.demand) : p.merchant_cycle_days;
        const double target_days = reorder_days + cycle;
        const double need = w.demand * target_days - n.stock[World::idx(w.g)] - inbound[World::idx(w.g)];
        if (need <= 0) continue;
        if (p.lead_stages > 0 && need < 0.25 * load_units) continue;  // not worth sending a wagon for (the same bar a supplier uses)
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
        const bool had_order = n.order_qty[gi] > 0;
        const double offer = had_order ? n.order_price[gi] : best * 1.15;
        budget += n.order_qty[gi] * n.order_price[gi];  // the standing order is re-set below, so its cost is free to commit again
        const double units = std::min(need, budget / offer);  // what the town needs, as far as its cash goes
        if (units < 1.0) {
            n.order_qty[gi] = n.order_price[gi] = 0;  // nothing left to ask for
            continue;
        }
        if (!had_order) {
            n.order_price[gi] = n.order_first_price[gi] = offer;
            n.order_posted[gi] = n.order_raised[gi] = n.order_hired[gi] = world_.today;
        }
        const double before = n.order_qty[gi];
        n.order_qty[gi] = units;
        budget -= units * offer;
        if (had_order && units < before * 1.5) continue;  // the same standing order, trimmed or topped up a little: not news
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
    for (NodeId sid : horizon(town)) {  // the nearest sources and the eastern markets: a storekeeper writes to wholesalers he knows
        const Node& sn = world_.nodes[World::idx(sid)];
        if (sid == town || !sn.founded || !world_.route(sid, town, scratch_route_)) continue;
        double spare = 1e12;
        if (!sn.fixed_market) {
            // A farm hub or neighbour sells only what it holds beyond its own month's cover.
            // (a supplier keeps the cover it needs to be resupplied itself)
            double keep = p.merchant_reorder_days;
            if (p.lead_stages > 0) { const LeadEstimate se = lead_estimate(sid); keep = se.lead + p.lead_safety_k * se.spread; }
            spare = sn.stock[World::idx(g)] - world_.daily_demand(sid, g) * keep;
            if (spare < load_units * 0.25) continue;
        }
        double miles[kModeCount] = {0, 0, 0};
        double gang_risk = 0;
        for (EdgeId eid : scratch_route_) {
            const Edge& e = world_.edges[World::idx(eid)];
            miles[static_cast<int>(e.mode)] += e.freight_miles;
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
            LeadEstimate est{0, 0};
            if (p.lead_stages > 0) est = lead_estimate(static_cast<NodeId>(ni));
            const int expire = p.lead_stages > 0 ? std::max<int>(p.merchant_order_expire_days, static_cast<int>(est.lead + p.lead_safety_k * est.spread)) : p.merchant_order_expire_days;
            if (world_.today - n.order_posted[g] >= expire) {
                ++orders_expired_;
                log_.with({field("town", n.name), field("good", world_.goods[g].id), field("units", n.order_qty[g]), field("offer", n.order_price[g])});
                log_.add(world_.today, EventType::Trade, "order_expired", static_cast<NodeId>(ni),
                         strf("Merchants at %s withdraw their order for %.0f %s of %s", n.name.c_str(), n.order_qty[g],
                              world_.goods[g].unit.c_str(), world_.goods[g].name.c_str()),
                         strf("nobody filled it in %d days, even at $%.2f", expire, n.order_price[g]));
                n.order_qty[g] = n.order_price[g] = 0;
                continue;
            }
            // Still open after a fortnight: the town stops waiting for a freighter to come by and hires one.
            // The town can wait for a passing freighter only as long as its shelf will outlast the haul: with food that
            // will run out before wagons could arrive, it hires at once.
            int wait = p.merchant_hire_after_days;
            if (p.lead_stages > 0 && p.merchant_hire_after_days > 0 && world_.goods[g].food_share > 0) {
                const double dem = world_.daily_demand(static_cast<NodeId>(ni), static_cast<GoodId>(g));
                if (dem > 0) wait = std::clamp(static_cast<int>(n.stock[g] / dem - est.lead), 0, p.merchant_hire_after_days);
            }
            if (p.merchant_hire_after_days > 0 && world_.today - n.order_hired[g] >= wait)
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
    double remaining = n.order_qty[gi];
    double budget = world_.purse(id) * p.merchant_cash_share;
    int loads = 0;
    double hauled = 0;
    while (remaining > 1.0 && budget > 0) {
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
    c.ordered_day = tn.order_qty[World::idx(g)] > 0 ? tn.order_posted[World::idx(g)] : world_.today;
    c.drover = world_.goods[World::idx(g)].self_propelled;   // cattle walk: only a drover's outfit can buy them (see act_buy)
    c.capacity_lb = p.trader_capacity_lb;
    c.upkeep = p.trader_upkeep;
    c.base_risk_aversion = c.risk_aversion = 1.0;
    c.cash = advance;
    world_.draw_purse(town, advance);  // a transfer inside the economy: the town's money rides with the wagons

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
// What a town believes about its supply line: before it has seen a delivery, the route time plus slippage and a review
// interval; as deliveries come in, what they really took. The prior counts as a few deliveries so one early arrival
// does not set the town's mind.
Simulation::LeadEstimate Simulation::lead_estimate(NodeId id) const {
    const Params& p = world_.params;
    const Node& n = world_.nodes[World::idx(id)];
    const double prior = lead_days(id) * p.lead_prior_mult + p.merchant_interval_days;
    const double w = n.lead_n / (n.lead_n + p.lead_prior_weight);
    return {w * n.lead_obs + (1 - w) * prior, w * n.lead_dev + (1 - w) * prior * p.lead_dev_prior};
}

void Simulation::note_delivery(NodeId town, long ordered_day) {
    Node& n = world_.nodes[World::idx(town)];
    const double lag = static_cast<double>(std::max(1L, static_cast<long>(world_.today) - ordered_day));
    if (n.lead_n == 0) { n.lead_obs = lag; n.lead_dev = 0.25 * lag; }
    else {
        const double a = std::max(world_.params.lead_obs_alpha, 1.0 / (n.lead_n + 1));
        n.lead_dev += a * (std::fabs(lag - n.lead_obs) - n.lead_dev);
        n.lead_obs += a * (lag - n.lead_obs);
    }
    n.lead_n = std::min(n.lead_n + 1, 1000);
}

void Simulation::finish_convoy(Trader& c) {
    if (!c.alive) return;
    if (c.contract >= 0) { finish_contract_convoy(c); return; }
    if (c.express) { finish_express(c); return; }
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
        home.last_delivery[World::idx(c.cargo)] = world_.today;
        note_delivery(c.agent_for, c.ordered_day);
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
