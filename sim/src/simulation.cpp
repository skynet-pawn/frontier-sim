#include "frontier/simulation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "frontier/date.h"
#include "frontier/format.h"

namespace frontier {

namespace {
// Population thresholds at which a town gets a "grows past / shrinks below" log line.
constexpr double kPopBands[] = {100, 250, 500, 1000, 2500, 5000, 10000, 25000};
constexpr int kBandCount = static_cast<int>(sizeof kPopBands / sizeof kPopBands[0]);
}  // namespace

Simulation::Simulation(const WorldDef& def, const Scenario& scenario)
    : world_(def, scenario.params, scenario.start), rng_(scenario.seed), sc_(scenario) {
    edge_gangs_.resize(world_.edges.size());
    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        Node& n = world_.nodes[i];
        while (n.pop_band < kBandCount && n.population >= kPopBands[n.pop_band]) ++n.pop_band;
    }
    for (int i = 0; i < sc_.traders; ++i)
        spawn_trader(static_cast<NodeId>(rng_.next_below(world_.nodes.size())));
    for (const LeaderSeed& l : sc_.leaders) {
        Leader ld;
        ld.id = next_leader_id_++;
        ld.name = l.name;
        ld.at = l.home;
        ld.skill = l.skill;
        leaders_.push_back(std::move(ld));
    }
    for (const GangSeed& g : sc_.gangs) create_gang(g.hideout, g.men, "seeded in scenario.json");
}

void Simulation::step() {
    update_network();
    apply_shocks();
    economy_day();
    traders_day();
    crime_day();
    end_of_day();
    ++world_.today;
}

// ---------------------------------------------------------------- network and shocks

void Simulation::update_network() {
    std::vector<EdgeId> opened, closed;
    if (!world_.update_active_edges(&opened, &closed)) return;
    world_.rebuild_routes();
    auto describe = [&](EdgeId id, const char* verb) {
        const Edge& e = world_.edges[World::idx(id)];
        const std::string cause = e.note.empty() ? "scheduled in world.json" : e.note;
        log_.add(world_.today, EventType::Network, e.a,
                 strf("%s between %s and %s %s (%.0f miles)", mode_name(e.mode), nname(e.a).c_str(),
                      nname(e.b).c_str(), verb, e.miles),
                 cause);
    };
    for (EdgeId id : opened) describe(id, "opens");
    for (EdgeId id : closed) describe(id, "closes");
}

void Simulation::apply_shocks() {
    for (std::size_t i = 0; i < sc_.shocks.size(); ++i) {
        const ShockDef& sh = sc_.shocks[i];
        if (sh.date != world_.today) continue;
        if (sh.kind == "stock_loss") {
            for (std::size_t n = 0; n < world_.nodes.size(); ++n) {
                Node& node = world_.nodes[n];
                if (std::find(node.tags.begin(), node.tags.end(), sh.tag) == node.tags.end()) continue;
                for (GoodId g : sh.goods) {
                    const double lost = node.stock[World::idx(g)] * sh.fraction;
                    node.stock[World::idx(g)] -= lost;
                    log_.add(world_.today, EventType::Shock, static_cast<NodeId>(n),
                             strf("%s: %s loses %s %s", sh.name.c_str(), node.name.c_str(), with_commas(lost).c_str(),
                                  good(g).unit.c_str()),
                             sh.cause);
                }
            }
        } else if (sh.kind == "rush") {
            Node& node = world_.nodes[World::idx(sh.node)];
            node.population += sh.people;
            node.cash += sh.people * sh.cash_per_person;
            // Newcomers arrive on expectations of income that has not shown up yet.
            node.income_ema += sh.people * world_.params.support_income_per_person * 0.8;
            node.capacity = std::max(node.capacity, node.population);
            log_.add(world_.today, EventType::Shock, sh.node,
                     strf("%s: %s people arrive at %s", sh.name.c_str(), with_commas(sh.people).c_str(),
                          node.name.c_str()),
                     sh.cause);
        }
    }
}

// ---------------------------------------------------------------- economy

void Simulation::economy_day() {
    const Params& p = world_.params;
    for (std::size_t ni = 0; ni < world_.nodes.size(); ++ni) {
        Node& n = world_.nodes[ni];
        const NodeId id = static_cast<NodeId>(ni);
        if (n.fixed_market) continue;

        // Production, limited by labor and by finite reserves (mines, buffalo herds).
        for (std::size_t i = 0; i < n.production.size(); ++i) {
            const ProductionDef& pd = n.production[i];
            if (world_.today < pd.from || world_.today >= pd.until) continue;
            const std::size_t g = World::idx(pd.good);
            if (!n.production_started[i]) {
                n.production_started[i] = 1;
                if (pd.from != LONG_MIN)
                    log_.add(world_.today, EventType::Production, id,
                             strf("%s: %s output begins (%.0f %s/day at full labor)", n.name.c_str(),
                                  good(pd.good).name.c_str(), pd.rate, good(pd.good).unit.c_str()),
                             "scheduled start in world.json (strike, new range, or new post)");
            }
            const double labor = pd.labor_pop > 0 ? std::min(1.0, n.population / pd.labor_pop) : 1.0;
            double amount = pd.rate * labor;
            if (n.reserve[g] >= 0) {
                amount = std::min(amount, n.reserve[g]);
                n.reserve[g] -= amount;
                if (n.reserve[g] <= 1e-6 && amount > 0) {
                    n.reserve[g] = 0;
                    log_.add(world_.today, EventType::Production, id,
                             strf("%s: %s is exhausted", n.name.c_str(), good(pd.good).name.c_str()),
                             "finite reserve fully extracted; output stops and the local economy loses its base");
                }
            }
            n.stock[g] += amount;
            n.stock[g] = std::min(n.stock[g], pd.rate * p.stock_cap_days);  // unsold output is lost
        }

        n.cash += n.gov_income;
        n.inflow_today += n.gov_income;

        // Consumption. Food shortfall is what drives hunger.
        double fed = 0;
        for (std::size_t g = 0; g < world_.goods.size(); ++g) {
            const GoodDef& gd = world_.goods[g];
            const double want = n.market_population() * gd.need_per_person_per_day;
            const double take = std::min(n.stock[g], want);
            n.stock[g] -= take;
            if (want > 0 && gd.food_share > 0) fed += gd.food_share * (take / want);
            const double extra = std::min(n.stock[g], n.extra_demand[g]);  // fort and agency contracts
            n.stock[g] -= extra;
        }
        // Substitution: people short of beef eat more flour (and vice versa) before they go hungry.
        for (std::size_t g = 0; g < world_.goods.size() && fed < 1.0; ++g) {
            const GoodDef& gd = world_.goods[g];
            if (gd.food_share <= 0) continue;
            const double full_diet = n.market_population() * gd.need_per_person_per_day / gd.food_share;
            const double more = std::min(n.stock[g], (1.0 - fed) * full_diet);
            n.stock[g] -= more;
            if (full_diet > 0) fed += more / full_diet;
        }
        for (std::size_t g = 0; g < world_.goods.size(); ++g) n.stock[g] *= 1.0 - world_.goods[g].spoilage_per_day;
        if (n.market_population() > 0) n.hunger += p.hunger_alpha * ((1.0 - std::min(1.0, fed)) - n.hunger);

        if (n.hunger > 0.25 && !n.starving && n.population >= 100) {
            n.starving = true;
            double cover = 1e9;
            std::string who;
            for (std::size_t g = 0; g < world_.goods.size(); ++g) {
                const GoodDef& gd = world_.goods[g];
                if (gd.food_share <= 0) continue;
                const double need = n.population * gd.need_per_person_per_day;
                const double days = need > 0 ? n.stock[g] / need : 1e9;
                if (days < cover) { cover = days; who = gd.name; }
            }
            const bool repeat = world_.today - n.last_famine_log < 90;
            n.last_famine_log = world_.today;
            log_.add(world_.today, EventType::Famine, id,
                     strf("%s is short of food (pop %s)", n.name.c_str(), with_commas(n.population).c_str()),
                     strf("%s stock covers %.1f days; town cash %s limits what traders can be paid", who.c_str(), cover,
                          money(n.cash).c_str()),
                     repeat);
        } else if (n.hunger < 0.08 && n.starving) {
            n.starving = false;
            log_.add(world_.today, EventType::Famine, id, strf("%s has food again", n.name.c_str()),
                     "supply caught up with demand", true);
        }

        if (n.transient_inflow) n.drifters += n.population * p.transient_inflow_rate;
        n.drifters *= 1.0 - p.drifter_decay;

        update_population(id);
    }
}

// A town holds as many people as its income supports (up to what land and water allow).
// This is how the trail moving away kills a town: income falls, so population does.
void Simulation::update_population(NodeId id) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(id)];
    // Farmers and ranchers around a town live off the land; everyone else lives off trade and government money.
    const double trade_pop = std::clamp(n.income_ema / p.support_income_per_person, 0.0, std::max(0.0, n.capacity - n.base_population));
    // Hunger drives away the people who depend on the market, not the farmers who feed themselves.
    const double hunger_keep = n.hunger > p.hunger_threshold ? 1.0 - n.hunger : 1.0;
    const double target = std::max(n.base_population + trade_pop * hunger_keep, p.floor_pop);
    // A town with almost nobody left has nobody to starve; newcomers arrive fed, so hunger can't lock it at zero.
    if (n.market_population() < p.ghost_pop) n.hunger = 0;

    if (n.population < target) {
        if (n.hunger <= p.hunger_threshold) {
            const double arrivals = std::min(target - n.population, n.population * p.pop_growth_rate + 0.1);
            n.population += arrivals;
            // Newcomers bring a grubstake, so a camp can start from nothing before the first freight wagon arrives.
            for (std::size_t g = 0; g < world_.goods.size(); ++g)
                if (world_.goods[g].food_share > 0)
                    n.stock[g] += arrivals * world_.goods[g].need_per_person_per_day * p.newcomer_provision_days;
        }
    } else {
        const double loss = (n.population - target) * p.pop_decline_rate;
        // People leaving take their savings with them; a dead mining camp doesn't keep its gold money.
        if (n.population > 0) n.cash -= n.cash * (loss / n.population) * p.emigrant_cash_share;
        n.population -= loss;
        n.drifters += loss * p.leaver_drifter_fraction;
    }
    n.population = std::max(n.population, 0.0);
    n.peak_pop = std::max(n.peak_pop, n.population);

    // Milestone log lines, with hysteresis so a town hovering at a threshold doesn't spam the log.
    while (n.pop_band < kBandCount && n.population >= kPopBands[n.pop_band]) {
        ++n.pop_band;
        const bool quiet_up = world_.today - n.last_pop_log < 60 && n.last_pop_dir < 0;  // flip-flopping at a threshold
        n.last_pop_log = world_.today;
        n.last_pop_dir = 1;
        log_.add(world_.today, EventType::Population, id,
                 strf("%s grows past %s", n.name.c_str(), with_commas(kPopBands[n.pop_band - 1]).c_str()),
                 strf("income %s/day supports ~%s people; hunger %.2f", money(n.income_ema).c_str(),
                      with_commas(n.income_ema / p.support_income_per_person).c_str(), n.hunger),
                 quiet_up);
    }
    while (n.pop_band > 0 && n.population < kPopBands[n.pop_band - 1] * 0.9) {
        --n.pop_band;
        const bool quiet_down = world_.today - n.last_pop_log < 60 && n.last_pop_dir > 0;
        n.last_pop_log = world_.today;
        n.last_pop_dir = -1;
        log_.add(world_.today, EventType::Population, id,
                 strf("%s shrinks below %s", n.name.c_str(), with_commas(kPopBands[n.pop_band]).c_str()),
                 strf("income fell to %s/day from a peak of %s; hunger %.2f", money(n.income_ema).c_str(),
                      money(n.income_peak).c_str(), n.hunger),
                 quiet_down);
    }
    if (!n.ghost && n.population < p.ghost_pop && n.peak_pop >= 150) {
        n.ghost = true;
        log_.add(world_.today, EventType::Population, id,
                 strf("%s becomes a ghost town (peak pop %s)", n.name.c_str(), with_commas(n.peak_pop).c_str()),
                 strf("income collapsed to %s/day from a peak of %s: no traffic or output left to live on",
                      money(n.income_ema).c_str(), money(n.income_peak).c_str()));
    } else if (n.ghost && n.population >= p.ghost_pop * 2) {
        n.ghost = false;
        log_.add(world_.today, EventType::Population, id, strf("%s is repopulating", n.name.c_str()),
                 strf("income recovered to %s/day", money(n.income_ema).c_str()));
    }
}

// ---------------------------------------------------------------- end of day

void Simulation::end_of_day() {
    const Params& p = world_.params;
    for (Node& n : world_.nodes) {
        if (n.fixed_market) continue;
        const double tax = std::min(n.cash, n.inflow_today * p.law_tax_rate);
        n.cash -= tax;
        n.law_fund += tax;
        n.income_ema += p.income_ema_alpha * (n.inflow_today - n.income_ema);
        n.income_peak = std::max(n.income_peak, n.income_ema);
        n.outrage *= 1.0 - p.outrage_decay;
        n.inflow_today = 0;
    }
    for (Edge& e : world_.edges) {
        e.robbed_ema += p.risk_ema_alpha * (e.robbed_today - e.robbed_ema);
        e.transits_ema += p.risk_ema_alpha * (e.transits_today - e.transits_ema);
        e.traffic_ema += 0.05 * (e.traffic_today - e.traffic_ema);
        e.robbed_today = e.transits_today = e.traffic_today = 0;
    }

    if (!traders_.empty()) {
        const double avg = profit_today_ / static_cast<double>(traders_.size());
        avg_profit_ema_ += 0.02 * (avg - avg_profit_ema_);
    }
    profit_today_ = 0;

    // Eastern capital follows visible opportunity: where price gaps exceed costs, new outfits appear.
    // This is the damper on excess profit, and the reason a depleted trade network recruits its own recovery.
    if (static_cast<int>(traders_.size()) < sc_.max_traders) {
        ensure_opportunities();
        double pull = 0;
        for (const auto& opp : opportunity_) {
            std::vector<double> best = opp;
            std::sort(best.begin(), best.end(), std::greater<double>());
            const std::size_t top = std::min<std::size_t>(6, best.size());
            double sum = 0;
            for (std::size_t i = 0; i < top; ++i) sum += best[i];
            if (top) pull = std::max(pull, std::clamp(sum / static_cast<double>(top) / p.trader_join_opportunity_ref, 0.0, 2.0));
        }
        if (rng_.chance(p.trader_join_rate * pull)) {
            std::vector<NodeId> markets;
            for (std::size_t i = 0; i < world_.nodes.size(); ++i)
                if (world_.nodes[i].fixed_market) markets.push_back(static_cast<NodeId>(i));
            if (!markets.empty()) spawn_trader(markets[rng_.next_below(markets.size())]);
        }
    }
    traders_.erase(std::remove_if(traders_.begin(), traders_.end(), [](const Trader& t) { return !t.alive; }),
                   traders_.end());
}

std::string Simulation::ename(EdgeId e) const {
    const Edge& ed = world_.edges[World::idx(e)];
    return nname(ed.a) + "-" + nname(ed.b);
}

Leader* Simulation::find_leader(int id) {
    for (Leader& l : leaders_)
        if (l.id == id) return &l;
    return nullptr;
}

void Simulation::release_leader(Leader& l, NodeId at) {
    l.busy = false;
    l.at = at;
}

Gang* Simulation::find_gang(int id) {
    for (Gang& g : gangs_)
        if (g.id == id) return &g;
    return nullptr;
}

}  // namespace frontier
