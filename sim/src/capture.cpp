// A gang taking over a town, and what comes of it.
//
// Historically this happened by corruption more than conquest: a gang that spends money in a town and
// fences its loot there gains influence, buys the marshal, and ends up running the place. It could only
// happen where the law had no backup. The question is whether the gang can run a town as well as it can
// rob one. A competent boss takes a moderate cut and the town may thrive; a clumsy one squeezes harder as
// trade dries up, which dries up trade further, until the gang starves, splits, or is hanged.
#include <algorithm>
#include <cmath>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

// Influence is the share of the town's economy the gang's spending makes up, discounted where the law is
// well funded. A big town's economy dwarfs any gang, so only small, poor, isolated towns can be captured.
void Simulation::capture_progress(Gang& g, double spent) {
    const Params& p = world_.params;
    Node& h = world_.nodes[World::idx(g.hideout)];
    if (h.fixed_market || h.captor >= 0) return;
    // The town's whole economy includes the farmers and ranchers around it, not just its trade income.
    const double economy = h.income_ema + h.base_population * p.support_income_per_person + 50.0;
    const double gain = p.capture_gain * spent / economy / (1.0 + h.law_fund / p.capture_law_resist);
    g.influence = g.influence * (1.0 - p.capture_decay) + gain;
    g.last_spend = spent;
    if (g.influence < p.capture_threshold || g.men < p.capture_min_men || has_backup(g.hideout)) return;

    // The new boss is usually mediocre: skewed toward low competence.
    const double u = rng_.next_double();
    g.competence = 0.1 + 0.8 * u * u;
    g.tribute = p.tribute_base_min + p.tribute_spread * (1.0 - g.competence);
    g.ruling = g.hideout;
    h.captor = g.id;
    h.toll = g.tribute;
    h.captured_day = world_.today;
    h.captured_income = std::max(h.income_ema, 1.0);
    log_.with({field("gang", g.id), field("men", g.men), field("node", h.name), field("pop", h.population), field("competence", g.competence), field("tribute", g.tribute), field("spent", spent)});
    log_.add(world_.today, EventType::Gang, "gang_takeover", g.hideout,
             strf("Gang #%d (%d men) takes over %s (pop %s)", g.id, g.men, h.name.c_str(), with_commas(h.population).c_str()),
             strf("its spending of %s/day is large against the town's income of %s/day, so its influence reached %.1f; "
                  "the law fund held only %s and no railroad, express or stock interest was within %.0f miles to back "
                  "the law. Boss competence %.2f; he will take %.0f%% of what moves through town",
                  money(spent).c_str(), money(h.income_ema).c_str(), g.influence, money(h.law_fund).c_str(),
                  p.backup_range, g.competence, g.tribute * 100));
}

// Daily business of a ruling gang: collect tribute, adjust the squeeze, and risk losing control.
void Simulation::rule_town(Gang& g) {
    const Params& p = world_.params;
    Node& h = world_.nodes[World::idx(g.ruling)];
    if (h.captor != g.id) {
        g.ruling = -1;
        return;
    }
    const double take = std::min(h.cash, g.tribute * h.inflow_today);
    h.cash -= take;
    g.cash += take;

    // A broke gang squeezes harder; a flush one eases off toward its natural rate. Squeezing a town hurts its
    // trade (the toll is added to every price), which is how a clumsy boss spirals.
    const double upkeep = g.men * p.gang_upkeep;
    const double natural = p.tribute_base_min + p.tribute_spread * (1.0 - g.competence);
    if (g.cash < upkeep * 5)
        g.tribute = std::min(p.tribute_cap, g.tribute + p.tribute_squeeze * 0.1);
    else if (g.cash > upkeep * 20)
        g.tribute = std::max(natural, g.tribute - p.tribute_squeeze * 0.04);
    h.toll = g.tribute;

    if (rng_.chance(p.infight_rate * (1.0 - g.competence) * (1.0 - g.competence))) {
        const int killed = std::max(1, static_cast<int>(std::ceil(g.men * 0.3)));
        g.men -= killed;
        g.competence *= 0.85;
        log_.with({field("gang", g.id), field("node", h.name), field("killed", killed), field("competence", g.competence)});
        log_.add(world_.today, EventType::Gang, "gang_infighting", g.ruling,
                 strf("Gang #%d tears at itself in %s: %d men killed in a fight over the spoils", g.id, h.name.c_str(), killed),
                 strf("the boss's competence (%.2f) is too low to keep his lieutenants in line; the town pays %.0f%% on everything",
                      g.competence, g.tribute * 100));
        if (g.men < p.gang_min_men) gang_dissolve(g, "the gang tore itself apart");
    }
}

// Called by gang_dissolve: the town is no longer ruled.
void Simulation::free_town(Gang& g, const char* why) {
    if (g.ruling < 0) return;
    Node& h = world_.nodes[World::idx(g.ruling)];
    if (h.captor == g.id) {
        log_.with({field("gang", g.id), field("node", h.name), field("days", world_.today - h.captured_day), field("income", h.income_ema)});
        log_.add(world_.today, EventType::Gang, "town_freed", g.ruling,
                 strf("%s is free of gang #%d after %ld days", h.name.c_str(), g.id, world_.today - h.captured_day),
                 strf("%s; income is now %s/day against %s when the gang took over (pop %s)", why,
                      money(h.income_ema).c_str(), money(h.captured_income).c_str(), with_commas(h.population).c_str()));
        h.captor = -1;
        h.toll = 0;
    }
    g.ruling = -1;
}

// Townspeople rise against a ruling gang when the damage is bad enough. They need enough people, and they
// can hire a leader with whatever the town has left.
void Simulation::try_vigilance(NodeId n) {
    const Params& p = world_.params;
    Node& nd = world_.nodes[World::idx(n)];
    if (nd.captor < 0) return;
    Gang* g = find_gang(nd.captor);
    if (!g || !g->active) {
        nd.captor = -1;
        nd.toll = 0;
        return;
    }
    if (world_.today - nd.captured_day < p.vigilance_min_days || nd.market_population() < p.vigilance_min_pop) return;
    for (const Posse& q : posses_)
        if (q.active && q.kind == PosseKind::Vigilance && q.origin == n) return;

    const double drop = std::max(0.0, 1.0 - nd.income_ema / nd.captured_income);
    const double grievance = drop + g->tribute + nd.hunger;
    if (!rng_.chance(p.vigilance_rate * grievance)) return;
    const int men = static_cast<int>(std::min<double>(p.posse_max_men, std::floor(nd.market_population() / p.vigilance_pop_per_man)));
    if (men < p.posse_min_men) return;

    double lead_days = 0, fee = 0;
    Leader* hire = pick_leader(nd, 2, nd.cash * 0.15, 0, lead_days, fee);
    Posse q;
    q.id = next_posse_id_++;
    q.kind = PosseKind::Vigilance;
    q.origin = n;
    q.men = men;
    q.gang_id = g->id;
    q.days_left = lead_days + 2;
    std::string led;
    if (hire) {
        hire->busy = true;
        pay_leader(*hire, fee);
        nd.cash -= fee;
        ++hire->jobs;
        q.leader_id = hire->id;
        led = strf(", with %s hired to lead them (fee %s)", hire->name.c_str(), money(fee).c_str());
    }
    log_.with({field("node", nd.name), field("men", men), field("gang", g->id)});
    log_.add(world_.today, EventType::Law, "vigilance_formed", n,
             strf("Citizens of %s organize a vigilance committee of %d men against gang #%d (%d men)%s", nd.name.c_str(), men,
                  g->id, g->men, led.c_str()),
             strf("grievances: income is down %.0f%% since the takeover, the gang takes %.0f%% of all trade, hunger %.2f",
                  drop * 100, g->tribute * 100, nd.hunger));
    posses_.push_back(q);
}

}  // namespace frontier
