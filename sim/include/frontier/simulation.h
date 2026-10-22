#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "frontier/config.h"
#include "frontier/event_log.h"
#include "frontier/rng.h"
#include "frontier/world.h"

namespace frontier {

// Adds the time from construction to destruction to one phase's total (profiling only).
struct PhaseTimer {
    double& total;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit PhaseTimer(double& t) : total(t) {}
    ~PhaseTimer() { total += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

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
    long ordered_day = 0;        // the day the town posted the order this load fills (for the lead time it measures)
    bool express = false;        // an eastern express agent's shipment of gold dust, bound for an eastern market (see express.cpp)
    int contract = -1;           // >= 0: a load for this standing contract, funded by its house (see SupplyContract)
    double contract_units = 0;   // what the load was meant to deliver, to be made good if it is robbed
    int job_town = -1;           // >= 0: hauling the supplier's goods to this town's open order (see jobs.cpp)
    NodeId job_src = -1;         // where the goods were loaded
    double job_debt = 0, job_unit = 0, job_price = 0;  // goods owed to the supplier, their cost a unit, and what the town pays a unit
    int job_guards = 0;
    bool leaving = false;        // its backers have pulled out: it finishes the load it carries and then quits the road
    bool firm = false;           // an eastern firm's freight line or cattle syndicate, not a freelancer
    double capacity_lb = 12000;  // what the outfit can haul in one trip
    double upkeep = 1.5;         // $/day running costs, before guards
    double start_capital = 0;
    double worth = 0;            // cash plus the cargo at cost, as of yesterday: its daily change is his income
    double income = 0;           // what he has been clearing a day, smoothed over `income_half_life`

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

// What traders earned and spent, summed over the days stepped in this process (not saved: after a load it counts from the load).
// Read as: gross margin = revenue - basis_sold (basis = goods and freight paid for what was sold); what was bought but never sold
// was robbed, lost or still on the wagon. Upkeep and guard wages are paid whether or not he trades; visits are what the party spends at each town it stops in.
struct TradeAcc {
    double revenue = 0, basis_sold = 0, goods_bought = 0, freight = 0, upkeep = 0, guards = 0, visits = 0, job_pay = 0;
    long sales = 0, loads = 0, jobs = 0;   // sales made, loads bought, freight jobs delivered
    TradeAcc& operator+=(const TradeAcc& o) {
        revenue += o.revenue; basis_sold += o.basis_sold; goods_bought += o.goods_bought; freight += o.freight;
        upkeep += o.upkeep; guards += o.guards; visits += o.visits; job_pay += o.job_pay;
        sales += o.sales; loads += o.loads; jobs += o.jobs;
        return *this;
    }
};
struct TradeBook {
    enum Class { Lone, House, Firm, Convoy, ClassCount };   // lone freelancers, freight-house outfits, eastern firms, town-hired convoys
    TradeAcc cls[ClassCount];
    std::vector<TradeAcc> lone_by_good;   // lone freelancers only, by good
    std::vector<TradeAcc> lone_by_year;   // lone freelancers only, by year of the run
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
    bool retired = false;        // the owner has cashed out; the house runs on under managers
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
// Where idle armed men come from and go to, so the gang cycle can be audited: the pool of drifters is fed by arrivals and by
// failure, and drained by decay (they find work, die or leave) and by gangs forming. Gang numbers add the rest.
enum class DrifterFlow {
    Arrived, BustLeavers, FarmLeavers, FailedTraders, Reprisal, GangScatter, BreakdownScatter, ReturnedUnformed,   // into the pool
    Decay, FormedGang,                                                                                              // out of it
    TurnedFighter, FighterDecay,                                                                                    // drifters who turn to crime; fighters who give up
    GangRecruited,                                                                                                  // into gangs from nowhere
    GangLosses, GangVanished,                                                                                       // out of gangs for good
    Jailed, Released, Hanged,                                                                                       // a caught band's men: locked up, let out with a record, hanged
    Count
};
const char* drifter_flow_name(DrifterFlow f);

struct Gang {
    int id = 0;
    bool active = true;
    int men = 0;
    int cells = 1;               // raiding parties: it robs as several parties of a few men each, and fights as one body
    std::vector<int> fights;     // shootouts each man has survived (same length as men; see gang_add_men / gang_lose_men)
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
    double competence = 0;       // how well the boss holds men together and runs a town: 0 hopeless .. 1 shrewd
    double tribute = 0;          // share of the town's inflow taken
    bool controlled = false;     // moves only when told to (act_gang_relocate)

    // Infamy. How well each jurisdiction knows this gang's name and deeds (index = region). Witnesses make it locally; the
    // papers carry it to other regions after a delay (news.cpp). A region that has never heard of a gang does not hunt it.
    std::vector<float> known;
    int peak = 0;                // most men it ever had
    bool fleeing = false;        // the next review may range far afield: the heat here is too much

    // Camp (crime.cpp): where it really lives, off the roads. `hideout` is the town it trades with; the camp lies this many
    // miles up its trail from that town. The kind is a label for the viewer; nothing in the sim reads it.
    double camp_miles = 0;
    int camp_kind = -1;
    double heat = 0;             // hold-ups and killings on its current trail lately; what lawmen can follow to the camp
    bool qualified = false;      // has pulled enough jobs for a gang to take it (counted once, for the funnel)
    int record_men = 0;          // of its men, how many have done time
    int kills = 0;               // men its hold-ups and ambushes have killed
    int thefts = 0;              // petty thefts around its camp: small takes that are not hold-ups and add no loot
    int care = 1;                // how well the boss keeps his men quiet in town: 0 careless, 1 ordinary, 2 careful
};

// One line in a newspaper. `gang` is the outlaw gang it names (its fame rises where the story is read), -1 if none.
struct Headline {
    long day = 0;
    NodeId node = -1;            // where it happened
    std::string kind;            // the log entry it came from
    std::string text;
    int gang = -1;
    double weight = 0.5;         // how far and how loudly it travels
    bool crime = false;          // an outlaw's deed (raises the gang's fame where it is read)
};
struct NewsDelivery {
    long due = 0;
    NodeId paper = -1;
    std::shared_ptr<const Headline> headline;
};

// A man robbed (and maybe killed) on a lonely road near a band's hideout, whose fate is not yet known to anyone. Residents
// and well-connected men are missed and found; strangers pass unremarked. `outrage` is what the report will add.
struct Bushwhack {
    long due = 0;                // the day he is missed or found
    NodeId town = -1;            // where his people live
    EdgeId edge = -1;
    int gang = -1;               // set only if someone saw who did it
    bool killed = false;
    bool notable = false;        // a man with connections elsewhere (an officer, a mine owner, an express agent)
    double outrage = 0;
    std::string who;
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

enum class PosseKind { Town, Vigilance, Agency, Army };

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
    // Wall-clock seconds spent in each part of the day, for finding what costs time at scale (never read by the sim).
    static constexpr int kPhaseCount = 16;
    static const char* phase_name(int i);
    double phase_seconds(int i) const { return phase_time_[i]; }
    void run(int days) { for (int i = 0; i < days; ++i) step(); }
    // Save the whole running state to a file, and load it back into a simulation built from the same data and scenario
    // (the world's static definitions come from the data, the changing state from the file). A run saved at day D and
    // continued matches an unbroken run exactly (tests/save_test.cpp). Load throws on a file from a different world.
    void save_state(const std::string& path) const;
    void load_state(const std::string& path);

    World& world() { return world_; }
    const World& world() const { return world_; }
    EventLog& log() { return log_; }
    const EventLog& log() const { return log_; }
    const std::vector<Trader>& traders() const { return traders_; }
    const std::vector<Gang>& gangs() const { return gangs_; }
    int gang_peak_men() const { return gang_peak_men_; }
    bool is_band_gang(const Gang& g) const { return is_band(g); }
    double drifter_flow(DrifterFlow f) const { return drifter_flow_[static_cast<std::size_t>(f)]; }
    long gangs_ended() const { return gangs_ended_; }
    // The headlines a town's paper has printed lately, oldest first (empty if it has none).
    const std::vector<std::shared_ptr<const Headline>>& paper_headlines(NodeId n) const {
        static const std::vector<std::shared_ptr<const Headline>> none;
        return static_cast<std::size_t>(n) < papers_.size() ? papers_[static_cast<std::size_t>(n)] : none;
    }
    long headlines_printed() const { return headlines_printed_; }
    long band_qualified() const { return band_qualified_; }
    long join_tried() const { return join_tried_; }
    long join_nopartner() const { return join_nopartner_; }
    long bands_jailed() const { return bands_jailed_; }
    long bands_hanged() const { return bands_hanged_; }
    long men_hanged() const { return men_hanged_; }
    long camp_visits() const { return camp_visits_; }
    long camp_trouble() const { return camp_trouble_; }
    long camp_escapes() const { return camp_escapes_; }
    long camp_caught() const { return camp_caught_; }
    static const char* camp_kind_name(int kind);
    long bushwhacks() const { return bushwhacks_; }
    long bush_killed() const { return bush_killed_; }
    long bush_notable() const { return bush_notable_; }
    long large_ended() const { return large_ended_; }
    double large_days_lived() const { return large_days_lived_; }
    int large_end(int k) const { return large_end_[k]; }
    double gang_days_lived() const { return gang_days_lived_; }
    int veterans_of(const Gang& g) const { return gang_veterans(g); }
    double manageable_of(const Gang& g) const { return gang_manageable(g); }
    const std::vector<Posse>& posses() const { return posses_; }
    const std::vector<Leader>& leaders() const { return leaders_; }
    const std::vector<Interest>& interests() const { return interests_; }
    double avg_trader_profit() const { return avg_profit_ema_; }
    const TradeBook& trade_book() const { return book_; }
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
    int express_sent() const { return express_sent_; }
    int express_robbed() const { return express_robbed_; }
    double express_oz() const { return express_oz_; }
    double merchant_delivered() const { return delivered_value_; }
    double merchant_lost() const { return lost_orders_value_; }
    const std::vector<double>& imported_value() const { return imported_value_; }
    const std::vector<House>& houses() const { return houses_; }
    int houses_founded() const { return houses_founded_; }
    int house_recaps() const { return house_recaps_; }
    // Standing contracts (diagnostics): signed, dropped, units delivered, and what the houses took in and laid out.
    long log_from() const { return sc_.log_from; }  // events before this date are warm-up (towns settling) and not counted
    double starved_logged() const { return starved_logged_; }  // people who starved to death since the log began
    double hunted_logged() const { return hunted_logged_; }    // game taken by hunters since the log began (head of beef-equivalent)
    // Freight jobs (diagnostics): loads taken, loads delivered, what carriers earned, and what the road took from suppliers.
    int jobs_taken() const { return jobs_taken_; }
    double scarcity_talk() const { return scarcity_ema_; }
    int jobs_delivered() const { return jobs_delivered_; }
    double job_pay() const { return job_pay_; }
    double job_lost() const { return job_lost_; }
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
        int legs = 0;              // town stops on the way (the party spends money at each)
    };
    struct Plan {
        double utility = 0;
        NodeId dest = -1;
        GoodId good = -1;
        double qty = 0;
        int guards = 0;
        bool valid = false;
        double pay_unit = 0;       // a freight job: what the carrier is paid a unit
    };

private:
    template <class Ar> void io_state(Ar& ar);   // every changing member, once, for save and load (save.cpp)

    // ---- economy (simulation.cpp) ----
    void update_network();
    void apply_shocks();
    void apply_campaigns();
    void economy_day();
    void update_population(NodeId n);
    double idle_men_arriving(const Node& n) const;
    double crime_turn_factor(const Node& n) const;
    void end_of_day();

    // ---- farms and new towns (farms.cpp) ----
    void farms_day();
    void draw_weather();
    double crop_gate_ratio(NodeId id, GoodId g, NodeId& outlet);
    double farm_gate_ratio(NodeId id, NodeId& outlet, GoodId& crop);
    void found_town(NodeId id);
    void rush_arrives(NodeId id, double people, double cash_per_person, const std::string& name, const std::string& cause);
    void ship_farm_goods(NodeId id, Node& n);

    // ---- traders (traders.cpp) ----
    void spawn_trader(NodeId at, bool firm);
    void trader_entry();
    void settle_trader(Trader& t);
    double trait(int id, std::uint64_t salt) const;   // a fixed temperament in [0,1) drawn from a trader's id and the seed (nothing stored)
    double goal_of(int id) const;                     // the pile he is after
    bool wants_house(const Trader& t) const;    // would a house get him rich faster than he is getting there?
    bool maybe_settle(Trader& t);
    void review_rails();             // start building a latent rail link where a town has grown enough to justify it
    bool review_roads();             // raise and wear down trail edges by how many wagons use them; true if any changed   // a freelancer with his pile may settle where he stands; true if he did
    double spot_uncertainty(NodeId d, GoodId g, double q) const;
    double certainty_adjusted(const Trader& t, NodeId d, GoodId g, double q, double revenue) const;
    double daily_cost(const Trader& t, int guards) const;
    // What it costs this trader a unit to haul a good: the market rate for hired haulers (town convoys, express agents,
    // contract loads), `owner_haul_share` of it for a man hauling with his own wagon.
    double haul_unit_cost(const Trader& t, GoodId g, const double* miles_by_mode) const;
    double stop_cost(int guards, int legs, double cargo_value) const;   // spending at the stops of a trip, as the planner sees it (0 unless plan_stop_costs)
    void traders_day();
    // The cause chain of a food shortage, per food (shelf cover, open order, freight on the road, trader drain, last delivery).
    std::string supply_chain(NodeId id, double civilians, Fields& fields) const;
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
    const std::vector<NodeId>& horizon(NodeId n);
    Plan plan_job(const Trader& t, const std::vector<PathInfo>& paths);
    bool act_take_job(Trader& t, const Plan& j);
    void deliver_job(Trader& t);
    void settle_consignment(Trader& t);
    bool plan_deadhead(const Trader& t, NodeId& dest);
    void arrive(Trader& t);
    Receipt sell_cargo(Trader& t, double qty);   // the sale itself, with no check that the outfit is idle
    bool sell_along_the_way(Trader& t);          // true if the whole load went here and the trip is over
    double lead_days(NodeId n) const;   // travel days from the nearest eastern market (0 if none reaches)
    struct LeadEstimate { double lead, spread; };
    LeadEstimate lead_estimate(NodeId n) const;   // days from posting an order to its arrival, and how much that wanders
    void note_delivery(NodeId town, long ordered_day);
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

    // ---- gold express (express.cpp) ----
    void express_day();
    void finish_express(Trader& c);

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
    double phase_time_[kPhaseCount] = {};
    void rebuild_edge_gangs();
    bool resolve_encounter(Gang& g, Trader& t, EdgeId e);
    struct GangTake { double units = 0, value = 0, loot = 0; bool worth = false; };  // value: owner's loss; loot: what the goods are worth
    GangTake gang_take(double men, bool desperate, const Trader& t) const;
    bool gang_desperate(const Gang& g) const;
    void gang_day(Gang& g);
    void gang_review(Gang& g);
    void bushwhack(Gang& g);
    void bushwhack_reports();
    void send_cavalry(NodeId near_to, Gang& g, const std::string& why);
    void gang_review_pick(Gang& g);
    // Camps and town visits (crime.cpp).
    void choose_camp(Gang& g);
    double gang_town_visit(Gang& g);   // returns what it left in town
    // Infamy and flight (crime.cpp).
    double gang_known(const Gang& g, int region) const { return region >= 0 && static_cast<std::size_t>(region) < g.known.size() ? g.known[static_cast<std::size_t>(region)] : 0.0; }
    void gang_deed_region(Gang& g, int region, double weight);
    void gang_deed_edge(Gang& g, EdgeId e, double weight);
    double gang_heat(const Gang& g, EdgeId e) const;
    double law_presence(const Node& n) const;
    void gang_dissolve(Gang& g, const char* why, bool men_handled = false);
    // Jail (crime.cpp).
    void gang_arrest(Gang& g);
    void jail_day();
    // Gang wars and veterans (crime.cpp).
    void gang_add_men(Gang& g, int n, int fights = 0);
    void gang_lose_men(Gang& g, int n);
    int gang_veterans(const Gang& g) const;
    double gang_strength(const Gang& g) const;     // men, with veterans counting for more
    double gang_manageable(const Gang& g) const;   // how many men this boss can hold together
    void gang_fought(Gang& g);                     // every survivor has been in another shootout
    void gang_breakdown_check(Gang& g);
    void gang_wars_day();
    void gang_fix_cells(Gang& g);
    void gang_absorb(Gang& host, Gang& other);
    void gang_try_join(Gang& g);
    void band_catch_check(Gang& g, EdgeId e, double base = -1, const char* how = nullptr);
    void try_form_band(NodeId n);
    void scatter_men(NodeId at, double n, DrifterFlow why);
    bool is_band(const Gang& g) const { return g.ruling < 0 && g.men <= world_.params.band_max_men; }
    double gang_contact_prob(const Gang& g) const;
    void gang_war(Gang& a, Gang& b, EdgeId e);
    void try_form_gang(NodeId n);
    void create_gang(NodeId hideout, int men, const char* cause, int cells = 0, int record = 0);
    double gang_edge_score(const Gang& g, EdgeId e) const;
    double grudge_of(const Gang& g, NodeId n) const;
    void try_form_posse(NodeId n);
    void law_day();
    void news_day();
    bool make_headline(const Event& e, Headline& h) const;
    void set_law(NodeId n, int level, const std::string& cause);
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
    double starved_logged_ = 0, hunted_logged_ = 0;
    double contract_units_ = 0, contract_revenue_ = 0, contract_outlay_ = 0;
    int houses_founded_ = 0, house_recaps_ = 0;
    int house_exits_[2] = {0, 0};               // outfits of houses lost: [0] killed, [1] broke with the house unable to cover
    int orders_placed_ = 0, orders_expired_ = 0, convoys_hired_ = 0;
    int express_sent_ = 0, express_robbed_ = 0;
    double express_oz_ = 0;
    int jobs_taken_ = 0, jobs_delivered_ = 0;
    double job_pay_ = 0, job_lost_ = 0;
    double delivered_value_ = 0, lost_orders_value_ = 0;
    std::vector<double> imported_value_;
    std::vector<Gang> gangs_;
    std::vector<Posse> posses_;
    std::vector<Leader> leaders_;
    std::vector<Interest> interests_;
    int gang_peak_men_ = 0;                    // the largest gang seen so far
    double drifter_flow_[static_cast<std::size_t>(DrifterFlow::Count)] = {};
    std::vector<std::vector<std::shared_ptr<const Headline>>> papers_;   // per node: what its paper has printed lately
    std::vector<NewsDelivery> news_queue_;
    std::vector<NodeId> paper_nodes_;
    std::vector<Bushwhack> bush_pending_;
    long bushwhacks_ = 0, bush_killed_ = 0, bush_notable_ = 0;
    long bands_jailed_ = 0, bands_hanged_ = 0, men_hanged_ = 0;
    double found_budget_[2] = {0, 0};   // sites the settlement pace lets us found: farms, ranches (fractions carry over)
    long band_qualified_ = 0, join_tried_ = 0, join_nopartner_ = 0;
    long camp_visits_ = 0, camp_trouble_ = 0, camp_escapes_ = 0, camp_caught_ = 0;
    std::size_t news_cursor_ = 0;              // next log entry to consider for a headline
    long headlines_printed_ = 0;
    long large_ended_ = 0;                     // gangs that ever reached 6 men, how long they lasted, and how they ended
    double large_days_lived_ = 0;
    int large_end_[7] = {};
    long gangs_ended_ = 0;                     // gangs that broke up, and the days they lasted
    double gang_days_lived_ = 0;
    void note_drifters(DrifterFlow f, double n) { drifter_flow_[static_cast<std::size_t>(f)] += n; }
    std::vector<std::vector<int>> edge_gangs_;  // gang ids per edge, rebuilt daily
    int next_trader_id_ = 1, next_gang_id_ = 1, next_posse_id_ = 1, next_leader_id_ = 1;
    double avg_profit_ema_ = 0;
    double baseline_money_ = 0;
    // What entrants can see (never the true profit): rumor of success stories and the reported return on capital.
    double story_ema_ = 0, return_ema_ = 0, danger_ema_ = 0, scarcity_ema_ = 0, scarcity_norm_ = -1;
    int stories_today_ = 0, robbed_today_ = 0;
    int entered_freelancers_ = 0, entered_firms_ = 0, entered_window_f_ = 0, entered_window_firm_ = 0;
    std::vector<NodeId> gateways_, markets_;
    std::vector<double> bought_value_[2];
    std::vector<double> flour_bought_at_;
    int weather_year_ = -1;
    TradeBook book_;
    void book(const Trader& t, GoodId g, const TradeAcc& d);   // add to the trader's class, and for lone traders to the good and the year
    double profit_today_ = 0;                   // realized trader profit, net of upkeep, for the join rate

    std::vector<double> opportunity_[2];        // [freighter, drover]: best standard-load profit at each node
    long opportunity_day_ = -1;
    struct HorizonPath { double miles[kModeCount] = {0, 0, 0}, days = 0; };
    std::vector<std::vector<HorizonPath>> horizon_path_;   // per node, parallel to horizon_: miles by mode and days to each town in it
    std::vector<std::vector<NodeId>> horizon_;   // per node: the towns a trader there will consider (see Params::trade_horizon)
    long horizon_version_ = -1;
    std::vector<std::size_t> shock_done_;       // indices of shocks already applied
    std::vector<EdgeId> scratch_route_;
    std::vector<PathInfo> scratch_paths_;
    std::vector<double> inbound_;
};

}  // namespace frontier
