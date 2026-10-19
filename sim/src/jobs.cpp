// Freight jobs: an outfit carries a town's open order on the supplier's goods and is paid for the haul.
//
// Most frontier freight moved on contract. A storekeeper wrote to a wholesaler, the wholesaler loaded the goods and
// a teamster hauled them for the freight charge, paid when the wagons reached the town. The teamster did not own the
// flour and had no stake in its price; he sold wagon space, and what he earned followed weight, distance and the
// road's danger, not what the cargo was worth. Before this, every outfit was a merchant who bought a load with its own
// cash and sold it at the far end, so a wagon of flour paid far less than a wagon of ammunition and a man who lost one
// load was ruined.
//
// Here the town's order already carries a delivered price: goods at the cheapest source, plus freight by weight and
// mile, plus a premium for the road's danger (`cheapest_source`). An outfit at a market that holds the good takes the
// load on account. The supplier owns it; the town pays the goods and the haul on delivery from its cash; the carrier
// keeps what is left after the goods, so scarcity pays him (an unfilled order's offer rises) and a dangerous road pays
// more. A robbed or spoiled load is the supplier's loss: the carrier earns nothing for the trip and has paid his
// wages and guards, but he is not charged for the goods, and nothing he has already earned is carried on the road.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

// The best job an idle outfit can load here: an open order elsewhere for a good this market holds.
Simulation::Plan Simulation::plan_job(const Trader& t, const std::vector<PathInfo>& paths) {
    const Params& p = world_.params;
    ensure_opportunities();
    auto look = [&](std::size_t d) { return p.lookahead_weight * opportunity_[t.drover ? 1 : 0][d]; };
    const Node& hn = world_.nodes[World::idx(t.at)];
    const double d0 = defense(0);
    Plan best;
    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        if (world_.goods[g].self_propelled != t.drover) continue;  // drovers walk cattle; freighters haul the rest
        const GoodId gid = static_cast<GoodId>(g);
        const double avail = hn.fixed_market ? 1e12 : tradable_stock(t.at, gid);
        if (avail < 1) continue;
        const double load_units = t.capacity_lb / world_.goods[g].load_lb;
        for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
            const Node& dn = world_.nodes[d];
            const PathInfo& pi = paths[d];
            if (!pi.ok || static_cast<NodeId>(d) == t.at || dn.fixed_market || dn.order_qty[g] < 1) continue;
            const double price = world_.order_unit_price(static_cast<NodeId>(d), gid, 0);
            if (price <= 0) continue;
            const double q = std::floor(std::min({load_units, avail, dn.order_qty[g], world_.purse(static_cast<NodeId>(d)) / price}));
            if (q < 1) continue;
            // He is paid the freight rate on the weight and miles, a share of it (the rest of the real rate is his own wagons,
            // wages and feed, which he pays through his upkeep), and more as the town's order goes unfilled: scarcity pays.
            // The order's price caps it, since the town offered no more.
            const double goods_unit = world_.quote_buy(t.at, gid, q) / q;
            const double first = dn.order_first_price[g] > 0 ? dn.order_first_price[g] : price;
            const double escalation = std::min(p.merchant_order_cap_mult, price / first);
            const double pay_unit = std::min(p.job_rate_share * world_.freight_unit_cost(gid, pi.miles) * escalation, price - goods_unit);
            if (pay_unit <= 0) continue;
            const double pay = pay_unit * q;
            for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
                const double loss = std::min(0.95, pi.gang_risk * defense(k) / d0 + pi.hazard_risk);
                const double util = (1 - loss) * pay - daily_cost(t, k) * pi.days - (effective_risk(t) - 1) * loss * pay + look(d);
                if (!best.valid || util > best.utility) {
                    best = {util, static_cast<NodeId>(d), gid, q, k, true};
                    best.pay_unit = pay_unit;
                }
            }
        }
    }
    return best;
}

// Loads the supplier's goods on account and sets out. The outfit's cash goes negative by the cost of the goods: a debt
// to the supplier that the town's payment clears (or that is written off if the road takes the load).
bool Simulation::act_take_job(Trader& t, const Plan& j) {
    Node& town = world_.nodes[World::idx(j.dest)];
    const std::size_t gi = World::idx(j.good);
    const double q = std::min(j.qty, town.order_qty[gi]);
    if (j.pay_unit <= 0 || q < 1) return false;
    double spent = 0;
    const double units = world_.buy(t.at, j.good, q, 1e18, spent);
    if (units <= 0) return false;
    t.cash -= spent;
    bought_value_[t.firm ? 1 : 0][gi] += spent;
    if (j.good == world_.flour_good) flour_bought_at_[World::idx(t.at)] += spent;
    t.cargo = j.good;
    t.qty = units;
    t.unit_cost = spent / units;
    t.job_town = j.dest;
    t.job_src = t.at;
    t.job_debt = spent;
    t.job_unit = spent / units;
    t.job_price = spent / units + j.pay_unit;  // the goods at the supplier's price, and the haul
    t.job_guards = j.guards;
    // What he hauls comes off the order, so no one else loads it too.
    town.order_qty[gi] -= units;
    if (town.order_qty[gi] < 1e-6) town.order_qty[gi] = town.order_price[gi] = 0;
    ++jobs_taken_;
    if (act_travel(t, j.dest, j.guards) != ActionResult::Ok) {  // no road after all: the load is gone, settled tomorrow
        t.cargo = -1;
        t.qty = 0;
    }
    return true;
}

// The wagons are at the town: it takes the goods and pays the goods and the haul, as far as its cash goes.
void Simulation::deliver_job(Trader& t) {
    const Params& p = world_.params;
    Node& town = world_.nodes[World::idx(t.job_town)];
    const std::size_t gi = World::idx(t.cargo);
    const double units = t.qty;
    const double due = units * t.job_price;
    const double paid = world_.draw_purse(t.job_town, std::min(due, world_.purse(t.job_town)));
    const double goods = units * t.job_unit;
    town.stock[gi] += units;
    town.last_delivery[gi] = world_.today;
    t.cash += paid;
    world_.order_filled_value += paid;
    world_.order_filled_units += units;
    imported_value_[World::idx(t.job_town)] += goods;
    const double pay = paid - goods;
    profit_today_ += pay;
    t.recent_profit += pay;  // a good haul is a story, as a good trade is
    if (t.recent_profit > p.story_profit_threshold) {
        ++stories_today_;
        t.recent_profit = 0;
    }
    ++jobs_delivered_;
    job_pay_ += pay;
    log_.with({field("trader", t.id), field("town", town.name), field("good", good(t.cargo).id), field("units", units), field("paid", paid), field("pay", pay)});
    log_.add(world_.today, EventType::Trade, "job_delivery", t.job_town,
             strf("Outfit #%d delivers %.0f %s of %s to %s", t.id, units, good(t.cargo).unit.c_str(), good(t.cargo).name.c_str(), town.name.c_str()),
             strf("hauled on the supplier's goods; the town pays %s, of which %s is the carrier's", money(paid).c_str(), money(pay).c_str()), true);
    t.job_town = -1;
    t.job_debt = 0;
    t.cargo = -1;
    t.qty = 0;
}

// Goods the road took are the supplier's loss. The outfit's debt for them is cancelled, so a robbery costs him the
// trip's pay, not the price of the load.
void Simulation::settle_consignment(Trader& t) {
    const double expected = t.cargo >= 0 ? t.job_unit * t.qty : 0.0;
    if (t.job_debt > expected + 1e-6) {
        const double lost = t.job_debt - expected;
        t.cash += lost;
        Node& src = world_.nodes[World::idx(t.job_src)];
        const double taken = src.fixed_market ? 0.0 : std::min(lost, std::max(0.0, src.cash));
        src.cash -= taken;
        world_.record(MoneyFlow::SupplierLoss, lost - taken);  // an eastern supplier, or a town that could not cover it
        job_lost_ += lost;
        t.job_debt = expected;
    }
    if (t.cargo < 0) {
        t.job_town = -1;
        t.job_debt = 0;
    }
}

}  // namespace frontier
