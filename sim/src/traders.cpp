// Trader behavior: utility-scored route and cargo choices.
//
// A trader at a node considers every (destination, good, guard-count) option and
// scores it as expected profit minus risk. Nothing is scripted: caravans appear
// where price gaps exceed freight plus risk, and vanish where they don't.
#include <algorithm>
#include <cmath>
#include <numeric>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

void Simulation::spawn_trader(NodeId at) {
    const Params& p = world_.params;
    Trader t;
    t.id = next_trader_id_++;
    t.at = at;
    t.drover = rng_.chance(p.drover_fraction);
    t.cash = rng_.uniform(sc_.capital_min, sc_.capital_max);
    t.base_risk_aversion = rng_.uniform(p.risk_aversion_min, p.risk_aversion_max);
    t.risk_aversion = t.base_risk_aversion;
    traders_.push_back(std::move(t));
}

void Simulation::traders_day() {
    std::vector<std::size_t> order(traders_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    rng_.shuffle(order);  // so no trader always gets first pick of a market
    for (std::size_t i : order) step_trader(traders_[i]);
}

double Simulation::daily_cost(int guards) const {
    return world_.params.trader_upkeep + guards * world_.params.guard_wage;
}

// Chance a gang wins an encounter, scaled so that "no guards" is the baseline.
double Simulation::defense(int guards) const {
    const Params& p = world_.params;
    const double g2 = p.assumed_gang_men * p.assumed_gang_men;
    const double d = p.trader_base_defenders + p.guard_strength * guards;
    return g2 / (g2 + d * d);
}

void Simulation::retire(Trader& t, const char* why) {
    t.alive = false;
    log_.add(world_.today, EventType::Trader, t.at, strf("Trader #%d leaves the trade (%s)", t.id, why),
             why, true);
}

void Simulation::step_trader(Trader& t) {
    const Params& p = world_.params;
    if (!t.alive) return;

    // Wages, feed and supplies are money spent in the economy: on the road it is spent at the next stop,
    // and an idle trader spends it where he waits.
    const double cost = daily_cost(t.travelling ? t.guards : 0);
    t.cash -= cost;
    profit_today_ -= cost;
    if (t.travelling) {
        t.pending_spend += cost;
    } else if (Node& here = world_.nodes[World::idx(t.at)]; !here.fixed_market) {
        here.cash += cost;
        here.inflow_today += cost;
    }
    if (t.cash < 0 && t.cargo < 0) {
        retire(t, "capital exhausted by upkeep without profitable trades");
        return;
    }

    // Memory fades; risk aversion relaxes back toward the trader's temperament.
    for (auto& f : t.fear) f.second *= 1.0 - p.fear_decay;
    t.fear.erase(std::remove_if(t.fear.begin(), t.fear.end(), [](const auto& f) { return f.second < 1e-4; }),
                 t.fear.end());
    t.risk_aversion = std::max(t.base_risk_aversion, t.risk_aversion - p.risk_aversion_decay);

    if (!t.travelling) {
        decide(t);
        return;
    }

    const EdgeId eid = t.route[t.leg];
    Edge& e = world_.edges[World::idx(eid)];
    e.transits_today += 1;
    if (t.cargo >= 0) {
        e.traffic_today += t.qty * t.unit_cost;

        if (rng_.chance(e.hazard)) {
            const double lost = t.qty * p.mishap_loss_fraction;
            const double value = lost * t.unit_cost;
            t.qty -= lost;
            log_.add(world_.today, EventType::Trader, t.at,
                     strf("Trader #%d loses %.0f %s on the %s %s", t.id, lost, good(t.cargo).unit.c_str(),
                          mode_name(e.mode), ename(eid).c_str()),
                     strf("%s hazard (%.2f%%/day): stampede, washout, or river crossing", mode_name(e.mode),
                          e.hazard * 100),
                     value < 1000);
        }
        for (int gid : edge_gangs_[World::idx(eid)]) {
            Gang* g = find_gang(gid);
            if (g && g->active && rng_.chance(p.gang_encounter_prob))
                if (resolve_encounter(*g, t, eid) || !t.alive) return;
        }
    }

    t.days_left -= 1;
    if (t.days_left > 0) return;
    t.at = e.other(t.at);
    ++t.leg;
    arrive(t);
}

void Simulation::arrive(Trader& t) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(t.at)];
    // Teamsters, guards and drovers spend money wherever they stop. Towns live off this traffic.
    const double spend = p.visit_spend + t.guards * p.guard_visit_spend +
                         (t.cargo >= 0 ? t.qty * t.unit_cost * p.visit_spend_value_rate : 0.0);
    t.cash -= spend;
    profit_today_ -= spend;
    if (!n.fixed_market) {
        n.cash += spend + t.pending_spend;
        n.inflow_today += spend + t.pending_spend;
    }
    t.pending_spend = 0;

    if (t.leg < t.route.size() && world_.edges[World::idx(t.route[t.leg])].active) {
        t.days_left = world_.edges[World::idx(t.route[t.leg])].travel_days;
        return;
    }
    t.travelling = false;
    t.route.clear();
    t.guards = 0;
    decide(t);
}

// ---------------------------------------------------------------- planning

double Simulation::edge_risk(const Trader& t, EdgeId eid) const {
    const Params& p = world_.params;
    const Edge& e = world_.edges[World::idx(eid)];
    // Rumor: robberies per trader-day, with a prior so unknown trails aren't assumed safe or deadly.
    double risk = (e.robbed_ema + p.risk_prior * p.risk_prior_weight) / (e.transits_ema + p.risk_prior_weight);
    for (const auto& f : t.fear)
        if (f.first == eid) risk += f.second;  // personal experience
    return std::min(risk, 0.5);
}

void Simulation::plan_paths(const Trader& t, NodeId from, std::vector<PathInfo>& out) {
    out.assign(world_.nodes.size(), PathInfo{});
    for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
        PathInfo& pi = out[d];
        if (static_cast<NodeId>(d) == from) {
            pi.ok = true;
            continue;
        }
        if (!world_.route(from, static_cast<NodeId>(d), scratch_route_)) continue;
        pi.ok = true;
        for (EdgeId eid : scratch_route_) {
            const Edge& e = world_.edges[World::idx(eid)];
            pi.days += e.travel_days;
            pi.miles[static_cast<int>(e.mode)] += e.miles;
            pi.gang_risk += edge_risk(t, eid) * e.travel_days;
            pi.hazard_risk += e.hazard * e.travel_days * world_.params.mishap_loss_fraction;
        }
    }
}

namespace {
// Fraction of cargo expected to be lost on a route, given guards.
double loss_fraction(const Simulation::PathInfo& pi, double defense_ratio) {
    return std::min(0.95, pi.gang_risk * defense_ratio + pi.hazard_risk);
}
}  // namespace

Simulation::Plan Simulation::plan_sell(const Trader& t, const std::vector<PathInfo>& paths, bool allow_here) {
    const Params& p = world_.params;
    Plan best;
    const double d0 = defense(0);
    for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
        const PathInfo& pi = paths[d];
        if (!pi.ok) continue;
        const bool here = static_cast<NodeId>(d) == t.at;
        if (here && !allow_here) continue;
        const double noise = 1.0 + p.info_noise * rng_.uniform(-1, 1);
        const double revenue = world_.quote_sell(static_cast<NodeId>(d), t.cargo, t.qty) * noise;
        if (here) {
            if (!best.valid || revenue > best.utility) best = {revenue, t.at, t.cargo, t.qty, 0, true};
            continue;
        }
        const double freight = world_.freight_unit_cost(t.cargo, pi.miles) * t.qty;
        for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
            const double loss = loss_fraction(pi, defense(k) / d0);
            const double util = (1 - loss) * revenue - freight - daily_cost(k) * pi.days -
                                (t.risk_aversion - 1) * loss * (t.unit_cost * t.qty + freight);
            if (!best.valid || util > best.utility) best = {util, static_cast<NodeId>(d), t.cargo, t.qty, k, true};
        }
    }
    return best;
}

Simulation::Plan Simulation::plan_buy(const Trader& t, const std::vector<PathInfo>& paths) {
    const Params& p = world_.params;
    const Node& hn = world_.nodes[World::idx(t.at)];
    const double d0 = defense(0);
    std::vector<double> noise(world_.nodes.size());
    for (double& v : noise) v = 1.0 + p.info_noise * rng_.uniform(-1, 1);

    Plan best;
    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        if (world_.goods[g].self_propelled != t.drover) continue;
        const GoodId gid = static_cast<GoodId>(g);
        const double avail = hn.fixed_market ? 1e12 : hn.stock[g] * p.avail_fraction;
        if (avail < 1) continue;
        const double load_units = p.trader_capacity_lb / world_.goods[g].load_lb;
        const double unit_price = world_.buy_price(t.at, gid);

        for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
            const PathInfo& pi = paths[d];
            if (!pi.ok || static_cast<NodeId>(d) == t.at) continue;
            const Node& dn = world_.nodes[d];
            const double freight_unit = world_.freight_unit_cost(gid, pi.miles);
            const double est = (dn.fixed_market ? world_.sell_price_at(static_cast<NodeId>(d), gid, 0)
                                                : world_.sell_price(static_cast<NodeId>(d), gid)) * noise[d];
            if (est <= unit_price + freight_unit) continue;  // no gap worth a trip

            for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
                const double reserve = daily_cost(k) * pi.days;
                const double free_cash = t.cash - reserve;
                if (free_cash <= 0) continue;
                double q = std::floor(std::min({load_units, avail, free_cash / (unit_price * 1.05 + freight_unit)}));
                if (q < 1) continue;
                const double cost = world_.quote_buy(t.at, gid, q);
                const double revenue = world_.quote_sell(static_cast<NodeId>(d), gid, q) * noise[d];
                const double freight = freight_unit * q;
                const double loss = loss_fraction(pi, defense(k) / d0);
                const double util = (1 - loss) * revenue - cost - freight - reserve -
                                    (t.risk_aversion - 1) * loss * (cost + freight);
                if (!best.valid || util > best.utility) best = {util, static_cast<NodeId>(d), gid, q, k, true};
            }
        }
    }
    return best;
}

// When nothing profitable can be bought here, head for where the best opportunity is.
bool Simulation::plan_deadhead(const Trader& t, NodeId& dest) {
    ensure_opportunities();
    const Params& p = world_.params;
    double best = p.min_profit;
    bool found = false;
    for (std::size_t n = 0; n < world_.nodes.size(); ++n) {
        const NodeId id = static_cast<NodeId>(n);
        if (id == t.at || !world_.reachable(t.at, id)) continue;
        const double score = opportunity_[t.drover][n] * p.deadhead_discount - daily_cost(0) * world_.route_days(t.at, id);
        if (score > best) {
            best = score;
            dest = id;
            found = true;
        }
    }
    return found;
}

// Best standard-load profit available at each node, once per day. Point prices only; cheap enough
// for tens of nodes. At scale this should consider only each node's nearest trade partners.
void Simulation::ensure_opportunities() {
    if (opportunity_day_ == world_.today) return;
    opportunity_day_ = world_.today;
    const Params& p = world_.params;
    const std::size_t N = world_.nodes.size();
    opportunity_[0].assign(N, 0.0);
    opportunity_[1].assign(N, 0.0);
    const double standard_cash = 3000;
    for (std::size_t n = 0; n < N; ++n) {
        const Node& hn = world_.nodes[n];
        for (std::size_t d = 0; d < N; ++d) {
            if (d == n || !world_.route(static_cast<NodeId>(n), static_cast<NodeId>(d), scratch_route_)) continue;
            double miles[kModeCount] = {0, 0, 0}, days = 0;
            for (EdgeId eid : scratch_route_) {
                const Edge& e = world_.edges[World::idx(eid)];
                miles[static_cast<int>(e.mode)] += e.miles;
                days += e.travel_days;
            }
            for (std::size_t g = 0; g < world_.goods.size(); ++g) {
                const GoodId gid = static_cast<GoodId>(g);
                const double avail = hn.fixed_market ? 1e12 : hn.stock[g] * p.avail_fraction;
                if (avail < 1) continue;
                const double price = world_.buy_price(static_cast<NodeId>(n), gid);
                const double q = std::floor(std::min({p.trader_capacity_lb / world_.goods[g].load_lb, avail, standard_cash / price}));
                if (q < 1) continue;
                const double profit = world_.quote_sell(static_cast<NodeId>(d), gid, q) -
                                      world_.quote_buy(static_cast<NodeId>(n), gid, q) -
                                      world_.freight_unit_cost(gid, miles) * q;
                double& slot = opportunity_[world_.goods[g].self_propelled ? 1 : 0][n];
                slot = std::max(slot, profit - daily_cost(0) * days);
            }
        }
    }
}

// ---------------------------------------------------------------- acting

void Simulation::depart(Trader& t, NodeId dest, int guards) {
    if (!world_.route(t.at, dest, t.route) || t.route.empty()) return;
    t.leg = 0;
    t.dest = dest;
    t.guards = guards;
    t.days_left = world_.edges[World::idx(t.route[0])].travel_days;
    t.travelling = true;
}

void Simulation::sell_cargo(Trader& t) {
    double revenue = 0;
    const double sold = world_.sell(t.at, t.cargo, t.qty, revenue);
    if (sold <= 0) return;
    const double profit = revenue - sold * t.unit_cost;
    t.cash += revenue;
    profit_today_ += profit;
    if (trace)
        log_.add(world_.today, EventType::Trade, t.at,
                 strf("Trader #%d sells %.0f %s at %s for %s (%s)", t.id, sold, good(t.cargo).unit.c_str(),
                      nname(t.at).c_str(), money(revenue).c_str(), profit >= 0 ? "profit" : "loss"),
                 strf("profit %s after freight", money(profit).c_str()), true);
    t.qty -= sold;
    if (t.qty < 1e-6) {
        t.cargo = -1;
        t.qty = 0;
    }
}

void Simulation::decide(Trader& t) {
    const Params& p = world_.params;
    plan_paths(t, t.at, scratch_paths_);

    if (t.cargo >= 0) {
        const Plan plan = plan_sell(t, scratch_paths_, true);
        if (plan.valid && plan.dest != t.at) {
            const double freight = world_.freight_unit_cost(t.cargo, scratch_paths_[World::idx(plan.dest)].miles) * t.qty;
            t.cash -= freight;
            t.unit_cost += freight / t.qty;
            depart(t, plan.dest, plan.guards);
            return;
        }
        sell_cargo(t);
        if (t.cargo >= 0) {  // the town couldn't pay for everything: carry the rest elsewhere
            const Plan rest = plan_sell(t, scratch_paths_, false);
            if (!rest.valid) return;
            const double freight = world_.freight_unit_cost(t.cargo, scratch_paths_[World::idx(rest.dest)].miles) * t.qty;
            t.cash -= freight;
            t.unit_cost += freight / t.qty;
            depart(t, rest.dest, rest.guards);
            return;
        }
    }

    const Plan buy = plan_buy(t, scratch_paths_);
    if (buy.valid && buy.utility >= p.min_profit) {
        const PathInfo& pi = scratch_paths_[World::idx(buy.dest)];
        const double freight_unit = world_.freight_unit_cost(buy.good, pi.miles);
        const double max_spend = t.cash - daily_cost(buy.guards) * pi.days - freight_unit * buy.qty;
        double spent = 0;
        const double units = max_spend > 0 ? world_.buy(t.at, buy.good, buy.qty, max_spend, spent) : 0.0;
        if (units > 0) {
            const double freight = freight_unit * units;
            t.cash -= spent + freight;
            t.cargo = buy.good;
            t.qty = units;
            t.unit_cost = (spent + freight) / units;
            if (trace)
                log_.add(world_.today, EventType::Trade, t.at,
                         strf("Trader #%d buys %.0f %s at %s for %s, bound for %s", t.id, units,
                              good(buy.good).unit.c_str(), nname(t.at).c_str(), money(spent).c_str(),
                              nname(buy.dest).c_str()),
                         strf("expected profit %s with %d guards", money(buy.utility).c_str(), buy.guards), true);
            depart(t, buy.dest, buy.guards);
            return;
        }
    }

    NodeId dest = -1;
    if (plan_deadhead(t, dest)) depart(t, dest, 0);
}

}  // namespace frontier
