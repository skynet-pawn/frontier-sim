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

// How attractive an edge is to a gang: cargo value passing by, divided by competition,
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
    return (e.traffic_ema + 1.0) / (1.0 + 2.0 * rivals) / law / grudge;
}

// The edge endpoint that can serve as a hideout: a real town, not an eastern market.
NodeId Simulation::nearest_node(EdgeId eid, NodeId near_to) const {
    const Edge& e = world_.edges[World::idx(eid)];
    const Node& ref = world_.nodes[World::idx(near_to)];
    NodeId best = -1;
    double best_d = 1e18;
    for (NodeId n : {e.a, e.b}) {
        const Node& nd = world_.nodes[World::idx(n)];
        if (nd.fixed_market) continue;
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
    log_.add(world_.today, EventType::Gang, hideout,
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
    Node& h = world_.nodes[World::idx(g.hideout)];
    h.drifters += g.men * 0.5;  // the rest scatter or die
    log_.add(world_.today, EventType::Gang, g.hideout,
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
    if (stranded || world_.today - g.last_review >= p.gang_review_days) gang_review(g);
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
    log_.add(world_.today, EventType::Gang, g.hideout,
             strf("Gang #%d (%d men) moves from %s to the %s", g.id, g.men, from.c_str(), ename(pick).c_str()),
             strf("better pickings: %s of cargo/day passes the new trail and fewer lawmen are near it; hideout is now %s",
                  money(world_.edges[World::idx(pick)].traffic_ema).c_str(), nname(g.hideout).c_str()),
             true);
}

// Returns true if the trader was robbed (the trip is over).
bool Simulation::resolve_encounter(Gang& g, Trader& t, EdgeId eid) {
    const Params& p = world_.params;
    Edge& e = world_.edges[World::idx(eid)];
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
        log_.add(world_.today, EventType::Gang, t.at,
                 strf("Trader #%d (%d guards) beats off gang #%d on the %s; the gang loses %d men", t.id, t.guards,
                      g.id, ename(eid).c_str(), losses),
                 strf("defenders %.1f vs %d bandits", D, g.men + losses), t.guards == 0);
        if (g.men < p.gang_min_men) gang_dissolve(g, "the gang was bled white by armed freighters");
        return false;
    }

    const double value = t.qty * t.unit_cost;
    const GoodId cargo = t.cargo;
    const double units = t.qty;

    // Stolen goods need a buyer: the hideout town pays a discount from its own cash, then holds the stock.
    Node& hide = world_.nodes[World::idx(g.hideout)];
    const double fence_pay = std::min(units * world_.mid_price(g.hideout, cargo) * p.fence_rate,
                                      hide.cash * p.fence_cash_share);
    hide.cash -= fence_pay;
    hide.stock[World::idx(cargo)] += units;
    g.cash += fence_pay;
    g.loot += fence_pay;
    ++g.robberies;
    g.last_robbery = world_.today;
    note_interest_loss(eid, g, cargo, value);

    e.robbed_today += 1;
    for (NodeId n : {e.a, e.b})
        if (n != g.hideout) world_.nodes[World::idx(n)].outrage += p.outrage_per_robbery;

    ++t.robberies;
    bool found = false;
    for (auto& f : t.fear)
        if (f.first == eid) { f.second += p.fear_bump; found = true; }
    if (!found) t.fear.push_back({eid, p.fear_bump});
    t.risk_aversion = std::min(p.risk_aversion_cap, t.risk_aversion + p.risk_aversion_bump);

    log_.add(world_.today, EventType::Robbery, g.hideout,
             strf("Gang #%d (%d men) robs trader #%d (%d guards) on the %s: %.0f %s, cost %s", g.id, g.men, t.id,
                  t.guards, ename(eid).c_str(), units, good(cargo).unit.c_str(), money(value).c_str()),
             strf("%s of cargo/day passes this trail; %s fences the goods for %s (%.0f%% of value)",
                  money(e.traffic_ema).c_str(), nname(g.hideout).c_str(), money(fence_pay).c_str(),
                  value > 0 ? 100.0 * fence_pay / value : 0.0),
             value < 1500);

    t.cargo = -1;
    t.qty = 0;
    t.travelling = false;
    t.route.clear();
    t.guards = 0;  // back to the leg's starting town
    if (rng_.chance(p.trader_kill_prob)) retire(t, "killed in a holdup");
    return true;
}

}  // namespace frontier
