#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "frontier/config.h"

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
struct RunStats {
    std::uint64_t seed = 0;
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
    double drifter_flow[16] = {};   // men, whole run (warm-up included)
    long gangs_ended = 0;
    double gang_life_mean = 0;                                  // mean days a gang lasts
    long large_ended = 0;                                       // gangs that ever reached 6 men and broke up, and how: law, money, rivals, quarrels, freighters, other
    double large_life_mean = 0;
    int large_end[7] = {};
    int papers_end = 0, papers_founded = 0;
    long bushwhacks = 0, bush_killed = 0, bush_notable = 0;
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
    int contracts_signed = 0, contracts_cancelled = 0;
    int jobs_taken = 0, jobs_delivered = 0;
    double job_pay = 0, job_lost = 0;   // what carriers earned hauling suppliers' goods, and what the road took from suppliers
    double contract_units = 0, contract_revenue = 0, contract_outlay = 0;   // what the houses delivered, took in and laid out
    double merchant_delivered = 0, merchant_lost = 0;   // dollars of ordered goods that arrived / were robbed
    int famines = 0, ghost_towns = 0, waypoint_sales = 0;
    int houses_founded = 0, houses_end = 0, house_outfits_end = 0, house_recaps = 0, house_killed = 0, house_broke = 0;
    int famine_events = 0;   // raw famine log entries, before merging flicker into episodes
    int famine_towns = 0;    // distinct towns that had a famine (a spell of starvation that killed a real share of the town)
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
RunStats run_and_collect(const WorldDef& world, Scenario scenario, std::uint64_t seed);

}  // namespace frontier
