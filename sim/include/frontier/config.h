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
    double garrison_need = 0;   // extra daily need per soldier (horses, forage, repairs), beyond the per-person need
    bool money = false;         // dust and bullion pass as currency in the camps that mine it
    double market_depth = 0;    // eastern markets: units outstanding that move the price by half (0 = bottomless)
    double freight_mult[kModeCount] = {1, 1, 1};  // per-mode adjustment (e.g. cattle on rail)
};

struct ProductionDef {
    GoodId good = -1;
    double rate = 0;            // units per day at full labor
    double labor_pop = 0;       // population needed for full output (0 = no labor limit)
    long from = LONG_MIN, until = LONG_MAX;
};

// A standing federal contract: the Army or Indian Bureau buys a fixed quantity daily at a fixed price,
// paid by the Treasury rather than out of the post's cash. Unfilled quantity accumulates as backlog.
struct ContractDef {
    GoodId good = -1;
    double price = 0;            // fixed price, or 0 to price at delivered cost plus `markup`
    double markup = 0;           // cost-plus: contracts are bid to cover supply and freight, with a margin
    bool per_soldier = false;    // quantity scales with the garrison (food, forage, remounts, repairs)
    double units_per_day = 0;    // otherwise a fixed quantity (agency beef)
};

struct NodeDef {
    std::string id, name, kind;
    double x = 0, y = 0;        // miles on the map
    double population = 0, capacity = 0, cash = 0, gov_income = 0;
    double base_population = 0;  // residents who live off the surrounding land, not off trade
    double garrison = 0;         // soldiers: fed by contract, paid by the Treasury, independent of local income
    std::vector<ContractDef> contracts;
    bool fixed_market = false;      // an eastern market: fixed prices, unlimited cash and stock
    bool transient_inflow = false;  // railheads, camps, forts: where drifters collect
    std::vector<std::string> tags;
    std::vector<double> stock, fixed_price, reserve, extra_demand;
    std::vector<ProductionDef> production;

    // Farming (farms.cpp). Farmers live off their own land and sell the surplus grain to the town and, if the
    // price justifies the freight, to places further off. Capacity is what the land, rainfall and water allow.
    double farmers = 0;          // farm population at the start (counted within `population`)
    double farm_capacity = 0;    // most farmers the surrounding land can carry
    double farm_yield = 1;       // 0..1.2: soil and rainfall; arid country yields less
    long farm_open = LONG_MIN;   // date the land opens to settlers (treaty, survey, railroad land grant)
    std::string climate;         // weather zone: neighbouring farms share droughts and locusts
    GoodId cash_good = -1;       // the district's cash crop besides grain (cotton), if any
    double cash_rate = 0;        // units of it marketed per farmer-day, over a normal year
    // A latent site is only a place on the map: no town until a reason to settle appears (see farms.cpp).
    bool latent = false;
    double found_pop = 40;       // settlers who arrive when the town is founded
};

struct EdgeDef {
    NodeId a = -1, b = -1;
    Mode mode = Mode::Trail;
    double miles = 0;
    double hazard = -1;         // daily chance of a mishap; <0 means use the mode default
    long from = LONG_MIN, until = LONG_MAX;
    std::string note;           // why it opens or closes, shown in the event log
};

// A private interest with money and reach: a railroad, express company or stock association. When robbers
// cost it enough it hires detectives. Its stations are also what gives nearby towns "backup".
struct InterestDef {
    std::string id, name, kind;   // kind: "railroad", "express" or "stock_association"
    NodeId hq = -1;
    std::vector<NodeId> nodes;    // stations / members
    double budget_per_day = 0;    // standing security budget
    double dues_rate = 0;         // share of trade inflow at its stations skimmed into the fund
    long from = LONG_MIN, until = LONG_MAX;
};

struct WorldDef {
    std::vector<GoodDef> goods;
    std::vector<NodeDef> nodes;
    std::vector<EdgeDef> edges;
    std::vector<InterestDef> interests;
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
    double market_recovery = 0.03;   // per day: how fast an eastern market absorbs a glut (or restocks)

    // Movement and freight ($ per pound-mile). Verify against sources before calibrating.
    double speed[kModeCount] = {12, 50, 120};  // miles/day: ox wagon, steamboat, rail
    double freight_rate[kModeCount] = {0.00015, 0.00005, 0.00003};
    double hazard[kModeCount] = {0.002, 0.001, 0.0002};
    double mishap_loss_fraction = 0.25;

    // Towns: population follows the cash a town earns from trade and government.
    double support_income_per_person = 0.4;  // $/day of trade inflow that supports one non-farming resident
    double income_ema_alpha = 0.03;
    double pop_growth_rate = 0.002;          // per day
    double growth_cover_days = 30;           // newcomers hear of empty shelves: arrivals slow as days of food on hand fall below this (0 = ignore food)
    double pop_decline_rate = 0.03;          // fraction of the gap closed per day
    double hunger_alpha = 0.1;
    double hunger_threshold = 0.15;
    double floor_pop = 5;
    double ghost_pop = 25;
    double newcomer_provision_days = 20;     // days of food new arrivals carry with them
    double rush_provision_days = 60;         // gold-rush argonauts were told to bring a season's supplies
    double emigrant_cash_share = 0.8;        // share of a town's cash that leaves with its departing residents
    double leaver_drifter_fraction = 0.15;   // share of people leaving a bust town who turn to drifting
    double transient_inflow_rate = 0.0004;   // drifters arriving per resident per day
    double drifter_decay = 0.01;
    double law_tax_rate = 0.02;              // share of local inflow set aside for law enforcement
    double garrison_pay_per_soldier = 0.45;  // $/day a soldier spends in the post town (sutlers, saloons, laundresses)
    double quartermaster_share = 0.7;        // share of a post's contract needs the Army's own trains deliver
    int quartermaster_interval_days = 30;    // how often a supply train reaches a post
    double depot_floor_days = 45;            // a post never let its stores fall below this: a train that finds the store lower tops it up
    double contract_backlog_days = 60;       // unfilled contract quantity stops accumulating beyond this
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
    double risk_aversion_min = 0.5, risk_aversion_max = 1.1;          // freelancers: gamblers and fortune seekers
    double firm_risk_aversion_min = 1.6, firm_risk_aversion_max = 2.6;  // eastern firms: they want certainty
    double risk_aversion_bump = 0.3, risk_aversion_cap = 4, risk_aversion_decay = 0.002;
    double info_noise = 0.08;                // stale or imperfect price information
    double inbound_awareness = 0.0;          // share of the freight already on the road to a town that traders count against its price
                                             // (wagons pass each other and word travels; 0 = each trader prices the shelf as it stands today)
    double min_profit = 40;
    double local_reserve_days = 12;          // a town's merchants keep this many days of their own people's needs off the market
    double avail_fraction = 0.6;             // share of a node's stock a trader will try to buy
    double deadhead_discount = 0.7;
    // Merchant imports: a town short of a staple orders it ahead from the cheapest source and pays from its own cash.
    int merchant_interval_days = 7;          // each town reviews its shelves weekly
    double merchant_min_pop = 150;           // below this there is no merchant class to order anything
    double merchant_reorder_days = 40;       // a supplying town keeps this many days of its own cover before it will sell on
    double merchant_safety_days = 40;        // a town reorders when its cover falls below the lead time of its supply plus this:
                                             // the review interval, the wait for a freighter, and a cushion for bad weather and robbery
    double merchant_cycle_days = 45;         // ...and orders enough to cover this many days beyond that point
    double merchant_cash_share = 0.5;        // most of a town's cash one round of orders may commit
    double merchant_margin = 0.15;           // landed cost must leave this much under the shelf price
    int merchant_order_raise_days = 7;       // an unfilled order's offer rises this often...
    double merchant_order_raise = 0.25;      // ...by this much...
    double merchant_order_cap_mult = 3.0;    // ...up to this multiple of the opening offer
    int merchant_hire_after_days = 14;       // an order still open after this long: the town hires freighters itself (0 = never)
    int merchant_order_expire_days = 90;     // and is withdrawn after this long unfilled

    // Standing supply contracts (contracts.cpp): a town that has been short buys reliability from a freight house.
    double contract_share = 1.0;             // share of the town's daily need the contract promises (0 = no contracts)
    double contract_premium = 0.6;           // price paid over the landed cost: the insurance the house is paid for holding capacity
    int contract_memory_days = 730;          // a town signs only if it went short within this long
    int contract_term_days = 365;
    int contract_max_behind_days = 90;       // the town drops a house that is this many days of deliveries behind
    double lookahead_weight = 0.5;           // share of the best next load at a destination that counts toward a trip's worth (freighters ran round trips)

    // Who comes west. Agents never see the sim's true profit: freelancers hear rumors (only the winners talk),
    // eastern firms read reports that are months old. Both are deterred by danger in proportion to their risk aversion.
    int firms_initial = 20;                  // of the scenario's starting traders
    double firm_capital_min = 10000, firm_capital_max = 30000;
    double firm_capacity_lb = 48000;         // a freight line, not two wagons
    double firm_upkeep = 6.0;                // managers, teamsters, agents
    double firm_drover_fraction = 0.2;
    double freelancer_entry_rate = 0.25;     // arrivals per day when rumor is at its reference level
    double story_profit_threshold = 2000;    // a trip this profitable becomes a story worth telling
    double story_ref = 2.0;                  // stories per day at which arrivals run at the base rate
    double story_alpha = 0.05;               // rumor memory: about 20 days
    double story_fade = 0.03;                // a trader's recent winnings fade by this share a day (about a month)
    double entry_danger_floor = 0.2;         // danger never shuts the frontier completely: some always come
    double firm_entry_rate = 0.03;           // firms outfitted per day at one hurdle above the hurdle return
    double firm_hurdle = 0.0004;             // daily return on capital a firm needs (about 15% a year) after risk
    double return_alpha = 0.01;              // report lag: about 100 days
    double danger_ref = 0.004;               // robberies per trader-day that look like "the frontier is deadly"
    double danger_alpha = 0.02;
    // Freight houses: a trader this rich founds one and runs other men's wagons. Rough numbers; tune from the runs.
    double house_float = 8000;               // working capital the house gives each outfit
    double house_found_cash = 24000;         // a trader holding this much founds a house (three floats: his own and two to spare)
    double house_found_chance = 0.05;        // daily, once he is that rich
    double house_nerve = 1.0;                // 1: a leader weighs risk only by the share of his wealth on the load; 0: as a lone trader would
    int house_max_outfits = 12;
    int house_hire_days = 30;                // at most one new outfit a month
    double settle_cash = 20000;              // a freelancer this rich may settle down
    double settle_chance = 0.004;
    double settle_invest_share = 0.7;        // share of his stake invested in the town where he settles
    double firm_withdraw_rate = 0.01;        // daily, scaled by how far returns fall short of the hurdle
    double price_uncertainty_weight = 0.5;   // how much a thin spot market worries a risk-averse outfit
    double spot_uncertainty_base = 0.10;
    double spot_uncertainty_thin = 0.40;
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
    // What a gang can carry off, and whether a stop is worth it. A rider takes what fits on his saddle, so cheap bulk
    // (flour, timber) is never worth a fight while gold, ammunition and whiskey are. All rough; tune from the runs.
    double gang_carry_lb_per_man = 60;       // lb a rider can take off a wagon
    double gang_head_per_man = 15;           // head of livestock one man can drive off
    double gang_min_take_per_man = 5;        // $ a stop must yield per man before the gang bothers
    double gang_desperate_days = 5;          // days of cash on hand below which a gang takes whatever it can get
    double gang_desperate_floor_mult = 0.2;  // a desperate gang's floor (and it will take food)
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

    // Private interests and the detectives they hire.
    double interest_loss_decay = 0.02;       // per day: old losses are forgotten
    double agency_loss_threshold = 1500;     // recent losses at which an interest hires detectives
    double agency_min_fund = 2500;
    double agency_cost_per_man = 60;
    int agency_min_men = 8, agency_max_men = 40;
    double agency_skill = 1.8;               // professionals: strength multiplier
    int agency_cooldown = 90;
    double backup_range = 250;               // miles: an interest this close counts as backing the local law

    // Town capture by gangs.
    double capture_gain = 1.0;               // influence per day = gain * spending / (town income + 50) / law resistance
    double capture_decay = 0.02;
    double capture_threshold = 1.0;
    int capture_min_men = 8;
    double capture_law_resist = 3000;        // law fund at which a town halves a gang's influence
    double tribute_base_min = 0.10;          // a perfect boss takes this share of town inflow...
    double tribute_spread = 0.30;            // ...and a hopeless one this much more
    double tribute_squeeze = 0.05;           // how hard a broke boss squeezes (scaled per day)
    double tribute_cap = 0.6;
    double infight_rate = 0.004;             // daily chance of a gang tearing itself apart, scaled by incompetence squared
    double vigilance_rate = 0.01;            // daily chance per unit of grievance
    int vigilance_min_days = 60;
    double vigilance_min_pop = 150;
    double vigilance_pop_per_man = 12;

    // Leaders for hire: a seasoned man who can organize and train a posse (strength multiplier = skill).
    int posse_min_men_led = 4;               // a trained posse can be smaller than an untrained one
    double leader_fee_per_day = 8;
    double leader_bonus_share = 0.2;         // share of seized loot paid to the leader on a win
    double leader_range = 400;               // miles a town will look for a leader
    double leader_death_prob = 0.25;         // chance a leader dies when his posse loses
    double leader_skill_gain = 0.05;
    double leader_skill_cap = 2.5;
    double leader_emerge_chance = 0.2;       // chance a victorious posse produces a new leader

    // Farms and new towns. Yields and prices are rough; verify before calibrating.
    double farm_surplus_per_farmer = 0.05;   // cwt of flour-equivalent marketed per farmer-day, over a normal year
    int farm_harvest_start = 190;            // day of year the grain harvest begins (early July)
    int farm_harvest_days = 100;             // grain comes in over about three months...
    double farm_release_rate = 0.008;        // ...and goes to market steadily out of the barn (share of the store per day)
    double farm_forage_ratio = 1.0;          // hay and feed grain cwt per cwt of flour
    double farm_break_even_share = 0.6667;   // a farmer must net this share of a crop's base price to stay on the land
    double farm_marketing_share = 0.85;      // share of the merchant's price that reaches the farmer
    double farm_outlet_units = 300;          // size of the sale a farm district prices its outlets by
    double farm_weather_sd = 0.3;            // year-to-year swings in yield
    double farm_settle_base = 0.0005;        // new farmers per day, as a share of capacity...
    double farm_settle_growth = 0.002;       // ...plus a share of the farmers already there (neighbours follow neighbours)
    double farm_leave_rate = 0.0012;         // share of the surplus farmers who give up each day
    double farm_core_share = 0.6;            // share of the starting farmers who stay however the price falls: they eat what they grow
    double farm_gate_cap = 2.5;              // settlers discount price spikes: the gate price counts for at most this many break-evens
    double farm_gate_alpha = 0.1;            // how fast the remembered farm-gate price follows the market (monthly)
    double found_gate_ratio = 1.25;          // a site is settled when the remembered gate price is this many break-evens
    int found_months = 3;                    // ...for this many months running
    double found_cash_per_person = 40;       // savings settlers bring
};

struct ShockDef {
    std::string name;
    std::string kind;            // "stock_loss", "rush" or "crop_failure"
    long date = 0;
    std::string tag;             // stock_loss: nodes carrying this tag; crop_failure: the weather zone
    std::vector<GoodId> goods;   // stock_loss: goods affected
    double fraction = 0;         // stock_loss: share destroyed
    NodeId node = -1;            // rush: where
    double people = 0;           // rush: how many arrive
    double cash_per_person = 0;  // rush: savings they bring
    std::string cause;
};

// A change in garrison strength driven by events outside the model: a campaign, a new post, a withdrawal.
// Troop levels follow external threats, not local robberies.
struct CampaignDef {
    std::string name;
    long from = 0, until = LONG_MAX;
    std::vector<NodeId> nodes;
    double garrison_add = 0;
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
    std::vector<CampaignDef> campaigns;
    std::vector<ShockDef> shocks;
    Params params;
};

// Both throw std::runtime_error with a readable message on bad data.
WorldDef load_world(const std::string& dir);
Scenario load_scenario(const std::string& dir, const WorldDef& world);

}  // namespace frontier
