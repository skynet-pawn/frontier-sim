#pragma once
#include <string>
#include <utility>
#include <vector>

#include "frontier/config.h"
#include "frontier/event_log.h"
#include "frontier/rng.h"
#include "frontier/world.h"

namespace frontier {

// A mobile entity on the graph: one trading outfit (a wagon train, a drover's
// crew, a freight agent). Decisions are utility scores over (destination,
// good, guards) and are inspectable: see Simulation::plan_*.
struct Trader {
    int id = 0;
    bool alive = true;
    NodeId at = -1;              // current node, or the node the current leg started from
    double cash = 0;
    GoodId cargo = -1;
    double qty = 0, unit_cost = 0;
    int guards = 0;
    bool drover = false;         // drovers move cattle only; freighters move everything else
    bool controlled = false;     // driven by an outside controller (a player or a script), not the AI
    NodeId agent_for = -1;       // >= 0: a convoy hired by this town's merchants; it delivers there and disbands
    int contract = -1;           // >= 0: a load for this standing contract, funded by its house (see SupplyContract)
    double contract_units = 0;   // what the load was meant to deliver, to be made good if it is robbed
    bool firm = false;           // an eastern firm's freight line or cattle syndicate, not a freelancer
    double capacity_lb = 12000;  // what the outfit can haul in one trip
    double upkeep = 1.5;         // $/day running costs, before guards
    double start_capital = 0;

    // Personality and memory: experience reshapes these (principle 4).
    double base_risk_aversion = 1;
    double risk_aversion = 1;
    std::vector<std::pair<EdgeId, double>> fear;  // sparse: daily extra risk this trader assigns to an edge
    int house = -1;              // >= 0: a driver and wagons working for this freight house (see House)
    int robberies = 0;
    double pending_spend = 0;    // upkeep accrued on the road, spent in the next town
    double recent_profit = 0;    // what he has cleared lately, fading with time: a run of good legs makes a story

    // Travel state.
    std::vector<EdgeId> route;
    std::size_t leg = 0;
    double days_left = 0;
    NodeId dest = -1;
    bool travelling = false;
};

// A freight house: a trader who got rich enough to stop driving and run other men's wagons (the Russell, Majors
// & Waddell pattern, or an eastern firm's western agency). One owner's capital is spread over many outfits that
// he sends out on the strength of what he knows, so one holdup costs him a load and a driver, not his business.
// His outfits draw a working float from the house; a broken one is made good from house cash, not abandoned.
// Because a single load is a small part of his wealth he judges risk coolly where a lone freelancer, who is dead
// or ruined if it goes wrong, cannot. Index in Simulation::houses_ is the house id; closed houses stay in the list.
struct House {
    int id = 0;
    bool alive = true;
    bool eastern = false;        // an eastern firm's agency (large wagons, cautious temperament) rather than a freelancer's
    int founder = 0;             // id of the trader who founded it
    NodeId seat = -1;            // where it hires
    double cash = 0;             // owner's capital not out working in an outfit
    double start_capital = 0;
    double wealth = 0;           // cash plus everything in the outfits, refreshed each morning
    int outfits = 0;
    long founded = 0, last_hire = -100000;
    double base_risk_aversion = 1, risk_aversion = 1;   // the owner's temperament, and what experience has made of it
    std::vector<std::pair<EdgeId, double>> fear;        // the owner's memory of dangerous roads, shared by every outfit
};

// A standing arrangement between a town and a freight house: a fixed quantity a day at a price above landed cost,
// delivered on a schedule from the house's own cash. The town pays per delivery, as far as its cash goes.
struct SupplyContract {
    int id = 0;
    bool alive = true;
    NodeId town = -1;
    int house = -1;
    GoodId good = -1;
    double rate = 0;             // units a day promised (follows the town's need)
    double price = 0;            // paid per unit delivered
    double capital = 0;          // what the house ties up in it
    long signed_day = 0, ends = 0;
    double owed = 0;             // units fallen due and not yet delivered or on the road
    double arrears = 0;          // money the town could not pay at the last delivery
    double delivered = 0;
    int missed = 0;              // loads robbed on the way
};

// A band of outlaws, treated as one object with a headcount.
struct Gang {
    int id = 0;
    bool active = true;
    int men = 0;
    double cash = 0;
    EdgeId edge = -1;            // where it is currently waiting for traffic
    NodeId hideout = -1;         // town that fences its loot and sells it supplies
    long formed = 0;
    int robberies = 0;
    double loot = 0;
    long last_robbery = -100000;
    long last_review = 0;
    std::vector<std::pair<NodeId, double>> grudge;  // towns that fought back: avoid them

    // Town capture (capture.cpp).
    double influence = 0;        // grows with money spent in the hideout town relative to its economy
    double last_spend = 0;
    NodeId ruling = -1;          // the town this gang runs, if any
    double competence = 0;       // how well the boss runs a town: 0 hopeless .. 1 shrewd
    double tribute = 0;          // share of the town's inflow taken
    bool controlled = false;     // moves only when told to (act_gang_relocate)
};

// A leader for hire: someone who can raise, train and organize a posse. From the simulation's point of
// view he is a skill multiplier with a fee; the same record is what a player-controlled vigilante would use.
struct Leader {
    int id = 0;
    std::string name;
    NodeId at = -1;
    double skill = 1.5;          // multiplies the strength of any posse he leads
    double cash = 0;
    bool alive = true;
    bool busy = false;
    double earned = 0;           // lifetime fees and bonuses (cash can leave with his death)
    int jobs = 0, wins = 0;
};

enum class PosseKind { Town, Vigilance, Agency };

// A private interest with money and reach (see InterestDef).
struct Interest {
    std::string id, name, kind;
    NodeId hq = -1;
    std::vector<NodeId> nodes;
    double budget_per_day = 0, dues_rate = 0;
    long from = 0, until = 0;
    double fund = 0;
    double loss_ema = 0;         // recent losses to robbery
    double losses_total = 0;
    std::vector<std::pair<int, double>> victims;  // gang id -> recent loss it caused
    long last_op_day = -100000;
    int ops = 0, wins = 0;
};

struct Posse {
    int id = 0;
    bool active = true;
    NodeId origin = -1;
    int men = 0;
    int gang_id = -1;
    int leader_id = -1;
    int interest_id = -1;
    PosseKind kind = PosseKind::Town;
    double days_left = 0;
};

// What happened when something tried an action. The AI and any outside controller get the same answers.
enum class ActionResult { Ok, Busy, WrongPlace, HasCargo, NoCargo, WrongKind, NothingForSale, NoMoney, NoRoute };
const char* action_result_name(ActionResult r);
struct Receipt {
    ActionResult result = ActionResult::Ok;
    double units = 0;
    double money = 0;
};

// Result of reconciling every cash balance against the recorded flows in and out. A nonzero residual means
// money is being created or destroyed somewhere the ledger does not know about.
struct MoneyAudit {
    double baseline = 0;         // total money when the scenario began
    double net_flows = 0;        // recorded inflows minus outflows since
    double expected = 0;         // baseline + net_flows
    double actual = 0;           // sum of every cash balance now
    double residual = 0;         // actual - expected
    double flows[kMoneyFlowCount] = {};
};

class Simulation {
public:
    Simulation(const WorldDef& def, const Scenario& scenario);

    void step();                 // advance one day
    void run(int days) { for (int i = 0; i < days; ++i) step(); }

    World& world() { return world_; }
    const World& world() const { return world_; }
    EventLog& log() { return log_; }
    const EventLog& log() const { return log_; }
    const std::vector<Trader>& traders() const { return traders_; }
    const std::vector<Gang>& gangs() const { return gangs_; }
    const std::vector<Posse>& posses() const { return posses_; }
    const std::vector<Leader>& leaders() const { return leaders_; }
    const std::vector<Interest>& interests() const { return interests_; }
    double avg_trader_profit() const { return avg_profit_ema_; }
    double total_money() const;
    MoneyAudit money_audit() const;

    // ---- Actions. The AI uses exactly these; so can a player, a script or a test. ----
    // A trader must be idle (not travelling) at a node to trade.
    Receipt act_buy(Trader& t, GoodId g, double qty, double max_spend = -1);  // max_spend < 0: all his cash
    Receipt act_sell(Trader& t, double qty = -1);                             // qty < 0: everything
    ActionResult act_travel(Trader& t, NodeId dest, int guards);              // pays freight, hires guards
    ActionResult act_gang_relocate(Gang& g, EdgeId e);
    int add_trader(NodeId at, double cash, bool drover, bool controlled);     // returns the trader's id
    Trader* find_trader(int id);                                              // ids stay valid; pointers may not
    Gang* gang_by_id(int id) { return find_gang(id); }
    // Dollars of goods bought by each kind of trader, by good (diagnostic): index 0 freelancers, 1 firms.
    const std::vector<double>& bought_by_good(bool firm) const { return bought_value_[firm ? 1 : 0]; }
    // Dollars of flour traders bought at each node (diagnostic): who the farm hubs and the East really supply.
    const std::vector<double>& flour_bought_at() const { return flour_bought_at_; }
    // Merchant imports (diagnostics): loads ordered, dollars of goods delivered or robbed, and goods bought per town.
    int merchant_orders() const { return orders_placed_; }
    int orders_expired() const { return orders_expired_; }
    int convoys_hired() const { return convoys_hired_; }
    double merchant_delivered() const { return delivered_value_; }
    double merchant_lost() const { return lost_orders_value_; }
    const std::vector<double>& imported_value() const { return imported_value_; }
    const std::vector<House>& houses() const { return houses_; }
    int houses_founded() const { return houses_founded_; }
    int house_recaps() const { return house_recaps_; }
    // Standing contracts (diagnostics): signed, dropped, units delivered, and what the houses took in and laid out.
    int contracts_signed() const { return contracts_signed_; }
    int contracts_cancelled() const { return contracts_cancelled_; }
    double contract_units() const { return contract_units_; }
    double contract_revenue() const { return contract_revenue_; }
    double contract_outlay() const { return contract_outlay_; }
    int house_exits(bool ruined) const { return house_exits_[ruined ? 1 : 0]; }
    int entered_freelancers() const { return entered_freelancers_; }
    int entered_firms() const { return entered_firms_; }
    bool trace = false;          // log individual trades

    struct PathInfo {
        bool ok = false;
        double days = 0;
        double miles[kModeCount] = {0, 0, 0};
        double gang_risk = 0;      // perceived chance of robbery per cargo-day, summed over the route
        double hazard_risk = 0;    // expected cargo fraction lost to mishaps
    };
    struct Plan {
        double utility = 0;
        NodeId dest = -1;
        GoodId good = -1;
        double qty = 0;
        int guards = 0;
        bool valid = false;
    };

private:
    // ---- economy (simulation.cpp) ----
    void update_network();
    void apply_shocks();
    void apply_campaigns();
    void economy_day();
    void update_population(NodeId n);
    void end_of_day();

    // ---- farms and new towns (farms.cpp) ----
    void farms_day();
    void draw_weather();
    double crop_gate_ratio(NodeId id, GoodId g, NodeId& outlet);
    double farm_gate_ratio(NodeId id, NodeId& outlet, GoodId& crop);
    void found_town(NodeId id);
    void ship_farm_goods(NodeId id, Node& n);

    // ---- traders (traders.cpp) ----
    void spawn_trader(NodeId at, bool firm);
    void trader_entry();
    void settle_trader(Trader& t);
    double spot_uncertainty(NodeId d, GoodId g, double q) const;
    double certainty_adjusted(const Trader& t, NodeId d, GoodId g, double q, double revenue) const;
    double daily_cost(const Trader& t, int guards) const;
    void traders_day();
    // Freight already on the road to each town, per good (nodes x goods), as traders know it: wagons passing each other
    // and word from the towns. Refreshed each morning and bumped as each outfit sets out, so a dozen wagons deciding
    // on the same morning do not all chase the same price.
    void refresh_inbound();
    double known_inbound(const Trader& t, NodeId d, GoodId g) const;
    // What the town will consume while a load is on the road to it (as far as traders foresee it).
    double expected_drain(NodeId d, GoodId g, double days) const;
    void step_trader(Trader& t);
    void decide(Trader& t);
    void plan_paths(const Trader& t, NodeId from, std::vector<PathInfo>& out);
    double edge_risk(const Trader& t, EdgeId e) const;
    double defense(int guards) const;
    double daily_cost(int guards) const;
    Plan plan_sell(const Trader& t, const std::vector<PathInfo>& paths, bool allow_here);
    Plan plan_buy(const Trader& t, const std::vector<PathInfo>& paths);
    bool plan_deadhead(const Trader& t, NodeId& dest);
    void arrive(Trader& t);
    Receipt sell_cargo(Trader& t, double qty);   // the sale itself, with no check that the outfit is idle
    bool sell_along_the_way(Trader& t);          // true if the whole load went here and the trip is over
    double tradable_stock(NodeId n, GoodId g) const;   // what traders may buy from a town's shelves
    void ensure_opportunities();
    void retire(Trader& t, const char* why, bool ruined = false);

    // ---- merchant imports (merchants.cpp) ----
    void merchants_day();
    void order_for_town(NodeId id);
    void orders_day();
    bool cheapest_source(NodeId town, GoodId g, double load_units, NodeId& src, double& best);
    void hire_for_order(NodeId id, GoodId g);
    double dispatch_convoy(NodeId town, NodeId src, GoodId g, double units, double advance, double landed);
    void finish_convoy(Trader& c);

    // ---- standing supply contracts (contracts.cpp) ----
    void contracts_day();
    void seek_contracts(NodeId id);
    void run_contract(SupplyContract& k);
    bool dispatch_contract(SupplyContract& k, double units);
    void finish_contract_convoy(Trader& c);

    // ---- freight houses (houses.cpp) ----
    void houses_day();
    void found_house(Trader& founder);
    void hire_outfit(House& h);
    bool recapitalize(Trader& t);
    double effective_risk(const Trader& t) const;
    std::vector<std::pair<EdgeId, double>>& fear_book(Trader& t);
    void bump_fear(Trader& t, EdgeId eid, double amount, double risk_bump, double risk_cap);

    // ---- crime and law (crime.cpp) ----
    void crime_day();
    void rebuild_edge_gangs();
    bool resolve_encounter(Gang& g, Trader& t, EdgeId e);
    struct GangTake { double units = 0, value = 0, loot = 0; bool worth = false; };  // value: owner's loss; loot: what the goods are worth
    GangTake gang_take(double men, bool desperate, const Trader& t) const;
    bool gang_desperate(const Gang& g) const;
    void gang_day(Gang& g);
    void gang_review(Gang& g);
    void gang_dissolve(Gang& g, const char* why);
    void try_form_gang(NodeId n);
    void create_gang(NodeId hideout, int men, const char* cause);
    double gang_edge_score(const Gang& g, EdgeId e) const;
    double grudge_of(const Gang& g, NodeId n) const;
    void try_form_posse(NodeId n);
    Leader* pick_leader(const Node& at, double travel_days, double budget, double reserve, double& lead_days, double& fee);
    void interests_day();
    bool interest_active(const Interest& in) const;
    bool has_backup(NodeId n) const;
    void note_interest_loss(EdgeId e, const Gang& g, GoodId cargo, double value);

    // ---- gangs ruling towns (capture.cpp) ----
    void capture_progress(Gang& g, double spent);
    void rule_town(Gang& g);
    void free_town(Gang& g, const char* why);
    void try_vigilance(NodeId n);
    void posse_day(Posse& p);
    NodeId nearest_node(EdgeId e, NodeId prefer_not_fixed) const;

    std::string nname(NodeId n) const { return world_.nodes[World::idx(n)].name; }
    std::string ename(EdgeId e) const;
    const GoodDef& good(GoodId g) const { return world_.goods[World::idx(g)]; }
    Gang* find_gang(int id);
    Leader* find_leader(int id);
    void pay_leader(Leader& l, double amount);
    void release_leader(Leader& l, NodeId at);

    World world_;
    Rng rng_;
    EventLog log_;
    Scenario sc_;

    std::vector<Trader> traders_;
    std::vector<Trader> convoys_;               // freight hired by towns' merchants (not counted among the traders)
    std::vector<House> houses_;
    std::vector<SupplyContract> contracts_;
    int contracts_signed_ = 0, contracts_cancelled_ = 0;
    double contract_units_ = 0, contract_revenue_ = 0, contract_outlay_ = 0;
    int houses_founded_ = 0, house_recaps_ = 0;
    int house_exits_[2] = {0, 0};               // outfits of houses lost: [0] killed, [1] broke with the house unable to cover
    int orders_placed_ = 0, orders_expired_ = 0, convoys_hired_ = 0;
    double delivered_value_ = 0, lost_orders_value_ = 0;
    std::vector<double> imported_value_;
    std::vector<Gang> gangs_;
    std::vector<Posse> posses_;
    std::vector<Leader> leaders_;
    std::vector<Interest> interests_;
    std::vector<std::vector<int>> edge_gangs_;  // gang ids per edge, rebuilt daily
    int next_trader_id_ = 1, next_gang_id_ = 1, next_posse_id_ = 1, next_leader_id_ = 1;
    double avg_profit_ema_ = 0;
    double baseline_money_ = 0;
    // What entrants can see (never the true profit): rumor of success stories and the reported return on capital.
    double story_ema_ = 0, return_ema_ = 0, danger_ema_ = 0;
    int stories_today_ = 0, robbed_today_ = 0;
    int entered_freelancers_ = 0, entered_firms_ = 0, entered_window_f_ = 0, entered_window_firm_ = 0;
    std::vector<NodeId> gateways_, markets_;
    std::vector<double> bought_value_[2];
    std::vector<double> flour_bought_at_;
    int weather_year_ = -1;
    double profit_today_ = 0;                   // realized trader profit, net of upkeep, for the join rate

    std::vector<double> opportunity_[2];        // [freighter, drover]: best standard-load profit at each node
    long opportunity_day_ = -1;
    std::vector<std::size_t> shock_done_;       // indices of shocks already applied
    std::vector<EdgeId> scratch_route_;
    std::vector<PathInfo> scratch_paths_;
    std::vector<double> inbound_;
};

}  // namespace frontier
