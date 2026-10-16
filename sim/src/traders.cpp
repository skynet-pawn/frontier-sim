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

void Simulation::spawn_trader(NodeId at, bool firm) {
    const Params& p = world_.params;
    Trader t;
    t.id = next_trader_id_++;
    t.at = at;
    t.firm = firm;
    t.drover = rng_.chance(firm ? p.firm_drover_fraction : p.drover_fraction);
    t.cash = firm ? rng_.uniform(p.firm_capital_min, p.firm_capital_max) : rng_.uniform(sc_.capital_min, sc_.capital_max);
    t.start_capital = t.cash;
    t.capacity_lb = firm ? p.firm_capacity_lb : p.trader_capacity_lb;
    t.upkeep = firm ? p.firm_upkeep : p.trader_upkeep;
    t.base_risk_aversion = firm ? rng_.uniform(p.firm_risk_aversion_min, p.firm_risk_aversion_max)
                                : rng_.uniform(p.risk_aversion_min, p.risk_aversion_max);
    t.risk_aversion = t.base_risk_aversion;
    world_.record(MoneyFlow::NewTraderCapital, t.cash);
    traders_.push_back(std::move(t));
}

void Simulation::traders_day() {
    std::vector<std::size_t> order(traders_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    rng_.shuffle(order);  // so no trader always gets first pick of a market
    for (std::size_t i : order) step_trader(traders_[i]);
}

double Simulation::daily_cost(const Trader& t, int guards) const {
    return t.upkeep + guards * world_.params.guard_wage;
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

void Simulation::settle_trader(Trader& t) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(t.at)];
    const double invest = t.cash * p.settle_invest_share;  // a store, a saloon, a ranch: the stake stays in town
    n.cash += invest;
    world_.record(MoneyFlow::TraderExit, -(t.cash - invest + t.pending_spend));
    log_.with({field("trader", t.id), field("node", n.name), field("stake", t.cash), field("invested", invest)});
    log_.add(world_.today, EventType::Trader, "trader_settled", t.at,
             strf("Trader #%d settles in %s with a stake of %s", t.id, n.name.c_str(), money(t.cash).c_str()),
             strf("he made his pile and put %s into a business in town", money(invest).c_str()));
    t.alive = false;
    t.cash = 0;
    t.pending_spend = 0;
}

void Simulation::retire(Trader& t, const char* why, bool ruined) {
    if (ruined) {  // a failed freelancer is out of work and out of luck: raw material for a gang
        Node& n = world_.nodes[World::idx(t.at)];
        if (!n.fixed_market) n.drifters += t.firm ? 3.0 : 1.0;
    }
    t.alive = false;
    world_.record(MoneyFlow::TraderExit, -(t.cash + t.pending_spend));  // whatever he had (or owed) leaves with him
    t.cash = 0;
    t.pending_spend = 0;
    log_.with({field("trader", t.id), field("why", why)});
    log_.add(world_.today, EventType::Trader, "trader_exit", t.at, strf("Trader #%d leaves the trade (%s)", t.id, why),
             why, true);
}

void Simulation::step_trader(Trader& t) {
    const Params& p = world_.params;
    if (!t.alive) return;

    // Wages, feed and supplies are money spent in the economy: on the road it is spent at the next stop,
    // and an idle trader spends it where he waits.
    const double cost = daily_cost(t, t.travelling ? t.guards : 0);
    t.cash -= cost;
    profit_today_ -= cost;
    if (t.travelling) {
        t.pending_spend += cost;
    } else if (Node& here = world_.nodes[World::idx(t.at)]; !here.fixed_market) {
        here.cash += cost;
        here.inflow_today += cost;
        here.earned_visitors += cost;
    } else {
        world_.record(MoneyFlow::EastSpending, -cost);
    }
    if (t.cash < 0 && t.cargo < 0) {
        retire(t, "capital exhausted by upkeep without profitable trades", true);
        return;
    }

    // Memory fades; risk aversion relaxes back toward the trader's temperament.
    for (auto& f : t.fear) f.second *= 1.0 - p.fear_decay;
    t.fear.erase(std::remove_if(t.fear.begin(), t.fear.end(), [](const auto& f) { return f.second < 1e-4; }),
                 t.fear.end());
    t.risk_aversion = std::max(t.base_risk_aversion, t.risk_aversion - p.risk_aversion_decay);

    if (!t.travelling && !t.controlled && t.cargo < 0) {
        const Node& here = world_.nodes[World::idx(t.at)];
        // A freelancer who has made his pile may settle down.
        if (!t.firm && !here.fixed_market && t.cash > p.settle_cash && rng_.chance(p.settle_chance)) {
            settle_trader(t);
            return;
        }
        // An eastern firm whose returns have fallen short of its hurdle pulls its capital out.
        if (t.firm && t.cash > 1.3 * t.start_capital && return_ema_ < p.firm_hurdle &&
            rng_.chance(p.firm_withdraw_rate * (p.firm_hurdle - return_ema_) / p.firm_hurdle)) {
            log_.with({field("trader", t.id), field("capital", t.cash)});
            log_.add(world_.today, EventType::Trader, "firm_withdrew", t.at,
                     strf("An eastern firm withdraws its outfit #%d with %s", t.id, money(t.cash).c_str()),
                     strf("reported returns of %.0f%% a year fell short of its %.0f%% hurdle", return_ema_ * 36500, p.firm_hurdle * 36500),
                     true);
            world_.record(MoneyFlow::TraderExit, -(t.cash + t.pending_spend));
            t.cash = 0;
            t.pending_spend = 0;
            t.alive = false;
            return;
        }
    }
    if (!t.travelling) {
        if (!t.controlled) decide(t);  // a controlled outfit waits for its owner's orders
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
            log_.with({field("trader", t.id), field("edge", ename(eid)), field("lost_units", lost), field("value", value)});
            log_.add(world_.today, EventType::Trader, "trader_mishap", t.at,
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
        n.earned_visitors += spend + t.pending_spend;
    } else {
        world_.record(MoneyFlow::EastSpending, -(spend + t.pending_spend));
    }
    t.pending_spend = 0;

    const bool more_road = t.leg < t.route.size() && world_.edges[World::idx(t.route[t.leg])].active;
    // A teamster passing through a town with a good price for his load sells some of it there.
    const bool sold_out = more_road && t.cargo >= 0 && !t.controlled && sell_along_the_way(t);
    if (more_road && !sold_out) {
        t.days_left = world_.edges[World::idx(t.route[t.leg])].travel_days;
        return;
    }
    t.travelling = false;
    t.route.clear();
    t.guards = 0;
    if (!t.controlled) decide(t);
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
        const double revenue = certainty_adjusted(t, static_cast<NodeId>(d), t.cargo, t.qty,
                                                  world_.quote_sell(static_cast<NodeId>(d), t.cargo, t.qty) * noise);
        if (here) {
            if (!best.valid || revenue > best.utility) best = {revenue, t.at, t.cargo, t.qty, 0, true};
            continue;
        }
        const double freight = world_.freight_unit_cost(t.cargo, pi.miles) * t.qty;
        for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
            const double loss = loss_fraction(pi, defense(k) / d0);
            const double util = (1 - loss) * revenue - freight - daily_cost(t, k) * pi.days -
                                (t.risk_aversion - 1) * loss * (t.unit_cost * t.qty + freight);
            if (!best.valid || util > best.utility) best = {util, static_cast<NodeId>(d), t.cargo, t.qty, k, true};
        }
    }
    return best;
}

// A trader bound for a distant town passes through others on the way. Local merchants pay well for what they
// are short of, and a wagon that unloads part of its freight here saves the rest of the haul (teamsters bill by
// the mile carried). He sells whatever share beats carrying it on, judged with the same risk and certainty
// discount he used to pick the destination. This is how a supply line feeds the towns strung along it.
bool Simulation::sell_along_the_way(Trader& t) {
    const Params& p = world_.params;
    double miles[kModeCount] = {0, 0, 0}, days = 0, gang_risk = 0, hazard_risk = 0;
    for (std::size_t i = t.leg; i < t.route.size(); ++i) {
        const Edge& e = world_.edges[World::idx(t.route[i])];
        miles[static_cast<int>(e.mode)] += e.miles;
        days += e.travel_days;
        gang_risk += edge_risk(t, t.route[i]) * e.travel_days;
        hazard_risk += e.hazard * e.travel_days * p.mishap_loss_fraction;
    }
    PathInfo rest;
    rest.gang_risk = gang_risk;
    rest.hazard_risk = hazard_risk;
    const double loss = loss_fraction(rest, defense(t.guards) / defense(0));
    const double freight_unit = world_.freight_unit_cost(t.cargo, miles);
    const GoodId cargo = t.cargo;
    const double noise = 1.0 + p.info_noise * rng_.uniform(-1, 1);

    // Value of selling a share here and carrying the remainder on to the destination.
    auto value = [&](double share) {
        const double q_here = t.qty * share, q_on = t.qty - q_here;
        double v = certainty_adjusted(t, t.at, t.cargo, q_here, world_.quote_sell(t.at, t.cargo, q_here)) + freight_unit * q_here;
        if (q_on > 0) {
            const double on = certainty_adjusted(t, t.dest, t.cargo, q_on, world_.quote_sell(t.dest, t.cargo, q_on) * noise);
            v += (1 - loss) * on - (t.risk_aversion - 1) * loss * t.unit_cost * q_on;
        }
        return v;
    };
    double best_share = 0, best = value(0);
    for (double share : {0.25, 0.5, 0.75, 1.0}) {
        const double v = value(share);
        if (v > best + p.min_profit) { best = v; best_share = share; }
    }
    if (best_share <= 0) return false;

    // The unused part of the freight comes back to him: those units now travel no further.
    const double before_qty = t.qty, before_cost = t.unit_cost;
    t.unit_cost -= freight_unit;
    const Receipt r = sell_cargo(t, t.qty * best_share);
    if (r.units <= 0) { t.unit_cost = before_cost; return false; }
    const double refund = freight_unit * r.units;
    t.cash += refund;
    profit_today_ += refund;
    world_.record(MoneyFlow::Freight, refund);
    if (t.cargo >= 0) t.unit_cost = before_cost;
    log_.with({field("trader", t.id), field("node", nname(t.at)), field("good", good(cargo).id), field("units", r.units), field("share", r.units / before_qty)});
    log_.add(world_.today, EventType::Trade, "trade_waypoint", t.at,
             strf("Trader #%d, bound for %s, sells %.0f %s at %s for %s", t.id, nname(t.dest).c_str(), r.units,
                  good(cargo).unit.c_str(), nname(t.at).c_str(), money(r.money).c_str()),
             "local price beat what the rest of the road would earn after freight and risk", r.money < 1500);
    return t.cargo < 0;
}

// How uncertain the price is that a seller will actually get. A federal contract and the eastern markets are
// certain; a small town's spot market is not, and the thinner it is relative to the load, the less so.
double Simulation::spot_uncertainty(NodeId d, GoodId g, double q) const {
    const Params& p = world_.params;
    if (world_.nodes[World::idx(d)].fixed_market) return 0.03;
    return p.spot_uncertainty_base + p.spot_uncertainty_thin * std::min(1.0, q / world_.normal_stock(d, g));
}

// A risk-averse outfit marks down uncertain revenue; a gambler marks it up slightly. Contract revenue is certain.
double Simulation::certainty_adjusted(const Trader& t, NodeId d, GoodId g, double q, double revenue) const {
    const double contract = std::min(revenue, world_.quote_sell_contract(d, g, q));
    const double spot = revenue - contract;
    return revenue - (t.risk_aversion - 1.0) * world_.params.price_uncertainty_weight * spot_uncertainty(d, g, q) * spot;
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
        const double load_units = t.capacity_lb / world_.goods[g].load_lb;
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
                const double reserve = daily_cost(t, k) * pi.days;
                const double free_cash = t.cash - reserve;
                if (free_cash <= 0) continue;
                double q = std::floor(std::min({load_units, avail, free_cash / (unit_price * 1.05 + freight_unit)}));
                if (q < 1) continue;
                const double cost = world_.quote_buy(t.at, gid, q);
                const double revenue = certainty_adjusted(t, static_cast<NodeId>(d), gid, q,
                                                          world_.quote_sell(static_cast<NodeId>(d), gid, q) * noise[d]);
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

// ---------------------------------------------------------------- actions
//
// These are the only ways a trader changes the world. The AI planner above calls them, and so can anything
// else (a player, a script, a test): the same costs, the same risks and the same log entries apply, so a
// clever player finds chinks in the model itself rather than in a special-cased interface.

const char* action_result_name(ActionResult r) {
    switch (r) {
        case ActionResult::Ok:              return "ok";
        case ActionResult::Busy:            return "busy (already travelling)";
        case ActionResult::WrongPlace:      return "not possible here";
        case ActionResult::HasCargo:        return "already carrying cargo";
        case ActionResult::NoCargo:         return "no cargo";
        case ActionResult::WrongKind:       return "this kind of outfit cannot move that good";
        case ActionResult::NothingForSale:  return "nothing for sale";
        case ActionResult::NoMoney:         return "no money";
        case ActionResult::NoRoute:         return "no route";
    }
    return "?";
}

ActionResult Simulation::act_travel(Trader& t, NodeId dest, int guards) {
    if (t.travelling) return ActionResult::Busy;
    if (dest < 0 || dest >= static_cast<NodeId>(world_.nodes.size()) || dest == t.at) return ActionResult::WrongPlace;
    if (!world_.route(t.at, dest, scratch_route_) || scratch_route_.empty()) return ActionResult::NoRoute;
    if (t.cargo >= 0) {
        // Freight is paid on departure. Teamsters bill on delivery, so a trader may go into debt for it.
        double miles[kModeCount] = {0, 0, 0};
        for (EdgeId eid : scratch_route_) {
            const Edge& e = world_.edges[World::idx(eid)];
            miles[static_cast<int>(e.mode)] += e.miles;
        }
        const double freight = world_.freight_unit_cost(t.cargo, miles) * t.qty;
        t.cash -= freight;
        world_.record(MoneyFlow::Freight, -freight);
        t.unit_cost += freight / t.qty;
    }
    t.route = scratch_route_;
    t.leg = 0;
    t.dest = dest;
    t.guards = std::max(0, guards);
    t.days_left = world_.edges[World::idx(t.route[0])].travel_days;
    t.travelling = true;
    return ActionResult::Ok;
}

Receipt Simulation::act_buy(Trader& t, GoodId g, double qty, double max_spend) {
    Receipt r;
    if (t.travelling) { r.result = ActionResult::Busy; return r; }
    if (t.cargo >= 0) { r.result = ActionResult::HasCargo; return r; }
    if (g < 0 || g >= static_cast<GoodId>(world_.goods.size()) || good(g).self_propelled != t.drover) {
        r.result = ActionResult::WrongKind;  // drovers move cattle; freighters move everything else
        return r;
    }
    if (max_spend < 0) max_spend = t.cash;
    max_spend = std::min(max_spend, t.cash);
    if (max_spend <= 0) { r.result = ActionResult::NoMoney; return r; }
    qty = std::min(qty, t.capacity_lb / good(g).load_lb);  // what the outfit can carry
    double spent = 0;
    const double units = qty > 0 ? world_.buy(t.at, g, qty, max_spend, spent) : 0.0;
    if (units <= 0) { r.result = ActionResult::NothingForSale; return r; }
    t.cash -= spent;
    bought_value_[t.firm ? 1 : 0][World::idx(g)] += spent;
    if (g == world_.flour_good) flour_bought_at_[World::idx(t.at)] += spent;
    t.cargo = g;
    t.qty = units;
    t.unit_cost = spent / units;
    r.units = units;
    r.money = spent;
    if (trace)
        log_.add(world_.today, EventType::Trade, "trade_bought", t.at,
                 strf("Trader #%d buys %.0f %s at %s for %s", t.id, units, good(g).unit.c_str(), nname(t.at).c_str(),
                      money(spent).c_str()),
                 "", true);
    return r;
}

Receipt Simulation::act_sell(Trader& t, double qty) {
    if (t.travelling) { Receipt r; r.result = ActionResult::Busy; return r; }
    return sell_cargo(t, qty);
}

Receipt Simulation::sell_cargo(Trader& t, double qty) {
    Receipt r;
    if (t.cargo < 0) { r.result = ActionResult::NoCargo; return r; }
    if (qty < 0 || qty > t.qty) qty = t.qty;
    double revenue = 0;
    const double sold = world_.sell(t.at, t.cargo, qty, revenue);
    if (sold <= 0) { r.result = ActionResult::NoMoney; return r; }  // the town has no cash to pay with
    const double profit = revenue - sold * t.unit_cost;
    t.cash += revenue;
    profit_today_ += profit;
    if (profit > world_.params.story_profit_threshold) ++stories_today_;  // a trip worth telling about
    r.units = sold;
    r.money = revenue;
    if (trace)
        log_.add(world_.today, EventType::Trade, "trade_sold", t.at,
                 strf("Trader #%d sells %.0f %s at %s for %s (%s)", t.id, sold, good(t.cargo).unit.c_str(),
                      nname(t.at).c_str(), money(revenue).c_str(), profit >= 0 ? "profit" : "loss"),
                 strf("profit %s after freight", money(profit).c_str()), true);
    t.qty -= sold;
    if (t.qty < 1e-6) {
        t.cargo = -1;
        t.qty = 0;
    }
    return r;
}

int Simulation::add_trader(NodeId at, double cash, bool drover, bool controlled) {
    Trader t;
    t.id = next_trader_id_++;
    t.at = at;
    t.cash = cash;
    t.drover = drover;
    t.controlled = controlled;
    t.base_risk_aversion = t.risk_aversion = 1.0;
    world_.record(MoneyFlow::NewTraderCapital, cash);
    traders_.push_back(std::move(t));
    return traders_.back().id;
}

Trader* Simulation::find_trader(int id) {
    for (Trader& t : traders_)
        if (t.id == id && t.alive) return &t;
    return nullptr;
}

ActionResult Simulation::act_gang_relocate(Gang& g, EdgeId e) {
    if (!g.active || g.ruling >= 0) return ActionResult::WrongPlace;  // a ruling gang stays with its town
    if (e < 0 || e >= static_cast<EdgeId>(world_.edges.size())) return ActionResult::NoRoute;
    const Edge& ed = world_.edges[World::idx(e)];
    if (!ed.active || ed.mode != Mode::Trail) return ActionResult::NoRoute;
    const NodeId home = nearest_node(e, g.hideout);
    if (home < 0) return ActionResult::NoRoute;
    if (home != g.hideout) g.influence = 0;  // influence is local
    g.edge = e;
    g.hideout = home;
    g.last_review = world_.today;
    return ActionResult::Ok;
}

// ---------------------------------------------------------------- the AI's turn

void Simulation::decide(Trader& t) {
    const Params& p = world_.params;
    plan_paths(t, t.at, scratch_paths_);

    if (t.cargo >= 0) {
        const Plan plan = plan_sell(t, scratch_paths_, true);
        if (plan.valid && plan.dest != t.at) {
            act_travel(t, plan.dest, plan.guards);
            return;
        }
        act_sell(t);
        if (t.cargo >= 0) {  // the town couldn't pay for everything: carry the rest elsewhere
            const Plan rest = plan_sell(t, scratch_paths_, false);
            if (rest.valid) act_travel(t, rest.dest, rest.guards);
            return;
        }
    }

    const Plan buy = plan_buy(t, scratch_paths_);
    if (buy.valid && buy.utility >= p.min_profit) {
        const PathInfo& pi = scratch_paths_[World::idx(buy.dest)];
        const double freight_unit = world_.freight_unit_cost(buy.good, pi.miles);
        // Keep back enough to pay the freight and the trip's running costs.
        const double max_spend = t.cash - daily_cost(t, buy.guards) * pi.days - freight_unit * buy.qty;
        if (max_spend > 0 && act_buy(t, buy.good, buy.qty, max_spend).result == ActionResult::Ok) {
            act_travel(t, buy.dest, buy.guards);
            return;
        }
    }

    NodeId dest = -1;
    if (plan_deadhead(t, dest)) act_travel(t, dest, 0);
}

}  // namespace frontier
