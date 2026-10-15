// Law enforcement: town posses, leaders for hire, and the private interests (railroads, express
// companies, stock associations) that hire detective agencies when robbery costs them enough.
//
// The escalation ladder, bottom to top:
//   1. a town raises a posse from its own tax fund (optionally led by a hired leader)
//   2. a private interest whose shipments keep getting robbed hires detectives, who travel far and hit hard
//   3. (not yet modelled) state or federal forces
// A town with no interest nearby has no backup above rung 1, which is what makes capture possible.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

namespace {
double dist(double x1, double y1, double x2, double y2) { return std::hypot(x1 - x2, y1 - y2); }
}  // namespace

// ---------------------------------------------------------------- leaders

// The most skilled free leader within reach whose fee fits the budget (after keeping `reserve` for the men).
Leader* Simulation::pick_leader(const Node& at, double travel_days, double budget, double reserve, double& lead_days,
                                double& fee) {
    const Params& p = world_.params;
    Leader* best = nullptr;
    for (Leader& l : leaders_) {
        if (!l.alive || l.busy) continue;
        const Node& ln = world_.nodes[World::idx(l.at)];
        const double d = dist(at.x, at.y, ln.x, ln.y);
        if (d > p.leader_range || (best && l.skill <= best->skill)) continue;
        const double days = std::ceil(d / p.posse_speed);
        const double cost = p.leader_fee_per_day * (days + travel_days + 5);
        if (cost + reserve > budget) continue;
        best = &l;
        lead_days = days;
        fee = cost;
    }
    return best;
}

// ---------------------------------------------------------------- town posses

void Simulation::try_form_posse(NodeId n) {
    const Params& p = world_.params;
    Node& nd = world_.nodes[World::idx(n)];
    if (nd.fixed_market || nd.captor >= 0 || nd.outrage < p.posse_outrage_threshold) return;
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
    double lead_days = 0, fee = 0;
    Leader* hire = pick_leader(nd, travel_days, nd.law_fund, p.posse_min_men_led * p.posse_cost_per_man, lead_days, fee);
    int men = untrained;
    if (hire) men = static_cast<int>(std::min<double>(p.posse_max_men, std::floor((nd.law_fund - fee) / p.posse_cost_per_man)));
    else if (men < p.posse_min_men) return;

    nd.last_posse_day = world_.today;
    nd.law_fund -= men * p.posse_cost_per_man + fee;
    nd.cash += men * p.posse_cost_per_man;  // the men's pay is spent in town
    nd.inflow_today += men * p.posse_cost_per_man;
    Posse q;
    q.id = next_posse_id_++;
    q.origin = n;
    q.men = men;
    q.gang_id = target->id;
    q.days_left = lead_days + travel_days;
    if (hire) {
        hire->busy = true;
        pay_leader(*hire, fee);
        ++hire->jobs;
        q.leader_id = hire->id;
        log_.with({field("node", nd.name), field("leader", hire->name), field("fee", fee), field("men", men), field("gang", target->id)});
        log_.add(world_.today, EventType::Law, "posse_hired_leader", n,
                 strf("%s hires %s (skill %.1f, fee %s) to lead a posse of %d men against gang #%d (%d men) near the %s",
                      nd.name.c_str(), hire->name.c_str(), hire->skill, money(fee).c_str(), men, target->id, target->men,
                      ename(target->edge).c_str()),
                 strf("outrage %.1f over robberies on nearby trails; the gang has taken %s; the fund could field only %d "
                      "untrained men, but a seasoned leader multiplies their strength %.1fx",
                      nd.outrage, money(target->loot).c_str(), untrained, hire->skill));
    } else {
        log_.with({field("node", nd.name), field("men", men), field("gang", target->id)});
        log_.add(world_.today, EventType::Law, "posse_raised", n,
                 strf("%s raises a posse of %d men against gang #%d (%d men) near the %s", nd.name.c_str(), men,
                      target->id, target->men, ename(target->edge).c_str()),
                 strf("outrage %.1f over robberies on nearby trails; the gang has taken %s so far; funded by %s of local "
                      "taxes; no leader for hire was in reach",
                      nd.outrage, money(target->loot).c_str(), money(men * p.posse_cost_per_man).c_str()));
    }
    posses_.push_back(q);
}

// Resolves a posse, vigilance committee or detective party on arrival.
void Simulation::posse_day(Posse& q) {
    const Params& p = world_.params;
    if (!q.active) return;
    if (--q.days_left > 0) return;
    q.active = false;
    Node& origin = world_.nodes[World::idx(q.origin)];
    Leader* leader = q.leader_id >= 0 ? find_leader(q.leader_id) : nullptr;
    Interest* interest = q.interest_id >= 0 ? &interests_[World::idx(q.interest_id)] : nullptr;
    const std::string led = leader ? " led by " + leader->name : std::string();

    std::string who;
    switch (q.kind) {
        case PosseKind::Town:      who = "The posse from " + origin.name + led; break;
        case PosseKind::Vigilance: who = "The vigilance committee of " + origin.name + led; break;
        case PosseKind::Agency:    who = "The detectives hired by the " + (interest ? interest->name : std::string("owners")); break;
    }

    Gang* g = find_gang(q.gang_id);
    if (!g || !g->active) {
        log_.with({field("origin", origin.name), field("gang", q.gang_id)});
        log_.add(world_.today, EventType::Law, "posse_no_trace", q.origin, strf("%s find no trace of gang #%d", who.c_str(), q.gang_id),
                 "the gang had already broken up or moved on");
        if (leader) release_leader(*leader, q.origin);
        return;
    }

    const double skill = leader ? leader->skill : (q.kind == PosseKind::Agency ? p.agency_skill : 1.0);
    const double terrain = q.kind == PosseKind::Vigilance ? 1.0 : p.gang_terrain_advantage;  // a town is home ground
    const double P = q.men * skill;
    const double G = g->men * terrain;
    const bool wins = rng_.chance(P * P / (P * P + G * G));
    const std::string skill_note = skill != 1.0 ? strf(" x%.1f skill", skill) : std::string();
    const std::string odds =
        strf("%d men%s vs %d bandits (terrain favored the gang %.1fx)", q.men, skill_note.c_str(), g->men, terrain);

    if (wins) {
        const double seized = g->cash;
        const double bonus = leader ? seized * p.leader_bonus_share : 0.0;
        if (q.kind == PosseKind::Agency) {
            if (interest) { interest->fund += seized; ++interest->wins; }
        } else {
            origin.cash += seized - bonus;
            origin.outrage = 0;
        }
        const char* verb = q.kind == PosseKind::Vigilance ? "destroy gang #%d and hang its boss, seizing %s"
                           : q.kind == PosseKind::Agency  ? "run down gang #%d, recovering %s"
                                                          : "destroy gang #%d, seizing %s";
        log_.with({field("origin", origin.name), field("gang", g->id), field("men", q.men), field("seized", seized), field("party", q.kind == PosseKind::Vigilance ? "vigilance" : q.kind == PosseKind::Agency ? "agency" : "posse")});
        log_.add(world_.today, EventType::Law, "posse_victory", q.origin,
                 who + " " + strf(verb, g->id, money(seized).c_str()), odds);
        g->cash = 0;
        gang_dissolve(*g, q.kind == PosseKind::Vigilance ? "hanged by a vigilance committee"
                          : q.kind == PosseKind::Agency  ? "run down by hired detectives"
                                                         : "destroyed by a posse");
        if (leader) {
            pay_leader(*leader, bonus);
            ++leader->wins;
            leader->skill = std::min(p.leader_skill_cap, leader->skill + p.leader_skill_gain);  // experience
            release_leader(*leader, q.origin);
        } else if (q.kind != PosseKind::Agency && rng_.chance(p.leader_emerge_chance)) {
            // A man who carried himself well in the fight becomes someone other towns will hire.
            Leader ld;
            ld.id = next_leader_id_++;
            ld.name = strf("a deputy of %s (#%d)", origin.name.c_str(), ld.id);
            ld.at = q.origin;
            ld.skill = rng_.uniform(1.1, 1.5);
            log_.with({field("node", origin.name), field("leader", ld.name), field("skill", ld.skill)});
            log_.add(world_.today, EventType::Law, "leader_emerged", q.origin,
                     strf("%s emerges as a man for hire after the fight", ld.name.c_str()),
                     "a man who organized the others under fire earned a reputation");
            leaders_.push_back(std::move(ld));
        }
        return;
    }

    const int losses = std::max(1, static_cast<int>(std::lround(g->men * 0.25)));
    g->men -= losses;
    bool found = false;
    for (auto& gr : g->grudge)
        if (gr.first == q.origin) { gr.second += 2; found = true; }
    if (!found) g->grudge.push_back({q.origin, 2.0});
    std::string aftermath = strf("the gang lost %d but now avoids %s's trails", losses, origin.name.c_str());
    if (q.kind == PosseKind::Vigilance) {
        // Reprisal: the boss squeezes harder and the townspeople who backed the committee flee.
        g->tribute = std::min(p.tribute_cap, g->tribute + 0.1);
        const double fled = origin.population * 0.15;
        origin.population -= fled;
        origin.drifters += fled * 0.1;
        aftermath = strf("the boss raised his take to %.0f%% and %.0f townspeople fled the reprisals", g->tribute * 100, fled);
    } else {
        origin.outrage *= 0.5;
    }
    log_.with({field("origin", origin.name), field("gang", g->id), field("men", q.men)});
    log_.add(world_.today, EventType::Law, "posse_routed", q.origin, strf("%s is routed by gang #%d", who.c_str(), g->id),
             strf("%s; %s", odds.c_str(), aftermath.c_str()));
    if (g->men < p.gang_min_men) gang_dissolve(*g, "mauled by its pursuers");
    if (leader) {
        if (rng_.chance(p.leader_death_prob)) {
            leader->alive = false;
            world_.record(MoneyFlow::LeaderExit, -leader->cash);
            leader->cash = 0;
            log_.with({field("leader", leader->name), field("origin", origin.name)});
            log_.add(world_.today, EventType::Law, "leader_killed", q.origin, strf("%s is killed leading the posse", leader->name.c_str()),
                     strf("his men were outmatched (strength %.0f vs %.0f)", P, G));
        } else {
            leader->skill = std::max(0.5, leader->skill - p.leader_skill_gain);
            release_leader(*leader, q.origin);
        }
    }
}

// ---------------------------------------------------------------- private interests and detectives

bool Simulation::interest_active(const Interest& in) const { return world_.today >= in.from && world_.today < in.until; }

// A town has backup if a private interest operates within reach: the interest's detectives will come for
// anything that hurts it. An isolated town has only its own posse, so a determined gang can take it over.
bool Simulation::has_backup(NodeId n) const {
    const Node& nd = world_.nodes[World::idx(n)];
    // Government is the other kind of backup: a state capital, or an Army post close enough to send troops.
    if (std::find(nd.tags.begin(), nd.tags.end(), "state_capital") != nd.tags.end()) return true;
    for (const Node& post : world_.nodes)
        if (post.garrison >= 100 && dist(nd.x, nd.y, post.x, post.y) <= world_.params.backup_range * 0.4) return true;
    for (const Interest& in : interests_) {
        if (!interest_active(in)) continue;
        for (NodeId m : in.nodes) {
            const Node& o = world_.nodes[World::idx(m)];
            if (dist(nd.x, nd.y, o.x, o.y) <= world_.params.backup_range) return true;
        }
    }
    return false;
}

// Railroads and express companies carry goods; stock associations care about cattle. A robbery counts
// against an interest if it happens on a trail touching one of its stations.
void Simulation::note_interest_loss(EdgeId eid, const Gang& g, GoodId cargo, double value) {
    const Edge& e = world_.edges[World::idx(eid)];
    const bool cattle = good(cargo).self_propelled;
    for (Interest& in : interests_) {
        if (!interest_active(in) || (in.kind == "stock_association") != cattle) continue;
        const bool touches = std::find(in.nodes.begin(), in.nodes.end(), e.a) != in.nodes.end() ||
                             std::find(in.nodes.begin(), in.nodes.end(), e.b) != in.nodes.end();
        if (!touches) continue;
        in.loss_ema += value;
        in.losses_total += value;
        bool found = false;
        for (auto& v : in.victims)
            if (v.first == g.id) { v.second += value; found = true; }
        if (!found) in.victims.push_back({g.id, value});
    }
}

void Simulation::interests_day() {
    const Params& p = world_.params;
    for (Interest& in : interests_) {
        if (!interest_active(in)) continue;
        // Funding: a standing budget plus dues skimmed off the trade through its stations.
        in.fund += in.budget_per_day;
        world_.record(MoneyFlow::InterestBudget, in.budget_per_day);
        for (NodeId m : in.nodes) {
            Node& nd = world_.nodes[World::idx(m)];
            if (nd.fixed_market) continue;
            const double dues = std::min(nd.cash, in.dues_rate * nd.inflow_today);  // paid out of the town's cash
            nd.cash -= dues;
            in.fund += dues;
        }
        in.loss_ema *= 1.0 - p.interest_loss_decay;
        for (auto& v : in.victims) v.second *= 1.0 - p.interest_loss_decay;
        in.victims.erase(std::remove_if(in.victims.begin(), in.victims.end(),
                                        [&](const auto& v) {
                                            const Gang* g = find_gang(v.first);
                                            return v.second < 1.0 || !g || !g->active;
                                        }),
                         in.victims.end());

        if (world_.today - in.last_op_day < p.agency_cooldown || in.loss_ema < p.agency_loss_threshold ||
            in.fund < p.agency_min_fund)
            continue;

        // Go after whichever gang has cost the interest the most.
        const Gang* target = nullptr;
        double worst = 0;
        for (const auto& v : in.victims) {
            const Gang* g = find_gang(v.first);
            if (g && g->active && v.second > worst) { worst = v.second; target = g; }
        }
        if (!target) continue;
        const Edge& e = world_.edges[World::idx(target->edge)];
        const NodeId end = world_.route_days(in.hq, e.a) <= world_.route_days(in.hq, e.b) ? e.a : e.b;
        if (!world_.reachable(in.hq, end)) continue;
        const int men = static_cast<int>(std::min<double>(p.agency_max_men, std::floor(in.fund * 0.7 / p.agency_cost_per_man)));
        if (men < p.agency_min_men) continue;

        in.fund -= men * p.agency_cost_per_man;
        Node& hq = world_.nodes[World::idx(in.hq)];  // the operatives' pay is spent where they are hired
        if (hq.fixed_market) {
            world_.record(MoneyFlow::EastSpending, -men * p.agency_cost_per_man);
        } else {
            hq.cash += men * p.agency_cost_per_man;
            hq.inflow_today += men * p.agency_cost_per_man;
        }
        in.last_op_day = world_.today;
        ++in.ops;
        Posse q;
        q.id = next_posse_id_++;
        q.kind = PosseKind::Agency;
        q.origin = in.hq;
        q.men = men;
        q.gang_id = target->id;
        q.interest_id = static_cast<int>(&in - interests_.data());
        q.days_left = std::ceil(world_.route_days(in.hq, end)) + 2;
        log_.with({field("interest", in.name), field("men", men), field("gang", target->id), field("recent_loss", worst)});
        log_.add(world_.today, EventType::Law, "detectives_hired", in.hq,
                 strf("The %s hires detectives: %d operatives sent against gang #%d (%d men) near the %s", in.name.c_str(),
                      men, target->id, target->men, ename(target->edge).c_str()),
                 strf("the gang has cost it %s in recent weeks (total losses %s); its fund of %s can pay for them, and "
                      "it can reach the trail by %.0f days of travel",
                      money(worst).c_str(), money(in.losses_total).c_str(), money(in.fund + men * p.agency_cost_per_man).c_str(),
                      world_.route_days(in.hq, end)));
        posses_.push_back(q);
    }
}

}  // namespace frontier
