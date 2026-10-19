#pragma once
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

#include "frontier/terrain.h"
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
    bool game_substitute = false; // wild game stands in for this food where hunters can take it (beef)
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
    std::string region;             // the territory, state or reserve it lies in: law and news are regional
    int law = -1;                   // law at the start: 0 none, 1 town marshal, 2 county sheriff and deputies (-1: from size)
    int paper = -1;                 // 1 = a newspaper at the start, 0 none (-1: from size)
    std::vector<double> stock, fixed_price, reserve, extra_demand;
    std::vector<ProductionDef> production;

    // Farming (farms.cpp). Farmers live off their own land and sell the surplus grain to the town and, if the
    // price justifies the freight, to places further off. Capacity is what the land, rainfall and water allow.
    double farmers = 0;          // farm population at the start (counted within `population`)
    double farm_capacity = 0;    // most farmers the surrounding land can carry
    double game_capacity = 0;    // wild game within hunting range, in head of beef-equivalent meat (0 = none to speak of)
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
    std::vector<std::pair<double, double>> path;   // the way the route runs over the terrain (map miles, a to b); empty = straight
    int river_crossings = 0;    // rivers on the way (a ford, ferry or bridge each) and the biggest one's size
    int river_biggest = 0;
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

// A jurisdiction. Counties, courts and sheriffs exist only once the territory is organized; before that a camp has a
// miners' meeting or a marshal its citizens hire (Deadwood in 1876 stood on Sioux treaty land with no legal government).
struct RegionDef {
    std::string name;
    long organized = LONG_MIN;      // the day county government and sheriffs become possible
};

struct WorldDef {
    std::vector<RegionDef> regions;
    std::vector<GoodDef> goods;
    std::vector<NodeDef> nodes;
    std::vector<EdgeDef> edges;
    std::vector<InterestDef> interests;
    std::vector<RiverLine> rivers;   // from data/rivers/NAME when world.json names them
    std::vector<CrossingSite> crossings;   // the fords, ferries and bridges recorded there
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
    // Hunting (simulation.cpp): some of a town's people live partly off game. It thins as the town grows and returns slowly.
    double hunter_share = 0.03;              // share of the civilians who hunt for the table or the market
    double hunter_catch = 0.04;              // meat a hunter brings in a day at full game density, in head of beef-equivalent (about 20 lb)
    double game_regrowth = 0.0008;           // daily growth of the herd toward its capacity (about 30% a year when thinned)
    double game_inflow = 0.00004;            // game drifting in from the surrounding country, as a share of capacity a day
    double deprivation_alpha = 0.03;         // how slowly the deprivation average follows unmet food need (about a month)
    double starvation_threshold = 0.30;      // deprivation above this starts killing people (below it they forage, tighten belts, leave)
    double starvation_max_rate = 0.005;      // daily death rate among the market-fed at total deprivation (about 30% in ten weeks)
    double famine_death_share = 0.10;        // a famine is a spell of starvation that kills this share of the town
    int famine_end_days = 30;                // ...and it ends after this many days with nobody starving
    double floor_pop = 5;
    double ghost_pop = 25;
    double newcomer_provision_days = 20;     // days of food new arrivals carry with them
    double rush_provision_days = 60;         // gold-rush argonauts were told to bring a season's supplies
    double emigrant_cash_share = 0.8;        // share of a town's cash that leaves with its departing residents
    double leaver_drifter_fraction = 0.15;   // share of people leaving a bust town who turn to drifting
    double transient_inflow_rate = 0.0004;   // drifters arriving per resident per day
    // Where outlaws come from. With crime_source 1 every frontier town draws idle men in proportion to its people, and the men
    // there turn to crime in proportion to the money moving through it per head and how little law it has (a gold camp or a
    // railhead breeds outlaws; a county seat with a sheriff or a poor farm village barely does). The factors are soft weights,
    // never gates. 0 uses the old hand-set transient_inflow flag.
    // Hideouts. With camp_stages 1 a gang lives at a camp off the roads (a dugout, a taken or abandoned cabin, a ruined fort),
    // a few miles up its trail from the nearest town, and robs on the roads. It rides into town only to spend after a score,
    // so lawmen find it on the road or on a spree, not at home. A careful boss keeps his men quiet in town; a careless one
    // starts trouble, and trouble is what raises a posse.
    int camp_stages = 1;
    double camp_min_miles = 15;              // camp distance from the nearest town along its trail
    double camp_max_miles = 50;
    double camp_escape = 0.3;                // share of would-be catches on the road that end with the band reaching its camp
    double camp_visit_chance = 0.015;        // daily chance a gang with money rides in for a bath, a drink and a card game (about every two to three months, as it also needs the cash)
    double camp_visit_cash_per_man = 30;     // cash a man's share must reach before the gang rides in ($)
    double camp_visit_spend = 0.5;           // share of the gang's cash left in town on a visit
    // Bosses differ in how well they keep their men quiet in town, apart from how well they hold them together: a tenth are
    // careful, a tenth careless, the rest ordinary. The chance a visit ends in trouble (a fight, a shooting) follows the type.
    double camp_careful_share = 0.1;
    double camp_careless_share = 0.1;
    double camp_trouble_careless = 0.2;
    double camp_trouble_ordinary = 0.1;
    double camp_trouble_careful = 0.02;
    double camp_trouble_outrage = 1.5;       // outrage the town feels after trouble
    double camp_visit_catch = 0.10;          // chance lawmen take a band on a visit (three times that after trouble)
    // Being found. Lawmen follow a band's tracks only if it leaves some: repeated hold-ups on one trail near its camp (its heat).
    // Petty thefts in the wilderness leave nothing to follow. Heat fades with a half-life; a band that moves to a new trail starts
    // over; a camp far up the trail is harder to link to the crimes.
    double camp_heat_per_fame = 2.0;         // heat added by a deed, per unit of the fame it earns (robbery 1.0, toll 0.3, kill 1.0, fight 0.6)
    double camp_heat_bush = 0.2;             // heat from waylaying a lone man
    double camp_heat_halflife = 30;          // days
    double camp_heat_move = 0.25;            // share of the heat that follows a band to a new trail
    double camp_notice_ref = 3;              // heat at which a band is fully findable
    double camp_notice_floor = 0.1;          // how findable a band with no heat still is
    double camp_far_miles = 40;              // findability falls to 1 / (1 + camp miles / this)
    double camp_found_catch = 0.01;          // daily chance lawmen find a band's camp, at full heat and in the open, times the law around the nearest town
    double camp_fence_catch = 0.002;         // chance a fence sale of loot is noticed and a band caught
    // Jail. A band the law catches is not simply gone. Where there is law its men go to jail for a term that depends on what
    // they did (petty theft only: days; hold-ups: months; longer for men with a record, never more than a year: past that a town
    // hangs them), and killers or a long string of hold-ups risk the rope. They come out with a record, and as hardened men they
    // form the next bands. Where there is no law there is no jail, and the town runs them out.
    int jail_stages = 1;
    double jail_petty_min = 5, jail_petty_max = 30;     // days, band with no hold-up or killing behind it
    double jail_job_min = 60, jail_job_max = 180;       // days, band that has robbed
    double jail_repeat_mult = 2.0;                      // times longer for a man with a record
    double jail_cap_days = 365;                         // the longest a frontier jail holds anyone
    double jail_marshal_cap = 30;                       // a marshal's lockup holds men for days, not months
    int jail_hang_jobs = 8;                             // hold-ups after which a caught band risks hanging,
    int jail_hang_kills = 2;                            // or killings
    double jail_hang_chance = 0.5;                      // doubled if men in the band have a record
    double jail_hang_share = 0.4;                       // share of the men hanged, the leader among them; the rest serve a year
    double ex_con_fights = 10;                          // fights a man with a record counts as having survived (10 makes him a veteran)
    int crime_source = 1;
    double crime_source_scale = 4.0;         // rescales the whole rule so the 31-node map keeps its calibrated crime
    double crime_income_ref = 1.0;           // town income per head per day that counts as rich ($)
    double crime_income_floor = 0.25;        // what a town with no money flowing still draws
    double crime_law_none = 1.0;             // draw by law level: none, marshal, sheriff
    double crime_law_marshal = 0.5;
    double crime_law_sheriff = 0.15;
    double drifter_decay = 0.01;
    // Law by stages (law.cpp). A new camp has no law at all; a marshal comes when the town is big enough or has been hurt
    // enough, a sheriff and deputies when the territory is organized and the town is a county seat in all but name.
    // Money pays for law once it exists and makes posses better; it does not create it.
    int law_stages = 1;
    double law_marshal_pop = 150;            // civilians at which a town hires a marshal (after law_marshal_days of being that big)
    int law_marshal_days = 120;
    double law_marshal_min_pop = 60;         // a smaller camp hires one only after real outrage
    double law_marshal_outrage = 3;
    double law_sheriff_pop = 500;            // civilians at which the county puts a sheriff in an organized territory
    int law_sheriff_days = 60;
    int law_lapse_days = 240;                // a town this far below its threshold for this long loses its officer
    int posse_max_men_marshal = 8;           // a marshal and a few citizens; a sheriff can swear in a full posse
    double vigilance_cost_per_man = 4;       // a committee in a town with no law pays its men from the town's cash
    // Infamy (crime.cpp). A region hunts only the gangs it has heard of; a gang that has become too well known where it works
    // moves on to country that has not. Bigger gangs are noticed sooner.
    double posse_base_skill = 0.6;           // what a citizen posse man is worth against a hardened gang man, on average (varies 0.5x to 1.5x)
    int posse_rout_pause = 90;               // days a town waits to send another posse after losing one
    int gang_large_men = 8;                  // a gang that ever had this many men counts as a large gang in the statistics
    int fame_stages = 1;
    double fame_robbery = 0.5, fame_toll = 0.15, fame_kill = 0.5, fame_fight = 0.3;   // what a deed adds in the regions it touches
    double fame_decay = 0.005;               // daily fade: about 140 days to halve
    double fame_posse_min = 1.0;             // a town raises a posse only against a gang its region knows at least this well
    double fame_catch_floor = 0.3;           // a band nobody has heard of is caught at this share of the usual chance
    double fame_catch_cap = 2.0;             // and at most this multiple, however famous
    double fame_flee = 0.6;                  // weight of local fame against a trail's attraction
    double fame_flee_trigger = 3.0;          // heat (fame x size) at which a gang leaves for fresh country
    double fame_size_ref = 8;                // men at which a gang counts 1.5x for being noticed
    double gang_flee_range = 450;            // miles a fleeing gang may move its hideout
    double law_presence_none = 0.15, law_presence_marshal = 0.6;   // how much lawmen's reach counts where there is no law / a marshal (a sheriff counts 1)
    // Bushwhacking (crime.cpp): a band also waylays lone men leaving the hideout town. A resident who is killed is missed or found,
    // and that angers the town; a stranger passing through is noticed far less. A man with connections elsewhere can bring in the Army.
    int bush_stages = 1;
    double bush_rate = 0.02;                 // daily chance a band waylays someone
    double bush_take = 80;                   // dollars a victim carries, at most, drawn from the town's cash
    double bush_resident = 0.55;             // share of victims who live in the town (the rest are strangers: gamblers, drifters)
    double bush_kill = 0.25;                 // chance the victim is killed
    double bush_outrage_resident = 1.0;      // outrage a missed or found resident adds, doubled if he was killed
    double bush_outrage_stranger = 0.05;     // a stranger's loss: fear more than anger
    double bush_seen = 0.3;                  // chance a witness can name the gang
    double bush_notable = 0.01;              // chance the victim is a man with connections elsewhere
    double bush_notable_outrage = 4;
    double army_range = 300;                 // miles within which a post sends a detachment after the murder of one of its own
    double army_skill = 1.3;
    int news_stages = 1;
    int news_paper_days = 180;               // days a town must be big enough (or too small) before its paper opens (closes)
    double news_speed = 60;                  // miles a day a story travels by wire, rail and rider
    double news_range = 900;                 // miles a very big story carries (smaller stories carry less far)
    int news_keep = 12;                      // headlines a paper's recent file holds
    double news_fame = 0.4, news_fame_cap = 3.0;   // a gang's fame rises this much (times the story's weight) where a paper prints it
    double news_paper_pop = 800;             // civilians at which a town supports a newspaper (news.cpp)
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
    // The winter store: a northern storekeeper laid in goods in the autumn for the months the roads closed, because a camp
    // cut off by snow could not send for more. From the lay-in month to the end of February his reorder point and order-up-to
    // target both rise by this many days of cover (0 = off).
    double winter_cover_days = 0;
    double winter_min_y = 1000;              // towns this far north (map miles) lay in a winter store
    int winter_lay_in_month = 9;             // the store is built from this month (September) through February
    // A trader does not weigh every town on the map: news and acquaintance reach so far. He considers the nearest towns by
    // travel time and the eastern markets (which everyone knows of). Also what keeps the cost of a day linear in the map's size.
    int trade_horizon = 64;
    double merchant_cash_share = 0.5;        // most of a town's cash one round of orders may commit
    double merchant_margin = 0.15;           // landed cost must leave this much under the shelf price
    int merchant_order_raise_days = 7;       // an unfilled order's offer rises this often...
    double merchant_order_raise = 0.25;      // ...by this much...
    double merchant_order_cap_mult = 3.0;    // ...up to this multiple of the opening offer
    int merchant_hire_after_days = 14;       // an order still open after this long: the town hires freighters itself (0 = never)
    int merchant_order_expire_days = 90;     // and is withdrawn after this long unfilled

    // Freight jobs (jobs.cpp): a town's open order is work for a carrier, who hauls the supplier's goods for what the order pays beyond them.
    int carrier_jobs = 2;                    // 1: freelancers and house outfits may carry an open order on the supplier's goods instead of buying a load; 2: eastern firms too
    double job_rate_share = 0.25;             // share of the going freight rate a carrier is paid; scarcity (an order's escalated offer) scales it up
    double job_preference = 1.0;             // how a carrier weighs a job's pay against a speculative load of the same expected profit

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
    // Word of hunger also draws freelancers: a camp short of food is news, and it travels by letter and telegraph to the
    // railheads. It is rumor, not knowledge: no one is told what the gap is worth. (Escalated orders were tried as the signal
    // and are no use: some town is always waiting on something, and the talk did not move when a third of the freight left.)
    double entry_scarcity_weight = 1.0;        // arrivals from scarcity talk, as a multiple of the base rate (0 = off)
    double scarcity_ref = 0.1;               // a rise of this share of towns in shortage above their usual level counts as "the camps are crying out"
    double scarcity_memory = 0.003;          // what is usual: the talk averaged over about a year (some town is always waiting on something)
    double scarcity_alpha = 0.05;            // how long the talk lasts: about 20 days
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
    int gang_min_men = 2;                    // below this a band is no band (it takes two to hold up a wagon)
    int gang_max_men = 30;                   // beyond this a band can't hide, feed itself or keep order
    double gang_form_min_drifters = 6;
    double gang_form_chance = 0.03;
    int gang_review_days = 10;
    double gang_range_miles = 150;
    double fence_rate = 0.45;                // fraction of value a fence pays for stolen goods
    double fence_cash_share = 0.5;           // the fence can spend at most this share of its cash
    double gang_loss_when_repulsed = 0.3;
    // Gang wars, veterans and the size a boss can hold together (crime.cpp). Off (0) gives the old model: a hard
    // cap of gang_max_men, no fights between gangs, and every man counts the same in a fight. All rough guesses.
    int gang_wars = 1;
    double gang_manageable_base = 12;        // men a middling boss (competence 0.5) holds; a hopeless one holds half, a shrewd one twice
    double gang_veteran_fights = 10;         // shootouts that make a man a veteran
    double gang_veteran_hold = 0.5;          // men of manageable size each veteran adds (lieutenants who keep order)
    double gang_veteran_strength = 1.5;      // a veteran counts for this many men in a fight
    double gang_breakdown_rate = 0.02;       // daily chance of a breakdown times (men / manageable - 1) squared
    double gang_breakdown_loss = 0.3;        // share of the men lost to a breakdown (half scatter as drifters, the rest are killed)
    int gang_hard_max = 80;                  // recruiting stops here; the cost of each man rises with the gang's size well before
    double gang_casualty_random = 0.25;      // share of casualties that fall on a random man instead of the least experienced
    double gang_rival_fight_chance = 0.04;   // daily chance two gangs working the same trail fight
    double gang_rival_loser_loss = 0.6;      // share of the loser's men lost
    double gang_rival_winner_loss = 0.4;     // winner loses this times the loser's share of the combined strength
    double gang_rival_loot_share = 0.5;      // share of the loser's cash the winner takes
    double gang_boss_fall_chance = 0.5;      // chance the loser's boss falls, times the share of men the loser lost
    double gang_merge_chance = 0.7;          // chance the winner takes the survivors of a gang whose boss fell
    double gang_win_competence = 0.03;       // a boss who wins a fight is a little more respected
    double gang_hardness = 1.3;              // a hardened gang's edge over a posse of townsmen, on top of terrain and veterans
    // Two tiers (crime.cpp): small bands of 2 to 5 men from the idle men who can fight, and a few large organized gangs
    // made of several bands that keep their own raiding parties. Off (0) gives one kind of gang formed straight from drifters.
    int gang_tiers = 1;
    double fighter_turn_rate = 0.006;        // daily share of idle men who turn to crime and become fighters (the rest find work, move on or starve)
    double fighter_decay = 0.01;             // daily share of fighters who give up, leave, are jailed or die
    double band_form_chance = 0.02;          // daily chance, at a town with at least band_min_men fighters, that some of them band together
    int band_min_men = 2, band_max_men = 5;
    double band_catch_base = 0.015;           // chance a hold-up by a band ends with lawmen catching it, times 1 plus the local law fund over law_deter_scale (up to band_law_cap)
    double band_law_cap = 2;                 // most the law fund can multiply a band's chance of being caught
    int band_merge_min_robberies = 2;        // a band must have pulled this many jobs without being caught before a gang will have it
    double band_join_chance = 0.5;           // chance, at each ten-day review, that a successful band joins a gang in reach
    int band_absorb_quiet = 1;               // a band with its jobs behind it may take in a smaller band that has not pulled any yet
    double band_join_range = 150;            // miles between hideouts within which bands and gangs find each other
    double gang_hire_below = 1.0;            // a gang takes in bands only while it is under this share of what its boss can hold
    double band_petty_take = 0.5;            // $ a man a day a band lifts from its hideout town (pilfering, lone travellers, small hold-ups)
    double band_petty_catch = 0.002;         // daily chance lawmen catch a band at its petty work, times 1 plus the town law fund over law_deter_scale
    // With camps (camp_stages 1) a band lives off the land, so its cash needs are small: ammunition, powder, tobacco. It lifts a
    // little now and then (a stray horse, a lone traveller, a store's till) but not every day; the real money is hold-ups, and the
    // real expense a spree in town.
    double band_upkeep = 0.12;               // $/man/day for a band (larger gangs still pay gang_upkeep)
    double band_petty_chance = 0.03;         // daily chance a band lifts something small
    double band_petty_event = 2.0;           // $ a man in such a take
    double band_min_odds = 0.65;            // a raiding party only stops a wagon it expects to beat (soft targets only)
    double law_overmatch = 2;                // a posse or detective party is sized to about this many times the gang's men (the law knows roughly how big a gang is), within what it can pay for
    int gang_party_max = 5;                  // men in one raiding party; a bigger gang works as several parties on the same trail
    // What a gang can carry off, and whether a stop is worth it. A rider takes what fits on his saddle, so cheap bulk
    // (flour, timber) is never worth a fight while gold, ammunition and whiskey are. All rough; tune from the runs.
    double gang_carry_lb_per_man = 60;       // lb a rider can take off a wagon
    double gang_head_per_man = 15;           // head of livestock one man can drive off
    double gang_min_take_per_man = 5;        // $ a stop must yield per man before the gang bothers
    double gang_desperate_days = 5;          // days of cash on hand below which a gang takes whatever it can get
    double gang_desperate_floor_mult = 0.2;  // a desperate gang's floor (and it will take food)
    double law_deter_scale = 4000;           // law fund at which a town halves a gang's interest

    // Law.
    double outrage_per_robbery = 1.0;
    double outrage_decay = 0.03;
    double posse_outrage_threshold = 8.0;   // a frontier town puts up with a good deal before it sends men out
    double posse_cost_per_man = 25;
    int posse_min_men = 6, posse_max_men = 25;
    double posse_speed = 25;
    int posse_cooldown = 120;                // days; shortened further when outrage is well above the threshold
    double posse_range = 200;
    double gang_terrain_advantage = 1.3;

    // Private interests and the detectives they hire.
    double interest_loss_decay = 0.02;       // per day: old losses are forgotten
    double agency_loss_threshold = 6000;     // recent losses at which an interest hires detectives
    double agency_min_fund = 2500;
    double agency_cost_per_man = 60;
    int agency_min_men = 4, agency_max_men = 12;   // a handful of operatives, not an army
    double agency_skill = 1.5;               // professionals: strength multiplier
    int agency_cooldown = 365;
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
    std::string kind;            // "stock_loss", "rush", "crop_failure", "road_closure", "house_collapse" or "firm_withdrawal"
    long date = 0;
    std::string tag;             // stock_loss: nodes carrying this tag; crop_failure: the weather zone
    std::vector<GoodId> goods;   // stock_loss: goods affected
    double fraction = 0;         // stock_loss: share destroyed; house_collapse: share of its outfits lost (the rest go independent)
    NodeId node = -1;            // rush: where
    double people = 0;           // rush: how many arrive
    double cash_per_person = 0;  // rush: savings they bring
    int days = 0;                        // road_closure: how long the roads stay shut
    std::vector<NodeId> nodes;           // road_closure: roads touching any of these towns
    bool modes[kModeCount] = {true, true, false};  // road_closure: which kinds of road (trail, river, rail)
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
    int cells = 0;               // raiding parties it is made of (0: as many as its size needs)
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
