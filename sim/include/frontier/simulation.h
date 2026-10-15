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

    // Personality and memory: experience reshapes these (principle 4).
    double base_risk_aversion = 1;
    double risk_aversion = 1;
    std::vector<std::pair<EdgeId, double>> fear;  // sparse: daily extra risk this trader assigns to an edge
    int robberies = 0;
    double pending_spend = 0;    // upkeep accrued on the road, spent in the next town

    // Travel state.
    std::vector<EdgeId> route;
    std::size_t leg = 0;
    double days_left = 0;
    NodeId dest = -1;
    bool travelling = false;
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
    int jobs = 0, wins = 0;
};

struct Posse {
    int id = 0;
    bool active = true;
    NodeId origin = -1;
    int men = 0;
    int gang_id = -1;
    int leader_id = -1;
    double days_left = 0;
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
    double avg_trader_profit() const { return avg_profit_ema_; }
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
    void economy_day();
    void update_population(NodeId n);
    void end_of_day();

    // ---- traders (traders.cpp) ----
    void spawn_trader(NodeId at);
    void traders_day();
    void step_trader(Trader& t);
    void decide(Trader& t);
    void plan_paths(const Trader& t, NodeId from, std::vector<PathInfo>& out);
    double edge_risk(const Trader& t, EdgeId e) const;
    double defense(int guards) const;
    double daily_cost(int guards) const;
    Plan plan_sell(const Trader& t, const std::vector<PathInfo>& paths, bool allow_here);
    Plan plan_buy(const Trader& t, const std::vector<PathInfo>& paths);
    bool plan_deadhead(const Trader& t, NodeId& dest);
    void depart(Trader& t, NodeId dest, int guards);
    void arrive(Trader& t);
    void sell_cargo(Trader& t);
    void ensure_opportunities();
    void retire(Trader& t, const char* why);

    // ---- crime and law (crime.cpp) ----
    void crime_day();
    void rebuild_edge_gangs();
    bool resolve_encounter(Gang& g, Trader& t, EdgeId e);
    void gang_day(Gang& g);
    void gang_review(Gang& g);
    void gang_dissolve(Gang& g, const char* why);
    void try_form_gang(NodeId n);
    void create_gang(NodeId hideout, int men, const char* cause);
    double gang_edge_score(const Gang& g, EdgeId e) const;
    double grudge_of(const Gang& g, NodeId n) const;
    void try_form_posse(NodeId n);
    void posse_day(Posse& p);
    NodeId nearest_node(EdgeId e, NodeId prefer_not_fixed) const;

    std::string nname(NodeId n) const { return world_.nodes[World::idx(n)].name; }
    std::string ename(EdgeId e) const;
    const GoodDef& good(GoodId g) const { return world_.goods[World::idx(g)]; }
    Gang* find_gang(int id);
    Leader* find_leader(int id);
    void release_leader(Leader& l, NodeId at);

    World world_;
    Rng rng_;
    EventLog log_;
    Scenario sc_;

    std::vector<Trader> traders_;
    std::vector<Gang> gangs_;
    std::vector<Posse> posses_;
    std::vector<Leader> leaders_;
    std::vector<std::vector<int>> edge_gangs_;  // gang ids per edge, rebuilt daily
    int next_trader_id_ = 1, next_gang_id_ = 1, next_posse_id_ = 1, next_leader_id_ = 1;
    double avg_profit_ema_ = 0;
    double profit_today_ = 0;                   // realized trader profit, net of upkeep, for the join rate

    std::vector<double> opportunity_[2];        // [freighter, drover]: best standard-load profit at each node
    long opportunity_day_ = -1;
    std::vector<std::size_t> shock_done_;       // indices of shocks already applied
    std::vector<EdgeId> scratch_route_;
    std::vector<PathInfo> scratch_paths_;
};

}  // namespace frontier
