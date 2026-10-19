#include "frontier/stats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "frontier/date.h"
#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

namespace {

bool finite(double v) { return std::isfinite(v); }

// Cheap per-day sanity checks. These catch bugs, not economics: negative stock, NaNs, money from nowhere.
void check_invariants(const Simulation& sim, RunStats& out) {
    if (!out.invariants_ok) return;
    const World& w = sim.world();
    auto fail = [&](const std::string& what) {
        out.invariants_ok = false;
        out.first_violation = format_date(w.today) + ": " + what;
    };
    for (const Node& n : w.nodes) {
        if (n.fixed_market) continue;
        if (!finite(n.cash) || !finite(n.population) || !finite(n.law_fund)) return fail("non-finite value at " + n.name);
        if (n.population < -1e-9) return fail("negative population at " + n.name);
        if (n.cash < -1e-6) return fail("negative cash at " + n.name);
        for (std::size_t g = 0; g < n.stock.size(); ++g) {
            if (!finite(n.stock[g])) return fail("non-finite stock at " + n.name);
            if (n.stock[g] < -1e-6) return fail("negative stock of " + w.goods[g].id + " at " + n.name);
        }
    }
    for (const Trader& t : sim.traders())
        if (!finite(t.cash) || !finite(t.qty) || t.qty < -1e-9) return fail("bad trader state #" + std::to_string(t.id));
}

}  // namespace

RunStats run_and_collect(const WorldDef& world, Scenario scenario, std::uint64_t seed) {
    scenario.seed = seed;
    Simulation sim(world, scenario);
    RunStats r;
    r.seed = seed;
    r.days = scenario.days;
    r.traders_start = r.traders_min = r.traders_max = static_cast<int>(sim.traders().size());
    r.money_start = sim.money_audit().baseline;

    const std::size_t N = sim.world().nodes.size();
    r.towns.resize(N);
    for (std::size_t i = 0; i < N; ++i) {
        const Node& n = sim.world().nodes[i];
        r.towns[i].id = n.id;
        r.towns[i].name = n.name;
        r.towns[i].pop_start = n.population;
        r.towns[i].pop_peak = n.population;
        r.towns[i].farmers_start = r.towns[i].farmers_peak = n.farmers;
        r.towns[i].latent = n.latent;
        if (!n.fixed_market) r.pop_start_total += n.population;
    }

    // FRONTIER_PROGRESS=1 prints a line to stderr every 250 simulated days, so a long run shows where it is.
    const bool progress = std::getenv("FRONTIER_PROGRESS") != nullptr;
    long gang_days = 0, gang_men_days = 0, gang_big_days = 0, sampled_days = 0, band_days = 0;
    double lawless_days = 0;
    for (int d = 0; d < scenario.days; ++d) {
        sim.step();
        if (progress && d % 250 == 249) std::fprintf(stderr, "seed %llu: day %d of %d, %zu traders\n", static_cast<unsigned long long>(seed), d + 1, scenario.days, sim.traders().size());
        const int tc = static_cast<int>(sim.traders().size());
        r.traders_min = std::min(r.traders_min, tc);
        r.traders_max = std::max(r.traders_max, tc);
        for (std::size_t i = 0; i < N; ++i) {
            const Node& n = sim.world().nodes[i];
            if (n.fixed_market) {
                for (std::size_t g = 0; g < n.pressure.size(); ++g)
                    if (n.fixed_price[g] > 0)
                        r.east_price_low = std::min(r.east_price_low, sim.world().fixed_price_at(static_cast<NodeId>(i), static_cast<GoodId>(g), n.pressure[g]) / n.fixed_price[g]);
                continue;
            }
            TownOutcome& o = r.towns[i];
            o.pop_peak = std::max(o.pop_peak, n.population);
            o.farmers_peak = std::max(o.farmers_peak, n.farmers);
            if (n.latent && n.founded && o.founded_day < 0) o.founded_day = static_cast<long>(d);
            o.ever_ghost = o.ever_ghost || n.ghost;
            if (n.captor >= 0) ++o.days_ruled_by_gang;
        }
        check_invariants(sim, r);
        if (sim.world().today >= sim.log_from()) {
            for (const Gang& g : sim.gangs()) {
                if (!g.active) continue;
                ++gang_days;
                gang_men_days += g.men;
                gang_big_days += g.men >= 30;
                band_days += sim.is_band_gang(g);
            }
            int towns = 0, lawless = 0;
            for (const Node& n : sim.world().nodes) {
                if (n.fixed_market || !n.founded || n.ghost) continue;
                ++towns;
                lawless += n.law == 0;
            }
            lawless_days += towns ? static_cast<double>(lawless) / towns : 0.0;
            ++sampled_days;
        }
    }

    r.hash = sim.log().hash();
    r.traders_end = static_cast<int>(sim.traders().size());
    int drovers = 0;
    for (const Trader& t : sim.traders()) drovers += t.drover ? 1 : 0;
    r.drover_share_end = r.traders_end ? static_cast<double>(drovers) / r.traders_end : 0;
    for (const Trader& t : sim.traders()) r.firms_end += t.firm ? 1 : 0;
    r.houses_founded = sim.houses_founded();
    r.house_recaps = sim.house_recaps();
    r.house_killed = sim.house_exits(false);
    r.house_broke = sim.house_exits(true);
    for (const House& h : sim.houses()) r.houses_end += h.alive ? 1 : 0;
    for (const Trader& t : sim.traders()) r.house_outfits_end += t.house >= 0 ? 1 : 0;
    r.freelancers_entered = sim.entered_freelancers();
    r.firms_entered = sim.entered_firms();
    r.settled = static_cast<int>(sim.log().count_kind("trader_settled"));
    r.firm_withdrawals = static_cast<int>(sim.log().count_kind("firm_withdrew"));
    r.trader_exits = static_cast<int>(sim.log().count_kind("trader_exit"));

    const EventLog& log = sim.log();
    r.robberies = static_cast<int>(log.count_kind("robbery"));
    r.tolls = static_cast<int>(log.count_kind("toll"));
    for (const Event& e : log.events())
        if (e.kind == "robbery" && !e.minor) ++r.robberies_over_1500;  // robberies are only minor below $1,500
    r.gangs_formed = static_cast<int>(log.count_kind("gang_formed"));
    r.gangs_disbanded = static_cast<int>(log.count_kind("gang_disbanded"));
    r.posses = static_cast<int>(log.count_kind("posse_raised") + log.count_kind("posse_hired_leader"));
    r.posse_victories = static_cast<int>(log.count_kind("posse_victory"));
    r.posse_routs = static_cast<int>(log.count_kind("posse_routed"));
    r.leaders_killed = static_cast<int>(log.count_kind("leader_killed"));
    r.leaders_emerged = static_cast<int>(log.count_kind("leader_emerged"));
    r.detective_parties = static_cast<int>(log.count_kind("detectives_hired"));
    r.takeovers = static_cast<int>(log.count_kind("gang_takeover"));
    r.towns_freed = static_cast<int>(log.count_kind("town_freed"));
    r.infighting = static_cast<int>(log.count_kind("gang_infighting"));
    r.gang_wars = static_cast<int>(log.count_kind("gang_war"));
    r.gang_mergers = static_cast<int>(log.count_kind("gang_merged"));
    r.gang_breakdowns = static_cast<int>(log.count_kind("gang_breakdown"));
    r.gang_peak = sim.gang_peak_men();
    for (int f = 0; f < static_cast<int>(DrifterFlow::Count); ++f) r.drifter_flow[f] = sim.drifter_flow(static_cast<DrifterFlow>(f));
    r.gangs_ended = sim.gangs_ended();
    r.gang_life_mean = sim.gangs_ended() ? sim.gang_days_lived() / sim.gangs_ended() : 0;
    r.gang_size_mean = gang_days ? static_cast<double>(gang_men_days) / gang_days : 0;
    r.gangs_active_mean = sampled_days ? static_cast<double>(gang_days) / sampled_days : 0;
    r.gang_big_days = gang_big_days;
    r.bands_mean = sampled_days ? static_cast<double>(band_days) / sampled_days : 0;
    r.large_gangs_mean = sampled_days ? static_cast<double>(gang_days - band_days) / sampled_days : 0;
    r.large_ended = sim.large_ended();
    r.large_life_mean = sim.large_ended() ? sim.large_days_lived() / sim.large_ended() : 0;
    for (int k = 0; k < 7; ++k) r.large_end[k] = sim.large_end(k);
    for (const Node& n : sim.world().nodes) r.papers_end += n.paper;
    r.papers_founded = static_cast<int>(log.count_kind("paper_founded"));
    r.headlines = sim.headlines_printed();
    r.bushwhacks = sim.bushwhacks();
    r.bush_killed = sim.bush_killed();
    r.bands_jailed = sim.bands_jailed();
    r.bands_hanged = sim.bands_hanged();
    r.men_hanged = sim.men_hanged();
    r.band_qualified = sim.band_qualified();
    r.join_tried = sim.join_tried();
    r.join_nopartner = sim.join_nopartner();
    r.camp_visits = sim.camp_visits();
    r.camp_trouble = sim.camp_trouble();
    r.camp_escapes = sim.camp_escapes();
    r.camp_caught = sim.camp_caught();
    r.bush_notable = sim.bush_notable();
    r.cavalry_sent = static_cast<int>(log.count_kind("cavalry_sent"));
    r.lawless_share = sampled_days ? lawless_days / sampled_days : 0;
    r.law_established = static_cast<int>(log.count_kind("law_established"));
    r.law_lapsed = static_cast<int>(log.count_kind("law_lapsed"));
    r.band_catches = static_cast<int>(log.count_kind("band_caught"));
    r.band_joins = static_cast<int>(log.count_kind("band_joined"));
    r.vigilance = static_cast<int>(log.count_kind("vigilance_formed"));
    r.waypoint_sales = static_cast<int>(log.count_kind("trade_waypoint"));
    // A town on the edge of food supply crosses the hunger thresholds again and again; that is one hungry
    // spell, not many. Famine events at one town fewer than kFamineMergeDays apart (counting from the last
    // famine or recovery) are one episode, and distinct towns say whether it was regional or chronic.
    constexpr long kFamineMergeDays = 90;
    // Shortages (food stores empty, the hunger average high) and famines (a spell of starvation that killed a real share of
    // the town) are counted the same way: merged into episodes, with the distinct towns they touched.
    auto count_episodes = [&](const char* start, const char* end, int& episodes, int& towns, int& events) {
        events = 0;
        std::vector<long> last_event(N, -1000000);
        std::vector<bool> seen(N, false);
        for (const Event& e : log.events()) {
            if ((e.kind != start && e.kind != end) || e.day < sim.log_from()) continue;  // the first year is warm-up
            const std::size_t i = World::idx(e.node);
            if (e.kind == start) {
                ++events;
                if (e.day - last_event[i] >= kFamineMergeDays) ++episodes;
                if (!seen[i]) ++towns;
                seen[i] = true;
            }
            last_event[i] = e.day;
        }
    };
    count_episodes("shortage", "shortage_over", r.shortages, r.shortage_towns, r.shortage_events);
    count_episodes("famine", "famine_over", r.famines, r.famine_towns, r.famine_events);
    r.starved = sim.starved_logged();
    r.game_taken = sim.hunted_logged();
    double game_now = 0, game_cap = 0;
    for (std::size_t i = 0; i < N; ++i) { game_now += sim.world().nodes[i].game; game_cap += sim.world().nodes[i].game_capacity; }
    r.game_left = game_cap > 0 ? game_now / game_cap : 1.0;
    r.ghost_towns = static_cast<int>(log.count_kind("ghost_town"));

    for (std::size_t i = 0; i < N; ++i) {
        const Node& n = sim.world().nodes[i];
        if (n.fixed_market) continue;
        TownOutcome& o = r.towns[i];
        o.pop_end = n.population;
        o.income_end = n.income_ema;
        o.farmers_end = n.farmers;
        o.earned_sales = n.earned_sales;
        o.earned_visitors = n.earned_visitors;
        o.earned_government = n.earned_government;
        o.earned_total = n.earned_total;
        o.ghost_end = n.ghost;
        r.pop_end_total += n.population;
        if (n.cash > r.richest_cash) {
            r.richest_cash = n.cash;
            r.richest_town = n.name;
        }
    }

    const MoneyAudit audit = sim.money_audit();
    r.money_end = audit.actual;
    r.residual = audit.residual;
    r.east_buys = audit.flows[static_cast<int>(MoneyFlow::EastBuys)];
    r.east_sells = audit.flows[static_cast<int>(MoneyFlow::EastSells)];
    r.emigrated = audit.flows[static_cast<int>(MoneyFlow::Emigration)];
    for (std::size_t i = 0; i < N; ++i) r.towns[i].flour_bought = sim.flour_bought_at()[i];
    for (std::size_t i = 0; i < N; ++i) r.towns[i].imported = sim.imported_value()[i];
    r.merchant_orders = sim.merchant_orders();
    r.merchant_delivered = sim.merchant_delivered() + sim.world().order_filled_value;
    r.orders_expired = sim.orders_expired();
    r.convoys_hired = sim.convoys_hired();
    r.jobs_taken = sim.jobs_taken();
    r.jobs_delivered = sim.jobs_delivered();
    r.job_pay = sim.job_pay();
    r.job_lost = sim.job_lost();
    r.contracts_signed = sim.contracts_signed();
    r.contracts_cancelled = sim.contracts_cancelled();
    r.contract_units = sim.contract_units();
    r.contract_revenue = sim.contract_revenue();
    r.contract_outlay = sim.contract_outlay();
    r.merchant_lost = sim.merchant_lost();
    r.east_bought_by_good = sim.world().east_bought_value;
    r.east_sold_by_good = sim.world().east_sold_value;
    r.bought_by_freelancers = sim.bought_by_good(false);
    r.bought_by_firms = sim.bought_by_good(true);
    return r;
}

}  // namespace frontier
