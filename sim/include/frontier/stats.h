#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "frontier/config.h"
#include "frontier/simulation.h"
#include "frontier/world.h"

namespace frontier {

// How one town fared in one run.
struct TownOutcome {
    std::string id, name;
    double pop_start = 0, pop_peak = 0, pop_end = 0;
    double income_end = 0;
    bool ghost_end = false;
    bool ever_ghost = false;
    int days_ruled_by_gang = 0;
    double farmers_start = 0, farmers_peak = 0, farmers_end = 0;
    bool latent = false;         // started as a site on the map, not a town
    long founded_day = -1;       // days since the run began when it became a town (-1 = never / from the start)
    double imported = 0;         // dollars of goods the town's merchants ordered in
    double flour_bought = 0;     // dollars of flour traders bought here
    double earned_sales = 0, earned_visitors = 0, earned_government = 0, earned_total = 0;   // lifetime, by source
};

// Everything the batch runner wants to know about one complete run. Collected by stepping the simulation
// day by day, so it can also check invariants that only matter along the way.
// One row of the economy's vital signs on one day, so a single run shows how things changed, not just where they ended.
// Counts marked "so far" are running totals since day 0 (a loaded snapshot carries them forward).
struct SeriesRow {
    long day = 0;                       // days since the scenario start
    int traders = 0, freelancers = 0, house_outfits = 0, firms = 0, houses_open = 0, gangs = 0;
    double money = 0;                   // money in circulation
    double avg_profit = 0;              // smoothed profit per trader per day
    double pop = 0;                     // people in the frontier towns
    int towns = 0, towns_short = 0;     // towns of 30 or more people, and those in a food shortage today
    long shortages = 0, crises = 0;    // episodes so far
    double starved = 0;                 // people starved so far
    long robberies = 0, trader_exits = 0, settled = 0, orders = 0, convoys_hired = 0, jobs_delivered = 0, towns_founded = 0, ghosts = 0;
    double flows[kMoneyFlowCount] = {}; // where money came from and went, so far (see MoneyFlow)
};

// How one group of traders fared, followed by id from the day each was first seen. For each horizon (1, 2, 3, 5 and 10 years)
// `seen[h]` counts the members old enough to have been watched that long and `gone[h]` those of them who had left by then, so
// gone / seen is the share that failed or quit within that time. A trader still trading at the end is censored, not counted as gone.
struct CohortRow {
    static constexpr int kHorizons = 5;
    static constexpr int horizon_days[kHorizons] = {365, 730, 1095, 1825, 3650};
    std::string name;
    int n = 0, left = 0;                       // members, and those who left before the run ended
    int seen[kHorizons] = {}, gone[kHorizons] = {};
    int ruined = 0, killed = 0, settled = 0, other = 0;   // how the ones who left did so
};

struct RunStats {
    std::uint64_t seed = 0;
    std::vector<SeriesRow> series;
    std::vector<CohortRow> cohorts;
    TradeBook trade_book;                      // what traders earned and spent, by kind, good and year (see simulation.h)
    std::vector<std::string> good_names;
    std::uint64_t hash = 0;
    int days = 0;

    // Traders
    int traders_start = 0, traders_end = 0, traders_min = 0, traders_max = 0;
    double drover_share_end = 0;
    int firms_end = 0, freelancers_entered = 0, firms_entered = 0, settled = 0, firm_withdrawals = 0, trader_exits = 0;

    // Events (by kind)
    int robberies = 0, tolls = 0, robberies_over_1500 = 0, gangs_formed = 0, gangs_disbanded = 0;
    int posses = 0, posse_victories = 0, posse_routs = 0, leaders_killed = 0, leaders_emerged = 0;
    int detective_parties = 0, takeovers = 0, towns_freed = 0, infighting = 0, vigilance = 0;
    double drifter_flow[24] = {};   // men, whole run (warm-up included)
    long gangs_ended = 0;
    double gang_life_mean = 0;                                  // mean days a gang lasts
    long large_ended = 0;                                       // gangs that ever reached 6 men and broke up, and how: law, money, rivals, quarrels, freighters, other
    double large_life_mean = 0;
    int large_end[7] = {};
    int papers_end = 0, papers_founded = 0;
    long bushwhacks = 0, bush_killed = 0, bush_notable = 0;
    long bands_jailed = 0, bands_hanged = 0, men_hanged = 0;
    long band_qualified = 0, join_tried = 0, join_nopartner = 0;
    long camp_visits = 0, camp_trouble = 0, camp_escapes = 0, camp_caught = 0;
    int cavalry_sent = 0;
    long headlines = 0;
    double lawless_share = 0;                                   // mean share of towns with no law at all, per day since the log began
    int law_established = 0, law_lapsed = 0;
    double bands_mean = 0, large_gangs_mean = 0;                // bands (5 men or fewer) and larger gangs on the road, mean a day since the log began
    int band_catches = 0, band_joins = 0;
    int gang_wars = 0, gang_mergers = 0, gang_breakdowns = 0;   // fights between gangs, a winner taking in a beaten gang, big bands quarrelling apart
    int gang_peak = 0;                                          // the largest gang at any time
    double gangs_active_mean = 0, gang_size_mean = 0;           // per day since the log began: gangs on the road, and men per gang
    long gang_big_days = 0;                                     // gang-days spent at 30 or more men
    int merchant_orders = 0, orders_expired = 0, convoys_hired = 0;
    int express_sent = 0, express_robbed = 0;
    double express_oz = 0;
    int contracts_signed = 0, contracts_cancelled = 0;
    int jobs_taken = 0, jobs_delivered = 0;
    double job_pay = 0, job_lost = 0;   // what carriers earned hauling suppliers' goods, and what the road took from suppliers
    double contract_units = 0, contract_revenue = 0, contract_outlay = 0;   // what the houses delivered, took in and laid out
    double merchant_delivered = 0, merchant_lost = 0;   // dollars of ordered goods that arrived / were robbed
    int crises = 0, ghost_towns = 0, waypoint_sales = 0;
    int houses_founded = 0, houses_end = 0, house_outfits_end = 0, house_recaps = 0, house_killed = 0, house_broke = 0;
    int regional_famines = 0;   // region-wide famines (many towns starving at once, deaths above the mortality line)
    int crisis_events = 0;   // raw food-crisis log entries, before merging flicker into episodes
    int crisis_towns = 0;    // distinct towns that had a food crisis (a spell of starvation that killed a real share of the town)
    int shortages = 0, shortage_events = 0, shortage_towns = 0;   // food stores empty and hunger high, whether or not anyone died
    double starved = 0;      // people who starved to death, all towns
    double game_taken = 0, game_left = 0;   // head of beef-equivalent hunted since the log began; share of the herds left at the end

    // Towns
    std::vector<TownOutcome> towns;
    double pop_start_total = 0, pop_end_total = 0;

    // Money
    double money_start = 0, money_end = 0, residual = 0;
    double east_buys = 0, east_sells = 0, emigrated = 0;
    double richest_cash = 0;
    std::string richest_town;
    std::vector<double> east_bought_by_good;   // dollars, by good id
    std::vector<double> east_sold_by_good;
    std::vector<double> bought_by_freelancers, bought_by_firms;   // dollars by good
    double east_price_low = 1;                 // lowest eastern price multiplier seen (depth check)

    // Invariants
    bool invariants_ok = true;
    std::string first_violation;
};

// Runs the scenario to completion with the given seed and collects statistics.
// How a run is driven beyond "step to the end". `load` names a saved state to continue from (it must come from the same data and
// seed; the run then covers the days that remain, so counters taken from the event log include the history before the snapshot
// while ones sampled each day, such as the lowest trader count, cover only the days after it). `checkpoint_every` > 0 saves
// `checkpoint_prefix`_sSEED_dDAY.sav every that many days. `series_every` > 0 records one SeriesRow that often.
struct RunOptions {
    std::string load;
    int checkpoint_every = 0;
    std::string checkpoint_prefix;
    int series_every = 0;
};
RunStats run_and_collect(const WorldDef& world, Scenario scenario, std::uint64_t seed, const RunOptions& opt = {});

}  // namespace frontier
