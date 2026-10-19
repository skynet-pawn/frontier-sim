// Standing supply contracts: a town that has been burned by a shortage buys insurance.
//
// A camp three months from the railhead cannot count on a freighter happening by with flour at the moment its
// barrels run low. Spot freight chases whatever pays best per wagon, and a bulky staple loses to whiskey and
// ammunition even when the camp is starving. What the storekeepers of Deadwood or Helena did instead was deal
// with a freighting house on a standing arrangement: a fixed monthly quantity, delivered on a schedule, at a price
// above what the goods cost landed. The premium is the insurance: the house is paid to hold capacity and take
// the road's risk, and the town pays it per delivery out of its income instead of laying out a season's cash at once.
//
// Only a town that remembers going short signs (experience shapes motivation), only a house with free capital can
// carry it, and a house that falls behind is dropped. The house funds each load from its own cash, so how much
// it can contract is limited by its capital, not by an arbitrary count of wagons.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

namespace {
// Capital a house ties up in a contract: the goods it pays for and hauls before the town pays it back.
double working_capital(double rate, double landed, double route_days) { return rate * landed * (2.0 * route_days + 30.0); }
}  // namespace

void Simulation::contracts_day() {
    const Params& p = world_.params;
    if (p.contract_share <= 0 || houses_.empty()) return;

    for (SupplyContract& k : contracts_)
        if (k.alive) run_contract(k);

    for (std::size_t ni = 0; ni < world_.nodes.size(); ++ni) {
        const Node& n = world_.nodes[ni];
        const NodeId id = static_cast<NodeId>(ni);
        if (n.fixed_market || !n.founded || n.ghost) continue;
        if (n.market_population() - std::min(n.garrison, n.market_population()) < p.merchant_min_pop) continue;
        if ((world_.today + id) % std::max(1, p.merchant_interval_days) != 0) continue;  // staggered, like orders
        seek_contracts(id);
    }
}

void Simulation::seek_contracts(NodeId id) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(id)];
    // No scar, no contract: a town that has never gone short does not pay a premium against it.
    if (world_.today - n.last_shortage_log >= p.contract_memory_days) return;

    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        const GoodDef& gd = world_.goods[g];
        if (gd.food_share <= 0) continue;
        bool held = false;
        for (const SupplyContract& k : contracts_)
            if (k.alive && k.town == id && k.good == static_cast<GoodId>(g)) held = true;
        if (held) continue;
        const double rate = world_.daily_demand(id, static_cast<GoodId>(g)) * p.contract_share;
        if (rate <= 0) continue;

        const double load_units = p.trader_capacity_lb / gd.load_lb;
        NodeId src = -1;
        double landed = 0;
        if (!cheapest_source(id, static_cast<GoodId>(g), load_units, src, landed)) continue;
        const double price = landed * (1.0 + p.contract_premium);
        // A town signs only what its income can bear: it pays this every day for as long as the contract runs.
        if (rate * price > 0.5 * n.income_ema) continue;

        // The house with the most free capital takes it, if it has enough.
        const double capital = working_capital(rate, landed, world_.route_days(src, id));
        int best_house = -1;
        double best_free = capital;
        for (const House& h : houses_) {
            if (!h.alive) continue;
            double committed = 0;
            for (const SupplyContract& k : contracts_)
                if (k.alive && k.house == h.id) committed += k.capital;
            const double free = h.cash - committed;
            if (free >= best_free) { best_free = free; best_house = h.id; }
        }
        if (best_house < 0) continue;

        SupplyContract k;
        k.id = static_cast<int>(contracts_.size());
        k.town = id;
        k.house = best_house;
        k.good = static_cast<GoodId>(g);
        k.rate = rate;
        k.price = price;
        k.capital = capital;
        k.signed_day = world_.today;
        k.ends = world_.today + p.contract_term_days;
        contracts_.push_back(k);
        ++contracts_signed_;
        log_.with({field("town", n.name), field("good", gd.id), field("house", best_house), field("rate", rate), field("price", price), field("landed_cost", landed),
                   field("capital", capital)});
        log_.add(world_.today, EventType::Trade, "supply_contract", id,
                 strf("%s signs a standing contract with freight house #%d for %.0f %s of %s a day at $%.2f", n.name.c_str(), best_house, rate, gd.unit.c_str(),
                      gd.name.c_str(), price),
                 strf("the town went short %d days ago and will pay %.0f%% over the landed cost of $%.2f for deliveries it can count on",
                      static_cast<int>(world_.today - n.last_shortage_log), p.contract_premium * 100, landed));
    }
}

void Simulation::run_contract(SupplyContract& k) {
    const Params& p = world_.params;
    const GoodDef& gd = world_.goods[World::idx(k.good)];
    Node& n = world_.nodes[World::idx(k.town)];
    const House& h = houses_[static_cast<std::size_t>(k.house)];

    const char* why = nullptr;
    if (!h.alive) why = "the freight house closed";
    else if (n.ghost || n.market_population() < p.merchant_min_pop * 0.5) why = "the town emptied";
    else if (k.owed > k.rate * p.contract_max_behind_days) why = "the house fell more than three months behind";
    if (why) {
        k.alive = false;
        ++contracts_cancelled_;
        log_.with({field("town", n.name), field("good", gd.id), field("house", k.house), field("delivered", k.delivered), field("owed", k.owed)});
        log_.add(world_.today, EventType::Trade, "contract_cancelled", k.town,
                 strf("%s's standing contract for %s with freight house #%d ends", n.name.c_str(), gd.name.c_str(), k.house), why);
        return;
    }
    if (world_.today >= k.ends) {
        k.alive = false;  // expired; a town that still remembers going short will sign again
        return;
    }

    // The quantity follows the town's need as it grows or shrinks. Each day's share falls due; a load goes out when
    // it adds up to a wagon, or to a month's need if that is less (a small post is not sent a train a year).
    const double demand = world_.daily_demand(k.town, k.good) * p.contract_share;
    if (demand > 0) k.rate = demand;
    k.owed += k.rate;
    const double load_units = p.trader_capacity_lb / gd.load_lb;
    const double lot = std::min(load_units, std::max(1.0, k.rate * 30.0));
    for (int sent = 0; sent < 4 && k.owed >= lot; ++sent)
        if (!dispatch_contract(k, lot)) break;
}

// One load for a contract, funded from the house's cash and driven by an outfit hired for the trip.
bool Simulation::dispatch_contract(SupplyContract& k, double units) {
    const Params& p = world_.params;
    House& h = houses_[static_cast<std::size_t>(k.house)];
    const GoodDef& gd = world_.goods[World::idx(k.good)];
    const double load_units = p.trader_capacity_lb / gd.load_lb;
    NodeId src = -1;
    double landed = 0;
    if (!cheapest_source(k.town, k.good, load_units, src, landed)) return false;

    Trader c;
    c.id = next_trader_id_++;
    c.at = src;
    c.agent_for = k.town;
    c.house = h.id;  // it shares the house's memory of dangerous roads and the owner's nerve
    c.contract = k.id;
    c.drover = gd.self_propelled;
    c.capacity_lb = p.trader_capacity_lb;
    c.upkeep = p.trader_upkeep;
    c.base_risk_aversion = h.base_risk_aversion;
    c.risk_aversion = h.risk_aversion;

    const double advance = units * landed * 1.15 + 30;  // a margin for price slippage and wages on the road
    if (h.cash < advance) return false;

    plan_paths(c, src, scratch_paths_);
    const PathInfo pi = scratch_paths_[World::idx(k.town)];
    if (!pi.ok) return false;
    // Guards as the road's reputation warrants, judged on what the load is worth to the town.
    const double value = units * k.price;
    const double d0 = defense(0);
    int guards = 0;
    double best_u = -1e18;
    for (int g = 0; g <= p.max_guards; g += std::max(1, p.guard_step)) {
        const double loss = std::min(0.95, pi.gang_risk * defense(g) / d0 + pi.hazard_risk);
        const double u = (1 - loss) * value - daily_cost(c, g) * pi.days;
        if (u > best_u) { best_u = u; guards = g; }
    }

    h.cash -= advance;
    c.cash = advance;
    c.start_capital = advance;
    const Receipt r = act_buy(c, k.good, units, advance - daily_cost(c, guards) * pi.days - world_.freight_unit_cost(k.good, pi.miles) * units);
    if (r.result != ActionResult::Ok || act_travel(c, k.town, guards) != ActionResult::Ok) {
        if (r.result == ActionResult::Ok) {
            // Bought but cannot go: put the goods back on the seller's books by dropping them (no route is the only cause).
            c.cargo = -1;
        }
        h.cash += c.cash;
        return false;
    }
    c.contract_units = r.units;
    k.owed -= r.units;
    imported_value_[World::idx(k.town)] += r.money;
    convoys_.push_back(std::move(c));
    return true;
}

// A contract load's journey is over: it reached the town, or was robbed on the way.
void Simulation::finish_contract_convoy(Trader& c) {
    SupplyContract& k = contracts_[static_cast<std::size_t>(c.contract)];
    House& h = houses_[static_cast<std::size_t>(k.house)];
    Node& town = world_.nodes[World::idx(k.town)];
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

    double paid = 0;
    if (c.cargo >= 0 && c.at == k.town) {
        const double units = c.qty;
        const double value = units * world_.mid_price(k.town, c.cargo);
        town.stock[World::idx(c.cargo)] += units;
        town.last_delivery[World::idx(c.cargo)] = world_.today;
        delivered_value_ += value;
        // The town pays on delivery, as far as its cash goes; what it cannot pay is owed and collected with the next load.
        const double due = units * k.price + k.arrears;
        paid = world_.draw_purse(k.town, std::min(due, world_.purse(k.town)));
        h.cash += paid;
        k.arrears = due - paid;
        k.delivered += units;
        contract_units_ += units;
        contract_revenue_ += paid;
        log_.with({field("town", town.name), field("good", good(c.cargo).id), field("units", units), field("house", k.house), field("paid", paid), field("landed_cost", c.unit_cost)});
        log_.add(world_.today, EventType::Trade, "contract_delivery", k.town,
                 strf("%.0f %s of %s reach %s under its standing contract", units, good(c.cargo).unit.c_str(), good(c.cargo).name.c_str(), town.name.c_str()),
                 strf("freight house #%d is paid %s ($%.2f a unit)", k.house, money(paid).c_str(), k.price), true);
    } else {
        // Robbed on the road: the house makes the load good with the next one, and the town notes the miss.
        k.owed += c.contract_units;
        ++k.missed;
    }
    // What is left of the house's advance goes back to the house; an outfit that ran into debt is written off.
    contract_outlay_ += c.start_capital - std::max(0.0, c.cash);
    if (c.cash >= 0) h.cash += c.cash;
    else world_.record(MoneyFlow::TraderExit, -c.cash);
    c.cash = 0;
    c.cargo = -1;
    c.qty = 0;
    c.alive = false;
}

}  // namespace frontier
