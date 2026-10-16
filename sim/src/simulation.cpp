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
    bought_value_[0].assign(world_.goods.size(), 0.0);
    bought_value_[1].assign(world_.goods.size(), 0.0);
    flour_bought_at_.assign(world_.nodes.size(), 0.0);
    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        Node& n = world_.nodes[i];
        while (n.pop_band < kBandCount && n.population >= kPopBands[n.pop_band]) ++n.pop_band;
    }
    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        const Node& n = world_.nodes[i];
        if (n.fixed_market) markets_.push_back(static_cast<NodeId>(i));
        if (n.fixed_market || n.kind == "railhead") gateways_.push_back(static_cast<NodeId>(i));  // where newcomers step off the train
    }
    for (int i = 0; i < sc_.traders; ++i) {
        const bool firm = i < world_.params.firms_initial && !markets_.empty();
        NodeId start = firm ? markets_[rng_.next_below(markets_.size())] : static_cast<NodeId>(rng_.next_below(world_.nodes.size()));
        while (!world_.nodes[World::idx(start)].founded) start = static_cast<NodeId>(rng_.next_below(world_.nodes.size()));
        spawn_trader(start, firm);
    }
    for (const LeaderSeed& l : sc_.leaders) {
        Leader ld;
        ld.id = next_leader_id_++;
        ld.name = l.name;
        ld.at = l.home;
        ld.skill = l.skill;
        leaders_.push_back(std::move(ld));
    }
    for (const InterestDef& d : def.interests) {
        Interest in;
        in.id = d.id; in.name = d.name; in.kind = d.kind; in.hq = d.hq; in.nodes = d.nodes;
        in.budget_per_day = d.budget_per_day; in.dues_rate = d.dues_rate; in.from = d.from; in.until = d.until;
        interests_.push_back(std::move(in));
    }
    for (const GangSeed& g : sc_.gangs) create_gang(g.hideout, g.men, "seeded in scenario.json");

    // Everything created so far is the starting stock of money, not a flow.
    for (double& f : world_.ledger) f = 0;
    baseline_money_ = total_money();
}

double Simulation::total_money() const {
    double m = 0;
    for (const Node& n : world_.nodes)
        if (!n.fixed_market) m += n.cash + n.law_fund;
    for (const Trader& t : traders_) m += t.cash + t.pending_spend;  // pending: wages on the road, paid at the next stop
    for (const Gang& g : gangs_) m += g.cash;
    for (const Leader& l : leaders_) m += l.cash;
    for (const Interest& in : interests_) m += in.fund;
    return m;
}

MoneyAudit Simulation::money_audit() const {
    MoneyAudit a;
    a.baseline = baseline_money_;
    for (int i = 0; i < kMoneyFlowCount; ++i) {
        a.flows[i] = world_.ledger[i];
        a.net_flows += world_.ledger[i];
    }
    a.expected = a.baseline + a.net_flows;
    a.actual = total_money();
    a.residual = a.actual - a.expected;
    return a;
}

void Simulation::pay_leader(Leader& l, double amount) {
    l.cash += amount;
    l.earned += amount;
}

void Simulation::step() {
    update_network();
    apply_shocks();
    apply_campaigns();
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
    world_.refresh_contract_prices();
    auto describe = [&](EdgeId id, const char* verb) {
        const Edge& e = world_.edges[World::idx(id)];
        const std::string cause = e.note.empty() ? "scheduled in world.json" : e.note;
        log_.with({field("mode", mode_name(e.mode)), field("a", nname(e.a)), field("b", nname(e.b)), field("miles", e.miles), field("change", verb)});
        log_.add(world_.today, EventType::Network, "route_changed", e.a,
                 strf("%s between %s and %s %s (%.0f miles)", mode_name(e.mode), nname(e.a).c_str(),
                      nname(e.b).c_str(), verb, e.miles),
                 cause);
    };
    for (EdgeId id : opened) describe(id, "opens");
    for (EdgeId id : closed) describe(id, "closes");
}

// Troop strength follows threats from outside the model (campaigns, new posts, withdrawals), not local
// robbery: the Army was there to face large-scale conflict, not to police every road.
// Who comes west. Entrants never see the true profit. Freelancers act on rumor: success stories travel
// fast and the failures stay quiet. Eastern firms act on reported returns, which are accurate but months
// old. Both are put off by danger in proportion to their risk aversion. Because entry lags returns, the
// market overshoots: crowding cuts returns, but the stories keep coming for a while.
int draw_count(Rng& rng, double expected) {
    int n = static_cast<int>(std::floor(expected));
    if (rng.chance(expected - n)) ++n;
    return n;
}

void Simulation::trader_entry() {
    const Params& p = world_.params;
    const double danger = std::min(1.0, danger_ema_ / p.danger_ref);
    const double rumor = std::clamp(story_ema_ / p.story_ref, 0.0, 3.0);
    const double mean_free = (p.risk_aversion_min + p.risk_aversion_max) / 2;
    const double mean_firm = (p.firm_risk_aversion_min + p.firm_risk_aversion_max) / 2;
    const double report = std::clamp((return_ema_ - p.firm_hurdle) / p.firm_hurdle, 0.0, 3.0);

    int free_in = draw_count(rng_, p.freelancer_entry_rate * rumor * std::max(p.entry_danger_floor, 1.0 - 0.5 * mean_free * danger));
    int firm_in = draw_count(rng_, p.firm_entry_rate * report * std::max(p.entry_danger_floor, 1.0 - 0.5 * mean_firm * danger));
    free_in = std::min(free_in, std::max(0, sc_.max_traders - static_cast<int>(traders_.size())));
    for (int i = 0; i < free_in && !gateways_.empty(); ++i) spawn_trader(gateways_[rng_.next_below(gateways_.size())], false);
    for (int i = 0; i < firm_in && !markets_.empty(); ++i) spawn_trader(markets_[rng_.next_below(markets_.size())], true);
    entered_freelancers_ += free_in;
    entered_firms_ += firm_in;
    entered_window_f_ += free_in;
    entered_window_firm_ += firm_in;

    if (world_.today % 30 == 0 && (entered_window_f_ > 0 || entered_window_firm_ > 0)) {
        log_.with({field("freelancers", entered_window_f_), field("firms", entered_window_firm_), field("rumor", story_ema_),
                   field("reported_return", return_ema_ * 365), field("danger", danger_ema_)});
        log_.add(world_.today, EventType::Trader, "traders_arrive", -1,
                 strf("%d freelancers and %d eastern firms take up trading in the past month", entered_window_f_,
                      entered_window_firm_),
                 strf("rumor of %.1f success stories a day; firms read a reported return of %.0f%% a year against a %.0f%% hurdle; "
                      "robbery danger %.0f%% of the 'deadly' level",
                      story_ema_, return_ema_ * 36500, p.firm_hurdle * 36500, danger * 100),
                 true);
        entered_window_f_ = entered_window_firm_ = 0;
    }
}

void Simulation::apply_campaigns() {
    for (const CampaignDef& c : sc_.campaigns) {
        const bool starting = c.from == world_.today;
        const bool ending = c.until != LONG_MAX && c.until == world_.today;
        if (!starting && !ending) continue;
        for (NodeId id : c.nodes) {
            Node& nd = world_.nodes[World::idx(id)];
            const double delta = std::max(starting ? c.garrison_add : -c.garrison_add, -nd.garrison);
            nd.garrison += delta;
            nd.population = std::max(0.0, nd.population + delta);
            nd.capacity = std::max(nd.capacity, nd.population);
            log_.with({field("node", nd.name), field("campaign", c.name), field("delta", delta), field("garrison", nd.garrison)});
            log_.add(world_.today, EventType::Law, "garrison_change", id,
                     strf("%s: %s at %s (garrison now %.0f)", c.name.c_str(),
                          delta >= 0 ? "reinforcements arrive" : "troops withdrawn", nd.name.c_str(), nd.garrison),
                     c.cause);
        }
    }
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
                    log_.with({field("node", node.name), field("good", good(g).id), field("lost", lost), field("shock", sh.name)});
                    log_.add(world_.today, EventType::Shock, "stock_loss", static_cast<NodeId>(n),
                             strf("%s: %s loses %s %s", sh.name.c_str(), node.name.c_str(), with_commas(lost).c_str(),
                                  good(g).unit.c_str()),
                             sh.cause);
                }
            }
        } else if (sh.kind == "crop_failure") {
            for (std::size_t z = 0; z < world_.zone_names.size(); ++z) {
                if (world_.zone_names[z] != sh.tag) continue;
                world_.zone_yield[z] = std::min(world_.zone_yield[z], 1.0 - sh.fraction);
                for (Node& nd : world_.nodes)
                    if (nd.zone == static_cast<int>(z)) nd.yield_memory = std::min(nd.yield_memory, 1.0 - sh.fraction);
                log_.with({field("zone", sh.tag), field("lost", sh.fraction), field("shock", sh.name)});
                log_.add(world_.today, EventType::Shock, "crop_failure", -1,
                         strf("%s: the %s harvest is %.0f%% short", sh.name.c_str(), sh.tag.c_str(), sh.fraction * 100), sh.cause);
            }
        } else if (sh.kind == "rush") {
            Node& node = world_.nodes[World::idx(sh.node)];
            node.population += sh.people;
            // Argonauts came with a few weeks' supplies; the shortage hits when those run out.
            for (std::size_t g = 0; g < world_.goods.size(); ++g)
                if (world_.goods[g].food_share > 0)
                    node.stock[g] += sh.people * world_.goods[g].need_per_person_per_day * world_.params.rush_provision_days;
            node.cash += sh.people * sh.cash_per_person;
            world_.record(MoneyFlow::Rush, sh.people * sh.cash_per_person);
            // Newcomers arrive on expectations of income that has not shown up yet.
            node.income_ema += sh.people * world_.params.support_income_per_person * 0.8;
            node.capacity = std::max(node.capacity, node.population);
            log_.with({field("node", node.name), field("people", sh.people)});
            log_.add(world_.today, EventType::Shock, "rush", sh.node,
                     strf("%s: %s people arrive at %s", sh.name.c_str(), with_commas(sh.people).c_str(),
                          node.name.c_str()),
                     sh.cause);
        }
    }
}

// ---------------------------------------------------------------- economy

void Simulation::economy_day() {
    const Params& p = world_.params;
    farms_day();
    for (std::size_t ni = 0; ni < world_.nodes.size(); ++ni) {
        Node& n = world_.nodes[ni];
        const NodeId id = static_cast<NodeId>(ni);
        if (n.fixed_market || !n.founded) continue;

        // Production, limited by labor and by finite reserves (mines, buffalo herds).
        for (std::size_t i = 0; i < n.production.size(); ++i) {
            const ProductionDef& pd = n.production[i];
            if (world_.today < pd.from || world_.today >= pd.until) continue;
            const std::size_t g = World::idx(pd.good);
            if (!n.production_started[i]) {
                n.production_started[i] = 1;
                if (pd.from != LONG_MIN)
                    log_.add(world_.today, EventType::Production, "output_begins", id,
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
                    log_.with({field("node", n.name), field("good", good(pd.good).id)});
                    log_.add(world_.today, EventType::Production, "reserve_exhausted", id,
                             strf("%s: %s is exhausted", n.name.c_str(), good(pd.good).name.c_str()),
                             "finite reserve fully extracted; output stops and the local economy loses its base");
                }
            }
            n.stock[g] += amount;
            // Where gold is mined it is money: the miners spend their dust at the stores as they dig it, whether or
            // not a trader has yet come to carry it out. (Traders buying it later is not counted a second time.)
            if (good(pd.good).money) {
                n.inflow_today += amount * good(pd.good).base_cost;
                n.earned_sales += amount * good(pd.good).base_cost;
            }
            n.stock[g] = std::min(n.stock[g], pd.rate * p.stock_cap_days);  // unsold output is lost
        }

        // Soldiers' pay is spent in the post town: this is the sutlers, saloons and laundresses that grow up
        // around a fort. It does not depend on how well the town is supplied.
        const double pay = n.gov_income + n.garrison * p.garrison_pay_per_soldier;
        n.cash += pay;
        n.inflow_today += pay;
        n.earned_government += pay;
        world_.record(MoneyFlow::Government, pay);

        // Standing contracts: the quantity wanted each day accumulates until a contractor delivers it.
        for (const ContractDef& c : n.contracts) {
            const double rate = world_.contract_rate(id, c);
            double& backlog = n.contract_backlog[World::idx(c.good)];
            backlog = std::min(backlog + rate, rate * p.contract_backlog_days);
        }

        // The Quartermaster Department also ran its own trains from the nearest depot, so a post was never left
        // wholly to private contractors. A train covers most of a month's contract needs; traders can supply the rest.
        if (!n.contracts.empty() && n.garrison > 0 && (world_.today + id) % p.quartermaster_interval_days == 0) {
            bool depot = false;
            for (std::size_t m = 0; m < world_.nodes.size() && !depot; ++m)
                depot = world_.nodes[m].fixed_market && world_.reachable(static_cast<NodeId>(m), id);
            if (depot) {
                double units = 0;
                for (const ContractDef& c : n.contracts) {
                    const std::size_t g = World::idx(c.good);
                    const double load = std::min(n.contract_backlog[g], world_.contract_rate(id, c) * p.quartermaster_interval_days * p.quartermaster_share);
                    n.contract_backlog[g] -= load;
                    n.stock[g] += load;
                    units += load;
                }
                log_.with({field("node", n.name), field("units", units)});
                log_.add(world_.today, EventType::Law, "supply_train", id,
                         strf("A Quartermaster supply train reaches %s (%.0f units)", n.name.c_str(), units),
                         "scheduled Army logistics: the post's contract needs, delivered from the nearest depot", true);
            }
        }

        // Consumption. Food shortfall is what drives hunger.
        double fed = 0;
        for (std::size_t g = 0; g < world_.goods.size(); ++g) {
            const GoodDef& gd = world_.goods[g];
            const double want = n.market_population() * gd.need_per_person_per_day;
            const double take = std::min(n.stock[g], want);
            n.stock[g] -= take;
            if (want > 0 && gd.food_share > 0) fed += gd.food_share * (take / want);
            const double extra = std::min(n.stock[g], n.extra_demand[g]);  // agency rations
            n.stock[g] -= extra;
            const double military = std::min(n.stock[g], n.garrison * gd.garrison_need);  // forage, remounts, repairs
            n.stock[g] -= military;
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
            std::string covers;
            for (std::size_t g = 0; g < world_.goods.size(); ++g) {
                const GoodDef& gd = world_.goods[g];
                if (gd.food_share <= 0) continue;
                const double need = n.market_population() * gd.need_per_person_per_day;
                const double days = need > 0 ? n.stock[g] / need : 1e9;
                covers += strf("%s%s %.1f days", covers.empty() ? "" : ", ", gd.name.c_str(), days);
            }
            const bool repeat = world_.today - n.last_famine_log < 90;
            n.last_famine_log = world_.today;
            log_.with({field("node", n.name), field("pop", n.population), field("hunger", n.hunger)});
            log_.add(world_.today, EventType::Famine, "famine", id,
                     strf("%s is short of food (pop %s)", n.name.c_str(), with_commas(n.population).c_str()),
                     strf("stocks cover: %s; town cash %s limits what traders can be paid", covers.c_str(),
                          money(n.cash).c_str()),
                     repeat);
        } else if (n.hunger < 0.08 && n.starving) {
            n.starving = false;
            log_.with({field("node", n.name)});
            log_.add(world_.today, EventType::Famine, "famine_over", id, strf("%s has food again", n.name.c_str()),
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
    const double trade_pop = std::clamp(n.income_ema / p.support_income_per_person, 0.0, std::max(0.0, n.capacity - n.base_population - n.farmers - n.garrison));
    // Hunger drives away the people who depend on the market, not the farmers who feed themselves.
    const double hunger_keep = n.hunger > p.hunger_threshold ? 1.0 - n.hunger : 1.0;
    // The garrison is posted by the Army and stays however the town is doing.
    const double target = std::max(n.garrison + n.base_population + n.farmers + trade_pop * hunger_keep, p.floor_pop);
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
        if (n.population > 0) {
            const double leaves = n.cash * (loss / n.population) * p.emigrant_cash_share;
            n.cash -= leaves;
            world_.record(MoneyFlow::Emigration, -leaves);
        }
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
        log_.with({field("node", n.name), field("threshold", kPopBands[n.pop_band - 1]), field("pop", n.population), field("income", n.income_ema)});
        log_.add(world_.today, EventType::Population, "pop_grew", id,
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
        log_.with({field("node", n.name), field("threshold", kPopBands[n.pop_band]), field("pop", n.population), field("income", n.income_ema), field("peak_income", n.income_peak)});
        log_.add(world_.today, EventType::Population, "pop_shrank", id,
                 strf("%s shrinks below %s", n.name.c_str(), with_commas(kPopBands[n.pop_band]).c_str()),
                 strf("income fell to %s/day from a peak of %s; hunger %.2f", money(n.income_ema).c_str(),
                      money(n.income_peak).c_str(), n.hunger),
                 quiet_down);
    }
    if (!n.ghost && n.population < p.ghost_pop && n.peak_pop >= 150) {
        n.ghost = true;
        log_.with({field("node", n.name), field("peak_pop", n.peak_pop), field("income", n.income_ema)});
        log_.add(world_.today, EventType::Population, "ghost_town", id,
                 strf("%s becomes a ghost town (peak pop %s)", n.name.c_str(), with_commas(n.peak_pop).c_str()),
                 strf("income collapsed to %s/day from a peak of %s: no traffic or output left to live on",
                      money(n.income_ema).c_str(), money(n.income_peak).c_str()));
    } else if (n.ghost && n.population >= p.ghost_pop * 2) {
        n.ghost = false;
        log_.with({field("node", n.name), field("pop", n.population)});
        log_.add(world_.today, EventType::Population, "repopulating", id, strf("%s is repopulating", n.name.c_str()),
                 strf("income recovered to %s/day", money(n.income_ema).c_str()));
    }
}

// ---------------------------------------------------------------- end of day

void Simulation::end_of_day() {
    const Params& p = world_.params;
    const double p_market_recovery = p.market_recovery;
    for (Node& n : world_.nodes) {
        if (n.fixed_market) {
            for (double& press : n.pressure) press *= 1.0 - p_market_recovery;
            continue;
        }
        const double tax = std::min(n.cash, n.inflow_today * p.law_tax_rate);
        n.cash -= tax;
        // Under a gang's rule the taxes go to the gang, not to the sheriff.
        Gang* boss = n.captor >= 0 ? find_gang(n.captor) : nullptr;
        if (boss) boss->cash += tax;
        else n.law_fund += tax;
        n.income_ema += p.income_ema_alpha * (n.inflow_today - n.income_ema);
        n.income_peak = std::max(n.income_peak, n.income_ema);
        n.earned_total += n.inflow_today;
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
    // Reported return on capital: the audited figure an eastern firm would read, months late.
    double capital = 0;
    for (const Trader& t : traders_) capital += std::max(0.0, t.cash) + (t.cargo >= 0 ? t.qty * t.unit_cost : 0.0);
    if (capital > 0) return_ema_ += p.return_alpha * (profit_today_ / capital - return_ema_);
    const double trader_count = std::max<double>(1.0, static_cast<double>(traders_.size()));
    danger_ema_ += p.danger_alpha * (robbed_today_ / trader_count - danger_ema_);
    story_ema_ += p.story_alpha * (stories_today_ - story_ema_);
    profit_today_ = 0;
    robbed_today_ = 0;
    stories_today_ = 0;
    trader_entry();
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
