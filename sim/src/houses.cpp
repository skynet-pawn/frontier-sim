// Freight houses: the step from driving your own wagons to running other men's.
//
// A frontier teamster who struck it rich did not keep driving. He bought more wagons, hired drivers, and sent
// them where his own experience said the money was (Russell, Majors & Waddell ran thousands of wagons this way;
// eastern firms did the same through western agents). Here a trader who can afford a second wagon, and is not
// getting rich fast enough alone, founds a house (see `wants_house`). His own outfit
// stays on the road as the first of several, each working off a float drawn from the house's cash. A holdup then
// costs the house one load and one driver, which it can absorb, instead of ruining the only outfit it has. And
// since one load is a small part of his wealth, the owner weighs risk against payout far more coolly than a lone
// freelancer, who is dead or broke if it goes wrong.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

void Simulation::houses_day() {
    const Params& p = world_.params;

    // A trader who can afford a second wagon and driver, and for whom that is the quicker road to the pile he is after,
    // founds a house. (Checked every day, not only on idle days: the busiest rarely sit still.)
    for (Trader& t : traders_) {
        if (!t.alive || t.house >= 0 || t.controlled || t.agent_for >= 0) continue;
        if (t.cash >= p.house_found_cash && wants_house(t) && rng_.chance(p.house_found_chance)) found_house(t);
    }
    if (houses_.empty()) return;

    // The owner's memory fades, and his nerve returns, once a day however many outfits he runs.
    for (House& h : houses_) {
        if (!h.alive) continue;
        for (auto& f : h.fear) f.second *= 1.0 - p.fear_decay;
        h.fear.erase(std::remove_if(h.fear.begin(), h.fear.end(), [](const auto& f) { return f.second < 1e-4; }), h.fear.end());
        h.risk_aversion = std::max(h.base_risk_aversion, h.risk_aversion - p.risk_aversion_decay);
        h.outfits = 0;
        h.wealth = h.cash;
    }

    // Move working capital between the house and its outfits. Money passes by draft and telegraph, so distance
    // does not matter: a driver in a camp is funded the day he is idle.
    for (Trader& t : traders_) {
        if (!t.alive || t.house < 0) continue;
        House& h = houses_[static_cast<std::size_t>(t.house)];
        t.base_risk_aversion = h.base_risk_aversion;
        t.risk_aversion = h.risk_aversion;
        if (t.cash > 1.5 * p.house_float) {  // sale proceeds beyond his float go back to the house
            h.cash += t.cash - p.house_float;
            t.cash = p.house_float;
        } else if (!t.travelling && t.cargo < 0 && t.cash < 0.5 * p.house_float && h.cash > 0) {
            const double give = std::min(p.house_float - t.cash, h.cash);
            if (t.cash < 0) ++house_recaps_;  // he would have been ruined
            h.cash -= give;
            t.cash += give;
        }
        ++h.outfits;
        h.wealth += std::max(0.0, t.cash) + (t.cargo >= 0 ? t.qty * t.unit_cost : 0.0);
    }

    for (std::size_t i = 0; i < houses_.size(); ++i) {
        House& h = houses_[i];
        if (!h.alive) continue;

        // The owner of a house, like any trader, is tempted to take his pile and stop: it is dangerous work and a fortune is no use
        // to a dead man. When the house holds in cash what he set out to get, he retires; he takes most of it (invested in the
        // seat town as a settler's stake is) and the house carries on under managers, a little poorer, with his memory of the roads.
        if (!h.eastern && !h.retired && h.outfits > 0) {
            const double goal = goal_of(h.founder);
            if (h.cash >= goal && rng_.chance(std::min(0.05, p.house_retire_chance * h.cash / goal))) {
                const double take = h.cash * p.house_retire_share;
                Node& seat = world_.nodes[World::idx(h.seat)];
                const double invest = seat.fixed_market ? 0.0 : take * p.settle_invest_share;   // a retiree puts his stake into a frontier town, not the East
                seat.cash += invest;
                world_.record(MoneyFlow::TraderExit, -(take - invest));
                h.cash -= take;
                h.retired = true;
                log_.with({field("house", h.id), field("trader", h.founder), field("took", take), field("invested", invest), field("outfits", h.outfits)});
                log_.add(world_.today, EventType::Trader, "house_owner_retired", h.seat,
                         strf("The owner of freight house #%d retires at %s with %s", h.id, nname(h.seat).c_str(), money(take).c_str()),
                         strf("the house held %s in cash, more than the %s he set out to make; managers run its %d outfits", money(h.cash + take).c_str(), money(goal).c_str(), h.outfits));
            }
        }

        // An eastern firm that finds returns below its hurdle takes its capital back East, as a lone firm outfit would.
        if (h.eastern && return_ema_ < p.firm_hurdle && h.cash > 1.3 * h.start_capital &&
            rng_.chance(p.firm_withdraw_rate * (p.firm_hurdle - return_ema_) / p.firm_hurdle)) {
            const double out = h.cash - h.start_capital;
            world_.record(MoneyFlow::TraderExit, -out);
            h.cash -= out;
            log_.with({field("house", h.id), field("withdrawn", out), field("outfits", h.outfits)});
            log_.add(world_.today, EventType::Trader, "house_withdrew", h.seat,
                     strf("An eastern freight house withdraws %s of its capital from the frontier", money(out).c_str()),
                     strf("reported returns of %.0f%% a year fell short of its %.0f%% hurdle", return_ema_ * 36500, p.firm_hurdle * 36500));
        }

        // A house with no wagons left and too little to put one on the road is finished.
        if (h.outfits == 0 && h.cash < p.house_float) {
            world_.record(MoneyFlow::TraderExit, -h.cash);
            log_.with({field("house", h.id), field("cash", h.cash), field("founded", static_cast<long>(h.founded))});
            log_.add(world_.today, EventType::Trader, "house_closed", h.seat,
                     strf("Freight house #%d closes its doors with %s left", h.id, money(h.cash).c_str()),
                     "its last outfit was lost and the owner could not fund another");
            h.cash = 0;
            h.alive = false;
            continue;
        }

        // Growth: a new outfit whenever the house can fund a float and still hold one in reserve for a bad month.
        // (A house of one or two outfits is a man with a second wagon: it has no reserve to hold back.)
        const double needed = (h.outfits == 0 || h.outfits < p.house_small_outfits) ? p.house_float : 2 * p.house_float;
        if (h.outfits < p.house_max_outfits && world_.today - h.last_hire >= p.house_hire_days &&
            static_cast<int>(traders_.size()) < sc_.max_traders && h.cash >= needed)
            hire_outfit(h);
    }
}

void Simulation::found_house(Trader& t) {
    const Params& p = world_.params;
    House h;
    h.id = static_cast<int>(houses_.size());
    h.eastern = t.firm;
    h.founder = t.id;
    h.seat = t.at;
    h.cash = t.cash - p.house_float;
    h.start_capital = h.cash;
    h.founded = world_.today;
    h.base_risk_aversion = t.base_risk_aversion;
    h.risk_aversion = t.risk_aversion;
    h.fear = std::move(t.fear);  // what he learned the hard way is now the house's
    t.fear.clear();
    t.cash = p.house_float;
    t.house = h.id;
    log_.with({field("house", h.id), field("trader", t.id), field("node", nname(h.seat)), field("capital", h.cash + t.cash), field("eastern", h.eastern ? 1 : 0)});
    log_.add(world_.today, EventType::Trader, "house_founded", h.seat,
             strf("Trader #%d founds a freight house at %s with %s", t.id, nname(h.seat).c_str(), money(h.cash + t.cash).c_str()),
             strf("he held more than one outfit could put to work (%s); his own wagons stay on the road and he hires more",
                  money(t.cash + h.cash).c_str()));
    houses_.push_back(std::move(h));
    ++houses_founded_;
}

void Simulation::hire_outfit(House& h) {
    const Params& p = world_.params;
    Trader t;
    t.id = next_trader_id_++;
    t.at = h.seat;
    t.firm = h.eastern;
    t.house = h.id;
    t.drover = rng_.chance(h.eastern ? p.firm_drover_fraction : p.drover_fraction);
    t.cash = p.house_float;
    t.start_capital = p.house_float;
    t.worth = t.cash;
    t.capacity_lb = h.eastern ? p.firm_capacity_lb : p.trader_capacity_lb;
    t.upkeep = h.eastern ? p.firm_upkeep : p.trader_upkeep;
    t.base_risk_aversion = h.base_risk_aversion;
    t.risk_aversion = h.risk_aversion;
    h.cash -= p.house_float;
    h.last_hire = world_.today;
    ++h.outfits;
    log_.with({field("house", h.id), field("trader", t.id), field("node", nname(h.seat)), field("outfits", h.outfits)});
    log_.add(world_.today, EventType::Trader, "house_hired", h.seat,
             strf("Freight house #%d puts outfit #%d on the road from %s (%d outfits)", h.id, t.id, nname(h.seat).c_str(), h.outfits),
             strf("house cash %s covers another float of %s with a reserve to spare", money(h.cash + p.house_float).c_str(), money(p.house_float).c_str()),
             true);
    traders_.push_back(std::move(t));
}

// A broken outfit is made good from the house, if the house can. Returns false when it cannot.
bool Simulation::recapitalize(Trader& t) {
    const Params& p = world_.params;
    House& h = houses_[static_cast<std::size_t>(t.house)];
    const double give = std::min(p.house_float - t.cash, h.cash);
    if (give < -t.cash) return false;
    h.cash -= give;
    t.cash += give;
    ++house_recaps_;
    return true;
}

// How much a loss would hurt, as the trader weighs it. A house outfit's risk aversion counts only for the share of
// the owner's wealth riding on this load: the leader of a big house can take a gamble a lone freelancer cannot.
double Simulation::effective_risk(const Trader& t) const {
    if (t.house < 0 || t.risk_aversion <= 1.0) return t.risk_aversion;
    const House& h = houses_[static_cast<std::size_t>(t.house)];
    const double at_stake = std::max(0.0, t.cash) + (t.cargo >= 0 ? t.qty * t.unit_cost : 0.0);
    const double share = std::min(1.0, at_stake / std::max(h.wealth, 1.0));
    const double exposure = 1.0 - world_.params.house_nerve * (1.0 - share);
    return 1.0 + (t.risk_aversion - 1.0) * exposure;
}

std::vector<std::pair<EdgeId, double>>& Simulation::fear_book(Trader& t) {
    return t.house >= 0 ? houses_[static_cast<std::size_t>(t.house)].fear : t.fear;
}

// A robbery frightens the whole house, not just the driver: the owner's memory of the road and his nerve.
void Simulation::bump_fear(Trader& t, EdgeId eid, double amount, double risk_bump, double risk_cap) {
    auto& book = fear_book(t);
    bool found = false;
    for (auto& f : book)
        if (f.first == eid) { f.second += amount; found = true; }
    if (!found) book.push_back({eid, amount});
    t.risk_aversion = std::min(risk_cap, t.risk_aversion + risk_bump);
    if (t.house >= 0) houses_[static_cast<std::size_t>(t.house)].risk_aversion = t.risk_aversion;
}

}  // namespace frontier
