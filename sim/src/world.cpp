#include "frontier/world.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>

namespace frontier {

World::World(const WorldDef& def, const Params& p, long start_day) : goods(def.goods), params(p), today(start_day) {
    for (const NodeDef& d : def.nodes) {
        Node n;
        n.id = d.id; n.name = d.name; n.kind = d.kind;
        n.x = d.x; n.y = d.y;
        n.fixed_market = d.fixed_market;
        n.transient_inflow = d.transient_inflow;
        n.gov_income = d.gov_income;
        n.capacity = d.capacity;
        n.base_population = d.base_population;
        n.tags = d.tags;
        n.population = d.population;
        n.cash = d.cash;
        n.stock = d.stock;
        n.fixed_price = d.fixed_price;
        n.reserve = d.reserve;
        n.extra_demand = d.extra_demand;
        n.production = d.production;
        n.production_started.assign(d.production.size(), 0);
        // Assume the town was in steady state when the scenario begins.
        n.income_ema = n.fixed_market ? 0 : std::max(0.0, n.population - n.base_population) * p.support_income_per_person;
        n.income_peak = n.income_ema;
        n.peak_pop = n.population;
        nodes.push_back(std::move(n));
    }
    for (const EdgeDef& d : def.edges) {
        Edge e;
        e.a = d.a; e.b = d.b; e.mode = d.mode; e.miles = d.miles;
        e.hazard = d.hazard >= 0 ? d.hazard : p.hazard[static_cast<int>(d.mode)];
        e.travel_days = std::max(1.0, std::ceil(d.miles / p.speed[static_cast<int>(d.mode)]));
        e.from = d.from; e.until = d.until; e.note = d.note;
        const EdgeId id = static_cast<EdgeId>(edges.size());
        nodes[idx(d.a)].edges.push_back(id);
        nodes[idx(d.b)].edges.push_back(id);
        edges.push_back(e);
    }
    update_active_edges(nullptr, nullptr);
    rebuild_routes();
}

// ---------------------------------------------------------------- prices

double World::daily_demand(NodeId n, GoodId g) const {
    const Node& node = nodes[idx(n)];
    return node.market_population() * goods[idx(g)].need_per_person_per_day + node.extra_demand[idx(g)];
}

double World::daily_output(NodeId n, GoodId g) const {
    double produced = 0;
    for (const ProductionDef& p : nodes[idx(n)].production)
        if (p.good == g) produced += p.rate;
    return produced;
}

double World::normal_stock(NodeId n, GoodId g) const {
    const double produced = daily_output(n, g);
    // A month of demand or of output. Only a pure waystation (no demand, no output) uses the
    // floor, so a hamlet that eats little beef is not priced as if it were desperate for it.
    const double demand = daily_demand(n, g);
    if (demand <= 0 && produced <= 0) return goods[idx(g)].min_target;
    return std::max(demand, produced) * params.cover_days;
}

double World::mid_price_at(NodeId n, GoodId g, double stock) const {
    const Node& node = nodes[idx(n)];
    if (node.fixed_price[idx(g)] > 0) return node.fixed_price[idx(g)];
    const double normal = normal_stock(n, g);
    const double s = std::max(stock, normal * 1e-3);
    // Scarcity pricing: the damper that pulls traders toward shortages.
    // Only local consumers pay a real scarcity premium. A producer whose shelves are bare asks a little more
    // for what is left, but nobody pays 12x for the thing they dig or raise themselves. A waystation that
    // neither consumes nor produces a good has no reason to pay above base at all.
    const double ceiling = daily_demand(n, g) > 0   ? params.price_max_mult
                           : daily_output(n, g) > 0 ? params.price_max_mult_producer
                                                    : 1.0;
    const double mult = std::clamp(std::pow(normal / s, params.price_elasticity), params.price_min_mult, ceiling);
    return goods[idx(g)].base_cost * mult;
}

double World::buy_price_at(NodeId n, GoodId g, double stock) const {
    return mid_price_at(n, g, stock) * (1 + params.spread / 2);
}

double World::sell_price_at(NodeId n, GoodId g, double stock) const {
    return mid_price_at(n, g, stock) * (1 - params.spread / 2);
}

double World::mid_price(NodeId n, GoodId g) const { return mid_price_at(n, g, nodes[idx(n)].stock[idx(g)]); }

namespace {
constexpr int kChunks = 8;  // a trade moves the price as it goes; quotes and executions use the same steps
}

double World::quote_buy(NodeId n, GoodId g, double q) const {
    const Node& node = nodes[idx(n)];
    if (node.fixed_market) return q * buy_price_at(n, g, 0);
    double total = 0, stock = node.stock[idx(g)];
    const double u = q / kChunks;
    for (int i = 0; i < kChunks; ++i) {
        total += u * buy_price_at(n, g, std::max(0.0, stock - u / 2));  // midpoint of the chunk
        stock -= u;
    }
    return total;
}

double World::quote_sell(NodeId n, GoodId g, double q) const {
    const Node& node = nodes[idx(n)];
    if (node.fixed_market) return q * sell_price_at(n, g, 0);
    double total = 0, stock = node.stock[idx(g)];
    const double u = q / kChunks;
    for (int i = 0; i < kChunks; ++i) {
        total += u * sell_price_at(n, g, stock + u / 2);
        stock += u;
    }
    return std::min(total, std::max(0.0, node.cash));  // cash scarcity: a town can't pay what it doesn't have
}

double World::buy(NodeId n, GoodId g, double q, double max_spend, double& spent) {
    Node& node = nodes[idx(n)];
    spent = 0;
    double bought = 0;
    for (int i = 0; i < kChunks; ++i) {
        double u = q / kChunks;
        if (!node.fixed_market) u = std::min(u, node.stock[idx(g)]);
        const double price = node.fixed_market ? buy_price_at(n, g, 0)
                                               : buy_price_at(n, g, std::max(0.0, node.stock[idx(g)] - u / 2));
        double cost = u * price;
        if (spent + cost > max_spend) {
            u = (max_spend - spent) / price;
            cost = u * price;
        }
        if (u <= 0) break;
        spent += cost;
        bought += u;
        if (!node.fixed_market) {
            node.stock[idx(g)] -= u;
            node.cash += cost;
            node.inflow_today += cost;
        }
    }
    return bought;
}

double World::sell(NodeId n, GoodId g, double q, double& revenue) {
    Node& node = nodes[idx(n)];
    revenue = 0;
    double sold = 0;
    for (int i = 0; i < kChunks; ++i) {
        double u = q / kChunks;
        const double price = node.fixed_market ? sell_price_at(n, g, 0) : sell_price_at(n, g, node.stock[idx(g)] + u / 2);
        double pay = u * price;
        if (!node.fixed_market) {
            if (node.cash <= 0) break;
            if (pay > node.cash) {
                u = node.cash / price;
                pay = node.cash;
            }
            node.cash -= pay;
            node.stock[idx(g)] += u;
        }
        revenue += pay;
        sold += u;
    }
    return sold;
}

double World::freight_unit_cost(GoodId g, const double miles_by_mode[kModeCount]) const {
    const GoodDef& good = goods[idx(g)];
    double cost = 0;
    for (int m = 0; m < kModeCount; ++m)
        cost += miles_by_mode[m] * params.freight_rate[m] * good.weight_lb * good.freight_mult[m];
    return cost;
}

// ---------------------------------------------------------------- network

bool World::update_active_edges(std::vector<EdgeId>* opened, std::vector<EdgeId>* closed) {
    bool changed = false;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        Edge& e = edges[i];
        const bool should = today >= e.from && today < e.until;
        if (should != e.active) {
            e.active = should;
            changed = true;
            if (should && opened) opened->push_back(static_cast<EdgeId>(i));
            if (!should && closed) closed->push_back(static_cast<EdgeId>(i));
        }
    }
    return changed;
}

// Dijkstra from every node by travel time. Fine for tens of nodes; at ~2,000
// nodes this should become lazy or hierarchical (noted for stage 1's scale test).
void World::rebuild_routes() {
    const std::size_t N = nodes.size();
    dist_.assign(N * N, 1e18);
    prev_edge_.assign(N * N, -1);
    using Item = std::pair<double, int>;
    for (std::size_t s = 0; s < N; ++s) {
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        double* dist = &dist_[s * N];
        int* prev = &prev_edge_[s * N];
        dist[s] = 0;
        pq.push({0.0, static_cast<int>(s)});
        while (!pq.empty()) {
            const auto [d, u] = pq.top();
            pq.pop();
            if (d > dist[u]) continue;
            for (EdgeId eid : nodes[idx(u)].edges) {
                const Edge& e = edges[idx(eid)];
                if (!e.active) continue;
                const int v = e.other(u);
                const double nd = d + e.travel_days;
                if (nd < dist[v]) {
                    dist[v] = nd;
                    prev[v] = eid;
                    pq.push({nd, v});
                }
            }
        }
    }
}

bool World::route(NodeId s, NodeId d, std::vector<EdgeId>& out) const {
    out.clear();
    if (s == d) return true;
    if (!reachable(s, d)) return false;
    const std::size_t N = nodes.size();
    NodeId cur = d;
    while (cur != s) {
        const int e = prev_edge_[idx(s) * N + idx(cur)];
        if (e < 0) { out.clear(); return false; }
        out.push_back(e);
        cur = edges[idx(e)].other(cur);
    }
    std::reverse(out.begin(), out.end());
    return true;
}

}  // namespace frontier
