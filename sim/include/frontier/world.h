#pragma once
#include <algorithm>
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
    std::vector<std::string> tags;

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
    double hunger = 0;           // smoothed share of food need left unmet
    double law_fund = 0;         // taxes set aside for sheriffs and posses
    double outrage = 0;          // local anger over robberies; triggers posses
    double drifters = 0;         // idle armed men: the raw material of gangs
    double peak_pop = 0;
    // Residents who buy from the market. Farmers and ranchers around a town (base_population) grow most of
    // their own food, so only a fifth of them draw on the town's shelves.
    double market_population() const { return std::max(0.0, population - 0.8 * base_population); }
    // Rule by a gang (see capture.cpp). The toll is added to every price traders face here.
    int captor = -1;
    double toll = 0;
    long captured_day = 0;
    double captured_income = 1;
    long last_posse_day = -100000;
    long last_famine_log = -100000, last_pop_log = -100000;  // keep the log readable when a town hovers at a threshold
    int last_pop_dir = 0;
    bool ghost = false;
    bool starving = false;
    int pop_band = 0;
};

struct Edge {
    NodeId a = -1, b = -1;
    Mode mode = Mode::Trail;
    double miles = 0, hazard = 0, travel_days = 1;
    long from = 0, until = 0;
    std::string note;
    bool active = false;

    // Public knowledge of how dangerous the route is (rumor): robberies per trader-day.
    double robbed_ema = 0, transits_ema = 0;
    double robbed_today = 0, transits_today = 0;
    // What a gang can see: how much cargo value moves along here.
    double traffic_today = 0, traffic_ema = 0;

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
    void record(MoneyFlow f, double amount) { ledger[static_cast<int>(f)] += amount; }

    // ---- Prices ----
    // Normal stock a node expects to hold: a month of local consumption, a month
    // of local output, or a floor. Stock above it makes goods cheap, below it dear.
    double normal_stock(NodeId n, GoodId g) const;
    double daily_demand(NodeId n, GoodId g) const;
    double daily_output(NodeId n, GoodId g) const;
    double mid_price_at(NodeId n, GoodId g, double stock) const;
    // An eastern market's price once `pressure` units have been sold into it: finite depth, so no
    // arbitrage is unlimited. Selling a lot depresses the price; buying a lot raises it. Pressure decays.
    double fixed_price_at(NodeId n, GoodId g, double pressure) const;
    // Price a trader pays to buy / receives to sell one unit when the node holds `stock`.
    double buy_price_at(NodeId n, GoodId g, double stock) const;
    double sell_price_at(NodeId n, GoodId g, double stock) const;
    double buy_price(NodeId n, GoodId g) const { return buy_price_at(n, g, nodes[idx(n)].stock[idx(g)]); }
    double sell_price(NodeId n, GoodId g) const { return sell_price_at(n, g, nodes[idx(n)].stock[idx(g)]); }
    // The node's own mid price, used for display and for the "price" other code reasons about.
    double mid_price(NodeId n, GoodId g) const;

    // Planning quotes: total cost / revenue for q units, averaging price slippage.
    double quote_buy(NodeId n, GoodId g, double q) const;
    double quote_sell(NodeId n, GoodId g, double q) const;  // limited by the node's cash

    // Execution. Both move stock and cash and count the node's inflow.
    double buy(NodeId n, GoodId g, double q, double max_spend, double& spent);
    double sell(NodeId n, GoodId g, double q, double& revenue);

    // ---- Freight ----
    double freight_unit_cost(GoodId g, const double miles_by_mode[kModeCount]) const;

    // ---- Network and routing ----
    // Activates or deactivates edges by date; returns true if the network changed.
    bool update_active_edges(std::vector<EdgeId>* opened, std::vector<EdgeId>* closed);
    void rebuild_routes();
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
