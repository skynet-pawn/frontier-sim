#pragma once
#include <algorithm>
#include <climits>
#include <string>
#include <vector>

#include "frontier/config.h"
#include "frontier/types.h"

namespace frontier {

// Every way money enters or leaves the frontier economy. Anything else that changes a cash balance is a
// transfer between two parties inside it. The money audit checks that nothing else leaks.
enum class MoneyFlow {
    EastBuys,          // + an eastern market pays traders for frontier goods
    EastSells,         // - traders pay an eastern market for manufactured goods
    Government,        // + army pay, agency contracts
    Rush,              // + savings newcomers bring
    NewTraderCapital,  // + eastern capital outfitting a new trader
    InterestBudget,    // + standing security budgets of railroads and associations
    GangStake,         // + the purse a new gang starts with
    Freight,           // - teamsters, boats and railroads paid
    EastSpending,      // - spending that lands in an eastern market (visits, wages, upkeep)
    Emigration,        // - savings leaving with departing residents
    TraderExit,        // - a trader's remaining cash when he quits or dies
    GangExit,          // - a dissolved gang's remaining cash
    LeaderExit,        // - a dead leader's cash
    DustPaid,          // + gold dust a camp pays for supplies becomes money in the trader's hands (coined at the assay office)
    SupplierLoss,      // + an eastern supplier (or a town too poor to cover it) absorbs a consignment the road took
    Count
};
inline constexpr int kMoneyFlowCount = static_cast<int>(MoneyFlow::Count);
const char* money_flow_name(MoneyFlow f);

// Runtime state of one place on the strategic graph: a town, fort, mine, range
// or eastern market. Everything the economy needs is aggregate: no individual
// residents, just totals.
struct Node {
    std::string id, name, kind;
    double x = 0, y = 0;
    bool fixed_market = false, transient_inflow = false;
    double gov_income = 0, capacity = 0, base_population = 0;
    double garrison = 0;                    // soldiers currently posted
    std::vector<ContractDef> contracts;
    std::vector<double> contract_price, contract_backlog;  // per good: fixed price (0 = none) and unfilled quantity
    // The post's government store, per good: what contractors and the Quartermaster have delivered. The garrison and the
    // agency are fed from it, not from the town's shelf, and traders cannot buy it (it is not for sale).
    std::vector<double> depot;
    // For the cause chain of a shortage (see the famine event): units traders have bought off the shelf lately (fades over
    // about a month), and the day goods last reached the shelf from outside.
    std::vector<double> bought_out;
    std::vector<long> last_delivery;
    // Open orders the town's merchants have posted (merchants.cpp), per good: units wanted and the delivered price offered.
    // Unlike a government contract these are paid out of the town's own cash, and the offer rises while nobody fills it.
    std::vector<double> order_qty, order_price, order_first_price;
    std::vector<long> order_posted, order_raised, order_hired;
    std::vector<std::string> tags;

    // Farming and settlement (farms.cpp).
    double farmers = 0, farm_capacity = 0, farm_yield = 1;
    long farm_open = LONG_MIN;
    int zone = -1;                          // weather zone (index into World::zone_names)
    double farm_core = 0;                   // farmers who stay through bad prices (subsistence)
    GoodId cash_good = -1;                  // cash crop other than grain (cotton)
    double cash_rate = 0;
    double cash_store = 0;                  // cash crop in the barns
    GoodId gate_good = -1;                  // which crop currently pays best
    double farm_store = 0;                  // grain in the barns, waiting to be sold
    double farm_gate = 0;                   // what a farmer here can net per cwt, remembered (smoothed)
    double yield_memory = 1;                // recent yields: bad years discourage new settlers
    bool latent = false;                    // only a site on the map; not yet a town
    bool founded = true;
    double found_pop = 40;
    GoodId found_good = -1;                 // the good whose net price decides founding this site (-1: grain)
    double found_quality = 1;               // scales that price: good land, plentiful game or timber
    double strike_chance = 0, strike_people = 0;   // a prospecting site: monthly chance of a strike, and the rush it brings
    int found_streak = 0;                   // months the gate price has justified settling
    long founded_day = LONG_MIN;
    NodeId gate_outlet = -1;                // where the best price for this district's grain is

    double population = 0, cash = 0;
    std::vector<double> stock, fixed_price, reserve, extra_demand;
    std::vector<double> pressure;           // eastern markets: net units sold into the market, decaying
    std::vector<ProductionDef> production;
    std::vector<char> production_started;   // parallel to `production`, for one-time log lines
    std::vector<EdgeId> edges;

    // Dynamics
    double inflow_today = 0;     // cash received today from trade, visitors and government
    double income_ema = 0;       // smoothed daily inflow: this is what a town lives on
    double income_peak = 0;
    // Lifetime earnings by source, for asking what a town lives on: goods sold to traders, money traders and
    // their crews spend there, government pay, and everything else (gangs, posses, detectives).
    double earned_sales = 0, earned_visitors = 0, earned_government = 0, earned_total = 0;
    double hunger = 0;           // smoothed share of food need left unmet (the civilians', where a garrison is fed from the depot)
    double garrison_hunger = 0;  // the same for the soldiers: how far the depot and the shelf together fell short
    double law_fund = 0;         // taxes set aside for sheriffs and posses (a town with no law sets none aside)
    int region = -1;             // index into World::regions; -1 outside any jurisdiction
    int law = 2;                 // 0 none (a miners' meeting), 1 town marshal, 2 county sheriff and deputies
    int law_ready = 0, law_lapse = 0;   // days the town has met the next stage's size, and days below its present stage's
    int paper_streak = 0;        // days toward opening (or closing) a paper
    int paper = 0;               // a newspaper prints here (news.cpp)
    double fear = 0;             // townspeople's unease after strangers are robbed or killed nearby: small, and it fades
    double outrage = 0;          // local anger over robberies; triggers posses
    double drifters = 0;         // idle men: most find work or move on
    double ex_cons = 0;          // of the fighters, the men who have done time (they carry a record and fight harder)
    std::vector<std::pair<long, int>> jailed;   // (release day, men) serving time here
    double fighters = 0;         // the few who turn to crime and can fight: the raw material of bands and gangs
    double peak_pop = 0;
    // Residents who buy from the market. Farmers and ranchers around a town (base_population) grow most of
    // their own food, so only a fifth of them draw on the town's shelves.
    double market_population() const { return std::max(0.0, population - 0.8 * (base_population + farmers)); }
    // Rule by a gang (see capture.cpp). The toll is added to every price traders face here.
    int captor = -1;
    double toll = 0;
    long captured_day = 0;
    double captured_income = 1;
    long last_posse_day = -100000;
    long posse_pause_until = -100000;   // a town whose posse was routed does not send another for a while
    long last_shortage_log = -100000, last_pop_log = -100000;  // keep the log readable when a town hovers at a threshold
    int last_pop_dir = 0;
    bool ghost = false;
    double game = 0, game_capacity = 0;   // wild game in hunting range (head of beef-equivalent), and what the country holds untouched
    double hunted_total = 0;
    bool game_thin = false;               // the herd has been hunted down (logged once per spell)
    bool starving = false;       // in a food shortage: the hunger average is high (nobody need have died yet)
    // Starvation (simulation.cpp). Deaths follow sustained deprivation, not a bad week: a slow average of unmet food need.
    // A famine is a spell of starvation that kills a real share of the town (famine_death_share).
    double deprivation = 0;
    double starved_total = 0;
    double spell_deaths = 0, spell_pop = 0;
    int spell_quiet = 0;         // days since anyone starved in this spell
    bool spell_active = false, in_famine = false;
    bool garrison_short = false; // the soldiers are going short (logged once per spell)
    int pop_band = 0;
};

struct Edge {
    NodeId a = -1, b = -1;
    Mode mode = Mode::Trail;
    double miles = 0, hazard = 0, travel_days = 1;
    long from = 0, until = 0;
    std::string note;
    std::vector<std::pair<double, double>> path;   // the route over the terrain, a to b (empty = a straight line); drawing only
    int river_crossings = 0, river_biggest = 0;    // rivers the route crosses, and the size of the biggest (information; the miles already include the crossings)
    long blocked_until = LONG_MIN;      // a storm, flood or washout shuts the road until this day (a "road_closure" shock)
    std::string block_cause;
    bool active = false;

    // Public knowledge of how dangerous the route is (rumor): robberies per trader-day.
    double robbed_ema = 0, transits_ema = 0;
    double robbed_today = 0, transits_today = 0;
    // What a gang can see: how much cargo value moves along here.
    double traffic_today = 0, traffic_ema = 0;
    // The part of it a gang would stop a wagon for (gold, livestock, goods dense enough to carry off).
    double loot_today = 0, loot_ema = 0;

    NodeId other(NodeId n) const { return n == a ? b : a; }
};

// The strategic-layer world: nodes, edges, prices, and routing. No agents here.
class World {
public:
    World(const WorldDef& def, const Params& params, long start_day);

    std::vector<GoodDef> goods;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    Params params;
    long today = 0;
    double ledger[kMoneyFlowCount] = {};   // signed running total per MoneyFlow
    std::vector<double> east_bought_value, east_sold_value;  // dollars per good the East bought / sold (diagnostics)
    std::vector<RiverLine> rivers;           // for drawing; the crossings are already in the routes' miles
    std::vector<CrossingSite> crossings;     // fords, ferries and bridges (for drawing)
    std::vector<std::string> region_names;   // jurisdictions (territories, states, reserves)
    std::vector<long> region_organized;      // the day a region's county government begins
    bool organized(const Node& n) const { return n.region < 0 || today >= region_organized[static_cast<std::size_t>(n.region)]; }
    std::vector<std::string> zone_names;     // weather zones
    std::vector<double> zone_yield;          // this year's yield multiplier per zone (1 = normal)
    double order_filled_value = 0, order_filled_units = 0;  // diagnostics: what traders have been paid for open orders
    GoodId flour_good = -1, forage_good = -1, money_good = -1;   // money_good: gold, which passes as currency in the camps
    // Flour-equivalent a node's farmers bring to market in a normal year, per day.
    double farm_output(NodeId n) const;
    double farm_cash_output(NodeId n) const;   // cash crop per day (cotton)
    void record(MoneyFlow f, double amount) { ledger[static_cast<int>(f)] += amount; }

    // ---- Prices ----
    // Normal stock a node expects to hold: a month of local consumption, a month
    // of local output, or a floor. Stock above it makes goods cheap, below it dear.
    double normal_stock(NodeId n, GoodId g) const;
    double daily_demand(NodeId n, GoodId g) const;
    double daily_output(NodeId n, GoodId g) const;
    // Units a federal contract wants delivered per day, given the current garrison.
    double contract_rate(NodeId n, const ContractDef& c) const;
    // Re-prices cost-plus contracts: cheapest eastern supply plus freight to the post, plus the markup.
    void refresh_contract_prices();
    double mid_price_at(NodeId n, GoodId g, double stock) const;
    // An eastern market's price once `pressure` units have been sold into it: finite depth, so no
    // arbitrage is unlimited. Selling a lot depresses the price; buying a lot raises it. Pressure decays.
    double fixed_price_at(NodeId n, GoodId g, double pressure) const;
    // Price a trader pays to buy / receives to sell one unit when the node holds `stock`.
    double buy_price_at(NodeId n, GoodId g, double stock) const;
    double sell_price_at(NodeId n, GoodId g, double stock) const;
    double buy_price(NodeId n, GoodId g) const { return buy_price_at(n, g, nodes[idx(n)].stock[idx(g)]); }
    double sell_price(NodeId n, GoodId g, double inbound = 0) const { return sell_price_at(n, g, nodes[idx(n)].stock[idx(g)] + inbound); }
    // The node's own mid price, used for display and for the "price" other code reasons about.
    double mid_price(NodeId n, GoodId g) const;

    // Planning quotes: total cost / revenue for q units, averaging price slippage.
    double quote_buy(NodeId n, GoodId g, double q) const;
    // `inbound` is freight already on the road to the node: it fills the contract and the open order first, then crowds the shelf.
    // `drain` is what the town will eat before this load arrives, which thins the shelf the load is sold into.
    double quote_sell(NodeId n, GoodId g, double q, double inbound = 0, double drain = 0) const;  // limited by the node's cash and dust
    double dust_on_hand(NodeId n) const;                    // value of the gold a camp could pay out in place of cash
    double quote_sell_contract(NodeId n, GoodId g, double q, double inbound = 0) const;  // the part of a sale that a contract or open order pays (certain)
    double order_unit_price(NodeId n, GoodId g, double extra_stock) const;  // per unit for an open order: its offer or the shelf price, whichever is higher

    // Execution. Both move stock and cash and count the node's inflow.
    double buy(NodeId n, GoodId g, double q, double max_spend, double& spent);
    double sell(NodeId n, GoodId g, double q, double& revenue);

    // ---- Freight ----
    double freight_unit_cost(GoodId g, const double miles_by_mode[kModeCount]) const;

    // ---- Network and routing ----
    // Activates or deactivates edges by date; returns true if the network changed.
    bool update_active_edges(std::vector<EdgeId>* opened, std::vector<EdgeId>* closed);
    void rebuild_routes();
    long route_version = 0;      // bumped whenever the routes are rebuilt, so caches built from them know to refresh
    bool reachable(NodeId s, NodeId d) const { return dist_[idx(s) * nodes.size() + idx(d)] < 1e17; }
    double route_days(NodeId s, NodeId d) const { return dist_[idx(s) * nodes.size() + idx(d)]; }
    // Fills `out` with the edge ids from s to d. Returns false if unreachable.
    bool route(NodeId s, NodeId d, std::vector<EdgeId>& out) const;

    static std::size_t idx(int i) { return static_cast<std::size_t>(i); }

private:
    std::vector<double> dist_;      // travel days, nodes x nodes
    std::vector<int> prev_edge_;    // edge used to enter d on the best route from s
};

}  // namespace frontier
