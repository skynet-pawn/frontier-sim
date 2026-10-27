#include "frontier/config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>

#include "frontier/date.h"
#include "frontier/terrain.h"
#include "nlohmann/json.hpp"

namespace frontier {

using json = nlohmann::json;

namespace {

json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    try {
        return json::parse(f, nullptr, true, true);
    } catch (const json::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

GoodId find_good(const std::vector<GoodDef>& goods, const std::string& id, const std::string& ctx) {
    for (std::size_t i = 0; i < goods.size(); ++i)
        if (goods[i].id == id) return static_cast<GoodId>(i);
    throw std::runtime_error(ctx + ": unknown good '" + id + "'");
}

NodeId find_node(const std::vector<NodeDef>& nodes, const std::string& id, const std::string& ctx) {
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id) return static_cast<NodeId>(i);
    throw std::runtime_error(ctx + ": unknown node '" + id + "'");
}

long date_or(const json& j, const char* key, long fallback) {
    return j.contains(key) ? parse_date(j.at(key).get<std::string>()) : fallback;
}

Mode parse_mode(const std::string& s) {
    if (s == "trail") return Mode::Trail;
    if (s == "river") return Mode::River;
    if (s == "rail") return Mode::Rail;
    throw std::runtime_error("unknown edge mode '" + s + "'");
}

// Reads {"good_id": number} into a per-good vector.
void read_good_map(const json& j, const char* key, const std::vector<GoodDef>& goods,
                   const std::string& ctx, std::vector<double>& out) {
    if (!j.contains(key)) return;
    for (auto it = j.at(key).begin(); it != j.at(key).end(); ++it)
        out[static_cast<std::size_t>(find_good(goods, it.key(), ctx))] = it.value().get<double>();
}

}  // namespace

WorldDef load_world(const std::string& dir) {
    WorldDef w;

    const json gj = read_json(dir + "/goods.json");
    for (const auto& jg : gj.at("goods")) {
        GoodDef g;
        g.id = jg.at("id").get<std::string>();
        g.name = jg.value("name", g.id);
        g.unit = jg.value("unit", "unit");
        g.base_cost = jg.at("base_cost").get<double>();
        g.weight_lb = jg.at("weight_lb").get<double>();
        g.load_lb = jg.value("load_lb", g.weight_lb);
        g.need_per_person_per_day = jg.value("need_per_person_per_day", 0.0);
        g.food_share = jg.value("food_share", 0.0);
        g.spoilage_per_day = jg.value("spoilage_per_day", 0.0);
        g.self_propelled = jg.value("self_propelled", false);
        g.game_substitute = jg.value("game_substitute", false);
        g.weather_sensitive = jg.value("weather_sensitive", false);
        g.min_target = jg.value("min_target", 10.0);
        g.market_depth = jg.value("market_depth", 0.0);
        g.garrison_need = jg.value("garrison_need", 0.0);
        g.money = jg.value("money", false);
        if (jg.contains("freight_mult"))
            for (auto it = jg["freight_mult"].begin(); it != jg["freight_mult"].end(); ++it)
                g.freight_mult[static_cast<int>(parse_mode(it.key()))] = it.value().get<double>();
        w.goods.push_back(g);
    }
    const std::size_t G = w.goods.size();

    const json wj = read_json(dir + "/world.json");
    std::map<std::string, double> terrain = {{"trail", 1.3}, {"river", 1.4}, {"rail", 1.15}};
    if (wj.contains("terrain_factor"))
        for (auto it = wj["terrain_factor"].begin(); it != wj["terrain_factor"].end(); ++it)
            terrain[it.key()] = it.value().get<double>();

    // Terrain: the world names a terrain grid and says how many extra miles each 100 m of climbing costs per mode.
    TerrainGrid grid;
    std::map<std::string, double> climb;
    double climb_descent = 0.3;   // coming down a steep grade slows a wagon too, but less than going up
    // River crossings: extra miles to cross a creek, a river, a great river (a ford, a ferry, a steamboat transfer; a bridge for rail).
    std::map<std::string, std::array<double, 3>> river_cross = {{"trail", {1.0, 4.0, 12.0}}, {"rail", {0.2, 0.5, 1.5}}, {"river", {0.0, 0.0, 0.0}}};
    if (wj.contains("terrain")) {
        grid = read_terrain(dir, wj["terrain"].get<std::string>());
        if (wj.contains("rivers")) {
            const RiverSet set = read_rivers(dir, wj["rivers"].get<std::string>());
            w.rivers = set.rivers;
            w.crossings = set.crossings;
            add_rivers(grid, set);
            if (wj.contains("river_cross"))
                for (auto it = wj["river_cross"].begin(); it != wj["river_cross"].end(); ++it) {
                    if (it.key().empty() || it.key()[0] == '_') continue;
                    for (std::size_t k = 0; k < 3 && k < it.value().size(); ++k) river_cross[it.key()][k] = it.value()[k].get<double>();
                }
        }
        climb = {{"trail", 1.0}, {"rail", 0.25}, {"river", 0.0}};
        if (wj.contains("terrain_climb"))
            for (auto it = wj["terrain_climb"].begin(); it != wj["terrain_climb"].end(); ++it) {
                if (it.key().empty() || it.key()[0] == '_') continue;
                if (it.key() == "descent") climb_descent = it.value().get<double>();
                else climb[it.key()] = it.value().get<double>();
            }
    }

    for (const auto& jn : wj.at("nodes")) {
        NodeDef n;
        n.id = jn.at("id").get<std::string>();
        const std::string ctx = "node " + n.id;
        n.name = jn.value("name", n.id);
        n.kind = jn.value("kind", "town");
        n.x = jn.at("x").get<double>();
        n.y = jn.at("y").get<double>();
        n.population = jn.value("population", 0.0);
        n.capacity = jn.value("capacity", n.population);
        n.base_population = jn.value("base_population", 0.0);
        n.garrison = jn.value("garrison", 0.0);
        n.region = jn.value("region", "");
        n.law = jn.value("law", -1);
        n.paper = jn.value("paper", -1);
        n.farmers = jn.value("farmers", 0.0);
        n.farm_capacity = jn.value("farm_capacity", 0.0);
        n.game_capacity = jn.value("game_capacity", 0.0);
        n.farm_yield = jn.value("farm_yield", 1.0);
        n.farm_open = date_or(jn, "farm_open", LONG_MIN);
        n.climate = jn.value("climate", "");
        if (jn.contains("cash_crop")) {
            n.cash_good = find_good(w.goods, jn["cash_crop"].get<std::string>(), ctx);
            n.cash_rate = jn.value("cash_rate", 0.006);
        }
        n.latent = jn.value("latent", false);
        n.found_pop = jn.value("found_pop", 40.0);
        n.found_crop = jn.value("found_crop", "");
        if (!n.found_crop.empty()) find_good(w.goods, n.found_crop, ctx);   // fail early on a misspelt good
        n.found_quality = jn.value("found_quality", 1.0);
        n.strike_chance = jn.value("strike_chance", 0.0);
        n.strike_people = jn.value("strike_people", 0.0);
        n.paced = jn.value("paced", false);
        if (jn.contains("contracts"))
            for (auto it = jn["contracts"].begin(); it != jn["contracts"].end(); ++it) {
                ContractDef c;
                c.good = find_good(w.goods, it.key(), ctx);
                c.price = it.value().value("price", 0.0);
                c.markup = it.value().value("markup", 0.0);
                if (c.price <= 0 && c.markup <= 0) throw std::runtime_error(ctx + ": contract needs a price or a markup");
                c.per_soldier = it.value().value("per_soldier", false);
                c.units_per_day = it.value().value("units_per_day", 0.0);
                n.contracts.push_back(c);
            }
        n.cash = jn.value("cash", 0.0);
        n.gov_income = jn.value("gov_income", 0.0);
        n.fixed_market = jn.value("fixed_market", false);
        n.transient_inflow = jn.value("transient_inflow", false);
        if (jn.contains("tags")) n.tags = jn["tags"].get<std::vector<std::string>>();
        n.stock.assign(G, 0.0);
        n.fixed_price.assign(G, 0.0);
        n.reserve.assign(G, -1.0);  // negative means unlimited
        n.extra_demand.assign(G, 0.0);
        read_good_map(jn, "stock", w.goods, ctx, n.stock);
        read_good_map(jn, "fixed_price", w.goods, ctx, n.fixed_price);
        read_good_map(jn, "reserves", w.goods, ctx, n.reserve);
        read_good_map(jn, "extra_demand", w.goods, ctx, n.extra_demand);
        if (jn.contains("production")) {
            for (auto it = jn["production"].begin(); it != jn["production"].end(); ++it) {
                ProductionDef p;
                p.good = find_good(w.goods, it.key(), ctx);
                if (it.value().is_number()) {
                    p.rate = it.value().get<double>();
                } else {
                    p.rate = it.value().at("rate").get<double>();
                    p.labor_pop = it.value().value("labor_pop", 0.0);
                    p.from = date_or(it.value(), "from", LONG_MIN);
                    p.until = date_or(it.value(), "until", LONG_MAX);
                }
                n.production.push_back(p);
            }
        }
        w.nodes.push_back(std::move(n));
    }

    if (wj.contains("regions"))
        for (auto it = wj["regions"].begin(); it != wj["regions"].end(); ++it) {
            if (it.key().empty() || it.key()[0] == '_') continue;
            RegionDef r;
            r.name = it.key();
            r.organized = date_or(it.value(), "organized", LONG_MIN);
            w.regions.push_back(r);
        }
    for (const auto& je : wj.at("edges")) {
        EdgeDef e;
        const std::string a = je.at("a").get<std::string>(), b = je.at("b").get<std::string>();
        const std::string ctx = "edge " + a + "-" + b;
        e.a = find_node(w.nodes, a, ctx);
        e.b = find_node(w.nodes, b, ctx);
        e.mode = parse_mode(je.value("mode", "trail"));
        if (je.contains("miles")) {
            e.miles = je.at("miles").get<double>();
        } else {
            const NodeDef& na = w.nodes[static_cast<std::size_t>(e.a)];
            const NodeDef& nb = w.nodes[static_cast<std::size_t>(e.b)];
            const double straight = std::hypot(na.x - nb.x, na.y - nb.y);
            e.miles = straight * terrain[mode_name(e.mode)];
            // Over a terrain grid the route takes the cheapest way between the towns: it goes round a ridge when that costs less than
            // climbing it, so the miles are the flat distance plus whatever the way costs beyond a straight run (detour and climbing).
            const auto rate = climb.find(mode_name(e.mode));
            if (grid.ok && straight > 0 && rate != climb.end() && rate->second > 0) {
                const TerrainPath tp = least_cost_path(grid, na.x, na.y, nb.x, nb.y, rate->second, climb_descent, river_cross[mode_name(e.mode)].data());
                e.miles += std::max(0.0, tp.cost - 1.01 * straight);   // the 1% is the grid's own wobble
                e.path = tp.points;
                e.river_crossings = tp.crossings;
                e.river_biggest = tp.biggest;
            }
        }
        e.hazard = je.value("hazard", -1.0);
        e.from = date_or(je, "from", LONG_MIN);
        e.until = date_or(je, "until", LONG_MAX);
        e.latent = je.value("latent", false);
        e.note = je.value("note", "");
        w.edges.push_back(e);
    }

    if (wj.contains("interests"))
        for (const auto& ji : wj["interests"]) {
            InterestDef d;
            d.id = ji.at("id").get<std::string>();
            const std::string ctx = "interest " + d.id;
            d.name = ji.value("name", d.id);
            d.kind = ji.at("kind").get<std::string>();
            if (d.kind != "railroad" && d.kind != "express" && d.kind != "stock_association")
                throw std::runtime_error(ctx + ": unknown kind '" + d.kind + "'");
            d.hq = find_node(w.nodes, ji.at("hq").get<std::string>(), ctx);
            for (const auto& nid : ji.at("nodes")) d.nodes.push_back(find_node(w.nodes, nid.get<std::string>(), ctx));
            d.budget_per_day = ji.value("budget_per_day", 0.0);
            d.dues_rate = ji.value("dues_rate", 0.0);
            d.from = date_or(ji, "from", LONG_MIN);
            d.until = date_or(ji, "until", LONG_MAX);
            w.interests.push_back(std::move(d));
        }
    return w;
}

Scenario load_scenario(const std::string& dir, const WorldDef& world) {
    Scenario s;
    const json j = read_json(dir + "/scenario.json");
    s.seed = j.value("seed", 1ULL);
    s.start = parse_date(j.value("start", "1875-01-01"));
    s.log_from = j.contains("log_from") ? parse_date(j["log_from"].get<std::string>()) : s.start;
    s.days = j.value("days", 3650);
    s.traders = j.value("traders", 120);
    s.max_traders = j.value("max_traders", 220);
    s.capital_min = j.value("capital_min", 1500.0);
    s.capital_max = j.value("capital_max", 5000.0);

    if (j.contains("gangs"))
        for (const auto& jg : j["gangs"]) {
            GangSeed g;
            g.hideout = find_node(world.nodes, jg.at("hideout").get<std::string>(), "gang seed");
            g.men = jg.value("men", 8);
            g.cells = jg.value("cells", 0);
            s.gangs.push_back(g);
        }

    if (j.contains("leaders"))
        for (const auto& jl : j["leaders"]) {
            LeaderSeed l;
            l.name = jl.at("name").get<std::string>();
            l.home = find_node(world.nodes, jl.at("home").get<std::string>(), "leader " + l.name);
            l.skill = jl.value("skill", 1.5);
            s.leaders.push_back(l);
        }

    if (j.contains("campaigns"))
        for (const auto& jc : j["campaigns"]) {
            CampaignDef c;
            c.name = jc.at("name").get<std::string>();
            c.from = parse_date(jc.at("from").get<std::string>());
            c.until = jc.contains("until") ? parse_date(jc["until"].get<std::string>()) : LONG_MAX;
            for (const auto& nid : jc.at("nodes")) c.nodes.push_back(find_node(world.nodes, nid.get<std::string>(), "campaign " + c.name));
            c.garrison_add = jc.at("garrison_add").get<double>();
            c.cause = jc.value("cause", "");
            s.campaigns.push_back(c);
        }

    if (j.contains("shocks"))
        for (const auto& js : j["shocks"]) {
            ShockDef sh;
            sh.name = js.value("name", "shock");
            sh.kind = js.at("kind").get<std::string>();
            sh.date = parse_date(js.at("date").get<std::string>());
            sh.cause = js.value("cause", "");
            if (sh.kind == "stock_loss") {
                sh.tag = js.at("tag").get<std::string>();
                sh.fraction = js.at("fraction").get<double>();
                for (const auto& gid : js.at("goods"))
                    sh.goods.push_back(find_good(world.goods, gid.get<std::string>(), "shock " + sh.name));
            } else if (sh.kind == "crop_failure") {
                sh.tag = js.at("zone").get<std::string>();
                sh.fraction = js.at("fraction").get<double>();
            } else if (sh.kind == "firm_withdrawal") {
                sh.fraction = js.value("fraction", 1.0);  // share of the eastern outfits that quit the road
            } else if (sh.kind == "house_collapse") {
                sh.fraction = js.value("fraction", 0.3);  // the largest freight house on the day loses its leadership; most of its drivers go independent
            } else if (sh.kind == "road_closure") {
                sh.days = js.at("days").get<int>();
                for (const auto& nid : js.at("nodes")) sh.nodes.push_back(find_node(world.nodes, nid.get<std::string>(), "shock " + sh.name));
                if (js.contains("modes")) {
                    for (bool& m : sh.modes) m = false;
                    for (const auto& m : js["modes"]) {
                        const std::string name = m.get<std::string>();
                        const int mi = name == "trail" ? 0 : name == "river" ? 1 : name == "rail" ? 2 : -1;
                        if (mi < 0) throw std::runtime_error("shock " + sh.name + ": unknown mode '" + name + "'");
                        sh.modes[mi] = true;
                    }
                }
            } else if (sh.kind == "rush") {
                sh.node = find_node(world.nodes, js.at("node").get<std::string>(), "shock " + sh.name);
                sh.people = js.at("people").get<double>();
                sh.cash_per_person = js.value("cash_per_person", 0.0);
            } else {
                throw std::runtime_error("shock " + sh.name + ": unknown kind '" + sh.kind + "'");
            }
            s.shocks.push_back(sh);
        }

    Params& p = s.params;
    const json pj = j.contains("params") ? j["params"] : json::object();
    // Any scalar parameter can be overridden by name (see Params in config.h).
    auto dbl = [&](const char* name, double& field) { if (pj.contains(name)) field = pj[name].get<double>(); };
    auto num = [&](const char* name, int& field) { if (pj.contains(name)) field = pj[name].get<int>(); };
#define P_D(f) dbl(#f, p.f)
#define P_I(f) num(#f, p.f)
    P_D(price_elasticity); P_D(price_min_mult); P_D(price_max_mult); P_D(price_max_mult_producer); P_D(cover_days);
    P_D(stock_cap_days); P_D(spread); P_D(market_recovery); P_D(mishap_loss_fraction);
    P_D(support_income_per_person); P_D(income_ema_alpha); P_D(pop_growth_rate);
    P_D(growth_cover_days); P_D(pop_decline_rate); P_D(hunger_alpha); P_D(hunter_share); P_D(hunter_catch); P_D(game_regrowth); P_D(game_inflow); P_D(deprivation_alpha); P_D(starvation_threshold); P_D(starvation_max_rate); P_D(crisis_death_share); P_I(crisis_end_days); P_D(famine_region_share); P_D(famine_region_death_rate); P_D(famine_region_min_civilians); P_D(hunger_threshold); P_D(floor_pop);
    P_D(ghost_pop); P_D(leaver_drifter_fraction); P_D(emigrant_cash_share); P_D(newcomer_provision_days); P_D(rush_provision_days); P_D(rush_stake_days); P_I(express_stages); P_D(express_interval_days); P_D(express_keep_days); P_D(express_min_value); P_D(transient_inflow_rate); P_I(camp_stages); P_D(camp_min_miles); P_D(camp_max_miles); P_D(camp_escape); P_D(camp_visit_chance); P_D(camp_visit_cash_per_man); P_D(camp_visit_spend); P_D(camp_careful_share); P_D(camp_careless_share); P_D(camp_trouble_careless); P_D(camp_trouble_ordinary); P_D(camp_trouble_careful); P_D(camp_trouble_outrage); P_D(camp_visit_catch); P_I(jail_stages); P_D(jail_petty_min); P_D(jail_petty_max); P_D(jail_job_min); P_D(jail_job_max); P_D(jail_repeat_mult); P_D(jail_cap_days); P_D(jail_marshal_cap); P_I(jail_hang_jobs); P_I(jail_hang_kills); P_D(jail_hang_chance); P_D(jail_hang_share); P_D(ex_con_fights); P_I(band_absorb_quiet); P_D(camp_heat_per_fame); P_D(camp_heat_bush); P_D(camp_heat_halflife); P_D(camp_heat_move); P_D(camp_notice_ref); P_D(camp_notice_floor); P_D(camp_far_miles); P_D(band_upkeep); P_D(band_petty_chance); P_D(band_petty_event); P_D(camp_found_catch); P_D(camp_fence_catch); P_I(crime_source); P_D(crime_source_scale); P_D(crime_income_ref); P_D(crime_income_floor); P_D(crime_law_none); P_D(crime_law_marshal); P_D(crime_law_sheriff);
    P_D(drifter_decay); P_D(news_paper_pop); P_I(bush_stages); P_D(bush_rate); P_D(bush_take); P_D(bush_resident); P_D(bush_kill); P_D(bush_outrage_resident); P_D(bush_outrage_stranger); P_D(bush_seen); P_D(bush_notable); P_D(bush_notable_outrage); P_D(army_range); P_D(army_skill); P_I(news_stages); P_I(news_paper_days); P_D(news_speed); P_D(news_range); P_I(news_keep); P_D(news_fame); P_D(news_fame_cap); P_I(fame_stages); P_I(posse_rout_pause); P_D(posse_base_skill); P_I(gang_large_men); P_D(fame_robbery); P_D(fame_toll); P_D(fame_kill); P_D(fame_fight); P_D(fame_decay); P_D(fame_posse_min); P_D(fame_catch_floor); P_D(fame_catch_cap); P_D(fame_flee); P_D(fame_flee_trigger); P_D(fame_size_ref); P_D(gang_flee_range); P_D(law_presence_none); P_D(law_presence_marshal); P_D(law_tax_rate); P_I(law_stages); P_D(law_marshal_pop); P_I(law_marshal_days); P_D(law_marshal_min_pop); P_D(law_marshal_outrage); P_D(law_sheriff_pop); P_I(law_sheriff_days); P_I(law_lapse_days); P_I(posse_max_men_marshal); P_D(vigilance_cost_per_man); P_D(visit_spend); P_D(garrison_pay_per_soldier); P_D(contract_backlog_days); P_D(quartermaster_share); P_I(quartermaster_interval_days); P_D(depot_floor_days); P_D(guard_visit_spend); P_D(visit_spend_value_rate); P_I(plan_stop_costs); P_I(books_stages);
    P_D(trader_capacity_lb); P_D(drover_fraction); P_D(trader_upkeep); P_D(guard_wage); P_I(guard_step);
    P_I(max_guards); P_D(trader_base_defenders); P_D(guard_strength); P_D(assumed_gang_men);
    P_D(risk_prior); P_D(risk_prior_weight); P_D(risk_ema_alpha); P_I(road_review_days); P_I(rail_stages); P_I(rail_review_days); P_D(rail_build_pop); P_D(rail_build_rate); P_I(rail_max_building); P_D(road_use_alpha); P_D(road_down_share); P_D(road_up_wagon); P_D(road_up_graded); P_D(road_speed_wagon); P_D(road_speed_graded); P_D(road_freight_wagon); P_D(road_freight_graded); P_D(fear_bump);
    P_D(fear_decay); P_D(risk_aversion_min); P_D(risk_aversion_max); P_D(firm_risk_aversion_min); P_D(firm_risk_aversion_max); P_D(risk_aversion_bump);
    P_D(risk_aversion_cap); P_D(risk_aversion_decay); P_D(info_noise); P_D(inbound_awareness); P_D(min_profit);
    P_D(avail_fraction); P_D(local_reserve_days); P_D(deadhead_discount); P_D(lookahead_weight); P_I(merchant_interval_days); P_D(merchant_min_pop); P_D(merchant_reorder_days); P_D(merchant_safety_days); P_D(merchant_cycle_days); P_I(trade_horizon); P_D(winter_cover_days); P_D(winter_min_y); P_I(winter_lay_in_month); P_D(merchant_cash_share); P_D(merchant_margin); P_D(contract_share); P_D(contract_premium); P_I(contract_memory_days); P_I(contract_term_days); P_I(contract_max_behind_days); P_I(carrier_jobs); P_D(job_preference); P_D(job_rate_share); P_D(owner_haul_share); 
    P_I(merchant_order_raise_days); P_D(merchant_order_raise); P_D(merchant_order_cap_mult); P_I(merchant_order_expire_days); P_I(merchant_hire_after_days); P_I(lead_stages); P_D(lead_prior_mult); P_D(lead_dev_prior); P_D(lead_prior_weight); P_D(lead_safety_k); P_D(lead_obs_alpha); P_D(growth_cap_mult); P_D(trader_kill_prob);
    P_I(firms_initial); P_D(firm_capital_min); P_D(firm_capital_max); P_D(firm_capacity_lb); P_D(firm_upkeep);
    P_D(firm_drover_fraction); P_D(freelancer_entry_rate); P_D(story_profit_threshold); P_D(story_ref); P_D(story_alpha); P_D(story_fade); P_D(entry_scarcity_weight); P_D(scarcity_ref); P_D(scarcity_alpha); P_D(scarcity_memory); P_D(entry_danger_floor);
    P_D(firm_entry_rate); P_D(firm_hurdle); P_D(return_alpha); P_D(danger_ref); P_D(danger_alpha); P_D(settle_cash);
    P_D(settle_chance); P_D(settle_ref_cash); P_D(settle_chance_cap); P_D(goal_min); P_D(goal_max); P_D(patience_min_days); P_D(patience_max_days); P_D(income_half_life); P_D(house_min_income); P_I(house_small_outfits); P_D(house_retire_chance); P_D(house_retire_share);
    P_D(house_float); P_D(house_found_cash); P_D(house_found_chance); P_D(house_nerve); P_I(house_max_outfits); P_I(house_hire_days); P_D(settle_invest_share); P_D(firm_withdraw_rate); P_D(price_uncertainty_weight);
    P_D(spot_uncertainty_base); P_D(spot_uncertainty_thin);
    P_D(gang_encounter_prob); P_D(gang_upkeep); P_D(gang_recruit_cost); P_I(gang_min_men); P_I(gang_max_men);
    P_D(gang_form_min_drifters); P_D(gang_form_chance); P_I(gang_review_days);
    P_D(gang_range_miles); P_D(fence_rate); P_D(fence_cash_share);
    P_D(gang_loss_when_repulsed);
    P_I(gang_wars); P_D(gang_manageable_base); P_D(gang_veteran_fights); P_D(gang_veteran_hold); P_D(gang_veteran_strength);
    P_D(gang_breakdown_rate); P_D(gang_breakdown_loss); P_I(gang_hard_max); P_D(gang_casualty_random);
    P_D(gang_rival_fight_chance); P_D(gang_rival_loser_loss); P_D(gang_rival_winner_loss); P_D(gang_rival_loot_share);
    P_D(gang_boss_fall_chance); P_D(gang_merge_chance); P_D(gang_win_competence); P_D(gang_hardness);
    P_I(gang_tiers); P_D(fighter_turn_rate); P_D(fighter_decay); P_D(band_form_chance); P_I(band_min_men); P_I(band_max_men);
    P_D(band_catch_base); P_I(band_merge_min_robberies); P_D(band_join_chance); P_D(band_join_range); P_D(gang_hire_below); P_I(gang_party_max); P_D(band_min_odds); P_D(band_petty_take); P_D(band_petty_catch); P_D(band_law_cap); P_D(law_overmatch);
    P_D(gang_carry_lb_per_man); P_D(gang_head_per_man); P_D(gang_min_take_per_man);
    P_D(gang_desperate_days); P_D(gang_desperate_floor_mult);
    P_D(interest_loss_decay); P_D(agency_loss_threshold); P_D(agency_min_fund); P_D(agency_cost_per_man);
    P_I(agency_min_men); P_I(agency_max_men); P_D(agency_skill); P_I(agency_cooldown); P_D(backup_range);
    P_D(capture_gain); P_D(capture_decay); P_D(capture_threshold); P_I(capture_min_men); P_D(capture_law_resist);
    P_D(tribute_base_min); P_D(tribute_spread); P_D(tribute_squeeze); P_D(tribute_cap); P_D(infight_rate);
    P_D(vigilance_rate); P_I(vigilance_min_days); P_D(vigilance_min_pop); P_D(vigilance_pop_per_man); P_D(law_deter_scale);
    P_D(outrage_per_robbery); P_D(outrage_decay); P_D(posse_outrage_threshold);
    P_D(posse_cost_per_man); P_I(posse_min_men); P_I(posse_max_men); P_D(posse_speed);
    P_I(posse_cooldown); P_I(posse_min_men_led); P_D(leader_fee_per_day); P_D(leader_bonus_share);
    P_D(leader_range); P_D(leader_death_prob); P_D(leader_skill_gain); P_D(leader_skill_cap); P_D(leader_emerge_chance); P_D(posse_range); P_D(gang_terrain_advantage);
    P_D(farm_surplus_per_farmer); P_I(farm_harvest_start); P_I(farm_harvest_days); P_D(farm_release_rate);
    P_D(farm_forage_ratio); P_D(farm_break_even_share); P_D(farm_marketing_share); P_D(farm_outlet_units); P_D(farm_weather_sd); P_D(range_weather); P_D(food_cost_mult); P_D(class_wild_share); P_D(class_exit_share); P_D(class_wild_move);
    P_D(farm_settle_base); P_D(farm_settle_growth); P_D(farm_leave_rate); P_D(farm_core_share); P_D(farm_gate_cap); P_D(farm_gate_alpha); P_D(found_gate_ratio);
    P_I(found_months); P_D(found_cash_per_person); P_D(farm_pace_start); P_D(farm_pace_end); P_D(ranch_pace_start); P_D(ranch_pace_end);
#undef P_D
#undef P_I
    // Per-mode arrays.
    for (int m = 0; m < kModeCount; ++m) {
        const std::string name = mode_name(static_cast<Mode>(m));
        if (pj.contains("speed_" + name)) p.speed[m] = pj["speed_" + name].get<double>();
        if (pj.contains("freight_rate_" + name)) p.freight_rate[m] = pj["freight_rate_" + name].get<double>();
        if (pj.contains("hazard_" + name)) p.hazard[m] = pj["hazard_" + name].get<double>();
    }
    return s;
}

}  // namespace frontier
