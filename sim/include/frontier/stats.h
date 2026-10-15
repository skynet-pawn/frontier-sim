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

    // Events (by kind)
    int robberies = 0, robberies_over_1500 = 0, gangs_formed = 0, gangs_disbanded = 0;
    int posses = 0, posse_victories = 0, posse_routs = 0, leaders_killed = 0, leaders_emerged = 0;
    int detective_parties = 0, takeovers = 0, towns_freed = 0, infighting = 0, vigilance = 0;
    int famines = 0, ghost_towns = 0;

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
    double east_price_low = 1;                 // lowest eastern price multiplier seen (depth check)

    // Invariants
    bool invariants_ok = true;
    std::string first_violation;
};

// Runs the scenario to completion with the given seed and collects statistics.
RunStats run_and_collect(const WorldDef& world, Scenario scenario, std::uint64_t seed);

}  // namespace frontier
