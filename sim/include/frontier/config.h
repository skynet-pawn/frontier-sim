#pragma once
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

#include "frontier/types.h"

namespace frontier {

// ---- Definitions loaded from data/*.json. Nothing here changes at runtime. ----

struct GoodDef {
    std::string id, name, unit;
    double base_cost = 1;       // "balanced" price at a node holding its normal stock
    double weight_lb = 1;       // weight used to price freight
    double load_lb = -1;        // weight counted against a trader's carrying capacity
    double need_per_person_per_day = 0;
    double food_share = 0;      // share of a person's diet (essential foods sum to 1)
    double spoilage_per_day = 0;
    bool self_propelled = false;  // walks to market (cattle): moved by drovers, not by freight wagons
    double min_target = 10;     // floor on a node's "normal stock", for goods nobody consumes
    double freight_mult[kModeCount] = {1, 1, 1};  // per-mode adjustment (e.g. cattle on rail)
};

struct ProductionDef {
    GoodId good = -1;
    double rate = 0;            // units per day at full labor
    double labor_pop = 0;       // population needed for full output (0 = no labor limit)
    long from = LONG_MIN, until = LONG_MAX;
};

struct NodeDef {
    std::string id, name, kind;
    double x = 0, y = 0;        // miles on the map
    double population = 0, capacity = 0, cash = 0, gov_income = 0;
    double base_population = 0;  // residents who live off the surrounding land, not off trade
    bool fixed_market = false;      // an eastern market: fixed prices, unlimited cash and stock
    bool transient_inflow = false;  // railheads, camps, forts: where drifters collect
    std::vector<std::string> tags;
    std::vector<double> stock, fixed_price, reserve, extra_demand;
    std::vector<ProductionDef> production;
};

struct EdgeDef {
    NodeId a = -1, b = -1;
    Mode mode = Mode::Trail;
    double miles = 0;
    double hazard = -1;         // daily chance of a mishap; <0 means use the mode default
    long from = LONG_MIN, until = LONG_MAX;
    std::string note;           // why it opens or closes, shown in the event log
};

struct WorldDef {
    std::vector<GoodDef> goods;
    std::vector<NodeDef> nodes;
    std::vector<EdgeDef> edges;
};

// Every tunable number in the model. Defaults live here; scenario.json
// overrides any scalar by name under "params".
struct Params {
    // Prices: price = base_cost * (normal_stock / stock) ^ elasticity, clamped.
    double price_elasticity = 0.7;
    double price_min_mult = 0.15;
    double price_max_mult = 12;      // ceiling for goods the node consumes
    double price_max_mult_producer = 1.5;  // ceiling for goods the node only produces
    double cover_days = 30;          // days of consumption a node treats as "normal stock"
    double stock_cap_days = 90;      // producers lose output beyond this many days of stock
    double spread = 0.10;            // merchant margin between buy and sell price

    // Movement and freight ($ per pound-mile). Verify against sources before calibrating.
    double speed[kModeCount] = {12, 50, 120};  // miles/day: ox wagon, steamboat, rail
    double freight_rate[kModeCount] = {0.00015, 0.00005, 0.00003};
    double hazard[kModeCount] = {0.002, 0.001, 0.0002};
    double mishap_loss_fraction = 0.25;

    // Towns: population follows the cash a town earns from trade and government.
    double support_income_per_person = 0.4;  // $/day of trade inflow that supports one non-farming resident
    double income_ema_alpha = 0.03;
    double pop_growth_rate = 0.002;          // per day
    double pop_decline_rate = 0.03;          // fraction of the gap closed per day
    double hunger_alpha = 0.1;
    double hunger_threshold = 0.15;
    double floor_pop = 5;
    double ghost_pop = 25;
    double newcomer_provision_days = 20;     // days of food new arrivals carry with them
    double emigrant_cash_share = 0.8;        // share of a town's cash that leaves with its departing residents
    double leaver_drifter_fraction = 0.15;   // share of people leaving a bust town who turn to drifting
    double transient_inflow_rate = 0.0004;   // drifters arriving per resident per day
    double drifter_decay = 0.01;
    double law_tax_rate = 0.02;              // share of local inflow set aside for law enforcement
    double visit_spend = 3.0;                // what a trader party spends per town visit
    double guard_visit_spend = 1.0;
    double visit_spend_value_rate = 0.015;   // share of cargo value spent in town: drovers' wages, supplies, saloons

    // Traders.
    double drover_fraction = 0.35;           // share of trading outfits that are cattle drovers (the rest are freighters)
    double trader_capacity_lb = 12000;
    double trader_upkeep = 1.5;              // $/day
    double guard_wage = 1.2;                 // $/day per guard
    int guard_step = 3;
    int max_guards = 6;
    double trader_base_defenders = 2;
    double guard_strength = 1.5;
    double assumed_gang_men = 8;
    double risk_prior = 0.002;               // prior daily robbery risk per trader on any edge
    double risk_prior_weight = 0.3;
    double risk_ema_alpha = 0.03;
    double fear_bump = 0.04;                 // daily risk a trader adds to an edge after being robbed there
    double fear_decay = 0.01;
    double risk_aversion_min = 0.8, risk_aversion_max = 1.3;
    double risk_aversion_bump = 0.3, risk_aversion_cap = 4, risk_aversion_decay = 0.002;
    double info_noise = 0.08;                // stale or imperfect price information
    double min_profit = 40;
    double avail_fraction = 0.6;             // share of a node's stock a trader will try to buy
    double deadhead_discount = 0.7;
    double trader_join_rate = 0.02;          // daily chance an eastern investor outfits a new trader
    double trader_join_opportunity_ref = 400;  // profit on a standard load (avg of the best few) at which joining doubles
    double trader_kill_prob = 0.10;

    // Bandits.
    double gang_encounter_prob = 0.25;       // daily chance a gang on an edge meets a given trader
    double gang_upkeep = 0.6;                // $/man/day
    double gang_recruit_cost = 150;
    int gang_min_men = 3;
    int gang_max_men = 30;                   // beyond this a band can't hide, feed itself or keep order
    double gang_form_min_drifters = 6;
    double gang_form_chance = 0.03;
    int gang_review_days = 10;
    double gang_range_miles = 150;
    double fence_rate = 0.45;                // fraction of value a fence pays for stolen goods
    double fence_cash_share = 0.5;           // the fence can spend at most this share of its cash
    double gang_loss_when_repulsed = 0.3;
    double law_deter_scale = 1500;           // law fund at which a town halves a gang's interest

    // Law.
    double outrage_per_robbery = 1.0;
    double outrage_decay = 0.03;
    double posse_outrage_threshold = 2.0;
    double posse_cost_per_man = 25;
    int posse_min_men = 6, posse_max_men = 25;
    double posse_speed = 25;
    int posse_cooldown = 30;                 // days; shortened further when outrage is well above the threshold
    double posse_range = 200;
    double gang_terrain_advantage = 1.3;

    // Leaders for hire: a seasoned man who can organize and train a posse (strength multiplier = skill).
    int posse_min_men_led = 4;               // a trained posse can be smaller than an untrained one
    double leader_fee_per_day = 8;
    double leader_bonus_share = 0.2;         // share of seized loot paid to the leader on a win
    double leader_range = 400;               // miles a town will look for a leader
    double leader_death_prob = 0.25;         // chance a leader dies when his posse loses
    double leader_skill_gain = 0.05;
    double leader_skill_cap = 2.5;
    double leader_emerge_chance = 0.2;       // chance a victorious posse produces a new leader
};

struct ShockDef {
    std::string name;
    std::string kind;            // "stock_loss" or "rush"
    long date = 0;
    std::string tag;             // stock_loss: nodes carrying this tag
    std::vector<GoodId> goods;   // stock_loss: goods affected
    double fraction = 0;         // stock_loss: share destroyed
    NodeId node = -1;            // rush: where
    double people = 0;           // rush: how many arrive
    double cash_per_person = 0;  // rush: savings they bring
    std::string cause;
};

struct GangSeed {
    NodeId hideout = -1;
    int men = 8;
};

struct LeaderSeed {
    std::string name;
    NodeId home = -1;
    double skill = 1.5;
};

struct Scenario {
    std::uint64_t seed = 1;
    long start = 0;
    long log_from = 0;           // events before this date are warm-up (towns settling) and not shown
    int days = 3650;
    int traders = 120;
    int max_traders = 220;
    double capital_min = 1500, capital_max = 5000;
    std::vector<GangSeed> gangs;
    std::vector<LeaderSeed> leaders;
    std::vector<ShockDef> shocks;
    Params params;
};

// Both throw std::runtime_error with a readable message on bad data.
WorldDef load_world(const std::string& dir);
Scenario load_scenario(const std::string& dir, const WorldDef& world);

}  // namespace frontier
