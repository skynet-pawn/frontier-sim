// Farms and new towns.
//
// Farmers live off their own land and sell the surplus grain. Where they settle follows the price they can net
// at the farm gate: the best of the local market and any outlet worth the freight (the East by rail, a fort or
// mining camp by wagon). So a district near a railroad fills with farmers, a district in a drought empties, and a
// place whose neighbours are hungry and far from the railroad can become a grain hub that supplies them
// instead of waiting on long supply lines from the East.
//
// A latent site is only a spot on the map. It becomes a town when the remembered farm-gate price stays well
// above break-even for several months: settlers come where a line (rail or trail) already reaches it and a
// market already pays.
#include <algorithm>
#include <cmath>

#include "frontier/date.h"
#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

// Weather is drawn once a year per zone. Three uniforms sum to a close-enough normal, and keep runs portable.
void Simulation::draw_weather() {
    const Params& p = world_.params;
    for (std::size_t z = 0; z < world_.zone_yield.size(); ++z) {
        const double u = rng_.next_double() + rng_.next_double() + rng_.next_double();
        const double score = (u - 1.5) / 0.5;
        const double y = std::clamp(1.0 + p.farm_weather_sd * score, 0.15, 1.6);
        world_.zone_yield[z] = y;
        for (Node& n : world_.nodes)
            if (n.zone == static_cast<int>(z)) n.yield_memory += 0.4 * (y - n.yield_memory);
        if (y < 0.6) {
            log_.with({field("zone", world_.zone_names[z]), field("yield", y)});
            log_.add(world_.today, EventType::Shock, "crop_failure", -1,
                     strf("A dry year on the %s: crops come in at %.0f%% of normal", world_.zone_names[z].c_str(), y * 100),
                     "rainfall well below normal (a seeded weather draw; neighbouring farms share it)");
        }
    }
}

// What a farmer here could net for a unit of a crop, against his break-even: the best of the local market and any
// outlet worth the freight, less the merchant's cut. `outlet` is where that best price is paid.
double Simulation::crop_gate_ratio(NodeId id, GoodId fl, NodeId& outlet) {
    const Params& p = world_.params;
    const Node& n = world_.nodes[World::idx(id)];
    double best = 0;
    outlet = -1;
    if (n.founded) {
        best = world_.sell_price(id, fl);
        outlet = id;
    }
    const double q = p.farm_outlet_units;
    for (std::size_t d = 0; d < world_.nodes.size(); ++d) {
        const Node& dn = world_.nodes[d];
        if (static_cast<NodeId>(d) == id || (!dn.fixed_market && !dn.founded)) continue;
        if (!world_.route(id, static_cast<NodeId>(d), scratch_route_)) continue;
        double miles[kModeCount] = {0, 0, 0};
        for (EdgeId e : scratch_route_) miles[static_cast<int>(world_.edges[World::idx(e)].mode)] += world_.edges[World::idx(e)].miles;
        const double net = world_.quote_sell(static_cast<NodeId>(d), fl, q) / q - world_.freight_unit_cost(fl, miles);
        if (net > best) {
            best = net;
            outlet = static_cast<NodeId>(d);
        }
    }
    return std::min(best * p.farm_marketing_share / (p.farm_break_even_share * world_.goods[World::idx(fl)].base_cost), p.farm_gate_cap);
}

// A district sells grain and, where the land and climate allow, a cash crop: settlers follow whichever pays better.
double Simulation::farm_gate_ratio(NodeId id, NodeId& outlet, GoodId& crop) {
    const Node& n = world_.nodes[World::idx(id)];
    crop = world_.flour_good;
    double best = crop_gate_ratio(id, crop, outlet);
    if (n.cash_good >= 0) {
        NodeId o = -1;
        const double r = crop_gate_ratio(id, n.cash_good, o);
        if (r > best) {
            best = r;
            outlet = o;
            crop = n.cash_good;
        }
    }
    return best;
}

void Simulation::found_town(NodeId id) {
    const Params& p = world_.params;
    Node& n = world_.nodes[World::idx(id)];
    n.founded = true;
    n.founded_day = world_.today;
    n.population = n.found_pop;
    n.farmers = std::min(n.farm_capacity, n.found_pop * 0.75);
    n.farm_core = n.farmers * p.farm_core_share;
    const double savings = n.population * p.found_cash_per_person;
    n.cash += savings;
    world_.record(MoneyFlow::Rush, savings);
    for (std::size_t g = 0; g < world_.goods.size(); ++g)
        if (world_.goods[g].food_share > 0)
            n.stock[g] += n.population * world_.goods[g].need_per_person_per_day * p.newcomer_provision_days;
    n.capacity = std::max(n.capacity, n.population);
    n.peak_pop = n.population;
    n.income_ema = std::max(n.income_ema, std::max(0.0, n.population - n.farmers) * p.support_income_per_person);
    n.income_peak = n.income_ema;
    world_.refresh_contract_prices();  // a new grain district may now be the cheapest supplier to a post
    const std::string where = n.gate_outlet >= 0 ? nname(n.gate_outlet) : std::string("nearby markets");
    const std::string crop = n.gate_good >= 0 ? world_.goods[World::idx(n.gate_good)].name : std::string("grain");
    log_.with({field("node", n.name), field("settlers", n.population), field("gate_ratio", n.farm_gate), field("crop", crop), field("outlet", where)});
    log_.add(world_.today, EventType::Population, "town_founded", id,
             strf("%s is founded: %s settlers file claims", n.name.c_str(), with_commas(n.population).c_str()),
             strf("%s would net %.1fx its break-even, best sold at %s; a road already reaches the site",
                  crop.c_str(), n.farm_gate, where.c_str()));
}

void Simulation::farms_day() {
    const Params& p = world_.params;
    const int year = year_of(world_.today);
    if (year != weather_year_) {
        weather_year_ = year;
        draw_weather();
    }
    if (world_.flour_good < 0) return;
    const double jan1 = static_cast<double>(days_from_civil(year, 1, 1));
    const int doy = static_cast<int>(static_cast<double>(world_.today) - jan1);
    const bool harvest = doy >= p.farm_harvest_start && doy < p.farm_harvest_start + p.farm_harvest_days;

    // The pace of settlement, in sites a day, for farms and ranches (0: no limit). Sites that qualify wait in `ready` for their turn.
    const double run = std::max(1.0, static_cast<double>(sc_.days));
    const double through = std::clamp(static_cast<double>(world_.today - sc_.start) / run, 0.0, 1.0);
    const double pace[2] = {(p.farm_pace_start + (p.farm_pace_end - p.farm_pace_start) * through) / 30.0,
                            (p.ranch_pace_start + (p.ranch_pace_end - p.ranch_pace_start) * through) / 30.0};
    std::vector<NodeId> ready[2];

    for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
        Node& n = world_.nodes[i];
        const NodeId id = static_cast<NodeId>(i);
        if (n.fixed_market) continue;
        const bool monthly = (world_.today + id) % 30 == 0;

        if (!n.founded) {
            // A site becomes a town when settling it has paid for a few months running.
            if (!monthly && n.found_streak >= p.found_months && n.strike_chance <= 0 && world_.today >= n.farm_open) {   // already qualified: waiting for its turn
                const int line = !n.paced ? -1 : n.found_good < 0 ? 0 : (world_.goods[World::idx(n.found_good)].id == "beef_cattle" ? 1 : -1);
                if (line >= 0 && ((line == 0 && p.farm_pace_start + p.farm_pace_end > 0) || (line == 1 && p.ranch_pace_start + p.ranch_pace_end > 0))) ready[line].push_back(id);
                continue;
            }
            if (!monthly || world_.today < n.farm_open) continue;
            if (n.strike_chance > 0) {   // prospectors: a strike, or not, this month
                if (rng_.chance(n.strike_chance)) {
                    found_town(id);
                    rush_arrives(id, n.strike_people, p.found_cash_per_person, "Strike at " + n.name, "prospectors found paying ground (a random draw weighted by the deposit)");
                }
                continue;
            }
            NodeId outlet = -1;
            GoodId crop = -1;
            double gate = n.found_good >= 0 ? (crop = n.found_good, crop_gate_ratio(id, crop, outlet)) : farm_gate_ratio(id, outlet, crop);
            gate *= n.found_quality;   // good land, plentiful game or timber makes the same price worth more
            n.farm_gate = n.farm_gate > 0 ? n.farm_gate + p.farm_gate_alpha * (gate - n.farm_gate) : gate;
            n.gate_outlet = outlet;
            n.gate_good = crop;
            n.found_streak = n.farm_gate >= p.found_gate_ratio ? n.found_streak + 1 : 0;
            if (n.found_streak >= p.found_months) {
                const int line = !n.paced ? -1 : n.found_good < 0 ? 0 : (world_.goods[World::idx(n.found_good)].id == "beef_cattle" ? 1 : -1);   // farms and ranches wait for their turn
                if (line >= 0 && ((line == 0 && p.farm_pace_start + p.farm_pace_end > 0) || (line == 1 && p.ranch_pace_start + p.ranch_pace_end > 0))) ready[line].push_back(id);
                else found_town(id);
            }
            continue;
        }
        if (n.farm_capacity <= 0 && n.farmers <= 0 && n.farm_store <= 0) continue;

        if (n.farm_gate <= 0 || monthly) {
            NodeId outlet = -1;
            GoodId crop = -1;
            const double gate = farm_gate_ratio(id, outlet, crop);
            n.farm_gate = n.farm_gate > 0 ? n.farm_gate + p.farm_gate_alpha * (gate - n.farm_gate) : gate;
            n.gate_outlet = outlet;
            n.gate_good = crop;
        }

        // Settlers follow the price, held back by bad years and by land that is not yet open.
        const double cap = world_.today >= n.farm_open ? n.farm_capacity : 0.0;
        const double appeal = std::clamp((n.farm_gate - 0.8) / 0.6, 0.0, 1.0);
        const double target = std::max(n.farm_core, cap * appeal * std::clamp(n.yield_memory, 0.3, 1.0));
        if (n.farmers < target) {
            const double add = std::min(target - n.farmers, cap * p.farm_settle_base + n.farmers * p.farm_settle_growth);
            n.farmers += add;
            n.population += add;
            n.capacity = std::max(n.capacity, n.population);
        } else if (n.farmers > target) {
            const double leave = (n.farmers - target) * p.farm_leave_rate;
            n.farmers -= leave;
            n.population = std::max(0.0, n.population - leave);
            n.drifters += leave * p.leaver_drifter_fraction;  // some give up the land and go looking for work
            note_drifters(DrifterFlow::FarmLeavers, leave * p.leaver_drifter_fraction);
        }

        // The grain comes in over the harvest and goes to market through the year.
        if (harvest) {
            const double yield = n.zone >= 0 ? world_.zone_yield[static_cast<std::size_t>(n.zone)] : 1.0;
            n.farm_store += world_.farm_output(id) * yield * 365.0 / p.farm_harvest_days;
            n.cash_store += world_.farm_cash_output(id) * yield * 365.0 / p.farm_harvest_days;
        }
        if (n.cash_good >= 0) {
            const double out = n.cash_store * p.farm_release_rate;
            n.cash_store -= out;
            n.cash_store *= 1.0 - world_.goods[World::idx(n.cash_good)].spoilage_per_day;
            double& crop = n.stock[World::idx(n.cash_good)];
            crop = std::min(crop + out, world_.farm_cash_output(id) * p.stock_cap_days * 2 + 20);
        }
        const double release = n.farm_store * p.farm_release_rate;
        n.farm_store -= release;
        n.farm_store *= 1.0 - world_.goods[World::idx(world_.flour_good)].spoilage_per_day;
        double& flour = n.stock[World::idx(world_.flour_good)];
        flour += release;
        flour = std::min(flour, world_.farm_output(id) * p.stock_cap_days * 2 + 100);  // what the town cannot hold spoils
        if (world_.forage_good >= 0) n.stock[World::idx(world_.forage_good)] += release * p.farm_forage_ratio;
        ship_farm_goods(id, n);
    }

    // Settlement pace: the settlers who arrive this month take the best-served of the sites that qualify (best gate price times quality first).
    for (int line = 0; line < 2; ++line) {
        if (pace[line] <= 0) continue;
        found_budget_[line] += pace[line];
        if (ready[line].empty()) { found_budget_[line] = std::min(found_budget_[line], 3.0); continue; }   // unused places do not pile up
        std::sort(ready[line].begin(), ready[line].end(), [&](NodeId a, NodeId b) {
            const Node& na = world_.nodes[World::idx(a)];
            const Node& nb = world_.nodes[World::idx(b)];
            const double sa = na.farm_gate * na.found_quality, sb = nb.farm_gate * nb.found_quality;
            return sa != sb ? sa > sb : a < b;
        });
        for (NodeId id : ready[line]) {
            if (found_budget_[line] < 1.0) break;
            found_town(id);
            found_budget_[line] -= 1.0;
        }
    }
}

// Grain buyers and elevators ship what the town cannot use. A farm district sold to the East by the carload, not
// by the wagonload: a trader's two-wagon outfit would never bother with a flour run, so without this the surplus
// would simply rot and the farmers would give up. They ship only while the East nets more than the town pays.
void Simulation::ship_farm_goods(NodeId id, Node& n) {
    for (GoodId g : {world_.flour_good, world_.forage_good, n.cash_good}) {
        if (g < 0) continue;
        const std::size_t gi = World::idx(g);
        const double q = n.stock[gi] * 0.1;
        if (q < 5) continue;
        NodeId best = -1;
        double best_net = 0, best_freight = 0;
        for (NodeId m : markets_) {
            if (!world_.route(id, m, scratch_route_)) continue;
            double miles[kModeCount] = {0, 0, 0};
            for (EdgeId e : scratch_route_) miles[static_cast<int>(world_.edges[World::idx(e)].mode)] += world_.edges[World::idx(e)].miles;
            const double freight = world_.freight_unit_cost(g, miles);
            const double net = world_.quote_sell(m, g, q) / q - freight;
            if (net > best_net) {
                best_net = net;
                best = m;
                best_freight = freight;
            }
        }
        if (best < 0 || best_net <= world_.sell_price(id, g)) continue;
        double revenue = 0;
        const double sold = world_.sell(best, g, q, revenue);
        if (sold <= 0) continue;
        const double freight = best_freight * sold;
        n.stock[gi] -= sold;
        n.cash += revenue - freight;
        n.inflow_today += revenue - freight;
        n.earned_sales += revenue - freight;
        world_.record(MoneyFlow::Freight, -freight);
    }
}

}  // namespace frontier
