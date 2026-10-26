// Save and load of the running simulation.
//
// Why: a 4,400-day history of the 4,694-node world takes 25-40 minutes, and a question about the economy at year 8 should not
// cost a rerun from day 1. A snapshot also lets a game ship a pre-simulated start (the world arrives lived in) and branch
// "what if" runs from one day. The world's definitions (goods, routes, rivers, parameters) come from the data folder and
// scenario as usual; the file holds only what changes while the sim runs. The route table (nodes squared, about 1.3 GB at
// 4,694 nodes) and the trader horizons are rebuilt on load, not stored.
//
// Each struct's field list is written once and used for both directions (see archive.h). A forgotten field shows up in
// tests/save_test.cpp as a history that drifts from the unbroken run.
#include <algorithm>
#include <cstdio>
#include <memory>

#include "frontier/archive.h"
#include "frontier/simulation.h"

namespace frontier {

namespace {
constexpr char kMagic[8] = {'F', 'R', 'S', 'A', 'V', 'E', '0', '1'};
constexpr std::uint32_t kStateVersion = 6;
}  // namespace

// ---- struct field lists (found by argument-dependent lookup from the generic vector and pair code) ----

template <class Ar> void io(Ar& ar, ProductionDef& p) { io(ar, p.good); io(ar, p.rate); io(ar, p.labor_pop); io(ar, p.from); io(ar, p.until); }

template <class Ar> void io(Ar& ar, ContractDef& c) { io(ar, c.good); io(ar, c.price); io(ar, c.markup); io(ar, c.per_soldier); io(ar, c.units_per_day); }

template <class Ar> void io(Ar& ar, Node& n) {
    io(ar, n.id); io(ar, n.name); io(ar, n.kind); io(ar, n.x); io(ar, n.y);
    io(ar, n.fixed_market); io(ar, n.transient_inflow); io(ar, n.gov_income); io(ar, n.capacity); io(ar, n.base_population); io(ar, n.garrison);
    io(ar, n.contracts); io(ar, n.contract_price); io(ar, n.contract_backlog); io(ar, n.depot); io(ar, n.bought_out); io(ar, n.last_delivery);
    io(ar, n.order_qty); io(ar, n.order_price); io(ar, n.order_first_price); io(ar, n.order_posted); io(ar, n.order_raised); io(ar, n.order_hired);
    io(ar, n.lead_obs); io(ar, n.lead_dev); io(ar, n.lead_n); io(ar, n.growth_rate); io(ar, n.pop_mark); io(ar, n.pop_mark_day);
    io(ar, n.tags);
    io(ar, n.farmers); io(ar, n.farm_capacity); io(ar, n.farm_yield); io(ar, n.farm_open); io(ar, n.zone); io(ar, n.farm_core);
    io(ar, n.cash_good); io(ar, n.cash_rate); io(ar, n.cash_store); io(ar, n.gate_good); io(ar, n.farm_store); io(ar, n.farm_gate); io(ar, n.yield_memory);
    io(ar, n.latent); io(ar, n.founded); io(ar, n.found_pop); io(ar, n.found_good); io(ar, n.found_quality); io(ar, n.paced);
    io(ar, n.strike_chance); io(ar, n.strike_people); io(ar, n.found_streak); io(ar, n.founded_day); io(ar, n.gate_outlet);
    io(ar, n.population); io(ar, n.cash);
    io(ar, n.stock); io(ar, n.fixed_price); io(ar, n.reserve); io(ar, n.extra_demand); io(ar, n.pressure);
    io(ar, n.production); io(ar, n.production_started); io(ar, n.edges);
    io(ar, n.inflow_today); io(ar, n.income_ema); io(ar, n.income_peak);
    io(ar, n.earned_sales); io(ar, n.earned_visitors); io(ar, n.earned_government); io(ar, n.earned_total);
    io(ar, n.hunger); io(ar, n.garrison_hunger); io(ar, n.law_fund);
    io(ar, n.region); io(ar, n.law); io(ar, n.law_ready); io(ar, n.law_lapse); io(ar, n.paper_streak); io(ar, n.paper);
    io(ar, n.fear); io(ar, n.outrage); io(ar, n.drifters); io(ar, n.ex_cons); io(ar, n.jailed); io(ar, n.fighters); io(ar, n.peak_pop);
    io(ar, n.captor); io(ar, n.toll); io(ar, n.captured_day); io(ar, n.captured_income);
    io(ar, n.last_posse_day); io(ar, n.posse_pause_until); io(ar, n.last_shortage_log); io(ar, n.last_pop_log); io(ar, n.last_pop_dir);
    io(ar, n.ghost); io(ar, n.game); io(ar, n.game_capacity); io(ar, n.hunted_total); io(ar, n.game_thin); io(ar, n.starving);
    io(ar, n.deprivation); io(ar, n.starved_total); io(ar, n.spell_deaths); io(ar, n.spell_pop); io(ar, n.spell_quiet);
    io(ar, n.spell_active); io(ar, n.in_crisis); io(ar, n.garrison_short); io(ar, n.pop_band);
}

// A route's definition (ends, miles, dates, path) comes from the data; only what changes is saved.
template <class Ar> void io(Ar& ar, Edge& e) {
    io(ar, e.blocked_until); io(ar, e.block_cause); io(ar, e.active); io(ar, e.from);
    io(ar, e.robbed_ema); io(ar, e.transits_ema); io(ar, e.robbed_today); io(ar, e.transits_today);
    io(ar, e.traffic_today); io(ar, e.traffic_ema); io(ar, e.loot_today); io(ar, e.loot_ema); io(ar, e.road); io(ar, e.use_ema);
}

template <class Ar> void io(Ar& ar, Trader& t) {
    io(ar, t.id); io(ar, t.alive); io(ar, t.at); io(ar, t.cash); io(ar, t.cargo); io(ar, t.qty); io(ar, t.unit_cost); io(ar, t.guards);
    io(ar, t.drover); io(ar, t.controlled); io(ar, t.agent_for); io(ar, t.ordered_day); io(ar, t.express);
    io(ar, t.contract); io(ar, t.contract_units); io(ar, t.job_town); io(ar, t.job_src);
    io(ar, t.job_debt); io(ar, t.job_unit); io(ar, t.job_price); io(ar, t.job_guards);
    io(ar, t.leaving); io(ar, t.firm); io(ar, t.capacity_lb); io(ar, t.upkeep); io(ar, t.start_capital); io(ar, t.worth); io(ar, t.income);
    io(ar, t.base_risk_aversion); io(ar, t.risk_aversion); io(ar, t.fear); io(ar, t.house); io(ar, t.robberies);
    io(ar, t.pending_spend); io(ar, t.recent_profit);
    io(ar, t.route); io(ar, t.leg); io(ar, t.days_left); io(ar, t.dest); io(ar, t.travelling);
}

template <class Ar> void io(Ar& ar, House& h) {
    io(ar, h.id); io(ar, h.alive); io(ar, h.eastern); io(ar, h.founder); io(ar, h.seat); io(ar, h.cash); io(ar, h.start_capital);
    io(ar, h.wealth); io(ar, h.outfits); io(ar, h.founded); io(ar, h.last_hire); io(ar, h.retired);
    io(ar, h.base_risk_aversion); io(ar, h.risk_aversion); io(ar, h.fear);
}

template <class Ar> void io(Ar& ar, SupplyContract& k) {
    io(ar, k.id); io(ar, k.alive); io(ar, k.town); io(ar, k.house); io(ar, k.good); io(ar, k.rate); io(ar, k.price); io(ar, k.capital);
    io(ar, k.signed_day); io(ar, k.ends); io(ar, k.owed); io(ar, k.arrears); io(ar, k.delivered); io(ar, k.missed);
}

template <class Ar> void io(Ar& ar, Gang& g) {
    io(ar, g.id); io(ar, g.active); io(ar, g.men); io(ar, g.cells); io(ar, g.fights); io(ar, g.cash); io(ar, g.edge); io(ar, g.hideout);
    io(ar, g.formed); io(ar, g.robberies); io(ar, g.loot); io(ar, g.last_robbery); io(ar, g.last_review); io(ar, g.grudge);
    io(ar, g.influence); io(ar, g.last_spend); io(ar, g.ruling); io(ar, g.competence); io(ar, g.tribute); io(ar, g.controlled);
    io(ar, g.known); io(ar, g.peak); io(ar, g.fleeing);
    io(ar, g.camp_miles); io(ar, g.camp_kind); io(ar, g.heat); io(ar, g.qualified); io(ar, g.record_men); io(ar, g.kills); io(ar, g.thefts); io(ar, g.care);
}

template <class Ar> void io(Ar& ar, Headline& h) {
    io(ar, h.day); io(ar, h.node); io(ar, h.kind); io(ar, h.text); io(ar, h.gang); io(ar, h.weight); io(ar, h.crime);
}

// A headline is shared by every paper it reached, so it is written once and the rest point to it.
template <class Ar> void io(Ar& ar, std::shared_ptr<const Headline>& p) {
    std::uint32_t id = 0;
    if constexpr (Ar::loading) {
        io(ar, id);
        if (id == 0) { p.reset(); return; }
        if (id <= ar.headline_pool.size()) { p = ar.headline_pool[id - 1]; return; }
        auto h = std::make_shared<Headline>();
        io(ar, *h);
        ar.headline_pool.push_back(h);
        p = h;
    } else {
        if (!p) { io(ar, id); return; }
        const auto it = ar.headline_ids.find(p.get());
        if (it != ar.headline_ids.end()) { id = it->second; io(ar, id); return; }
        id = static_cast<std::uint32_t>(ar.headline_ids.size() + 1);
        ar.headline_ids.emplace(p.get(), id);
        io(ar, id);
        io(ar, const_cast<Headline&>(*p));
    }
}

template <class Ar> void io(Ar& ar, NewsDelivery& d) { io(ar, d.due); io(ar, d.paper); io(ar, d.headline); }

template <class Ar> void io(Ar& ar, Bushwhack& b) {
    io(ar, b.due); io(ar, b.town); io(ar, b.edge); io(ar, b.gang); io(ar, b.killed); io(ar, b.notable); io(ar, b.outrage); io(ar, b.who);
}

template <class Ar> void io(Ar& ar, Leader& l) {
    io(ar, l.id); io(ar, l.name); io(ar, l.at); io(ar, l.skill); io(ar, l.cash); io(ar, l.alive); io(ar, l.busy); io(ar, l.earned); io(ar, l.jobs); io(ar, l.wins);
}

template <class Ar> void io(Ar& ar, Interest& in) {
    io(ar, in.id); io(ar, in.name); io(ar, in.kind); io(ar, in.hq); io(ar, in.nodes); io(ar, in.budget_per_day); io(ar, in.dues_rate);
    io(ar, in.from); io(ar, in.until); io(ar, in.fund); io(ar, in.loss_ema); io(ar, in.losses_total); io(ar, in.victims);
    io(ar, in.last_op_day); io(ar, in.ops); io(ar, in.wins);
}

template <class Ar> void io(Ar& ar, Posse& p) {
    io(ar, p.id); io(ar, p.active); io(ar, p.origin); io(ar, p.men); io(ar, p.gang_id); io(ar, p.leader_id); io(ar, p.interest_id);
    io(ar, p.kind); io(ar, p.days_left);
}

template <class Ar> void io(Ar& ar, Field& f) { io(ar, f.key); io(ar, f.value); io(ar, f.numeric); }

template <class Ar> void io(Ar& ar, Event& e) {
    io(ar, e.day); io(ar, e.type); io(ar, e.kind); io(ar, e.node); io(ar, e.minor); io(ar, e.text); io(ar, e.cause); io(ar, e.fields);
}

// Private members of the generator and the log, reached through a friend.
struct StateIo {
    template <class Ar> static void rng(Ar& ar, Rng& r) { io(ar, r.s_); }
    template <class Ar> static void log(Ar& ar, EventLog& l) {
        io(ar, l.events_); io(ar, l.counts_); io(ar, l.kind_counts_); io(ar, l.hash_);
    }
};

template <class Ar> void Simulation::io_state(Ar& ar) {
    // ---- the world ----
    io(ar, world_.today);
    io(ar, world_.nodes);
    io(ar, world_.edges);
    io(ar, world_.ledger);
    io(ar, world_.east_bought_value); io(ar, world_.east_sold_value);
    io(ar, world_.zone_yield);
    io(ar, world_.order_filled_value); io(ar, world_.order_filled_units);
    io(ar, world_.route_version);

    // ---- random numbers and the log ----
    StateIo::rng(ar, rng_);
    StateIo::log(ar, log_);

    // ---- traders and their houses ----
    io(ar, traders_); io(ar, convoys_); io(ar, houses_); io(ar, contracts_);
    io(ar, contracts_signed_); io(ar, contracts_cancelled_);
    io(ar, starved_logged_); io(ar, hunted_logged_);
    io(ar, region_death_ema_); io(ar, region_famine_deaths_); io(ar, region_in_famine_); io(ar, region_quiet_);
    io(ar, contract_units_); io(ar, contract_revenue_); io(ar, contract_outlay_);
    io(ar, houses_founded_); io(ar, house_recaps_); io(ar, house_exits_);
    io(ar, orders_placed_); io(ar, orders_expired_); io(ar, convoys_hired_);
    io(ar, express_sent_); io(ar, express_robbed_); io(ar, express_oz_);
    io(ar, jobs_taken_); io(ar, jobs_delivered_); io(ar, job_pay_); io(ar, job_lost_);
    io(ar, delivered_value_); io(ar, lost_orders_value_); io(ar, imported_value_);

    // ---- crime, law and news ----
    io(ar, gangs_); io(ar, posses_); io(ar, leaders_); io(ar, interests_);
    io(ar, gang_peak_men_); io(ar, drifter_flow_);
    io(ar, papers_); io(ar, news_queue_); io(ar, paper_nodes_); io(ar, bush_pending_);
    io(ar, bushwhacks_); io(ar, bush_killed_); io(ar, bush_notable_);
    io(ar, bands_jailed_); io(ar, bands_hanged_); io(ar, men_hanged_);
    io(ar, found_budget_);
    io(ar, band_qualified_); io(ar, join_tried_); io(ar, join_nopartner_);
    io(ar, camp_visits_); io(ar, camp_trouble_); io(ar, camp_escapes_); io(ar, camp_caught_);
    io(ar, news_cursor_); io(ar, headlines_printed_);
    io(ar, large_ended_); io(ar, large_days_lived_); io(ar, large_end_);
    io(ar, gangs_ended_); io(ar, gang_days_lived_);
    io(ar, edge_gangs_);
    io(ar, next_trader_id_); io(ar, next_gang_id_); io(ar, next_posse_id_); io(ar, next_leader_id_);

    // ---- what entrants and traders have been told, and running averages ----
    io(ar, avg_profit_ema_); io(ar, baseline_money_);
    io(ar, story_ema_); io(ar, return_ema_); io(ar, danger_ema_); io(ar, scarcity_ema_); io(ar, scarcity_norm_);
    io(ar, stories_today_); io(ar, robbed_today_);
    io(ar, entered_freelancers_); io(ar, entered_firms_); io(ar, entered_window_f_); io(ar, entered_window_firm_);
    io(ar, gateways_); io(ar, markets_);
    io(ar, bought_value_); io(ar, flour_bought_at_);
    io(ar, weather_year_); io(ar, profit_today_);
    io(ar, opportunity_); io(ar, opportunity_day_);
    io(ar, shock_done_);
    io(ar, inbound_);
}

namespace {
// What identifies the world a snapshot belongs to: its shape and the names that index its vectors.
std::uint64_t world_fingerprint(const World& w) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    auto mix_str = [&](const std::string& s) { for (unsigned char c : s) mix(c); mix(0xff); };
    mix(w.nodes.size()); mix(w.edges.size()); mix(w.goods.size());
    for (const GoodDef& g : w.goods) mix_str(g.id);
    for (const Node& n : w.nodes) mix_str(n.id);
    for (const Edge& e : w.edges) { mix(static_cast<std::uint64_t>(e.a)); mix(static_cast<std::uint64_t>(e.b)); }
    return h;
}
}  // namespace

void Simulation::save_state(const std::string& path) const {
    Simulation& self = const_cast<Simulation&>(*this);   // writing does not change the state
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open " + path + " to save");
    try {
        StateWriter ar(f);
        char magic[8];
        std::copy(std::begin(kMagic), std::end(kMagic), magic);
        ar.raw(magic, sizeof magic);
        std::uint32_t version = kStateVersion;
        io(ar, version);
        std::uint64_t print = world_fingerprint(world_);
        io(ar, print);
        self.io_state(ar);
    } catch (...) {
        std::fclose(f);
        throw;
    }
    if (std::fclose(f) != 0) throw std::runtime_error("save failed: could not finish writing " + path);
}

void Simulation::load_state(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path + " to load");
    try {
        StateReader ar(f);
        char magic[8];
        ar.raw(magic, sizeof magic);
        if (!std::equal(std::begin(kMagic), std::end(kMagic), magic)) throw std::runtime_error(path + " is not a Frontier Sim snapshot");
        std::uint32_t version = 0;
        io(ar, version);
        if (version != kStateVersion) throw std::runtime_error(path + " was saved by a different version of the simulation (state version " + std::to_string(version) + ", this build reads " + std::to_string(kStateVersion) + ")");
        std::uint64_t print = 0;
        io(ar, print);
        if (print != world_fingerprint(world_)) throw std::runtime_error(path + " belongs to a different world (its towns, routes or goods differ from the data loaded)");
        io_state(ar);
    } catch (...) {
        std::fclose(f);
        throw;
    }
    std::fclose(f);
    // Derived tables are rebuilt from the restored state, not stored. The route table follows the active routes; the trader
    // horizons follow the route table. The version number is restored so caches keyed on it behave as they did.
    const long saved_version = world_.route_version;
    for (Edge& e : world_.edges) world_.apply_road(e);   // travel days and freight miles follow the saved road levels
    world_.rebuild_routes();
    world_.route_version = saved_version;
    horizon_version_ = -1;
    horizon_.clear();
    horizon_path_.clear();
}

}  // namespace frontier
