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
    { PhaseTimer t(phase_time_[6]); rebuild_edge_gangs();
    if (p.gang_wars) gang_wars_day(); }

    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        const Node& n = world_.nodes[i];
        if (p.gang_tiers) {
            if (!n.fixed_market && n.fighters >= p.band_min_men && rng_.chance(p.band_form_chance)) try_form_band(static_cast<NodeId>(i));
        } else if (!n.fixed_market && n.drifters >= p.gang_form_min_drifters && rng_.chance(p.gang_form_chance)) {
            try_form_gang(static_cast<NodeId>(i));
        }
    }
    { PhaseTimer t(phase_time_[7]); for (std::size_t i = 0; i < gangs_.size(); ++i) gang_day(gangs_[i]); }
    { PhaseTimer t(phase_time_[8]); for (std::size_t i = 0; i < posses_.size(); ++i) posse_day(posses_[i]); }
    {
        PhaseTimer t(phase_time_[9]);
        if (p.law_stages) law_day();
        for (std::size_t i = 0; i < world_.nodes.size(); ++i) try_form_posse(static_cast<NodeId>(i));
        for (std::size_t i = 0; i < world_.nodes.size(); ++i) try_vigilance(static_cast<NodeId>(i));
    }
    { PhaseTimer t(phase_time_[10]); interests_day(); bushwhack_reports(); }

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
    if (dist(mx, my, home.x, home.y) > (g.fleeing ? p.gang_flee_range : p.gang_range_miles) + e.miles / 2) return 0;

    int rivals = 0;
    for (int gid : edge_gangs_[World::idx(eid)])
        if (gid != g.id) ++rivals;
    const double law = 1.0 + (a.law_fund + b.law_fund) / p.law_deter_scale;
    const double grudge = 1.0 + grudge_of(g, e.a) + grudge_of(g, e.b);
    return (e.loot_ema + 1.0) / (1.0 + 2.0 * rivals) / law / grudge / gang_heat(g, eid);
}

// How hot a trail is for this gang: the fame it has in the regions the trail runs through, made worse by size (a big
// gang is seen from further off). Unknown country has heat 1.
double Simulation::gang_heat(const Gang& g, EdgeId eid) const {
    const Params& p = world_.params;
    if (!p.fame_stages || g.known.empty()) return 1.0;
    const Edge& e = world_.edges[World::idx(eid)];
    const int ra = world_.nodes[World::idx(e.a)].region, rb = world_.nodes[World::idx(e.b)].region;
    const double k = ra == rb ? gang_known(g, ra) : 0.5 * (gang_known(g, ra) + gang_known(g, rb));
    return 1.0 + p.fame_flee * k * (0.5 + g.men / p.fame_size_ref);
}

void Simulation::gang_deed_region(Gang& g, int region, double weight) {
    if (!world_.params.fame_stages || region < 0) return;
    if (g.known.size() < world_.region_names.size()) g.known.resize(world_.region_names.size(), 0.0f);
    g.known[static_cast<std::size_t>(region)] += static_cast<float>(weight);
}

void Simulation::gang_deed_edge(Gang& g, EdgeId eid, double weight) {
    const Edge& e = world_.edges[World::idx(eid)];
    const int ra = world_.nodes[World::idx(e.a)].region, rb = world_.nodes[World::idx(e.b)].region;
    gang_deed_region(g, ra, weight);
    if (rb != ra) gang_deed_region(g, rb, weight);
}

// How far the law's reach goes at a place: none where nobody is paid to enforce it, a marshal's beat in a town that
// hired one, the full county (sheriff and deputies) where one is seated.
double Simulation::law_presence(const Node& n) const {
    const Params& p = world_.params;
    if (!p.law_stages || n.fixed_market) return 1.0;
    return n.law == 0 ? p.law_presence_none : n.law == 1 ? p.law_presence_marshal : 1.0;
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

const char* drifter_flow_name(DrifterFlow f) {
    static const char* const names[] = {"arrived at camps and railheads", "left a dying town", "left the land (farms)", "ruined traders", "fled a vigilance reprisal",
                                        "scattered from a broken gang", "scattered from a quarrelling band", "returned (gang could not form)", "gave up, died or left (decay)",
                                        "formed a gang", "turned to crime (drifters)", "fighters who gave up or left", "recruited into a gang", "gang men killed or gone", "vanished when a gang broke up"};
    return names[static_cast<std::size_t>(f)];
}

void Simulation::create_gang(NodeId hideout, int men, const char* cause, int cells) {
    Gang g;
    g.id = next_gang_id_++;
    g.men = men;
    g.fights.assign(static_cast<std::size_t>(std::max(0, men)), 0);
    g.cells = cells > 0 ? cells : 1;
    gang_fix_cells(g);
    // The boss's grip on his men; usually middling, as with the bosses who take over towns (capture.cpp).
    if (world_.params.gang_wars) {
        const double u = rng_.next_double();
        g.competence = 0.1 + 0.8 * u * u;
    }
    {  // a tenth of bosses are careful in town, a tenth careless
        const double u = rng_.next_double();
        g.care = u < world_.params.camp_careless_share ? 0 : u < world_.params.camp_careless_share + world_.params.camp_careful_share ? 2 : 1;
    }
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
        scatter_men(hideout, men, DrifterFlow::ReturnedUnformed);
        return;
    }
    choose_camp(g);
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
    note_drifters(DrifterFlow::FormedGang, men);
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
    scatter_men(g.hideout, g.men * 0.5, DrifterFlow::GangScatter);  // the rest scatter or die
    note_drifters(DrifterFlow::GangVanished, g.men * 0.5);
    ++gangs_ended_;
    if (g.peak >= world_.params.gang_large_men) {   // a larger gang's end, sorted by cause: 0 law, 1 money, 2 rivals, 3 quarrels, 4 armed freighters, 5 other
        const std::string w = why;
        ++large_ended_;
        large_days_lived_ += static_cast<double>(world_.today - g.formed);
        const int k = (w.find("posse") != std::string::npos || w.find("vigilance") != std::string::npos || w.find("detectives") != std::string::npos ||
                       w.find("pursuers") != std::string::npos) ? 0
                    : w.find("lawmen") != std::string::npos ? 6
                    : w.find("money") != std::string::npos ? 1
                    : w.find("rival") != std::string::npos ? 2
                    : (w.find("tore") != std::string::npos || w.find("quarrel") != std::string::npos) ? 3
                    : w.find("freighters") != std::string::npos ? 4 : 5;
        ++large_end_[k];
    }
    gang_days_lived_ += static_cast<double>(world_.today - g.formed);
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
    if (p.gang_tiers && is_band(g)) {  // a band lives mostly off small thefts around its hideout, which the town's cash pays for
        const double take = std::min(h.cash * 0.01, g.men * p.band_petty_take);
        h.cash -= take;
        g.cash += take;
        // Without a camp the band is at home in town and lawmen find it there; with one they have to find the camp.
        if (!p.camp_stages) band_catch_check(g, -1, p.band_petty_catch);
        else band_catch_check(g, -1, p.camp_found_catch, "lawmen followed the band's tracks back to its camp");
        if (!g.active) return;
        bushwhack(g);
    }
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
        gang_lose_men(g, 1);
        if (g.men < p.gang_min_men) {
            gang_dissolve(g, "the gang ran out of money and men drifted off");
            return;
        }
    }
    // Success attracts recruits, but each extra man costs more to arm and hide: bigger bands are harder to keep.
    const double recruit_cost = p.gang_recruit_cost * std::max(1.0, g.men / 8.0);
    if (g.men < (p.gang_wars ? p.gang_hard_max : p.gang_max_men) && g.cash > recruit_cost * 2 && (!p.gang_tiers || h.fighters >= 1)) {
        gang_add_men(g, 1);
        if (p.gang_tiers) h.fighters -= 1;
        note_drifters(DrifterFlow::GangRecruited, 1);
        g.cash -= recruit_cost;
        spent += recruit_cost;
        h.cash += recruit_cost;  // horses, guns and whiskey are bought in the hideout town
        h.inflow_today += recruit_cost;
    }

    if (p.camp_stages && g.ruling < 0) {
        spent += gang_town_visit(g);
        if (!g.active) return;
    }

    gang_peak_men_ = std::max(gang_peak_men_, g.men);
    g.peak = std::max(g.peak, g.men);
    if (p.fame_stages && !g.known.empty()) {
        for (float& k : g.known) k *= static_cast<float>(1.0 - p.fame_decay);
        // Too well known where it works: ride for fresh country (a ruling gang or a player's gang stays put).
        if (g.ruling < 0 && !g.controlled && world_.today - g.last_review >= 10) {
            const double heat = gang_known(g, h.region) * (0.5 + g.men / p.fame_size_ref);
            if (heat >= p.fame_flee_trigger) {
                g.fleeing = true;
                gang_review(g);
            }
        }
    }
    gang_breakdown_check(g);
    if (!g.active) return;
    if (g.ruling >= 0) rule_town(g);
    else capture_progress(g, spent);
    if (!g.active) return;

    const bool stranded = g.edge < 0 || !world_.edges[World::idx(g.edge)].active;
    const bool review_due = world_.today - g.last_review >= p.gang_review_days;
    if (review_due && p.gang_tiers) {
        gang_try_join(g);
        if (!g.active) return;
    }
    if (stranded || (review_due && !g.controlled)) gang_review(g);
}

void Simulation::gang_review(Gang& g) {
    gang_review_pick(g);
    g.fleeing = false;
}

void Simulation::gang_review_pick(Gang& g) {
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
    choose_camp(g);
    log_.with({field("gang", g.id), field("men", g.men), field("node", nname(g.hideout)), field("from", from), field("to", ename(pick))});
    const Node& landed = world_.nodes[World::idx(g.hideout)];
    if (g.fleeing) {
        log_.add(world_.today, EventType::Gang, "gang_fled", g.hideout,
                 strf("Gang #%d (%d men) leaves for %s country, to the %s", g.id, g.men,
                      landed.region >= 0 ? world_.region_names[static_cast<std::size_t>(landed.region)].c_str() : "new", ename(pick).c_str()),
                 strf("its name is known too well where it was working; it rides to country that has not heard of it; hideout is now %s",
                      nname(g.hideout).c_str()));
    } else {
        log_.add(world_.today, EventType::Gang, "gang_moved", g.hideout,
                 strf("Gang #%d (%d men) moves from %s to the %s", g.id, g.men, from.c_str(), ename(pick).c_str()),
                 strf("better pickings: %s of lootable cargo/day passes the new trail and fewer lawmen are near it; hideout is now %s",
                      money(world_.edges[World::idx(pick)].loot_ema).c_str(), nname(g.hideout).c_str()),
                 true);
    }
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
    // One raiding party does the job, not the whole gang: a big gang is several parties spread along the trail.
    const int party = p.gang_tiers ? std::max(1, std::min(g.men, (g.men + g.cells - 1) / g.cells)) : g.men;
    const GangTake take = gang_take(party, short_of_cash, t);
    if (!take.worth) return false;  // not worth the risk of a fight: the wagons pass
    // Say so in the log only when being short of cash is what made this stop worthwhile.
    const bool desperate = short_of_cash && !gang_take(party, false, t).worth;
    const double D = p.trader_base_defenders + p.guard_strength * t.guards;
    const double G = p.gang_wars ? gang_strength(g) * party / std::max(1, g.men) : g.men;
    const double p_gang = G * G / (G * G + D * D);
    const bool shootout = t.guards > 0;  // armed resistance; an unguarded wagon gives up without a fight
    // Men who have a living to make do not stop a wagon they expect to lose to: guarded freight rides past.
    if (p.gang_tiers && p_gang < p.band_min_odds) return false;

    if (!rng_.chance(p_gang)) {
        // The trader's party drives them off: the gang loses men and remembers the town that outfitted them.
        const int losses = std::max(1, static_cast<int>(std::lround(party * p.gang_loss_when_repulsed)));
        gang_lose_men(g, losses);
        if (shootout) gang_fought(g);
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
    // Selling them is the moment a band is most likely to be noticed (checked below, once the sale is made).
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
    if (shootout) gang_fought(g);
    note_interest_loss(eid, g, cargo, value);
    gang_deed_edge(g, eid, (whole ? p.fame_robbery : p.fame_toll));

    // A toll is a smaller danger signal than being cleaned out, to the road's reputation and to the towns at either end.
    e.robbed_today += share;
    for (NodeId n : {e.a, e.b})
        if (n != g.hideout) world_.nodes[World::idx(n)].outrage += p.outrage_per_robbery * share;

    ++t.robberies;
    bump_fear(t, eid, p.fear_bump * share, p.risk_aversion_bump * share, p.risk_aversion_cap);

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
        band_catch_check(g, eid);
        if (p.camp_stages) band_catch_check(g, -1, p.camp_fence_catch);   // selling the loot gets it noticed
        return false;  // the wagons go on with the rest
    }

    band_catch_check(g, eid);
    if (p.camp_stages) band_catch_check(g, -1, p.camp_fence_catch);
    t.cargo = -1;
    t.qty = 0;
    t.travelling = false;
    t.route.clear();
    t.guards = 0;  // back to the leg's starting town
    if (rng_.chance(p.trader_kill_prob)) {
        retire(t, "killed in a holdup");
        gang_deed_edge(g, eid, p.fame_kill);
    }
    return true;
}

// ---------------------------------------------------------------- bands, gangs and the idle men who feed them

// Men who leave a gang or fail to form one go back to where fighters are found (or, in the old model, the drifters).
void Simulation::scatter_men(NodeId at, double n, DrifterFlow why) {
    Node& nd = world_.nodes[World::idx(at)];
    (world_.params.gang_tiers ? nd.fighters : nd.drifters) += n;
    note_drifters(why, n);
}

// A band is 2 to 5 men who could fight and had nothing else: they pool what they have and hold up what is soft.
void Simulation::try_form_band(NodeId n) {
    const Params& p = world_.params;
    Node& nd = world_.nodes[World::idx(n)];
    const int most = std::min<int>(p.band_max_men, static_cast<int>(nd.fighters));
    if (most < p.band_min_men) return;
    const int men = p.band_min_men + static_cast<int>(rng_.next_below(static_cast<std::uint64_t>(most - p.band_min_men + 1)));
    nd.fighters -= men;
    note_drifters(DrifterFlow::FormedGang, men);
    create_gang(n, men, strf("%d men who can fight and have no work band together at %s and pool what they have; %s of cash in town",
                              men, nd.name.c_str(), money(nd.cash).c_str()).c_str());
}

// A raiding party is 3 to 5 men; the gang's cells are how many such parties it can put on the trail. Cells never exceed
// half the men (a party needs two) and grow as the gang does.
void Simulation::gang_fix_cells(Gang& g) {
    const Params& p = world_.params;
    if (!p.gang_tiers) return;
    const int needed = (g.men + p.gang_party_max - 1) / std::max(1, p.gang_party_max);
    g.cells = std::max(g.cells, needed);
    g.cells = std::max(1, std::min(g.cells, g.men / 2));
}

// Each party meets traders on its own, so a gang of several parties is met more often.
double Simulation::gang_contact_prob(const Gang& g) const {
    const double p = world_.params.gang_encounter_prob;
    if (!world_.params.gang_tiers || g.cells <= 1) return p;
    return 1.0 - std::pow(1.0 - p, g.cells);
}

// The other gang's men, parties, cash and record become the host's.
void Simulation::gang_absorb(Gang& host, Gang& other) {
    host.fights.insert(host.fights.end(), other.fights.begin(), other.fights.end());
    host.men += other.men;
    host.cells += other.cells;
    host.cash += other.cash;
    host.robberies += other.robberies;
    host.loot += other.loot;
    other.cash = 0;
    other.active = false;
    other.men = 0;
    other.fights.clear();
    gang_fix_cells(host);
}

// A band that has pulled several jobs and not been caught is worth having. A gang short of men (it lost some in a fight)
// takes bands in reach; two bands with a record may also pool and become a gang. Loosely tied: each keeps its own parties.
void Simulation::gang_try_join(Gang& g) {
    const Params& p = world_.params;
    if (g.ruling >= 0 || g.controlled || g.men > p.band_max_men || g.robberies < p.band_merge_min_robberies) return;
    if (!rng_.chance(p.band_join_chance)) return;
    const Node& mine = world_.nodes[World::idx(g.hideout)];
    Gang* best = nullptr;
    for (Gang& h : gangs_) {
        if (&h == &g || !h.active || h.controlled) continue;
        const bool large = h.men > p.band_max_men;
        if (!large && h.robberies < p.band_merge_min_robberies) continue;
        const Node& theirs = world_.nodes[World::idx(h.hideout)];
        if (dist(mine.x, mine.y, theirs.x, theirs.y) > p.band_join_range) continue;
        const double hold = gang_manageable(h);
        if (h.men + g.men > hold) continue;
        if (large && h.men >= p.gang_hire_below * hold) continue;  // a gang at full strength does not need men
        if (!best || h.men > best->men) best = &h;
    }
    if (!best) return;
    const int before = best->men;
    const std::string how = best->men > p.band_max_men
        ? strf("gang #%d had %d of the %.0f men its boss can hold, and the band had pulled %d jobs without being caught",
               best->id, before, gang_manageable(*best), g.robberies)
        : strf("two small bands with %d and %d jobs behind them pool men and a hideout; neither could hold a road alone against posses",
               best->robberies, g.robberies);
    log_.with({field("band", g.id), field("gang", best->id), field("men", g.men), field("total", before + g.men)});
    log_.add(world_.today, EventType::Gang, "band_joined", best->hideout,
             strf("Band #%d (%d men) joins gang #%d, now %d men in %d parties", g.id, g.men, best->id, before + g.men, best->cells + g.cells), how);
    gang_absorb(*best, g);
}

// A band is small and bold: each hold-up risks lawmen catching up with it, more where the law is funded. Most do not last.
// With base < 0 this is a hold-up on a trail (the chance uses the law at both ends); otherwise it is the daily risk of
// the band's petty work around its hideout, with `base` the daily chance before the town's law fund raises it.
void Simulation::band_catch_check(Gang& g, EdgeId eid, double base, const char* how) {
    const Params& p = world_.params;
    if (!p.gang_tiers || !g.active || !is_band(g)) return;
    const bool daily = base >= 0;
    double law_fund = 0, reach = 1.0, fame = 1.0;
    if (daily) {
        const Node& hn = world_.nodes[World::idx(g.hideout)];
        law_fund = hn.law_fund;
        reach = law_presence(hn);
        if (p.fame_stages) fame = std::clamp(0.5 + gang_known(g, hn.region), p.fame_catch_floor, p.fame_catch_cap);
    } else {
        const Edge& e = world_.edges[World::idx(eid)];
        const Node& na = world_.nodes[World::idx(e.a)];
        const Node& nb = world_.nodes[World::idx(e.b)];
        law_fund = na.law_fund + nb.law_fund;
        reach = std::max(law_presence(na), law_presence(nb));
        if (p.fame_stages) fame = std::clamp(0.5 + std::max(gang_known(g, na.region), gang_known(g, nb.region)), p.fame_catch_floor, p.fame_catch_cap);
    }
    const double law = 1.0 + std::min(p.band_law_cap, law_fund / p.law_deter_scale);
    const double chance = std::min(0.9, (daily ? base : p.band_catch_base) * law * reach * fame);
    if (!rng_.chance(chance)) return;
    if (p.camp_stages && !daily && rng_.chance(p.camp_escape)) {  // the posse nearly has it, but the band knows the way to its camp
        ++camp_escapes_;
        return;
    }
    if (p.camp_stages && daily) ++camp_caught_;
    const std::string where = daily ? (how ? "at its camp near " : p.camp_stages ? "in " : "around ") + nname(g.hideout) : "on the " + ename(eid);
    log_.with({field("gang", g.id), field("men", g.men), field("robberies", g.robberies), field("node", nname(g.hideout))});
    log_.add(world_.today, EventType::Law, "band_caught", g.hideout,
             strf("Lawmen catch band #%d (%d men) %s after %d jobs", g.id, g.men, where.c_str(), g.robberies),
             strf("%s (%.1f%% a %s; the town's funded law raises it); what the band held goes to %s",
                  daily ? (how ? how : p.camp_stages ? "the band rode into town with money to spend and someone knew the law" : "the band lives off petty theft near the town, where people know the law") : "each hold-up risks being caught", chance * 100,
                  daily ? "day" : "hold-up", nname(g.hideout).c_str()),
             g.robberies < 3);
    Node& h = world_.nodes[World::idx(g.hideout)];
    h.cash += g.cash;  // seized and spent locally; keeps the money audit whole
    g.cash = 0;
    gang_dissolve(g, "caught by lawmen");
}

// Where a gang really lives. The camp lies a few miles up its trail from the town it trades with, away from anyone who
// would ask questions: a dugout, a cabin whose people were driven off (or "abandoned", with a story about a bear), a ruined
// fort or blockhouse from one of the old wars, a line shack, a canyon with water. The kind is only a label for the viewer.
const char* Simulation::camp_kind_name(int kind) {
    static const char* const names[] = {"a dugout in a cut bank", "a cabin whose owners were driven off", "an abandoned cabin", "a ruined blockhouse",
                                        "an empty line shack", "a canyon with water"};
    return kind >= 0 && kind < 6 ? names[kind] : "a camp";
}

void Simulation::choose_camp(Gang& g) {
    const Params& p = world_.params;
    if (!p.camp_stages || g.edge < 0) return;
    const Edge& e = world_.edges[World::idx(g.edge)];
    const double limit = std::min(p.camp_max_miles, 0.45 * e.miles);
    const double lo = std::min(p.camp_min_miles, limit);
    g.camp_miles = lo + (limit - lo) * rng_.next_double();
    const double u = rng_.next_double();
    g.camp_kind = u < 0.25 ? 0 : u < 0.45 ? 1 : u < 0.65 ? 2 : u < 0.80 ? 3 : u < 0.92 ? 4 : 5;
    log_.with({field("gang", g.id), field("miles", g.camp_miles), field("kind", g.camp_kind)});
    log_.add(world_.today, EventType::Gang, "camp_made", g.hideout,
             strf("Gang #%d makes camp in %s, %.0f miles up the %s from %s", g.id, camp_kind_name(g.camp_kind), g.camp_miles,
                  ename(g.edge).c_str(), nname(g.hideout).c_str()),
             "a camp off the road gives the gang somewhere to go when the law comes; it only rides into town to spend", true);
}

// A flush gang rides into town to spend what it has taken: whiskey, cards, women. It leaves money behind, which is why towns
// put up with the trade, and risks being seen. A careful boss keeps his men quiet; a careless one lets them start something,
// and that is what raises the town against them. Returns what was left in town.
double Simulation::gang_town_visit(Gang& g) {
    const Params& p = world_.params;
    if (g.camp_kind < 0 || g.cash < g.men * p.camp_visit_cash_per_man || !rng_.chance(p.camp_visit_chance)) return 0;
    Node& h = world_.nodes[World::idx(g.hideout)];
    const double spree = g.cash * p.camp_visit_spend;
    g.cash -= spree;
    h.cash += spree;
    h.inflow_today += spree;
    ++camp_visits_;
    const double trouble_chance = g.care == 0 ? p.camp_trouble_careless : g.care == 2 ? p.camp_trouble_careful : p.camp_trouble_ordinary;
    const bool trouble = rng_.chance(trouble_chance);
    if (trouble) {
        ++camp_trouble_;
        h.outrage += p.camp_trouble_outrage;
        gang_deed_region(g, h.region, p.fame_fight * 0.5);
        log_.with({field("gang", g.id), field("men", g.men), field("node", h.name)});
        log_.add(world_.today, EventType::Gang, "gang_trouble", g.hideout,
                 strf("Gang #%d (%d men) shoots up %s on a spree", g.id, g.men, h.name.c_str()),
                 strf("the boss %s keep his men quiet; the town is angry", g.care == 0 ? "did not trouble to" : "could not"), true);
    }
    band_catch_check(g, -1, p.camp_visit_catch * (trouble ? 3.0 : 1.0));
    return spree;
}

// ---------------------------------------------------------------- gang wars, veterans and size

// Each man carries a count of the shootouts he has survived. A man with gang_veteran_fights or more is a veteran:
// he counts for more in a fight and helps his boss keep order. Casualties fall mostly on the least experienced.
void Simulation::gang_add_men(Gang& g, int n, int fights) {
    for (int i = 0; i < n; ++i) g.fights.push_back(fights);
    g.men += n;
    gang_fix_cells(g);
}

void Simulation::gang_lose_men(Gang& g, int n) {
    const Params& p = world_.params;
    note_drifters(DrifterFlow::GangLosses, std::max(0, std::min(n, g.men)));
    if (!p.gang_wars) {  // the old model: men are interchangeable and the count may dip below zero
        g.men -= n;
        g.fights.resize(static_cast<std::size_t>(std::max(0, g.men)));
        return;
    }
    n = std::min<int>(n, static_cast<int>(g.fights.size()));
    for (int i = 0; i < n; ++i) {
        std::size_t k = 0;
        if (rng_.chance(p.gang_casualty_random)) k = static_cast<std::size_t>(rng_.next_below(g.fights.size()));
        else k = static_cast<std::size_t>(std::min_element(g.fights.begin(), g.fights.end()) - g.fights.begin());
        g.fights.erase(g.fights.begin() + static_cast<std::ptrdiff_t>(k));
    }
    g.men = static_cast<int>(g.fights.size());
    gang_fix_cells(g);
}

int Simulation::gang_veterans(const Gang& g) const {
    int v = 0;
    for (int f : g.fights) v += f >= world_.params.gang_veteran_fights;
    return v;
}

double Simulation::gang_strength(const Gang& g) const {
    return g.men + gang_veterans(g) * (world_.params.gang_veteran_strength - 1.0);
}

// A gang is as big as its boss can keep together: charisma (competence) sets the base, veterans add lieutenants.
// Past that the men quarrel (gang_breakdown_check), so the cap is soft, and only an exceptional boss holds a big band.
double Simulation::gang_manageable(const Gang& g) const {
    const Params& p = world_.params;
    return p.gang_manageable_base * (0.5 + 1.5 * g.competence) + p.gang_veteran_hold * gang_veterans(g);
}

void Simulation::gang_fought(Gang& g) {
    for (int& f : g.fights) ++f;
}

// A band bigger than its boss can hold quarrels over shares and watches. The chance rises with the square of how far
// over the limit it is; a breakdown costs men (half killed, half walk off as drifters) and the boss's standing.
void Simulation::gang_breakdown_check(Gang& g) {
    const Params& p = world_.params;
    if (!p.gang_wars) return;
    const double limit = std::max(1.0, gang_manageable(g));
    if (g.men <= limit) return;
    const double excess = g.men / limit - 1.0;
    if (!rng_.chance(p.gang_breakdown_rate * excess * excess)) return;
    const int lost = std::max(1, static_cast<int>(std::ceil(g.men * p.gang_breakdown_loss)));
    const int before = g.men;
    gang_lose_men(g, lost);
    g.competence *= 0.95;
    Node& h = world_.nodes[World::idx(g.hideout)];
    scatter_men(g.hideout, lost * 0.5, DrifterFlow::BreakdownScatter);
    log_.with({field("gang", g.id), field("node", h.name), field("lost", lost), field("men", before), field("limit", limit)});
    log_.add(world_.today, EventType::Gang, "gang_breakdown", g.hideout,
             strf("Gang #%d falls to quarrelling at %s: %d of %d men killed or gone", g.id, h.name.c_str(), lost, before),
             strf("its boss can hold about %.0f men together (competence %.2f, %d veterans) and it has %d", limit, g.competence, gang_veterans(g), before));
    if (g.men < p.gang_min_men) gang_dissolve(g, "the gang tore itself apart over the spoils");
}

// Two gangs working one trail do not share it. Each pair on a trail may fight; the loser is driven off (or, if its
// boss falls, taken in by the winner). Rivalry keeps the big bands few and feeds drifters back to be recruited again.
void Simulation::gang_wars_day() {
    const Params& p = world_.params;
    for (std::size_t e = 0; e < edge_gangs_.size(); ++e) {
        if (edge_gangs_[e].size() < 2) continue;
        const std::vector<int> ids = edge_gangs_[e];
        for (std::size_t i = 0; i < ids.size(); ++i)
            for (std::size_t j = i + 1; j < ids.size(); ++j) {
                Gang* a = find_gang(ids[i]);
                Gang* b = find_gang(ids[j]);
                if (!a || !b || !a->active || !b->active || a->edge != static_cast<EdgeId>(e) || b->edge != static_cast<EdgeId>(e)) continue;
                if (p.gang_tiers && is_band(*a) && is_band(*b)) continue;  // two small bands keep out of each other's way; pooling is the join rule
                if (rng_.chance(p.gang_rival_fight_chance)) gang_war(*a, *b, static_cast<EdgeId>(e));
            }
    }
}

void Simulation::gang_war(Gang& a, Gang& b, EdgeId eid) {
    const Params& p = world_.params;
    const double sa = gang_strength(a), sb = gang_strength(b);
    const bool a_wins = rng_.chance(sa * sa / (sa * sa + sb * sb));
    Gang& win = a_wins ? a : b;
    Gang& lose = a_wins ? b : a;
    const double sw = a_wins ? sa : sb, sl = a_wins ? sb : sa;
    const int win_before = win.men, lose_before = lose.men;
    const int lose_loss = std::max(1, static_cast<int>(std::lround(lose.men * p.gang_rival_loser_loss)));
    const int win_loss = static_cast<int>(std::lround(win.men * p.gang_rival_winner_loss * sl / (sl + sw)));
    gang_lose_men(win, win_loss);
    gang_lose_men(lose, lose_loss);
    gang_fought(win);
    gang_fought(lose);
    win.competence = std::min(1.0, win.competence + p.gang_win_competence);
    const double plunder = lose.cash * p.gang_rival_loot_share;
    win.cash += plunder;
    lose.cash -= plunder;

    const bool boss_fell = rng_.chance(p.gang_boss_fall_chance * static_cast<double>(lose_before - lose.men) / std::max(1, lose_before));
    const std::string odds = strf("both worked the %s; strength %.0f vs %.0f (a veteran of %.0f fights counts %.1fx)", ename(eid).c_str(), sw, sl,
                                  p.gang_veteran_fights, p.gang_veteran_strength);
    log_.with({field("winner", win.id), field("loser", lose.id), field("edge", ename(eid)), field("winner_men", win_before), field("loser_men", lose_before), field("loser_lost", lose_loss), field("winner_lost", win_loss), field("boss_fell", boss_fell ? 1 : 0)});
    log_.add(world_.today, EventType::Gang, "gang_war", win.hideout,
             strf("Gang #%d (%d men) beats gang #%d (%d men) on the %s: %d killed or gone against %d", win.id, win_before, lose.id, lose_before,
                  ename(eid).c_str(), lose_loss, win_loss),
             odds);

    if (boss_fell && lose.men > 0 && rng_.chance(p.gang_merge_chance)) {
        // The loser's boss is dead and the survivors have nobody to follow: the winner takes them in. The band may now
        // be bigger than its boss can hold, and the breakdown roll deals with that.
        const int taken = lose.men;
        free_town(lose, "its boss fell in a fight with a rival gang");
        gang_absorb(win, lose);
        log_.with({field("winner", win.id), field("loser", lose.id), field("men", taken), field("total", win.men)});
        log_.add(world_.today, EventType::Gang, "gang_merged", win.hideout,
                 strf("Gang #%d takes in the %d survivors of gang #%d and now has %d men", win.id, taken, lose.id, win.men),
                 strf("gang #%d's boss was killed in the fight and his men had no one to follow; gang #%d can hold about %.0f men together",
                      lose.id, win.id, gang_manageable(win)));
        return;
    }
    if (boss_fell) {  // no merger: a veteran takes over, shakier than the man before
        const double vets = lose.men > 0 ? static_cast<double>(gang_veterans(lose)) / lose.men : 0.0;
        lose.competence = 0.1 + 0.6 * vets * rng_.next_double();
    }
    if (lose.men < p.gang_min_men) {
        gang_dissolve(lose, "beaten by a rival gang");
    } else if (lose.ruling < 0) {
        lose.edge = -1;  // driven off the trail; it picks another at its next review
    }
}

// ---------------------------------------------------------------- bushwhacking

// Bands also waylay lone men leaving the hideout town: a miner with his pay, a rancher, a gambler on a winning night.
// A resident who is robbed or killed is missed and found, and his town gets angry; a stranger is nobody's business but his
// own (a little fear, not anger). A man with connections elsewhere, an officer on leave or a mine owner, is another matter.
void Simulation::bushwhack(Gang& g) {
    const Params& p = world_.params;
    if (!p.bush_stages || !g.active || g.edge < 0 || !rng_.chance(p.bush_rate)) return;
    Node& h = world_.nodes[World::idx(g.hideout)];
    const double take = std::min(h.cash * 0.02, p.bush_take);
    if (take <= 0) return;
    ++bushwhacks_;
    h.cash -= take;   // what the victim was carrying came out of the town's money
    g.cash += take;
    const Edge& e = world_.edges[World::idx(g.edge)];
    const bool notable = rng_.chance(p.bush_notable);
    const bool resident = notable || rng_.chance(p.bush_resident);
    const bool killed = notable ? rng_.chance(0.8) : rng_.chance(p.bush_kill);
    if (!resident) {   // a stranger: a few people notice
        h.fear += 0.2;
        h.outrage += p.bush_outrage_stranger;
        if (killed) { ++bush_killed_; gang_deed_edge(g, g.edge, p.fame_kill * 0.3); }
        return;
    }
    Bushwhack b;
    b.due = world_.today + 2 + static_cast<long>(rng_.next_below(7));
    // His people live here, or in the town at the far end of the road.
    b.town = rng_.chance(0.6) ? g.hideout : (e.a == g.hideout ? e.b : e.a);
    if (world_.nodes[World::idx(b.town)].fixed_market) b.town = g.hideout;
    b.edge = g.edge;
    b.killed = killed;
    b.notable = notable;
    b.outrage = notable ? p.bush_notable_outrage : p.bush_outrage_resident * (killed ? 2.0 : 1.0);
    b.gang = rng_.chance(p.bush_seen) ? g.id : -1;
    if (killed) ++bush_killed_;
    if (notable) ++bush_notable_;
    bush_pending_.push_back(b);
}

// A missing man is found, or comes home robbed: the town learns what happened and gets angry.
void Simulation::bushwhack_reports() {
    const Params& p = world_.params;
    if (bush_pending_.empty()) return;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < bush_pending_.size(); ++i) {
        Bushwhack& b = bush_pending_[i];
        if (b.due > world_.today) { bush_pending_[keep++] = b; continue; }
        Node& town = world_.nodes[World::idx(b.town)];
        town.outrage += b.outrage;
        const std::string road = ename(b.edge);
        if (b.gang >= 0) {
            if (Gang* g = find_gang(b.gang)) gang_deed_edge(*g, b.edge, p.fame_kill * (b.notable ? 3.0 : 1.0));
        }
        if (b.notable) {
            const Edge& e = world_.edges[World::idx(b.edge)];
            log_.with({field("node", town.name), field("edge", road), field("gang", b.gang), field("outrage", b.outrage)});
            log_.add(world_.today, EventType::Robbery, "bushwhack_notable", b.town,
                     strf("Horror Scene: Officer Found Robbed and Murdered on the Road From %s to %s", nname(e.a).c_str(), nname(e.b).c_str()),
                     strf("a man with friends in high places was waylaid near %s; the outcry reached well beyond the town", town.name.c_str()));
            if (b.gang >= 0) {
                if (Gang* g = find_gang(b.gang)) send_cavalry(b.town, *g, "an officer was murdered on the road");
            }
        } else if (b.killed) {
            log_.with({field("node", town.name), field("edge", road), field("gang", b.gang), field("outrage", b.outrage)});
            log_.add(world_.today, EventType::Robbery, "bushwhack_found", b.town,
                     strf("%s Man Found Robbed and Murdered on the %s", town.name.c_str(), road.c_str()),
                     strf("a resident of %s was waylaid; his people are angry (outrage +%.1f)", town.name.c_str(), b.outrage), false);
        }
    }
    bush_pending_.resize(keep);
}

// The Army answers the murder of one of its own, wherever a post is near enough to send men.
void Simulation::send_cavalry(NodeId near_to, Gang& g, const std::string& why) {
    const Params& p = world_.params;
    const Node& at = world_.nodes[World::idx(near_to)];
    NodeId best = -1;
    double best_d = p.army_range;
    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        const Node& post = world_.nodes[i];
        if (post.garrison < 50 || post.fixed_market) continue;
        const double d = dist(at.x, at.y, post.x, post.y);
        if (d < best_d) { best_d = d; best = static_cast<NodeId>(i); }
    }
    if (best < 0) return;
    const int men = std::min<int>(static_cast<int>(world_.nodes[World::idx(best)].garrison * 0.1), std::max(p.agency_min_men, static_cast<int>(std::ceil(p.law_overmatch * g.men))));
    if (men < 4) return;
    Posse q;
    q.id = next_posse_id_++;
    q.kind = PosseKind::Army;
    q.origin = best;
    q.men = men;
    q.gang_id = g.id;
    q.days_left = std::ceil(best_d / p.posse_speed) + 2;
    log_.with({field("post", nname(best)), field("men", men), field("gang", g.id)});
    log_.add(world_.today, EventType::Law, "cavalry_sent", best,
             strf("%s sends %d cavalry after gang #%d (%d men)", nname(best).c_str(), men, g.id, g.men), why);
    posses_.push_back(q);
}

}  // namespace frontier
