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

void Simulation::refresh_inbound() {
    const std::size_t goods = world_.goods.size();
    inbound_.assign(world_.nodes.size() * goods, 0.0);
    for (const auto* fleet : {&traders_, &convoys_})
        for (const Trader& t : *fleet)
            if (t.alive && t.travelling && t.cargo >= 0 && t.qty > 0 && t.dest >= 0)
                inbound_[World::idx(t.dest) * goods + World::idx(t.cargo)] += t.qty;
}

double Simulation::known_inbound(const Trader& t, NodeId d, GoodId g) const {
    const std::size_t goods = world_.goods.size();
    const std::size_t i = World::idx(d) * goods + World::idx(g);
    if (i >= inbound_.size()) return 0.0;
    double v = inbound_[i];
    if (t.travelling && t.dest == d && t.cargo == g) v -= t.qty;  // his own load is not a rival
    return std::max(0.0, v) * world_.params.inbound_awareness;
}

double Simulation::expected_drain(NodeId d, GoodId g, double days) const {
    if (world_.nodes[World::idx(d)].fixed_market) return 0.0;
    return world_.daily_demand(d, g) * days * world_.params.inbound_awareness;
}

void Simulation::traders_day() {
    refresh_inbound();
    std::vector<std::size_t> order(traders_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    rng_.shuffle(order);  // so no trader always gets first pick of a market
    for (std::size_t i : order) step_trader(traders_[i]);
    // Town-hired convoys ride the same roads and meet the same gangs. Index loop: none are added while they move.
    for (std::size_t i = 0; i < convoys_.size(); ++i) {
        Trader& c = convoys_[i];
        if (!c.alive) continue;
        step_trader(c);
        if (c.alive && !c.travelling) finish_convoy(c);  // robbed on the road: the outfit disbands
    }
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
    if (t.house >= 0) ++house_exits_[ruined ? 1 : 0];
    if (ruined) {  // a failed freelancer is out of work and out of luck: raw material for a gang
        Node& n = world_.nodes[World::idx(t.at)];
        if (!n.fixed_market) { n.drifters += t.firm ? 3.0 : 1.0; note_drifters(DrifterFlow::FailedTraders, t.firm ? 3.0 : 1.0); }
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
    if (t.job_town >= 0) settle_consignment(t);  // before the broke check: a robbed carrier owes nothing for the goods

    // Wages, feed and supplies are money spent in the economy: on the road it is spent at the next stop,
    // and an idle trader spends it where he waits.
    const double cost = daily_cost(t, t.travelling ? t.guards : 0);
    t.cash -= cost;
    profit_today_ -= cost;
    t.recent_profit *= 1.0 - p.story_fade;
    if (t.travelling) {
        t.pending_spend += cost;
    } else if (Node& here = world_.nodes[World::idx(t.at)]; !here.fixed_market) {
        here.cash += cost;
        here.inflow_today += cost;
        here.earned_visitors += cost;
    } else {
        world_.record(MoneyFlow::EastSpending, -cost);
    }
    if (t.cash < 0 && t.cargo < 0 && !(t.house >= 0 && recapitalize(t))) {  // a house makes good its broken outfits
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
        if (!t.firm && t.house < 0 && !here.fixed_market && t.agent_for < 0 && t.cash > p.settle_cash && rng_.chance(p.settle_chance)) {
            settle_trader(t);
            return;
        }
        // An eastern firm whose returns have fallen short of its hurdle pulls its capital out.
        if (t.firm && t.house < 0 && t.cash > 1.3 * t.start_capital && return_ema_ < p.firm_hurdle &&
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
        const GangTake visible = gang_take(p.assumed_gang_men, false, t);  // what an ordinary gang would see in the wagons
        if (visible.worth) e.loot_today += visible.loot;

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
            if (g && g->active && rng_.chance(gang_contact_prob(*g)))
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
    // (A town's convoy is carrying an order and does not peddle it on the way.)
    const bool sold_out = more_road && t.cargo >= 0 && !t.controlled && t.agent_for < 0 && t.job_town < 0 && sell_along_the_way(t);
    if (more_road && !sold_out) {
        t.days_left = world_.edges[World::idx(t.route[t.leg])].travel_days;
        return;
    }
    t.travelling = false;
    t.route.clear();
    t.guards = 0;
    if (t.agent_for >= 0) { finish_convoy(t); return; }
    if (!t.controlled) decide(t);
}

// ---------------------------------------------------------------- planning

double Simulation::edge_risk(const Trader& t, EdgeId eid) const {
    const Params& p = world_.params;
    const Edge& e = world_.edges[World::idx(eid)];
    // Rumor: robberies per trader-day, with a prior so unknown trails aren't assumed safe or deadly.
    double risk = (e.robbed_ema + p.risk_prior * p.risk_prior_weight) / (e.transits_ema + p.risk_prior_weight);
    for (const auto& f : (t.house >= 0 ? houses_[static_cast<std::size_t>(t.house)].fear : t.fear))
        if (f.first == eid) risk += f.second;  // personal experience (a house outfit has its owner's)
    return std::min(risk, 0.5);
}

// The towns a trader at `n` will consider: the nearest `trade_horizon` by travel time, plus every reachable eastern market.
const std::vector<NodeId>& Simulation::horizon(NodeId n) {
    const std::size_t N = world_.nodes.size();
    if (horizon_version_ != world_.route_version || horizon_.size() != N) {
        horizon_version_ = world_.route_version;
        horizon_.assign(N, {});
        horizon_path_.assign(N, {});
        const std::size_t keep = static_cast<std::size_t>(std::max(1, world_.params.trade_horizon));
        for (std::size_t s = 0; s < N; ++s) {
            std::vector<NodeId> reach;
            for (std::size_t d = 0; d < N; ++d)
                if (d != s && world_.reachable(static_cast<NodeId>(s), static_cast<NodeId>(d))) reach.push_back(static_cast<NodeId>(d));
            if (reach.size() > keep) {
                auto nearer = [&](NodeId a, NodeId b) {
                    const double da = world_.route_days(static_cast<NodeId>(s), a), db = world_.route_days(static_cast<NodeId>(s), b);
                    return da != db ? da < db : a < b;
                };
                std::sort(reach.begin(), reach.end(), nearer);
                std::vector<NodeId> far(reach.begin() + static_cast<std::ptrdiff_t>(keep), reach.end());
                reach.resize(keep);
                for (NodeId d : far)
                    if (world_.nodes[World::idx(d)].fixed_market) reach.push_back(d);
                std::sort(reach.begin(), reach.end());
            }
            horizon_path_[s].resize(reach.size());
            for (std::size_t i = 0; i < reach.size(); ++i) {
                world_.route(static_cast<NodeId>(s), reach[i], scratch_route_);
                for (EdgeId eid : scratch_route_) {
                    const Edge& e = world_.edges[World::idx(eid)];
                    horizon_path_[s][i].miles[static_cast<int>(e.mode)] += e.miles;
                    horizon_path_[s][i].days += e.travel_days;
                }
            }
            horizon_[s] = std::move(reach);
        }
    }
    return horizon_[World::idx(n)];
}

void Simulation::plan_paths(const Trader& t, NodeId from, std::vector<PathInfo>& out) {
    out.assign(world_.nodes.size(), PathInfo{});
    out[World::idx(from)].ok = true;
    for (NodeId dn : horizon(from)) {
        const std::size_t d = World::idx(dn);
        PathInfo& pi = out[d];
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
    ensure_opportunities();
    // A teamster does not judge a trip alone: where it ends he can load for the way back (gold out of a camp,
    // cattle out of Texas). Part of the best load waiting there counts toward the trip.
    auto look = [&](std::size_t d) { return p.lookahead_weight * opportunity_[t.drover ? 1 : 0][d]; };
    Plan best;
    const double d0 = defense(0);
    for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
        const PathInfo& pi = paths[d];
        if (!pi.ok) continue;
        const bool here = static_cast<NodeId>(d) == t.at;
        if (here && !allow_here) continue;
        const double noise = 1.0 + p.info_noise * rng_.uniform(-1, 1);
        const double revenue = certainty_adjusted(t, static_cast<NodeId>(d), t.cargo, t.qty,
                                                  world_.quote_sell(static_cast<NodeId>(d), t.cargo, t.qty, known_inbound(t, static_cast<NodeId>(d), t.cargo),
                                                                    here ? 0.0 : expected_drain(static_cast<NodeId>(d), t.cargo, pi.days)) * noise);
        if (here) {
            if (!best.valid || revenue + look(d) > best.utility) best = {revenue + look(d), t.at, t.cargo, t.qty, 0, true};
            continue;
        }
        const double freight = world_.freight_unit_cost(t.cargo, pi.miles) * t.qty;
        for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
            const double loss = loss_fraction(pi, defense(k) / d0);
            const double util = (1 - loss) * revenue - freight - daily_cost(t, k) * pi.days -
                                (effective_risk(t) - 1) * loss * (t.unit_cost * t.qty + freight) + look(d);
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
    if (world_.nodes[World::idx(t.at)].fixed_market) return false;  // dumping a load back on the East is turning round, not a sale
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
    // He set out for the load waiting at the far end as much as for the sale, so that counts here too: a trip
    // judged with the backhaul at departure must not be dropped at the first town for lack of it.
    ensure_opportunities();
    const double look_dest = p.lookahead_weight * opportunity_[t.drover ? 1 : 0][World::idx(t.dest)];
    const double look_here = p.lookahead_weight * opportunity_[t.drover ? 1 : 0][World::idx(t.at)];
    const double noise = 1.0 + p.info_noise * rng_.uniform(-1, 1);

    // Value of selling a share here and carrying the remainder on to the destination.
    auto value = [&](double share) {
        const double q_here = t.qty * share, q_on = t.qty - q_here;
        double v = certainty_adjusted(t, t.at, t.cargo, q_here, world_.quote_sell(t.at, t.cargo, q_here, known_inbound(t, t.at, t.cargo))) + freight_unit * q_here;
        if (q_on > 0) {
            const double on = certainty_adjusted(t, t.dest, t.cargo, q_on, world_.quote_sell(t.dest, t.cargo, q_on, known_inbound(t, t.dest, t.cargo), expected_drain(t.dest, t.cargo, world_.route_days(t.at, t.dest))) * noise);
            v += (1 - loss) * on - (effective_risk(t) - 1) * loss * t.unit_cost * q_on + (1 - loss) * look_dest;
        } else {
            v += look_here;  // unloaded here, he is free to take the best load going out of this town
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
    const double contract = std::min(revenue, world_.quote_sell_contract(d, g, q, known_inbound(t, d, g)));
    const double spot = revenue - contract;
    return revenue - (effective_risk(t) - 1.0) * world_.params.price_uncertainty_weight * spot_uncertainty(d, g, q) * spot;
}

Simulation::Plan Simulation::plan_buy(const Trader& t, const std::vector<PathInfo>& paths) {
    const Params& p = world_.params;
    ensure_opportunities();
    auto look = [&](std::size_t d) { return p.lookahead_weight * opportunity_[t.drover ? 1 : 0][d]; };
    const Node& hn = world_.nodes[World::idx(t.at)];
    const double d0 = defense(0);
    std::vector<double> noise(world_.nodes.size());
    for (double& v : noise) v = 1.0 + p.info_noise * rng_.uniform(-1, 1);

    Plan best;
    for (std::size_t g = 0; g < world_.goods.size(); ++g) {
        if (world_.goods[g].self_propelled != t.drover) continue;
        const GoodId gid = static_cast<GoodId>(g);
        const double avail = hn.fixed_market ? 1e12 : tradable_stock(t.at, gid);
        if (avail < 1) continue;
        const double load_units = t.capacity_lb / world_.goods[g].load_lb;
        const double unit_price = world_.buy_price(t.at, gid);

        for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
            const PathInfo& pi = paths[d];
            if (!pi.ok || static_cast<NodeId>(d) == t.at) continue;
            const Node& dn = world_.nodes[d];
            const double freight_unit = world_.freight_unit_cost(gid, pi.miles);
            // Wagons already on the road to the town are counted: they fill its order first and then crowd its shelf, so a
            // camp with four months of whiskey on the way is no longer the best market for more whiskey.
            const double inb = dn.fixed_market ? 0.0 : known_inbound(t, static_cast<NodeId>(d), gid);
            const double drain = expected_drain(static_cast<NodeId>(d), gid, pi.days);
            const double est = (dn.fixed_market ? world_.sell_price_at(static_cast<NodeId>(d), gid, 0)
                                                : world_.sell_price(static_cast<NodeId>(d), gid, inb - drain)) * noise[d];
            // A town's open order pays its offered price whatever the shelf says, so it counts as the going price here.
            const double order_open = std::max(0.0, dn.order_qty[g] - inb);
            const double order_here_price = !dn.fixed_market && order_open >= 1 ? dn.order_price[g] : 0.0;
            if (std::max(est, order_here_price) <= unit_price + freight_unit && look(d) <= 0) continue;  // no gap worth a trip, and nothing to load for the way back

            // A town that eats a few head a day cannot take a thousand: the price collapses once its shelves fill.
            // So he prices several lot sizes, including one that matches the town's open order, and ships the one
            // that pays best. This is how a small camp gets served from a big herd.
            const double cash_for_lot = t.cash - daily_cost(t, 0) * pi.days;
            if (cash_for_lot <= 0) continue;
            const double q_full = std::floor(std::min({load_units, avail, cash_for_lot / (unit_price * 1.05 + freight_unit)}));
            if (q_full < 1) continue;
            double lots[6];
            int n_lots = 0;
            auto add_lot = [&](double q) {
                q = std::floor(q);
                if (q < 1) return;
                for (int i = 0; i < n_lots; ++i)
                    if (lots[i] == q) return;
                lots[n_lots++] = q;
            };
            for (double share : {1.0, 0.5, 0.25, 0.1}) add_lot(q_full * share);
            if (!dn.fixed_market && order_open >= 1) add_lot(std::min(q_full, order_open));
            for (int li = 0; li < n_lots; ++li) {
                const double q = lots[li];
                const double cost = world_.quote_buy(t.at, gid, q);
                const double revenue = certainty_adjusted(t, static_cast<NodeId>(d), gid, q,
                                                          world_.quote_sell(static_cast<NodeId>(d), gid, q, inb, drain) * noise[d]);
                const double freight = freight_unit * q;
                for (int k = 0; k <= p.max_guards; k += std::max(1, p.guard_step)) {
                    const double reserve = daily_cost(t, k) * pi.days;
                    if (cost + freight + reserve > t.cash) continue;  // cannot afford this lot with this many guards
                    const double loss = loss_fraction(pi, defense(k) / d0);
                    const double util = (1 - loss) * revenue - cost - freight - reserve -
                                        (effective_risk(t) - 1) * loss * (cost + freight) + look(d);
                    if (!best.valid || util > best.utility) best = {util, static_cast<NodeId>(d), gid, q, k, true};
                }
            }
        }
    }
    return best;
}

// What a trader may try to buy from a town's shelves. Storekeepers do not sell the last of what their own
// customers will eat next fortnight, however high the offer from a richer camp; a poor fort that is outbid
// would otherwise starve to feed a boom town (and its own garrison would be the first to notice).
double Simulation::lead_days(NodeId n) const {
    double lead = 1e9;
    for (NodeId m : markets_)
        if (world_.reachable(m, n)) lead = std::min(lead, world_.route_days(m, n));
    return lead > 1e8 ? 0.0 : lead;
}

double Simulation::tradable_stock(NodeId n, GoodId g) const {
    const double stock = world_.nodes[World::idx(n)].stock[World::idx(g)];
    double days = world_.params.local_reserve_days;
    // A storekeeper far from supply cannot refill in a fortnight: what he sells away now is gone for the whole haul.
    if (world_.params.lead_stages > 0 && world_.goods[World::idx(g)].food_share > 0) days = std::max(days, lead_estimate(n).lead);
    const double reserve = world_.daily_demand(n, g) * days;
    return std::max(0.0, stock - reserve) * world_.params.avail_fraction;
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
    struct Load { double q, buy_cost; int kind; };
    std::vector<Load> loads;
    for (std::size_t n = 0; n < N; ++n) {
        const Node& hn = world_.nodes[n];
        horizon(static_cast<NodeId>(n));  // (refreshes the cache if the roads changed)
        // What he would load here does not depend on where he is bound, so it is priced once per good.
        loads.clear();
        std::vector<GoodId> goods_here;
        for (std::size_t g = 0; g < world_.goods.size(); ++g) {
            const GoodId gid = static_cast<GoodId>(g);
            const double avail = hn.fixed_market ? 1e12 : tradable_stock(static_cast<NodeId>(n), gid);
            if (avail < 1) continue;
            const double price = world_.buy_price(static_cast<NodeId>(n), gid);
            const double q = std::floor(std::min({p.trader_capacity_lb / world_.goods[g].load_lb, avail, standard_cash / price}));
            if (q < 1) continue;
            loads.push_back({q, world_.quote_buy(static_cast<NodeId>(n), gid, q), world_.goods[g].self_propelled ? 1 : 0});
            goods_here.push_back(gid);
        }
        const std::vector<NodeId>& dests = horizon_[n];
        for (std::size_t i = 0; i < dests.size(); ++i) {
            const HorizonPath& hp = horizon_path_[n][i];
            for (std::size_t k = 0; k < loads.size(); ++k) {
                const GoodId gid = goods_here[k];
                const double profit = world_.quote_sell(dests[i], gid, loads[k].q) - loads[k].buy_cost -
                                      world_.freight_unit_cost(gid, hp.miles) * loads[k].q;
                double& slot = opportunity_[loads[k].kind][n];
                slot = std::max(slot, profit - daily_cost(0) * hp.days);
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
    if (t.cargo >= 0 && t.job_town < 0) {  // (a carrier hauling a supplier's goods is the freight, not its customer)
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
    if (t.cargo >= 0 && t.qty > 0 && !inbound_.empty()) inbound_[World::idx(dest) * world_.goods.size() + World::idx(t.cargo)] += t.qty;
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
    // A freighter's good run is told as one story however many legs it took: the haul in and the haul back
    // count together, and the tally fades so only quick earners talk.
    t.recent_profit += profit;
    if (t.recent_profit > world_.params.story_profit_threshold) {
        ++stories_today_;
        t.recent_profit = 0;
    }
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
    if (t.job_town >= 0 && t.cargo >= 0) {
        if (t.at != t.job_town) {  // stopped short of the town (a road closed): carry on to the consignee
            if (act_travel(t, t.job_town, t.job_guards) != ActionResult::Ok) { t.cargo = -1; t.qty = 0; }  // no road left: the load is lost, settled tomorrow
            return;
        }
        deliver_job(t);
    }
    if (t.leaving && t.cargo < 0) {  // the load is down; the outfit goes home with what it holds
        retire(t, "its eastern backers pulled out", false);
        return;
    }
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

    // An open order is work for a carrier: the supplier's goods, paid for the haul. Taken when it beats buying a load.
    const Plan job = (p.carrier_jobs >= 2 || (p.carrier_jobs == 1 && !t.firm)) ? plan_job(t, scratch_paths_) : Plan{};
    const Plan buy = plan_buy(t, scratch_paths_);
    const double job_value = job.utility * p.job_preference;
    if (job.valid && job_value >= p.min_profit && (!buy.valid || buy.utility < p.min_profit || job_value >= buy.utility) && act_take_job(t, job)) return;
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
