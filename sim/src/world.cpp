#include "frontier/world.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>

namespace frontier {

const char* money_flow_name(MoneyFlow f) {
    switch (f) {
        case MoneyFlow::EastBuys:         return "east buys frontier goods";
        case MoneyFlow::EastSells:        return "east sells to traders";
        case MoneyFlow::Government:       return "government pay";
        case MoneyFlow::Rush:             return "rush savings";
        case MoneyFlow::NewTraderCapital: return "new trader capital";
        case MoneyFlow::InterestBudget:   return "interest budgets";
        case MoneyFlow::GangStake:        return "new gang purses";
        case MoneyFlow::Freight:          return "freight paid out";
        case MoneyFlow::EastSpending:     return "spending in eastern markets";
        case MoneyFlow::Emigration:       return "emigrants' savings";
        case MoneyFlow::TraderExit:       return "traders leaving";
        case MoneyFlow::GangExit:         return "dissolved gangs";
        case MoneyFlow::LeaderExit:       return "dead leaders";
        case MoneyFlow::DustPaid:         return "gold dust paid as money";
        case MoneyFlow::SupplierLoss:     return "suppliers' robbed consignments";
        case MoneyFlow::Count:            break;
    }
    return "?";
}

World::World(const WorldDef& def, const Params& p, long start_day) : goods(def.goods), params(p), today(start_day) {
    for (std::size_t g = 0; g < goods.size(); ++g) {
        if (goods[g].id == "flour") flour_good = static_cast<GoodId>(g);
        if (goods[g].id == "forage") forage_good = static_cast<GoodId>(g);
        if (goods[g].money && money_good < 0) money_good = static_cast<GoodId>(g);
    }
    rivers = def.rivers;
    crossings = def.crossings;
    for (const RegionDef& r : def.regions) { region_names.push_back(r.name); region_organized.push_back(r.organized); }
    for (const NodeDef& d : def.nodes) {
        Node n;
        if (!d.region.empty()) {
            auto it = std::find(region_names.begin(), region_names.end(), d.region);
            if (it == region_names.end()) { region_names.push_back(d.region); region_organized.push_back(LONG_MIN); it = region_names.end() - 1; }
            n.region = static_cast<int>(it - region_names.begin());
        }
        n.id = d.id; n.name = d.name; n.kind = d.kind;
        n.x = d.x; n.y = d.y;
        n.fixed_market = d.fixed_market;
        n.transient_inflow = d.transient_inflow;
        n.gov_income = d.gov_income;
        n.capacity = d.capacity;
        n.base_population = d.base_population;
        n.farmers = d.farmers;
        n.farm_core = d.farmers * p.farm_core_share;
        n.farm_capacity = d.farm_capacity;
        n.game_capacity = d.game_capacity;
        n.game = d.game_capacity;  // untouched country starts full
        n.farm_yield = d.farm_yield;
        n.cash_good = d.cash_good;
        n.cash_rate = d.cash_rate;
        n.farm_open = d.farm_open;
        n.latent = d.latent;
        n.founded = !d.latent;
        n.found_pop = d.found_pop;
        n.found_good = d.found_crop.empty() ? -1 : static_cast<GoodId>(std::find_if(goods.begin(), goods.end(), [&](const GoodDef& g) { return g.id == d.found_crop; }) - goods.begin());
        n.found_quality = d.found_quality;
        n.strike_chance = d.strike_chance;
        n.strike_people = d.strike_people;
        n.paced = d.paced;
        if (!d.climate.empty()) {
            auto it = std::find(zone_names.begin(), zone_names.end(), d.climate);
            if (it == zone_names.end()) { zone_names.push_back(d.climate); it = zone_names.end() - 1; }
            n.zone = static_cast<int>(it - zone_names.begin());
        }
        n.tags = d.tags;
        n.population = d.population;
        n.cash = d.cash;
        n.stock = d.stock;
        n.fixed_price = d.fixed_price;
        n.reserve = d.reserve;
        n.extra_demand = d.extra_demand;
        n.pressure.assign(d.stock.size(), 0.0);
        n.garrison = d.garrison;
        n.contracts = d.contracts;
        n.contract_price.assign(d.stock.size(), 0.0);
        n.contract_backlog.assign(d.stock.size(), 0.0);
        n.depot.assign(d.stock.size(), 0.0);
        n.bought_out.assign(d.stock.size(), 0.0);
        n.last_delivery.assign(d.stock.size(), -1000000000L);
        n.order_qty.assign(d.stock.size(), 0.0);
        n.order_price.assign(d.stock.size(), 0.0);
        n.order_first_price.assign(d.stock.size(), 0.0);
        n.order_posted.assign(d.stock.size(), 0);
        n.order_raised.assign(d.stock.size(), 0);
        n.order_hired.assign(d.stock.size(), 0);
        for (const ContractDef& c : d.contracts) n.contract_price[static_cast<std::size_t>(c.good)] = c.price;
        n.production = d.production;
        n.production_started.assign(d.production.size(), 0);
        // Assume the town was in steady state when the scenario begins.
        n.income_ema = n.fixed_market ? 0 : std::max(0.0, n.population - n.base_population - n.farmers - n.garrison) * p.support_income_per_person;
        n.income_peak = n.income_ema;
        n.peak_pop = n.population;
        // The law a place has on the first day: the stated level, else by size. An eastern market is fully policed, a
        // post has its provost guard, and a camp too small or too new for a marshal has none.
        if (d.law >= 0) n.law = d.law;
        else if (!p.law_stages || n.fixed_market) n.law = 2;
        else if (n.market_population() >= p.law_sheriff_pop && organized(n)) n.law = 2;
        else if (n.market_population() >= p.law_marshal_pop || n.garrison > 0) n.law = 1;
        else n.law = 0;
        n.paper = d.paper >= 0 ? d.paper : (n.market_population() >= p.news_paper_pop ? 1 : 0);
        nodes.push_back(std::move(n));
    }
    for (const EdgeDef& d : def.edges) {
        Edge e;
        e.a = d.a; e.b = d.b; e.mode = d.mode; e.miles = d.miles;
        e.hazard = d.hazard >= 0 ? d.hazard : p.hazard[static_cast<int>(d.mode)];
        e.travel_days = std::max(1.0, std::ceil(d.miles / p.speed[static_cast<int>(d.mode)]));
        e.from = d.from; e.until = d.until; e.note = d.note; e.path = d.path; e.river_crossings = d.river_crossings; e.river_biggest = d.river_biggest;
        const EdgeId id = static_cast<EdgeId>(edges.size());
        nodes[idx(d.a)].edges.push_back(id);
        nodes[idx(d.b)].edges.push_back(id);
        edges.push_back(e);
    }
    east_bought_value.assign(goods.size(), 0.0);
    east_sold_value.assign(goods.size(), 0.0);
    zone_yield.assign(zone_names.size(), 1.0);
    update_active_edges(nullptr, nullptr);
    rebuild_routes();
    refresh_contract_prices();
}

// ---------------------------------------------------------------- prices

double World::daily_demand(NodeId n, GoodId g) const {
    const Node& node = nodes[idx(n)];
    const GoodDef& gd = goods[idx(g)];
    // A good the Army contracts for reaches the garrison and the agency through the government store, so the town's
    // merchants do not stock it for them. Anything uncontracted (whiskey, say) is still bought off the shelf.
    const bool government = node.contract_price[idx(g)] > 0;
    const double soldiers = std::min(node.garrison, node.market_population());
    const double civilians = node.market_population() - (government ? soldiers : 0.0);
    return civilians * gd.need_per_person_per_day + (government ? 0.0 : node.extra_demand[idx(g)] + node.garrison * gd.garrison_need);
}

double World::farm_output(NodeId n) const {
    const Node& node = nodes[idx(n)];
    return node.farmers * params.farm_surplus_per_farmer * node.farm_yield;
}

double World::farm_cash_output(NodeId n) const {
    const Node& node = nodes[idx(n)];
    return node.cash_good < 0 ? 0.0 : node.farmers * node.cash_rate * node.farm_yield;
}

double World::daily_output(NodeId n, GoodId g) const {
    double produced = 0;
    for (const ProductionDef& p : nodes[idx(n)].production)
        if (p.good == g) produced += p.rate;
    if (g == flour_good) produced += farm_output(n);
    if (g >= 0 && g == nodes[idx(n)].cash_good) produced += farm_cash_output(n);
    if (g == forage_good) produced += farm_output(n) * params.farm_forage_ratio;
    return produced;
}

void World::refresh_contract_prices() {
    std::vector<EdgeId> path;
    for (std::size_t ni = 0; ni < nodes.size(); ++ni) {
        Node& node = nodes[ni];
        for (const ContractDef& c : node.contracts) {
            if (c.markup <= 0) continue;
            double best = 1e18;
            for (std::size_t m = 0; m < nodes.size(); ++m) {
                // Suppliers: eastern markets that sell the good, and frontier producers (cheaper for cattle, horses, hay).
                const double source = nodes[m].fixed_market ? nodes[m].fixed_price[idx(c.good)]
                                      : daily_output(static_cast<NodeId>(m), c.good) > 0 ? goods[idx(c.good)].base_cost : 0.0;
                if (source <= 0 || m == ni) continue;
                if (!route(static_cast<NodeId>(m), static_cast<NodeId>(ni), path)) continue;
                double miles[kModeCount] = {0, 0, 0};
                for (EdgeId e : path) miles[static_cast<int>(edges[idx(e)].mode)] += edges[idx(e)].miles;
                const double cost = source * (1 + params.spread / 2) + freight_unit_cost(c.good, miles);
                best = std::min(best, cost);
            }
            if (best < 1e17) node.contract_price[idx(c.good)] = best * (1 + c.markup);
            else if (c.price > 0) node.contract_price[idx(c.good)] = c.price;
        }
    }
}

double World::contract_rate(NodeId n, const ContractDef& c) const {
    if (!c.per_soldier) return c.units_per_day;
    const GoodDef& gd = goods[idx(c.good)];
    return nodes[idx(n)].garrison * (gd.need_per_person_per_day + gd.garrison_need);
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
    if (node.fixed_price[idx(g)] > 0) return fixed_price_at(n, g, node.pressure[idx(g)]);
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
    return mid_price_at(n, g, stock) * (1 + params.spread / 2 + nodes[idx(n)].toll);
}

double World::sell_price_at(NodeId n, GoodId g, double stock) const {
    return mid_price_at(n, g, stock) * std::max(0.05, 1 - params.spread / 2 - nodes[idx(n)].toll);
}

double World::fixed_price_at(NodeId n, GoodId g, double pressure) const {
    const Node& node = nodes[idx(n)];
    const double depth = goods[idx(g)].market_depth;
    if (depth <= 0) return node.fixed_price[idx(g)];
    const double p = std::clamp(pressure, -0.5 * depth, 5.0 * depth);
    return node.fixed_price[idx(g)] / (1.0 + p / depth);
}

double World::mid_price(NodeId n, GoodId g) const { return mid_price_at(n, g, nodes[idx(n)].stock[idx(g)]); }

namespace {
constexpr int kChunks = 8;  // a trade moves the price as it goes; quotes and executions use the same steps
}

double World::quote_buy(NodeId n, GoodId g, double q) const {
    const Node& node = nodes[idx(n)];
    if (node.fixed_market) {
        double total = 0, p = node.pressure[idx(g)];
        const double u = q / kChunks;
        for (int i = 0; i < kChunks; ++i) {
            total += u * fixed_price_at(n, g, p - u / 2) * (1 + params.spread / 2);
            p -= u;
        }
        return total;
    }
    double total = 0, stock = node.stock[idx(g)];
    const double u = q / kChunks;
    for (int i = 0; i < kChunks; ++i) {
        total += u * buy_price_at(n, g, std::max(0.0, stock - u / 2));  // midpoint of the chunk
        stock -= u;
    }
    return total;
}

double World::dust_on_hand(NodeId n) const {
    if (money_good < 0) return 0;
    const Node& node = nodes[idx(n)];
    return node.fixed_market ? 0.0 : std::max(0.0, node.stock[idx(money_good)]) * goods[idx(money_good)].base_cost;
}

// What the town pays a unit that fills its open order: its offer, or its own shelf price if that is higher. A camp
// whose beef sells for the price ceiling does not pay a stranger a quarter of that because it wrote a modest offer
// when the shelves were full. The order guarantees the quantity at no less than the offer; it never caps the price.
double World::order_unit_price(NodeId n, GoodId g, double extra_stock) const {
    const Node& node = nodes[idx(n)];
    if (node.order_qty[idx(g)] <= 0 || node.order_price[idx(g)] <= 0) return 0;
    return std::max(node.order_price[idx(g)], sell_price_at(n, g, node.stock[idx(g)] + extra_stock));
}

// Revenue a seller can count on at a node: government contract first, then the town's open order, which the
// town can pay only as far as its cash goes.
double World::quote_sell_contract(NodeId n, GoodId g, double q, double inbound) const {
    const Node& node = nodes[idx(n)];
    if (node.fixed_market) return 0;
    const double contract_left = std::max(0.0, node.contract_backlog[idx(g)] - inbound);
    const double after_contract = std::max(0.0, inbound - node.contract_backlog[idx(g)]);
    const double order_left = std::max(0.0, node.order_qty[idx(g)] - after_contract);
    const double shelf_extra = std::max(0.0, after_contract - node.order_qty[idx(g)]);
    const double under_contract = std::min(q, contract_left);
    const double price = order_unit_price(n, g, shelf_extra);
    const double order_units = price > 0 ? std::min({q - under_contract, order_left, std::max(0.0, node.cash) / price}) : 0.0;
    return under_contract * node.contract_price[idx(g)] + order_units * price;
}

double World::quote_sell(NodeId n, GoodId g, double q, double inbound, double drain) const {
    const Node& node = nodes[idx(n)];
    if (!node.founded) return 0;
    if (node.fixed_market) {
        double total = 0, p = node.pressure[idx(g)];
        const double u = q / kChunks;
        for (int i = 0; i < kChunks; ++i) {
            total += u * fixed_price_at(n, g, p + u / 2) * (1 - params.spread / 2);
            p += u;
        }
        return total;
    }
    // The federal contract takes the first units at its fixed price, paid by the Treasury.
    const double contract_left = std::max(0.0, node.contract_backlog[idx(g)] - inbound);
    const double after_contract = std::max(0.0, inbound - node.contract_backlog[idx(g)]);
    const double order_left = std::max(0.0, node.order_qty[idx(g)] - after_contract);
    const double shelf_extra = std::max(0.0, after_contract - node.order_qty[idx(g)]);
    const double under_contract = std::min(q, contract_left);
    const double contract_pay = under_contract * node.contract_price[idx(g)];
    q -= under_contract;
    // The town's own open order takes the next units at the offered price, as far as its cash goes.
    const double oprice = order_unit_price(n, g, shelf_extra - drain);
    const double order_units = oprice > 0 ? std::min({q, order_left, std::max(0.0, node.cash) / oprice}) : 0.0;
    const double order_pay = order_units * oprice;
    q -= order_units;
    double total = 0, stock = node.stock[idx(g)] + shelf_extra - drain + order_units;  // (contract units go to the government store, not the shelf)
    const double u = q / kChunks;
    for (int i = 0; i < kChunks; ++i) {
        total += u * sell_price_at(n, g, stock + u / 2);
        stock += u;
    }
    // Cash scarcity: a town can't pay more than it has. A camp that mines gold can pay the rest in dust.
    return contract_pay + order_pay + std::min(total, std::max(0.0, node.cash - order_pay) + dust_on_hand(n));
}

double World::buy(NodeId n, GoodId g, double q, double max_spend, double& spent) {
    Node& node = nodes[idx(n)];
    spent = 0;
    double bought = 0;
    for (int i = 0; i < kChunks; ++i) {
        double u = q / kChunks;
        if (!node.fixed_market) u = std::min(u, node.stock[idx(g)]);
        const double price = node.fixed_market ? fixed_price_at(n, g, node.pressure[idx(g)] - u / 2) * (1 + params.spread / 2)
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
            node.bought_out[idx(g)] += u;
            node.cash += cost;
            if (!goods[idx(g)].money) {
                node.inflow_today += cost;
                node.earned_sales += cost;
            }
        } else {
            node.pressure[idx(g)] -= u;  // the East is drawn down: its price rises
            east_sold_value[idx(g)] += cost;
            record(MoneyFlow::EastSells, -cost);
        }
    }
    return bought;
}

double World::sell(NodeId n, GoodId g, double q, double& revenue) {
    Node& node = nodes[idx(n)];
    revenue = 0;
    double sold = 0;
    if (!node.founded) return 0;
    if (!node.fixed_market) {
        const double under_contract = std::min(q, node.contract_backlog[idx(g)]);
        if (under_contract > 0) {
            const double pay = under_contract * node.contract_price[idx(g)];
            node.contract_backlog[idx(g)] -= under_contract;
            node.depot[idx(g)] += under_contract;
            record(MoneyFlow::Government, pay);  // the Treasury pays the contractor directly
            revenue += pay;
            sold += under_contract;
            q -= under_contract;
        }
        // The town's open order, paid from its own cash.
        const double oprice = order_unit_price(n, g, 0);
        const double ou = oprice > 0 ? std::min({q, node.order_qty[idx(g)], std::max(0.0, node.cash) / oprice}) : 0.0;
        if (ou > 0) {
            const double pay = ou * oprice;
            node.cash -= pay;
            node.stock[idx(g)] += ou;
            node.last_delivery[idx(g)] = today;
            node.order_qty[idx(g)] -= ou;
            if (node.order_qty[idx(g)] < 1e-6) node.order_qty[idx(g)] = node.order_price[idx(g)] = 0;
            order_filled_value += pay;
            order_filled_units += ou;
            revenue += pay;
            sold += ou;
            q -= ou;
        }
    }
    for (int i = 0; i < kChunks; ++i) {
        double u = q / kChunks;
        const double price = node.fixed_market ? fixed_price_at(n, g, node.pressure[idx(g)] + u / 2) * (1 - params.spread / 2)
                                               : sell_price_at(n, g, node.stock[idx(g)] + u / 2);
        double pay = u * price;
        if (!node.fixed_market) {
            // Cash first; a gold camp short of cash pays the balance in dust, which is money to the trader too.
            const double cash_pay = std::min(pay, std::max(0.0, node.cash));
            double dust_pay = 0;
            if (cash_pay < pay && money_good >= 0 && g != money_good) {
                const double face = goods[idx(money_good)].base_cost;
                dust_pay = std::min(pay - cash_pay, std::max(0.0, node.stock[idx(money_good)]) * face);
                node.stock[idx(money_good)] -= dust_pay / face;
                record(MoneyFlow::DustPaid, dust_pay);
            }
            const double paid = cash_pay + dust_pay;
            if (paid <= 0) break;
            if (paid < pay) {
                u *= paid / pay;
                pay = paid;
            }
            node.cash -= cash_pay;
            node.stock[idx(g)] += u;
            node.last_delivery[idx(g)] = today;
        } else {
            node.pressure[idx(g)] += u;  // the East takes the goods: its price falls
            east_bought_value[idx(g)] += pay;
            record(MoneyFlow::EastBuys, pay);
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
        const bool should = today >= e.from && today < e.until && today >= e.blocked_until;
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
    ++route_version;
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
