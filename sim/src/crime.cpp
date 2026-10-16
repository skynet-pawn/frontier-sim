// Bandit gangs and the posses that answer them.
//
// Gangs form from idle men (drifters) who collect where cash, liquor and transient
// labor meet. They park on the trail carrying the most cargo value and fence what
// they take at a nearby town. Robbery makes the towns at either end angry; angry
// towns with a funded law budget raise a posse. Nothing here is scripted: the
// "gang gets rich until a posse forms" arc falls out of these rules.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

namespace {
double dist(double x1, double y1, double x2, double y2) { return std::hypot(x1 - x2, y1 - y2); }
}  // namespace

void Simulation::rebuild_edge_gangs() {
    for (auto& v : edge_gangs_) v.clear();
    for (const Gang& g : gangs_)
        if (g.active && g.edge >= 0) edge_gangs_[World::idx(g.edge)].push_back(g.id);
}

void Simulation::crime_day() {
    const Params& p = world_.params;
    rebuild_edge_gangs();

    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        const Node& n = world_.nodes[i];
        if (!n.fixed_market && n.drifters >= p.gang_form_min_drifters && rng_.chance(p.gang_form_chance))
            try_form_gang(static_cast<NodeId>(i));
    }
    for (std::size_t i = 0; i < gangs_.size(); ++i) gang_day(gangs_[i]);
    for (std::size_t i = 0; i < posses_.size(); ++i) posse_day(posses_[i]);
    for (std::size_t i = 0; i < world_.nodes.size(); ++i) try_form_posse(static_cast<NodeId>(i));
    for (std::size_t i = 0; i < world_.nodes.size(); ++i) try_vigilance(static_cast<NodeId>(i));
    interests_day();

    gangs_.erase(std::remove_if(gangs_.begin(), gangs_.end(), [](const Gang& g) { return !g.active; }), gangs_.end());
    posses_.erase(std::remove_if(posses_.begin(), posses_.end(), [](const Posse& q) { return !q.active; }),
                  posses_.end());
}

// ---------------------------------------------------------------- gangs

double Simulation::grudge_of(const Gang& g, NodeId n) const {
    for (const auto& gr : g.grudge)
        if (gr.first == n) return gr.second;
    return 0;
}

// How attractive an edge is to a gang: lootable cargo passing by, divided by competition,
// discounted by nearby law and by towns that have already hit back.
double Simulation::gang_edge_score(const Gang& g, EdgeId eid) const {
    const Params& p = world_.params;
    const Edge& e = world_.edges[World::idx(eid)];
    if (!e.active || e.mode != Mode::Trail) return 0;
    const Node& a = world_.nodes[World::idx(e.a)];
    const Node& b = world_.nodes[World::idx(e.b)];
    const double mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
    const Node& home = world_.nodes[World::idx(g.hideout)];
    if (dist(mx, my, home.x, home.y) > p.gang_range_miles + e.miles / 2) return 0;

    int rivals = 0;
    for (int gid : edge_gangs_[World::idx(eid)])
        if (gid != g.id) ++rivals;
    const double law = 1.0 + (a.law_fund + b.law_fund) / p.law_deter_scale;
    const double grudge = 1.0 + grudge_of(g, e.a) + grudge_of(g, e.b);
    return (e.loot_ema + 1.0) / (1.0 + 2.0 * rivals) / law / grudge;
}

// The edge endpoint that can serve as a hideout: a real town, not an eastern market.
NodeId Simulation::nearest_node(EdgeId eid, NodeId near_to) const {
    const Edge& e = world_.edges[World::idx(eid)];
    const Node& ref = world_.nodes[World::idx(near_to)];
    NodeId best = -1;
    double best_d = 1e18;
    for (NodeId n : {e.a, e.b}) {
        const Node& nd = world_.nodes[World::idx(n)];
        if (nd.fixed_market || !nd.founded) continue;
        const double d = dist(nd.x, nd.y, ref.x, ref.y);
        if (d < best_d) { best_d = d; best = n; }
    }
    return best;
}

void Simulation::create_gang(NodeId hideout, int men, const char* cause) {
    Gang g;
    g.id = next_gang_id_++;
    g.men = men;
    g.hideout = hideout;
    g.cash = men * 20.0;
    world_.record(MoneyFlow::GangStake, g.cash);
    g.formed = world_.today;
    g.last_review = world_.today;
    double best = 0;
    g.edge = -1;
    for (std::size_t e = 0; e < world_.edges.size(); ++e) {
        const double s = gang_edge_score(g, static_cast<EdgeId>(e));
        if (s > best) { best = s; g.edge = static_cast<EdgeId>(e); }
    }
    if (g.edge < 0) {  // nowhere worth haunting: the men drift on
        world_.nodes[World::idx(hideout)].drifters += men;
        return;
    }
    log_.with({field("gang", g.id), field("men", men), field("node", nname(hideout))});
    log_.add(world_.today, EventType::Gang, "gang_formed", hideout,
             strf("Gang #%d (%d men) forms at %s and takes to the %s", g.id, men, nname(hideout).c_str(),
                  ename(g.edge).c_str()),
             cause);
    gangs_.push_back(std::move(g));
}

void Simulation::try_form_gang(NodeId n) {
    Node& nd = world_.nodes[World::idx(n)];
    const int men = static_cast<int>(std::min(nd.drifters, 20.0));
    nd.drifters -= men;
    create_gang(n, men,
                nd.transient_inflow
                    ? strf("%d idle men gathered at this camp/railhead (%s pop); no steady work and %s of cash in town",
                           men, with_commas(nd.population).c_str(), money(nd.cash).c_str()).c_str()
                    : strf("%d men left jobless as %s's income fell to %s/day (peak %s)", men, nd.name.c_str(),
                           money(nd.income_ema).c_str(), money(nd.income_peak).c_str()).c_str());
}

void Simulation::gang_dissolve(Gang& g, const char* why) {
    free_town(g, why);
    g.active = false;
    world_.record(MoneyFlow::GangExit, -g.cash);  // what the gang still holds scatters with its men
    g.cash = 0;
    Node& h = world_.nodes[World::idx(g.hideout)];
    h.drifters += g.men * 0.5;  // the rest scatter or die
    log_.with({field("gang", g.id), field("robberies", g.robberies), field("loot", g.loot), field("node", nname(g.hideout)), field("why", why)});
    log_.add(world_.today, EventType::Gang, "gang_disbanded", g.hideout,
             strf("Gang #%d breaks up after %d robberies, %s taken in all", g.id, g.robberies, money(g.loot).c_str()),
             why);
}

void Simulation::gang_day(Gang& g) {
    const Params& p = world_.params;
    if (!g.active) return;

    // Supplies are bought at the hideout town, which is why towns tolerate the gangs they fence for.
    const double upkeep = g.men * p.gang_upkeep;
    Node& h = world_.nodes[World::idx(g.hideout)];
    double spent = 0;  // money the gang puts into its hideout town today
    if (g.cash >= upkeep) {
        spent = upkeep;
        g.cash -= upkeep;
        h.cash += upkeep;
        h.inflow_today += upkeep;
    } else {
        spent = g.cash;  // the last of its money goes on supplies
        h.cash += g.cash;
        h.inflow_today += g.cash;
        g.cash = 0;
        if (--g.men < p.gang_min_men) {
            gang_dissolve(g, "the gang ran out of money and men drifted off");
            return;
        }
    }
    // Success attracts recruits, but each extra man costs more to arm and hide: bigger bands are harder to keep.
    const double recruit_cost = p.gang_recruit_cost * std::max(1.0, g.men / 8.0);
    if (g.men < p.gang_max_men && g.cash > recruit_cost * 2) {
        ++g.men;
        g.cash -= recruit_cost;
        spent += recruit_cost;
        h.cash += recruit_cost;  // horses, guns and whiskey are bought in the hideout town
        h.inflow_today += recruit_cost;
    }

    if (g.ruling >= 0) rule_town(g);
    else capture_progress(g, spent);
    if (!g.active) return;

    const bool stranded = g.edge < 0 || !world_.edges[World::idx(g.edge)].active;
    const bool review_due = world_.today - g.last_review >= p.gang_review_days;
    if (stranded || (review_due && !g.controlled)) gang_review(g);
}

void Simulation::gang_review(Gang& g) {
    g.last_review = world_.today;
    if (g.ruling >= 0) {  // a ruling gang works only the trails into its own town
        double best_score = -1;
        for (EdgeId e : world_.nodes[World::idx(g.ruling)].edges) {
            const double s = gang_edge_score(g, e);
            if (s > best_score) { best_score = s; g.edge = e; }
        }
        return;
    }
    const double current = g.edge >= 0 ? gang_edge_score(g, g.edge) : 0.0;
    double best = current * 1.25;  // staying put is cheaper than moving
    EdgeId pick = -1;
    for (std::size_t e = 0; e < world_.edges.size(); ++e) {
        if (static_cast<EdgeId>(e) == g.edge) continue;
        const double s = gang_edge_score(g, static_cast<EdgeId>(e));
        if (s > best) { best = s; pick = static_cast<EdgeId>(e); }
    }
    if (pick < 0) {
        if (g.edge >= 0 && current > 0) return;
        gang_dissolve(g, "no trail in reach carries enough traffic or has few enough lawmen to be worth hunting");
        return;
    }
    const NodeId new_home = nearest_node(pick, g.hideout);
    if (new_home < 0) return;
    const std::string from = g.edge >= 0 ? ename(g.edge) : std::string("nowhere");
    g.edge = pick;
    if (new_home != g.hideout) g.influence = 0;  // influence is local
    g.hideout = new_home;
    log_.with({field("gang", g.id), field("men", g.men), field("node", nname(g.hideout)), field("from", from), field("to", ename(pick))});
    log_.add(world_.today, EventType::Gang, "gang_moved", g.hideout,
             strf("Gang #%d (%d men) moves from %s to the %s", g.id, g.men, from.c_str(), ename(pick).c_str()),
             strf("better pickings: %s of lootable cargo/day passes the new trail and fewer lawmen are near it; hideout is now %s",
                  money(world_.edges[World::idx(pick)].loot_ema).c_str(), nname(g.hideout).c_str()),
             true);
}

// A gang running out of money has no choice: it takes what it can get, food included.
bool Simulation::gang_desperate(const Gang& g) const {
    const Params& p = world_.params;
    const double upkeep = g.men * p.gang_upkeep;
    return upkeep > 0 && g.cash < upkeep * p.gang_desperate_days;
}

// What a gang of `men` could carry off a trader's load, and whether that is worth a fight. Riders take what fits
// on a saddle, densest value first in effect: a handful of men can empty a gold shipment but not a flour wagon.
// Livestock walks away on its own (it needs hands, not saddlebags). Food is left alone unless the gang is hungry
// for cash, and a gang that is desperate lowers its standards.
Simulation::GangTake Simulation::gang_take(double men, bool desperate, const Trader& t) const {
    const Params& p = world_.params;
    GangTake take;
    if (t.cargo < 0 || t.qty <= 0) return take;
    const GoodDef& gd = good(t.cargo);
    if (gd.money) {
        take.units = t.qty;
    } else if (gd.self_propelled) {
        take.units = std::min(t.qty, men * p.gang_head_per_man);
    } else {
        if (gd.food_share > 0 && !desperate) return take;
        take.units = std::min(t.qty, men * p.gang_carry_lb_per_man / std::max(gd.weight_lb, 1e-9));
    }
    take.value = take.units * t.unit_cost;
    // The gang judges the goods, not the trader's bill: his unit cost carries freight, which would make heavy
    // bulk (timber, hides) look rich. The fence pays for the goods, so the goods' own price is the measure.
    take.loot = take.units * gd.base_cost;
    const double floor = men * p.gang_min_take_per_man * (desperate ? p.gang_desperate_floor_mult : 1.0);
    take.worth = gd.money || take.loot >= floor;
    return take;
}

// Returns true if the trader was robbed of everything (the trip is over). A gang that can carry off only part of
// a load takes a toll and lets the wagons go on.
bool Simulation::resolve_encounter(Gang& g, Trader& t, EdgeId eid) {
    const Params& p = world_.params;
    Edge& e = world_.edges[World::idx(eid)];
    const bool short_of_cash = gang_desperate(g);
    const GangTake take = gang_take(g.men, short_of_cash, t);
    if (!take.worth) return false;  // not worth the risk of a fight: the wagons pass
    // Say so in the log only when being short of cash is what made this stop worthwhile.
    const bool desperate = short_of_cash && !gang_take(g.men, false, t).worth;
    const double D = p.trader_base_defenders + p.guard_strength * t.guards;
    const double G = g.men;
    const double p_gang = G * G / (G * G + D * D);

    if (!rng_.chance(p_gang)) {
        // The trader's party drives them off: the gang loses men and remembers the town that outfitted them.
        const int losses = std::max(1, static_cast<int>(std::lround(g.men * p.gang_loss_when_repulsed)));
        g.men -= losses;
        if (!world_.nodes[World::idx(t.at)].fixed_market) {
            bool found = false;
            for (auto& gr : g.grudge)
                if (gr.first == t.at) { gr.second += 1; found = true; }
            if (!found) g.grudge.push_back({t.at, 1.0});
        }
        log_.with({field("trader", t.id), field("guards", t.guards), field("gang", g.id), field("lost", losses), field("edge", ename(eid))});
        log_.add(world_.today, EventType::Gang, "gang_repulsed", t.at,
                 strf("Trader #%d (%d guards) beats off gang #%d on the %s; the gang loses %d men", t.id, t.guards,
                      g.id, ename(eid).c_str(), losses),
                 strf("defenders %.1f vs %d bandits", D, g.men + losses), t.guards == 0);
        if (g.men < p.gang_min_men) gang_dissolve(g, "the gang was bled white by armed freighters");
        return false;
    }

    const double value = take.value;
    const double units = take.units;
    const GoodId cargo = t.cargo;
    const double load_value = t.qty * t.unit_cost;
    const bool whole = units >= t.qty - 1e-9;
    const double share = load_value > 0 ? std::min(1.0, value / load_value) : 1.0;  // how much of the load went
    if (t.agent_for >= 0) lost_orders_value_ += value;  // the town paid for this

    // Stolen goods need a buyer: the hideout town pays a discount from its own cash, then holds the stock.
    Node& hide = world_.nodes[World::idx(g.hideout)];
    const double fence_pay = std::min(units * world_.mid_price(g.hideout, cargo) * p.fence_rate,
                                      hide.cash * p.fence_cash_share);
    hide.cash -= fence_pay;
    hide.stock[World::idx(cargo)] += units;
    g.cash += fence_pay;
    g.loot += fence_pay;
    ++g.robberies;
    ++robbed_today_;
    g.last_robbery = world_.today;
    note_interest_loss(eid, g, cargo, value);

    // A toll is a smaller danger signal than being cleaned out, to the road's reputation and to the towns at either end.
    e.robbed_today += share;
    for (NodeId n : {e.a, e.b})
        if (n != g.hideout) world_.nodes[World::idx(n)].outrage += p.outrage_per_robbery * share;

    ++t.robberies;
    bool found = false;
    for (auto& f : t.fear)
        if (f.first == eid) { f.second += p.fear_bump * share; found = true; }
    if (!found) t.fear.push_back({eid, p.fear_bump * share});
    t.risk_aversion = std::min(p.risk_aversion_cap, t.risk_aversion + p.risk_aversion_bump * share);

    const std::string why_take =
        strf("%s of lootable cargo/day passes this trail; %s fences the goods for %s (%.0f%% of value)%s",
             money(e.loot_ema).c_str(), nname(g.hideout).c_str(), money(fence_pay).c_str(),
             value > 0 ? 100.0 * fence_pay / value : 0.0,
             desperate ? "; the gang is nearly out of cash and takes what it can get" : "");
    log_.with({field("gang", g.id), field("men", g.men), field("trader", t.id), field("guards", t.guards), field("edge", ename(eid)), field("good", good(cargo).id), field("units", units), field("value", value), field("fence_pay", fence_pay), field("hideout", nname(g.hideout)), field("desperate", desperate ? 1 : 0)});
    if (whole) {
        log_.add(world_.today, EventType::Robbery, "robbery", g.hideout,
                 strf("Gang #%d (%d men) robs trader #%d (%d guards) on the %s: %.0f %s, cost %s", g.id, g.men, t.id,
                      t.guards, ename(eid).c_str(), units, good(cargo).unit.c_str(), money(value).c_str()),
                 why_take, value < 1500);
    } else {
        log_.add(world_.today, EventType::Robbery, "toll", g.hideout,
                 strf("Gang #%d (%d men) takes a toll from trader #%d (%d guards) on the %s: %.0f of %.0f %s, cost %s",
                      g.id, g.men, t.id, t.guards, ename(eid).c_str(), units, t.qty, good(cargo).unit.c_str(),
                      money(value).c_str()),
                 strf("%s; the gang can carry off only %.0f%% of the load", why_take.c_str(), 100.0 * share),
                 value < 1500);
        t.qty -= units;
        return false;  // the wagons go on with the rest
    }

    t.cargo = -1;
    t.qty = 0;
    t.travelling = false;
    t.route.clear();
    t.guards = 0;  // back to the leg's starting town
    if (rng_.chance(p.trader_kill_prob)) retire(t, "killed in a holdup");
    return true;
}

}  // namespace frontier
