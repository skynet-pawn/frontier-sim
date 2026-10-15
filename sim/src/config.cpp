#include "frontier/config.h"

#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>

#include "frontier/date.h"
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
        g.min_target = jg.value("min_target", 10.0);
        g.market_depth = jg.value("market_depth", 0.0);
        g.garrison_need = jg.value("garrison_need", 0.0);
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
            e.miles = std::hypot(na.x - nb.x, na.y - nb.y) * terrain[mode_name(e.mode)];
        }
        e.hazard = je.value("hazard", -1.0);
        e.from = date_or(je, "from", LONG_MIN);
        e.until = date_or(je, "until", LONG_MAX);
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
    P_D(pop_decline_rate); P_D(hunger_alpha); P_D(hunger_threshold); P_D(floor_pop);
    P_D(ghost_pop); P_D(leaver_drifter_fraction); P_D(emigrant_cash_share); P_D(newcomer_provision_days); P_D(transient_inflow_rate);
    P_D(drifter_decay); P_D(law_tax_rate); P_D(visit_spend); P_D(garrison_pay_per_soldier); P_D(contract_backlog_days); P_D(quartermaster_share); P_I(quartermaster_interval_days); P_D(guard_visit_spend); P_D(visit_spend_value_rate);
    P_D(trader_capacity_lb); P_D(drover_fraction); P_D(trader_upkeep); P_D(guard_wage); P_I(guard_step);
    P_I(max_guards); P_D(trader_base_defenders); P_D(guard_strength); P_D(assumed_gang_men);
    P_D(risk_prior); P_D(risk_prior_weight); P_D(risk_ema_alpha); P_D(fear_bump);
    P_D(fear_decay); P_D(risk_aversion_min); P_D(risk_aversion_max); P_D(risk_aversion_bump);
    P_D(risk_aversion_cap); P_D(risk_aversion_decay); P_D(info_noise); P_D(min_profit);
    P_D(avail_fraction); P_D(deadhead_discount); P_D(trader_join_rate);
    P_D(trader_join_opportunity_ref); P_D(trader_kill_prob);
    P_D(gang_encounter_prob); P_D(gang_upkeep); P_D(gang_recruit_cost); P_I(gang_min_men); P_I(gang_max_men);
    P_D(gang_form_min_drifters); P_D(gang_form_chance); P_I(gang_review_days);
    P_D(gang_range_miles); P_D(fence_rate); P_D(fence_cash_share);
    P_D(gang_loss_when_repulsed);
    P_D(interest_loss_decay); P_D(agency_loss_threshold); P_D(agency_min_fund); P_D(agency_cost_per_man);
    P_I(agency_min_men); P_I(agency_max_men); P_D(agency_skill); P_I(agency_cooldown); P_D(backup_range);
    P_D(capture_gain); P_D(capture_decay); P_D(capture_threshold); P_I(capture_min_men); P_D(capture_law_resist);
    P_D(tribute_base_min); P_D(tribute_spread); P_D(tribute_squeeze); P_D(tribute_cap); P_D(infight_rate);
    P_D(vigilance_rate); P_I(vigilance_min_days); P_D(vigilance_min_pop); P_D(vigilance_pop_per_man); P_D(law_deter_scale);
    P_D(outrage_per_robbery); P_D(outrage_decay); P_D(posse_outrage_threshold);
    P_D(posse_cost_per_man); P_I(posse_min_men); P_I(posse_max_men); P_D(posse_speed);
    P_I(posse_cooldown); P_I(posse_min_men_led); P_D(leader_fee_per_day); P_D(leader_bonus_share);
    P_D(leader_range); P_D(leader_death_prob); P_D(leader_skill_gain); P_D(leader_skill_cap); P_D(leader_emerge_chance); P_D(posse_range); P_D(gang_terrain_advantage);
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
