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
    if (g.cash >= upkeep) {
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
        h.cash += recruit_cost;  // horses, guns and whiskey are bought in the hideout town
        h.inflow_today += recruit_cost;
    }

    const bool stranded = g.edge < 0 || !world_.edges[World::idx(g.edge)].active;
    if (stranded || world_.today - g.last_review >= p.gang_review_days) gang_review(g);
}

void Simulation::gang_review(Gang& g) {
    g.last_review = world_.today;
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

// ---------------------------------------------------------------- law

void Simulation::try_form_posse(NodeId n) {
    const Params& p = world_.params;
    Node& nd = world_.nodes[World::idx(n)];
    if (nd.fixed_market || nd.outrage < p.posse_outrage_threshold) return;
    // The angrier the town, the sooner it tries again: a town bled by robbers cannot wait out a long cooldown.
    const double cooldown = p.posse_cooldown / std::max(1.0, nd.outrage / p.posse_outrage_threshold);
    if (world_.today - nd.last_posse_day < cooldown) return;

    // Pick the most profitable gang in reach that isn't sheltered by this very town.
    const Gang* target = nullptr;
    double best = 0, target_dist = 0;
    for (const Gang& g : gangs_) {
        if (!g.active || g.hideout == n || g.robberies == 0) continue;
        const Edge& e = world_.edges[World::idx(g.edge)];
        const Node& a = world_.nodes[World::idx(e.a)];
        const Node& b = world_.nodes[World::idx(e.b)];
        const double d = dist(nd.x, nd.y, (a.x + b.x) / 2, (a.y + b.y) / 2);
        if (d > p.posse_range || world_.today - g.last_robbery > 120) continue;
        if (g.loot > best) { best = g.loot; target = &g; target_dist = d; }
    }
    if (!target) return;

    // Staffing. Without a leader the town fields as many untrained men as its fund buys. A leader costs a
    // fee but multiplies their strength and lets the town field a smaller, trained force.
    const double travel_days = std::ceil(target_dist / p.posse_speed) + 1;
    const int untrained = static_cast<int>(std::min<double>(p.posse_max_men, std::floor(nd.law_fund / p.posse_cost_per_man)));

    Leader* hire = nullptr;
    double lead_days = 0, fee = 0;
    for (Leader& l : leaders_) {
        if (!l.alive || l.busy) continue;
        const Node& at = world_.nodes[World::idx(l.at)];
        const double d = dist(nd.x, nd.y, at.x, at.y);
        if (d > p.leader_range || (hire && l.skill <= hire->skill)) continue;
        const double days = std::ceil(d / p.posse_speed);
        const double cost = p.leader_fee_per_day * (days + travel_days + 5);
        const int men_left = static_cast<int>(std::min<double>(p.posse_max_men, std::floor((nd.law_fund - cost) / p.posse_cost_per_man)));
        if (men_left < p.posse_min_men_led) continue;
        hire = &l;
        lead_days = days;
        fee = cost;
    }
    int men = untrained;
    if (hire) men = static_cast<int>(std::min<double>(p.posse_max_men, std::floor((nd.law_fund - fee) / p.posse_cost_per_man)));
    else if (men < p.posse_min_men) return;

    nd.last_posse_day = world_.today;
    nd.law_fund -= men * p.posse_cost_per_man + fee;
    Posse q;
    q.id = next_posse_id_++;
    q.origin = n;
    q.men = men;
    q.gang_id = target->id;
    q.days_left = lead_days + travel_days;
    if (hire) {
        hire->busy = true;
        hire->cash += fee;
        ++hire->jobs;
        q.leader_id = hire->id;
        log_.add(world_.today, EventType::Law, n,
                 strf("%s hires %s (skill %.1f, fee %s) to lead a posse of %d men against gang #%d (%d men) near the %s",
                      nd.name.c_str(), hire->name.c_str(), hire->skill, money(fee).c_str(), men, target->id, target->men,
                      ename(target->edge).c_str()),
                 strf("outrage %.1f over robberies on nearby trails; the gang has taken %s; the fund could field only %d "
                      "untrained men, but a seasoned leader multiplies their strength %.1fx",
                      nd.outrage, money(target->loot).c_str(), untrained, hire->skill));
    } else {
        log_.add(world_.today, EventType::Law, n,
                 strf("%s raises a posse of %d men against gang #%d (%d men) near the %s", nd.name.c_str(), men,
                      target->id, target->men, ename(target->edge).c_str()),
                 strf("outrage %.1f over robberies on nearby trails; the gang has taken %s so far; funded by %s of local "
                      "taxes; no leader for hire was in reach",
                      nd.outrage, money(target->loot).c_str(), money(men * p.posse_cost_per_man).c_str()));
    }
    posses_.push_back(q);
}

void Simulation::posse_day(Posse& q) {
    const Params& p = world_.params;
    if (!q.active) return;
    if (--q.days_left > 0) return;
    q.active = false;
    Node& origin = world_.nodes[World::idx(q.origin)];
    Leader* leader = q.leader_id >= 0 ? find_leader(q.leader_id) : nullptr;
    const std::string led = leader ? " led by " + leader->name : std::string();

    Gang* g = find_gang(q.gang_id);
    if (!g || !g->active) {
        log_.add(world_.today, EventType::Law, q.origin,
                 strf("The posse from %s%s finds no trace of gang #%d", origin.name.c_str(), led.c_str(), q.gang_id),
                 "the gang had already broken up or moved on");
        if (leader) release_leader(*leader, q.origin);
        return;
    }
    const double skill = leader ? leader->skill : 1.0;
    const double P = q.men * skill;
    const double G = g->men * p.gang_terrain_advantage;
    const bool posse_wins = rng_.chance(P * P / (P * P + G * G));
    const std::string skill_note = leader ? strf(" x%.1f leader skill", skill) : std::string();
    const std::string odds = strf("%d posse men%s vs %d bandits (terrain favored the gang %.1fx)", q.men,
                                  skill_note.c_str(), g->men, p.gang_terrain_advantage);
    if (posse_wins) {
        const double seized = g->cash;
        const double bonus = leader ? seized * p.leader_bonus_share : 0.0;
        origin.cash += seized - bonus;
        origin.outrage = 0;
        log_.add(world_.today, EventType::Law, q.origin,
                 strf("The posse from %s%s destroys gang #%d, seizing %s", origin.name.c_str(), led.c_str(), g->id,
                      money(seized).c_str()),
                 odds);
        g->cash = 0;
        gang_dissolve(*g, "destroyed by a posse");
        if (leader) {
            leader->cash += bonus;
            ++leader->wins;
            leader->skill = std::min(p.leader_skill_cap, leader->skill + p.leader_skill_gain);  // experience
            release_leader(*leader, q.origin);
        } else if (rng_.chance(p.leader_emerge_chance)) {
            // A man who carried himself well in the fight becomes someone other towns will hire.
            Leader ld;
            ld.id = next_leader_id_++;
            ld.name = strf("a deputy of %s (#%d)", origin.name.c_str(), ld.id);
            ld.at = q.origin;
            ld.skill = rng_.uniform(1.1, 1.5);
            log_.add(world_.today, EventType::Law, q.origin,
                     strf("%s emerges as a man for hire after the fight", ld.name.c_str()),
                     "a posse member who organized the others under fire earned a reputation");
            leaders_.push_back(std::move(ld));
        }
    } else {
        const int losses = std::max(1, static_cast<int>(std::lround(g->men * 0.25)));
        g->men -= losses;
        bool found = false;
        for (auto& gr : g->grudge)
            if (gr.first == q.origin) { gr.second += 2; found = true; }
        if (!found) g->grudge.push_back({q.origin, 2.0});
        origin.outrage *= 0.5;
        log_.add(world_.today, EventType::Law, q.origin,
                 strf("The posse from %s%s is routed by gang #%d", origin.name.c_str(), led.c_str(), g->id),
                 strf("%s; the gang lost %d but now avoids %s's trails", odds.c_str(), losses, origin.name.c_str()));
        if (g->men < p.gang_min_men) gang_dissolve(*g, "mauled by the posse");
        if (leader) {
            if (rng_.chance(p.leader_death_prob)) {
                leader->alive = false;
                log_.add(world_.today, EventType::Law, q.origin,
                         strf("%s is killed leading the posse", leader->name.c_str()),
                         strf("his posse was outmatched (strength %.0f vs %.0f)", P, G));
            } else {
                leader->skill = std::max(0.5, leader->skill - p.leader_skill_gain);
                release_leader(*leader, q.origin);
            }
        }
    }
}

}  // namespace frontier
