// Frontier Sim viewer: a top-down map of the running simulation.
//
// The viewer reads the sim through const accessors and the event log. The one exception is play mode (G on a town),
// which drives a single outfit through the public action API (act_buy, act_travel, act_sell), exactly as the AI traders
// do, so another presentation layer can replace this file without touching sim/. The map is drawn in the sim's own
// coordinates (miles, x east, y north).
//
//   frontier_viewer [--data DIR] [--seed N] [--load SNAPSHOT] [--play TOWN] [--shot FILE --days N]
//   --load starts from a state saved by frontier_cli --save (same data folder), so a pre-simulated world opens at once.
//   If DIR is data/slices/NAME and data/terrain/NAME exists, a shaded-relief terrain map is drawn under the towns (T toggles).
//   space pause/resume   1-5 speed   N one day (paused)   tab overlay   F fit map   click a town or a wagon   wheel zoom
//   right-drag or middle-drag pan   T terrain   C carrier filter   L loaded wagons only   X let go of a followed wagon
//   click a headline to jump to its town   P screenshot   B price board   Esc quit
//   Play mode: select a town, G starts you there as a freight wagon (shift-G as a drover). Pick a good, click a destination
//   town on the map, hire guards, set off. The world waits while you decide and runs while you travel; W waits a day.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "raylib.h"

#include "frontier/date.h"
#include "frontier/simulation.h"

using namespace frontier;

namespace {

std::string find_data_dir() {
    for (const char* c : {"data", "../data", "../../data", "../../../data", "../../../../data"}) {
        std::ifstream f(std::string(c) + "/goods.json");
        if (f) return c;
    }
    return "data";
}

// Camera: where the map is centred (in miles) and how many pixels a mile is.
struct Camera2 {
    float cx = 0, cy = 0, zoom = 1;
    Vector2 to_screen(float x, float y) const {
        return {GetScreenWidth() * 0.5f + (x - cx) * zoom, GetScreenHeight() * 0.5f - (y - cy) * zoom};
    }
    Vector2 to_world(Vector2 s) const {
        return {cx + (s.x - GetScreenWidth() * 0.5f) / zoom, cy - (s.y - GetScreenHeight() * 0.5f) / zoom};
    }
};

void fit(Camera2& cam, const World& w) {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const Node& n : w.nodes) {
        x0 = std::min(x0, static_cast<float>(n.x)); x1 = std::max(x1, static_cast<float>(n.x));
        y0 = std::min(y0, static_cast<float>(n.y)); y1 = std::max(y1, static_cast<float>(n.y));
    }
    cam.cx = (x0 + x1) / 2;
    cam.cy = (y0 + y1) / 2;
    cam.zoom = std::min((GetScreenWidth() - 360.0f) / std::max(1.0f, x1 - x0 + 120), (GetScreenHeight() - 120.0f) / std::max(1.0f, y1 - y0 + 120));
}

Color lerp(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {static_cast<unsigned char>(a.r + (b.r - a.r) * t), static_cast<unsigned char>(a.g + (b.g - a.g) * t),
            static_cast<unsigned char>(a.b + (b.b - a.b) * t), 255};
}

Color kind_color(const Node& n) {
    if (n.ghost) return {120, 120, 120, 255};
    const std::string& k = n.kind;
    if (k == "market") return {60, 90, 170, 255};
    if (k == "fort") return {40, 110, 60, 255};
    if (k == "mining camp" || k == "mining town") return {200, 160, 30, 255};
    if (k == "railhead") return {140, 70, 40, 255};
    if (k == "river port") return {40, 140, 170, 255};
    if (k == "farm town" || k == "farm district") return {120, 170, 70, 255};
    if (k == "ranch town" || k == "range") return {170, 130, 80, 255};
    return {110, 90, 70, 255};
}

Color mode_color(Mode m) {
    switch (m) {
        case Mode::Rail:  return {60, 60, 70, 255};
        case Mode::River: return {80, 140, 200, 255};
        default:          return {150, 125, 90, 255};
    }
}

const char* overlay_name(int o) { return o == 0 ? "towns by kind" : o == 1 ? "hunger" : "flour price"; }

GoodId find_good(const World& w, const char* id) {
    for (std::size_t g = 0; g < w.goods.size(); ++g)
        if (w.goods[g].id == id) return static_cast<GoodId>(g);
    return -1;
}

Color node_color(const World& w, std::size_t i, int overlay, GoodId flour) {
    const Node& n = w.nodes[i];
    if (overlay == 1 && !n.fixed_market) return lerp({70, 160, 70, 255}, {200, 40, 30, 255}, static_cast<float>(n.hunger) * 3.0f);
    if (overlay == 2 && !n.fixed_market && flour >= 0) {
        // Price against the good's balanced price on a log scale: green is cheap, red is dear.
        const double ratio = w.mid_price(static_cast<NodeId>(i), flour) / std::max(1e-9, w.goods[static_cast<std::size_t>(flour)].base_cost);
        return lerp({70, 160, 70, 255}, {200, 40, 30, 255}, static_cast<float>(std::log2(std::max(0.25, ratio)) / 4.0 + 0.25));
    }
    return kind_color(n);
}

// A point `f` (0 to 1) of the way along an edge, starting from town `from`, following the route over the terrain when the sim
// gave one and a straight line otherwise.
Vector2 point_along(const World& w, const Edge& e, NodeId from, double f) {
    const Node& a = w.nodes[World::idx(e.a)];
    const Node& b = w.nodes[World::idx(e.b)];
    f = std::clamp(f, 0.0, 1.0);
    if (from == e.b) f = 1.0 - f;   // the stored way runs from a to b
    if (e.path.size() < 2) return {static_cast<float>(a.x + (b.x - a.x) * f), static_cast<float>(a.y + (b.y - a.y) * f)};
    double total = 0;
    for (std::size_t i = 1; i < e.path.size(); ++i) total += std::hypot(e.path[i].first - e.path[i - 1].first, e.path[i].second - e.path[i - 1].second);
    double want = f * total;
    for (std::size_t i = 1; i < e.path.size(); ++i) {
        const double seg = std::hypot(e.path[i].first - e.path[i - 1].first, e.path[i].second - e.path[i - 1].second);
        if (want <= seg || i + 1 == e.path.size()) {
            const double t = seg > 0 ? std::clamp(want / seg, 0.0, 1.0) : 0.0;
            return {static_cast<float>(e.path[i - 1].first + (e.path[i].first - e.path[i - 1].first) * t),
                    static_cast<float>(e.path[i - 1].second + (e.path[i].second - e.path[i - 1].second) * t)};
        }
        want -= seg;
    }
    return {static_cast<float>(b.x), static_cast<float>(b.y)};
}

// Where a wagon train is on its road, interpolated along the current leg. False if it is standing in a town.
bool trader_position(const World& w, const Trader& t, Vector2& out) {
    if (!t.travelling || t.leg >= t.route.size()) return false;
    const Edge& e = w.edges[World::idx(t.route[t.leg])];
    const double f = std::clamp(1.0 - t.days_left / std::max(0.1, e.travel_days), 0.0, 1.0);
    out = point_along(w, e, t.at, f);
    return true;
}

Color trader_color(const Trader& t) {
    if (t.job_town >= 0) return {230, 130, 20, 255};   // hauling a supplier's goods on a freight job
    if (t.firm) return {40, 80, 200, 255};              // an eastern firm's outfit
    if (t.house >= 0) return {30, 150, 70, 255};        // a freight house's outfit
    return {40, 40, 40, 255};                           // a freelancer
}

// Who runs the wagon, in the same priority order as the colors: 0 freelancer, 1 house, 2 eastern firm, 3 freight job.
int carrier_kind(const Trader& t) {
    if (t.job_town >= 0) return 3;
    if (t.firm) return 2;
    if (t.house >= 0) return 1;
    return 0;
}
const char* const kCarrierNames[] = {"freelancer", "house outfit", "eastern firm", "freight job"};

// Filter state: which carriers show at full strength; the rest fade so one flow stands out.
struct Filter {
    int carrier = -1;        // -1 all, else carrier_kind
    bool loaded_only = false;
    bool shows(const Trader& t) const {
        if (carrier >= 0 && carrier_kind(t) != carrier) return false;
        return !loaded_only || t.cargo >= 0;
    }
};

// Plain-language headlines: the big stories out of the event log. Everything else stays in the raw feed. Posse
// raising and gang break-ups are left out: they repeat every few days and would drown the town stories.
bool is_headline(const Event& e) {
    static const char* const kinds[] = {"famine", "famine_over", "ghost_town", "town_founded", "gang_takeover", "town_freed", "shortage", "gang_war", "gang_merged",
                                        "leader_killed", "house_founded",
                                        "house_closed", "route_changed", "reserve_exhausted", "output_begins", "rush", "road_closure",
                                        "house_collapse", "firm_withdrawal", "road_improved", "road_worn", "trader_settled", "crop_failure", "vigilance_formed", "repopulating", "law_established", "bushwhack_notable", "cavalry_sent"};
    for (const char* k : kinds) if (e.kind == k) return true;
    return false;
}

// Break text into lines no wider than `width` pixels.
std::vector<std::string> wrap(const std::string& s, int size, int width) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        const std::string t = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && MeasureText(t.c_str(), size) > width) { lines.push_back(cur); cur = word; }
        else cur = t;
        word.clear();
    };
    for (char c : s) { if (c == ' ') flush_word(); else word += c; }
    flush_word();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

Color event_color(EventType t) {
    switch (t) {
        case EventType::Famine:     return {170, 30, 30, 255};
        case EventType::Robbery:
        case EventType::Gang:       return {200, 60, 20, 255};
        case EventType::Law:        return {30, 80, 160, 255};
        case EventType::Shock:      return {140, 30, 140, 255};
        case EventType::Population: return {60, 110, 60, 255};
        case EventType::Network:    return {90, 90, 90, 255};
        default:                    return {60, 50, 40, 255};
    }
}

void panel(int x, int y, int w, int h) {
    DrawRectangle(x, y, w, h, {250, 244, 228, 230});
    DrawRectangleLines(x, y, w, h, {120, 100, 70, 255});
}

void draw_town_panel(const Simulation& sim, std::size_t i) {
    const World& w = sim.world();
    const Node& n = w.nodes[i];
    const int x = GetScreenWidth() - 330, y = 40;
    panel(x, y, 320, 560);
    int ty = y + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}) { DrawText(s.c_str(), x + 10, ty, 15, c); ty += 19; };
    line(n.name + "  (" + n.kind + ")", {20, 20, 20, 255});
    if (n.fixed_market) { line("an eastern market: prices are fixed"); return; }
    char b[160];
    std::snprintf(b, sizeof b, "population %.0f   cash $%.0f", n.population, n.cash);
    line(b);
    std::snprintf(b, sizeof b, "hunger %.2f%s%s", n.hunger, n.starving ? "   SHORTAGE" : "", n.ghost ? "   ghost town" : "");
    line(b, n.starving ? Color{170, 30, 30, 255} : Color{40, 30, 20, 255});
    if (n.garrison > 0) { std::snprintf(b, sizeof b, "garrison %.0f", n.garrison); line(b); }
    {
        static const char* const kLaw[] = {"no law (a miners' meeting)", "town marshal", "county sheriff and deputies"};
        const std::string region = n.region >= 0 ? w.region_names[static_cast<std::size_t>(n.region)] : std::string("-");
        std::snprintf(b, sizeof b, "%s  |  law: %s%s", region.c_str(), kLaw[std::clamp(n.law, 0, 2)], n.paper ? "  |  paper" : "");
        line(b, n.law == 0 ? Color{170, 90, 20, 255} : Color{40, 30, 20, 255});
    }
    if (n.captor >= 0) { std::snprintf(b, sizeof b, "ruled by gang #%d (takes %.0f%%)", n.captor, n.toll * 100); line(b, {170, 30, 30, 255}); }
    ty += 4;
    line("good        shelf     price   on order", {90, 70, 40, 255});
    for (const char* id : {"flour", "beef_cattle", "tools", "whiskey", "ammunition", "gold"}) {
        const GoodId g = find_good(w, id);
        if (g < 0) continue;
        const std::size_t gi = static_cast<std::size_t>(g);
        char order[40] = "";
        if (n.order_qty[gi] >= 1) std::snprintf(order, sizeof order, "%.0f @ $%.2f", n.order_qty[gi], n.order_price[gi]);
        std::snprintf(b, sizeof b, "%-11s %7.0f  $%6.2f   %s", w.goods[gi].name.substr(0, 11).c_str(), n.stock[gi], w.mid_price(static_cast<NodeId>(i), g), order);
        line(b);
    }
    // Outlaws who trade here and where they really live.
    {
        int bands = 0, big = 0, shown = 0;
        for (const Gang& g : sim.gangs()) {
            if (!g.active || g.hideout != static_cast<NodeId>(i)) continue;
            (sim.is_band_gang(g) ? bands : big) += 1;
        }
        if (bands + big > 0) {
            std::snprintf(b, sizeof b, "outlaws trading here: %d band%s, %d large gang%s", bands, bands == 1 ? "" : "s", big, big == 1 ? "" : "s");
            line(b, {150, 60, 20, 255});
            for (const Gang& g : sim.gangs()) {
                if (!g.active || g.hideout != static_cast<NodeId>(i) || g.camp_kind < 0 || shown >= 3) continue;
                std::snprintf(b, sizeof b, "  #%d: %d men, camp in a %s, %.0f mi out", g.id, g.men, Simulation::camp_kind_name(g.camp_kind), g.camp_miles);
                line(b, {110, 70, 30, 255});
                ++shown;
            }
        }
    }
    // The town's paper: what it has printed lately, newest first (headlines travel, so this includes news from elsewhere).
    ty += 6;
    const auto& shelf = sim.paper_headlines(static_cast<NodeId>(i));
    if (n.paper) {
        line(n.name + " paper", {90, 70, 40, 255});
        int shown = 0;
        for (std::size_t k = shelf.size(); k-- > 0 && shown < 7; ++shown) {
            const auto lines = wrap(format_date(shelf[k]->day) + "  " + shelf[k]->text, 12, 300);
            for (const std::string& l : lines) { DrawText(l.c_str(), x + 10, ty, 12, {40, 30, 20, 255}); ty += 14; }
            ty += 2;
        }
    } else {
        line("no newspaper here", {120, 100, 70, 255});
    }
}

// ---------------------------------------------------------------- play mode: one outfit driven by the player

struct Player {
    int id = -1;
    bool drover = false;
    NodeId dest = -1;
    int guards = 0;
    GoodId sel = -1;
    double start_worth = 0;
    long start_day = 0;
    std::string msg;
    bool dead = false;
    bool active() const { return id >= 0; }
    static Rectangle rect() { return {10, 312, 370, 340}; }
};

Trader* player_trader(Simulation& sim, const Player& p) { return p.active() ? sim.find_trader(p.id) : nullptr; }

double worth_of(const Trader& t) { return t.cash + (t.cargo >= 0 ? t.qty * t.unit_cost : 0.0); }

void player_spawn(Simulation& sim, Player& p, NodeId at, bool drover) {
    p = Player{};
    p.id = sim.add_trader(at, 3000, drover, /*controlled=*/true);
    p.drover = drover;
    p.start_day = sim.world().today;
    if (const Trader* t = sim.find_trader(p.id)) p.start_worth = worth_of(*t);
    p.msg = "You have $3,000 and an empty wagon. Pick a good, click a town to haul it to.";
}

// A hold: the world waits while the player is in a town with a decision to make.
bool player_holding(Simulation& sim, const Player& p) {
    const Trader* t = player_trader(sim, p);
    return t && !t->travelling;
}

struct RouteInfo {
    bool ok = false;
    double days = 0, miles[kModeCount] = {0, 0, 0}, robbed = 0;   // robbed: the chance a wagon is robbed on the way, from the rumor on each road
};

RouteInfo route_info(const World& w, NodeId from, NodeId to) {
    RouteInfo r;
    std::vector<EdgeId> path;
    if (from == to || !w.route(from, to, path)) return r;
    r.ok = true;
    double safe = 1.0;
    for (EdgeId id : path) {
        const Edge& e = w.edges[World::idx(id)];
        r.days += e.travel_days;
        r.miles[static_cast<int>(e.mode)] += e.freight_miles;
        safe *= 1.0 - std::min(0.5, e.robbed_ema / std::max(e.transits_ema, 0.05)) * e.travel_days;
    }
    r.robbed = 1.0 - std::max(0.0, safe);
    return r;
}

bool button(Rectangle r, const char* label, bool enabled = true) {
    const bool hot = enabled && CheckCollisionPointRec(GetMousePosition(), r);
    DrawRectangleRec(r, !enabled ? Color{225, 215, 195, 255} : hot ? Color{235, 215, 165, 255} : Color{240, 228, 195, 255});
    DrawRectangleLinesEx(r, 1, enabled ? Color{120, 100, 70, 255} : Color{190, 180, 160, 255});
    DrawText(label, static_cast<int>(r.x) + 6, static_cast<int>(r.y) + 4, 13, enabled ? Color{40, 30, 20, 255} : Color{150, 140, 125, 255});
    return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// Draws the player's panel and carries out what he clicks. Sets `wait` when he asks to let a day pass.
void draw_player_panel(Simulation& sim, Player& p, bool& wait) {
    const World& w = sim.world();
    const Rectangle R = Player::rect();
    panel(static_cast<int>(R.x), static_cast<int>(R.y), static_cast<int>(R.width), static_cast<int>(R.height));
    const int x = static_cast<int>(R.x) + 10;
    int ty = static_cast<int>(R.y) + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}, int size = 14) { DrawText(s.c_str(), x, ty, size, c); ty += size + 4; };
    Trader* t = player_trader(sim, p);
    if (!t) {
        p.dead = true;
        line("YOUR OUTFIT IS FINISHED", {170, 30, 30, 255}, 16);
        line("(robbed, broke, or worn out)");
        for (const std::string& l : wrap(p.msg, 13, 340)) line(l, {90, 70, 40, 255}, 13);
        line("Select a town and press G to start again.", {90, 70, 40, 255}, 13);
        return;
    }
    char b[200];
    std::snprintf(b, sizeof b, "YOU: %s #%d   day %ld of your trade", p.drover ? "drover" : "freight wagon", t->id, w.today - p.start_day);
    line(b, {20, 20, 20, 255}, 15);
    std::snprintf(b, sizeof b, "cash $%.0f   net worth $%.0f   (started $%.0f)", t->cash, worth_of(*t), p.start_worth);
    line(b, worth_of(*t) >= p.start_worth ? Color{30, 110, 50, 255} : Color{170, 30, 30, 255});
    for (const std::string& l : wrap(p.msg, 13, 345)) line(l, {90, 70, 40, 255}, 13);
    ty += 2;

    if (t->travelling) {
        const NodeId dest = t->dest;
        std::snprintf(b, sizeof b, "on the road to %s: leg %zu of %zu, %.0f days left on it", dest >= 0 ? w.nodes[World::idx(dest)].name.c_str() : "?", t->leg + 1, t->route.size(), static_cast<double>(t->days_left));
        line(b);
        if (t->cargo >= 0) { std::snprintf(b, sizeof b, "carrying %.0f %s, guards %d", t->qty, w.goods[World::idx(t->cargo)].name.c_str(), t->guards); line(b); }
        line("(use 1-5 to speed up the trip)", {110, 100, 90, 255}, 12);
        return;
    }

    const Node& here = w.nodes[World::idx(t->at)];
    line(std::string("in ") + here.name, {20, 20, 20, 255}, 15);
    const RouteInfo ri = p.dest >= 0 ? route_info(w, t->at, p.dest) : RouteInfo{};
    if (p.dest >= 0 && p.dest != t->at) {
        if (ri.ok) {
            std::snprintf(b, sizeof b, "bound for %s: %.0f days, about %.0f%% of wagons robbed on this road", w.nodes[World::idx(p.dest)].name.c_str(), ri.days, ri.robbed * 100);
            line(b, ri.robbed > 0.25 ? Color{170, 30, 30, 255} : Color{40, 30, 20, 255}, 13);
        } else line(std::string("no road to ") + w.nodes[World::idx(p.dest)].name, {170, 30, 30, 255});
    } else line("click a town on the map to choose where to haul", {110, 100, 90, 255}, 13);

    if (t->cargo >= 0) {
        std::snprintf(b, sizeof b, "carrying %.0f %s (cost $%.2f a unit)", t->qty, w.goods[World::idx(t->cargo)].name.c_str(), t->unit_cost);
        line(b);
        std::snprintf(b, sizeof b, "here a unit sells for about $%.2f", w.quote_sell(t->at, t->cargo, t->qty) / std::max(1.0, t->qty));
        line(b);
        if (button({static_cast<float>(x), static_cast<float>(ty), 150, 22}, "Sell it here")) {
            const Receipt r = sim.act_sell(*t);
            if (r.result == ActionResult::Ok) std::snprintf(b, sizeof b, "Sold %.0f units for $%.0f.", r.units, r.money);
            else std::snprintf(b, sizeof b, "Could not sell (%s).", action_result_name(r.result));
            p.msg = b;
        }
        ty += 28;
    } else {
        line("good               price   shelf   est. profit", {90, 70, 40, 255}, 13);
        for (std::size_t gi = 0; gi < w.goods.size(); ++gi) {
            const GoodDef& g = w.goods[gi];
            const double stock = here.fixed_market ? 1e9 : here.stock[gi];   // an eastern market never runs out
            if (g.self_propelled != p.drover || stock < 1) continue;
            if (ty > static_cast<int>(R.y + R.height) - 90) break;
            const double units = std::min(t->capacity_lb / g.load_lb, stock);
            const double cost = w.quote_buy(t->at, static_cast<GoodId>(gi), units);
            std::string est = "-";
            if (ri.ok) {
                const double freight = w.freight_unit_cost(static_cast<GoodId>(gi), ri.miles) * units;
                const double sale = w.quote_sell(p.dest, static_cast<GoodId>(gi), units);
                char e[32];
                std::snprintf(e, sizeof e, "%+.0f", sale - cost - freight);
                est = e;
            }
            char shelf[16];
            if (here.fixed_market) std::snprintf(shelf, sizeof shelf, "plenty"); else std::snprintf(shelf, sizeof shelf, "%.0f", stock);
            std::snprintf(b, sizeof b, "%-16s %6.2f %7s   %s", g.name.substr(0, 16).c_str(), cost / std::max(1.0, units), shelf, est.c_str());
            const Rectangle row{static_cast<float>(x - 4), static_cast<float>(ty - 1), R.width - 12, 17};
            const bool sel = p.sel == static_cast<GoodId>(gi);
            if (sel) DrawRectangleRec(row, {230, 210, 160, 255});
            else if (CheckCollisionPointRec(GetMousePosition(), row)) DrawRectangleRec(row, {240, 228, 200, 255});
            if (CheckCollisionPointRec(GetMousePosition(), row) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) p.sel = static_cast<GoodId>(gi);
            DrawText(b, x, ty, 13, {40, 30, 20, 255});
            ty += 17;
        }
        if (button({static_cast<float>(x), static_cast<float>(ty + 4), 150, 22}, "Buy a full load", p.sel >= 0)) {
            // Keep back the freight, the guards and a little spending money.
            double reserve = 100;
            if (ri.ok) reserve += w.freight_unit_cost(p.sel, ri.miles) * t->capacity_lb / w.goods[World::idx(p.sel)].load_lb + w.params.guard_wage * p.guards * ri.days;
            const Receipt r = sim.act_buy(*t, p.sel, 1e9, std::max(0.0, t->cash - reserve));
            if (r.result == ActionResult::Ok) std::snprintf(b, sizeof b, "Bought %.0f units for $%.0f.", r.units, r.money);
            else std::snprintf(b, sizeof b, "Could not buy (%s).", action_result_name(r.result));
            p.msg = b;
        }
        ty += 30;
    }
    // Guards, then off.
    std::snprintf(b, sizeof b, "guards: %d  ($%.1f a day)", p.guards, w.params.guard_wage * p.guards);
    DrawText(b, x, ty + 3, 13, {40, 30, 20, 255});
    if (button({static_cast<float>(x + 190), static_cast<float>(ty), 26, 22}, "-", p.guards > 0)) p.guards = std::max(0, p.guards - w.params.guard_step);
    if (button({static_cast<float>(x + 222), static_cast<float>(ty), 26, 22}, "+", p.guards < w.params.max_guards)) p.guards = std::min(w.params.max_guards, p.guards + w.params.guard_step);
    ty += 28;
    if (button({static_cast<float>(x), static_cast<float>(ty), 150, 24}, "Set off", ri.ok && p.dest != t->at)) {
        const ActionResult r = sim.act_travel(*t, p.dest, p.guards);
        p.msg = r == ActionResult::Ok ? std::string("On the road.") : std::string("Cannot set off (") + action_result_name(r) + ").";
    }
    if (button({static_cast<float>(x + 160), static_cast<float>(ty), 150, 24}, "Wait a day (W)")) wait = true;
}

// What one wagon train is doing. The sim does not store why a trader picked a road, so this shows what it carries,
// where it is bound, and the temperament and fear that weigh on that choice.
// A price board: the biggest towns with what a pound of flour and a head of beef cost there, so a shortage or a glut can be
// read across the map. Prices are shown against the East's price, since distance and risk are what make the gap.
void draw_price_board(const World& w) {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < w.nodes.size(); ++i)
        if (!w.nodes[i].fixed_market && w.nodes[i].founded && !w.nodes[i].ghost) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return w.nodes[a].population > w.nodes[b].population; });
    if (order.size() > 16) order.resize(16);
    const GoodId flour = find_good(w, "flour"), beef = find_good(w, "beef_cattle");
    const int x = 210, y = 40, rows = static_cast<int>(order.size());
    panel(x, y, 430, 40 + rows * 17);
    DrawText("PRICE BOARD (B)      town         pop    flour   beef  hunger", x + 8, y + 6, 13, {90, 70, 40, 255});
    int ty = y + 26;
    for (std::size_t i : order) {
        const Node& n = w.nodes[i];
        char b[160];
        std::snprintf(b, sizeof b, "%-18s %7.0f  %6.2f %7.2f   %.2f", n.name.substr(0, 18).c_str(), n.population,
                      flour >= 0 ? w.mid_price(static_cast<NodeId>(i), flour) : 0.0, beef >= 0 ? w.mid_price(static_cast<NodeId>(i), beef) : 0.0, n.hunger);
        DrawText(b, x + 8, ty, 13, n.starving ? Color{170, 30, 30, 255} : Color{40, 30, 20, 255});
        ty += 17;
    }
}

void draw_trader_panel(const World& w, const Trader& t) {
    const int x = GetScreenWidth() - 330, y = 40;
    panel(x, y, 320, 250);
    int ty = y + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}) { DrawText(s.c_str(), x + 10, ty, 15, c); ty += 19; };
    char b[200];
    std::snprintf(b, sizeof b, "%s #%d%s", kCarrierNames[carrier_kind(t)], t.id, t.drover ? " (drover)" : "");
    line(b, trader_color(t));
    line(std::string("from ") + w.nodes[World::idx(t.at)].name + " to " + (t.dest >= 0 ? w.nodes[World::idx(t.dest)].name : "?"));
    if (t.cargo >= 0) {
        std::snprintf(b, sizeof b, "carrying %.0f %s", t.qty, w.goods[static_cast<std::size_t>(t.cargo)].name.c_str());
        line(b);
        std::snprintf(b, sizeof b, "bought at $%.2f a unit", t.unit_cost);
        line(b);
    } else line("empty");
    if (t.job_town >= 0) line(std::string("freight job for ") + w.nodes[static_cast<std::size_t>(t.job_town)].name);
    std::snprintf(b, sizeof b, "cash $%.0f   guards %d", t.cash, t.guards);
    line(b);
    std::snprintf(b, sizeof b, "robbed %d times   risk aversion %.2f (born %.2f)", t.robberies, t.risk_aversion, t.base_risk_aversion);
    line(b, t.risk_aversion > t.base_risk_aversion * 1.2 ? Color{170, 30, 30, 255} : Color{40, 30, 20, 255});
    // Its road ahead, with the fear it carries for each leg.
    std::string path;
    double fear_ahead = 0;
    NodeId cur = t.at;
    for (std::size_t i = t.leg; i < t.route.size() && i < t.leg + 4; ++i) {
        const Edge& e = w.edges[World::idx(t.route[i])];
        for (const auto& f : t.fear) if (f.first == t.route[i]) fear_ahead += f.second;
        cur = e.other(cur);
        path += (path.empty() ? "" : " > ") + w.nodes[World::idx(cur)].name;
    }
    if (!path.empty()) for (const std::string& l : wrap("ahead: " + path, 14, 296)) { DrawText(l.c_str(), x + 10, ty, 14, {40, 30, 20, 255}); ty += 16; }
    std::snprintf(b, sizeof b, "fear of the road ahead %.4f", fear_ahead);
    line(b);
    DrawText("click empty ground or press X to let go", x + 10, y + 232, 12, {110, 100, 90, 255});
}

// The big stories, newest first, wrapped. Click one to jump the map to its town.
// Returns the node of the clicked headline, or -1.
int draw_headlines(const Simulation& sim, long log_from, int top) {
    const int x = GetScreenWidth() - 330, h = GetScreenHeight() - top - 10;
    if (h < 60) return -1;
    panel(x, top, 320, h);
    DrawText("HEADLINES", x + 10, top + 6, 14, {90, 70, 40, 255});
    int ty = top + 26, clicked = -1;
    const auto& events = sim.log().events();
    for (std::size_t i = events.size(); i-- > 0 && ty < top + h - 30;) {
        const Event& e = events[i];
        if (e.day < log_from || !is_headline(e)) continue;
        const auto lines = wrap(e.text, 13, 300);
        const int eh = 14 + static_cast<int>(std::min<std::size_t>(lines.size(), 3)) * 15 + 4;
        if (ty + eh > top + h - 4) break;
        const Rectangle r{static_cast<float>(x + 4), static_cast<float>(ty - 2), 312.0f, static_cast<float>(eh)};
        const bool hot = e.node >= 0 && CheckCollisionPointRec(GetMousePosition(), r);
        if (hot) { DrawRectangleRec(r, {230, 215, 180, 255}); if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) clicked = e.node; }
        DrawText(format_date(e.day).c_str(), x + 10, ty, 12, {110, 90, 60, 255});
        int ly = ty + 14;
        for (std::size_t k = 0; k < lines.size() && k < 3; ++k, ly += 15)
            DrawText(lines[k].c_str(), x + 10, ly, 13, event_color(e.type));
        ty += eh + 2;
    }
    return clicked;
}

// Terrain under the map: a hillshade picture placed by the grid origin and cell size in terrain.json (written by
// tools/make_terrain.py). Found next to the data directory as terrain/NAME for a slice at slices/NAME.
struct Terrain {
    Texture2D tex{};
    bool ok = false;
    float x0 = 0, y0 = 0, w = 0, h = 0;   // the picture's extent in miles
};

double json_number(const std::string& text, const char* key) {
    const std::size_t at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return 0;
    return std::atof(text.c_str() + text.find(':', at) + 1);
}

Terrain load_terrain(const std::string& data) {
    Terrain t;
    std::string d = data;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    const std::size_t cut = d.find_last_of("/\\");
    const std::string name = cut == std::string::npos ? d : d.substr(cut + 1);
    // The world names its own grid ("terrain": "full"), which may differ from the folder name (slices/full_4400 uses "full").
    std::string grid = name;
    {
        std::ifstream wf(d + "/world.json");
        const std::string wt((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
        const std::size_t k = wt.find("\"terrain\": \"");
        if (k != std::string::npos) {
            const std::size_t b0 = k + 12, e0 = wt.find('"', b0);
            if (e0 != std::string::npos) grid = wt.substr(b0, e0 - b0);
        }
    }
    const std::string dir = d + "/../../terrain/" + grid;
    std::ifstream f(dir + "/terrain.json");
    const std::string pic = std::ifstream(dir + "/hillshade.jpg") ? dir + "/hillshade.jpg" : dir + "/hillshade.png";
    if (!f || !std::ifstream(pic)) return t;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const double cell = json_number(text, "cell_miles");
    t.x0 = static_cast<float>(json_number(text, "x_min"));
    t.y0 = static_cast<float>(json_number(text, "y_min"));
    t.w = static_cast<float>(json_number(text, "width") * cell);
    t.h = static_cast<float>(json_number(text, "height") * cell);
    t.tex = LoadTexture(pic.c_str());
    SetTextureFilter(t.tex, TEXTURE_FILTER_BILINEAR);
    t.ok = t.tex.id != 0 && t.w > 0 && t.h > 0;
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    std::string data = find_data_dir(), shot;
    std::uint64_t seed = 0;
    bool seed_set = false;
    std::string load_file, play_at;
    int shot_days = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") data = next();
        else if (a == "--load") load_file = next();
        else if (a == "--play") play_at = next();
        else if (a == "--seed") { seed = std::strtoull(next().c_str(), nullptr, 10); seed_set = true; }
        else if (a == "--shot") shot = next();
        else if (a == "--days") shot_days = std::atoi(next().c_str());
    }
    const WorldDef world = load_world(data);
    Scenario scenario = load_scenario(data, world);
    if (seed_set) scenario.seed = seed;
    Simulation sim(world, scenario);
    if (!load_file.empty()) sim.load_state(load_file);
    const World& w = sim.world();
    const long end_day = scenario.start + scenario.days;
    const GoodId flour = find_good(w, "flour");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1500, 900, "Frontier Sim");
    SetTargetFPS(60);
    SetExitKey(KEY_ESCAPE);
    const Terrain terrain = load_terrain(data);
    bool show_terrain = true, show_board = false;

    Camera2 cam;
    fit(cam, w);
    int speed = 1, overlay = 0;                       // speed index into days per second
    const double speeds[] = {1, 4, 16, 64, 256};
    bool paused = shot.empty() ? false : true;
    double accum = 0;
    int selected = -1, followed = -1;                  // a town index; a wagon (index into sim.traders())
    Filter filter;
    Player player;
    int headline_click = -1;
    Vector2 drag_from{};

    if (!shot.empty()) for (int d = 0; d < shot_days; ++d) sim.step();
    if (!play_at.empty()) {   // start in play mode at the named town (its id or its name)
        for (std::size_t i = 0; i < w.nodes.size(); ++i)
            if (w.nodes[i].id == play_at || w.nodes[i].name == play_at) { selected = static_cast<int>(i); player_spawn(sim, player, static_cast<NodeId>(i), false); break; }
    }
    int frames = 0;

    while (!WindowShouldClose()) {
        // ---- input
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        for (int k = 0; k < 5; ++k) if (IsKeyPressed(KEY_ONE + k)) { speed = k; paused = false; }
        if (IsKeyPressed(KEY_TAB)) overlay = (overlay + 1) % 3;
        if (IsKeyPressed(KEY_F)) fit(cam, w);
        if (IsKeyPressed(KEY_C)) filter.carrier = filter.carrier >= 3 ? -1 : filter.carrier + 1;
        if (IsKeyPressed(KEY_L)) filter.loaded_only = !filter.loaded_only;
        if (IsKeyPressed(KEY_T)) show_terrain = !show_terrain;
        if (IsKeyPressed(KEY_X)) followed = -1;
        if (IsKeyPressed(KEY_B)) show_board = !show_board;
        const bool wait_a_day = IsKeyPressed(KEY_W);
        if (IsKeyPressed(KEY_G) && selected >= 0 && (w.nodes[static_cast<std::size_t>(selected)].founded || w.nodes[static_cast<std::size_t>(selected)].fixed_market)) {
            player_spawn(sim, player, static_cast<NodeId>(selected), IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
            paused = false;
        }
        if (IsKeyPressed(KEY_P)) TakeScreenshot("frontier_shot.png");
        if (paused && IsKeyPressed(KEY_N) && w.today < end_day) sim.step();
        const float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            const Vector2 before = cam.to_world(GetMousePosition());
            cam.zoom = std::clamp(cam.zoom * std::pow(1.15f, wheel), 0.05f, 40.0f);
            const Vector2 after = cam.to_world(GetMousePosition());
            cam.cx += before.x - after.x;
            cam.cy += before.y - after.y;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) drag_from = GetMousePosition();
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            const Vector2 m = GetMousePosition();
            cam.cx -= (m.x - drag_from.x) / cam.zoom;
            cam.cy += (m.y - drag_from.y) / cam.zoom;
            drag_from = m;
        }
        const bool over_panel = (GetMouseX() > GetScreenWidth() - 340 && GetMouseY() > 40) ||
                                (player.active() && CheckCollisionPointRec(GetMousePosition(), Player::rect()));
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !over_panel) {
            selected = -1;
            followed = -1;
            // Wagons first (they sit on top of roads and towns), then towns.
            float best = 9;
            for (std::size_t i = 0; i < sim.traders().size(); ++i) {
                const Trader& t = sim.traders()[i];
                Vector2 pos;
                if (!t.alive || !filter.shows(t) || !trader_position(w, t, pos)) continue;
                const Vector2 s = cam.to_screen(pos.x, pos.y);
                const float d = std::hypot(s.x - GetMouseX(), s.y - GetMouseY());
                if (d < best) { best = d; followed = static_cast<int>(i); }
            }
            if (followed < 0) {
                best = 14;
                for (std::size_t i = 0; i < w.nodes.size(); ++i) {
                    if (!w.nodes[i].founded && !w.nodes[i].fixed_market) continue;
                    const Vector2 s = cam.to_screen(static_cast<float>(w.nodes[i].x), static_cast<float>(w.nodes[i].y));
                    const float d = std::hypot(s.x - GetMouseX(), s.y - GetMouseY());
                    if (d < best) { best = d; selected = static_cast<int>(i); }
                }
            }
        }
        if (player.active() && !player.dead && selected >= 0) {   // clicking a town sets where the player hauls to
            if (const Trader* me = player_trader(sim, player)) if (me->at != static_cast<NodeId>(selected)) player.dest = static_cast<NodeId>(selected);
        }
        if (headline_click >= 0) {                      // jump to the town a headline is about
            selected = headline_click;
            followed = -1;
            cam.cx = static_cast<float>(w.nodes[static_cast<std::size_t>(selected)].x);
            cam.cy = static_cast<float>(w.nodes[static_cast<std::size_t>(selected)].y);
            cam.zoom = std::max(cam.zoom, 1.5f);
            headline_click = -1;
        }

        // ---- simulate
        const bool holding = player.active() && player_holding(sim, player);   // the world waits for a player in town
        if (holding && wait_a_day && w.today < end_day) sim.step();
        if (!paused && !holding && w.today < end_day) {
            // A big world costs more than a frame to step, so run days only while a time budget lasts and never save up a
            // backlog: otherwise a slow frame brings more days due, which makes the next frame slower still.
            accum += std::min(GetFrameTime(), 0.1f) * speeds[speed];
            const double t0 = GetTime();
            while (accum >= 1.0 && w.today < end_day && GetTime() - t0 < 0.04) { sim.step(); accum -= 1.0; }
            accum = std::min(accum, 1.0);
        }

        // A followed wagon keeps the camera on it; if it dies or leaves the road in a town, follow it to the town.
        if (player.active()) {   // the camera stays with the player's wagon on the road
            if (const Trader* me = player_trader(sim, player); me && me->travelling)
                followed = static_cast<int>(me - sim.traders().data());
        }
        if (followed >= 0) {
            const Trader& t = sim.traders()[static_cast<std::size_t>(followed)];
            Vector2 pos;
            if (!t.alive) followed = -1;
            else if (trader_position(w, t, pos)) { cam.cx = pos.x; cam.cy = pos.y; }
            else if (t.at >= 0) { cam.cx = static_cast<float>(w.nodes[World::idx(t.at)].x); cam.cy = static_cast<float>(w.nodes[World::idx(t.at)].y); }
        }

        // ---- draw
        BeginDrawing();
        ClearBackground({232, 222, 196, 255});
        if (terrain.ok && show_terrain) {
            const Vector2 tl = cam.to_screen(terrain.x0, terrain.y0 + terrain.h);   // the picture is north up; y grows north on the map
            DrawTexturePro(terrain.tex, {0, 0, static_cast<float>(terrain.tex.width), static_cast<float>(terrain.tex.height)},
                           {tl.x, tl.y, terrain.w * cam.zoom, terrain.h * cam.zoom}, {0, 0}, 0, {255, 255, 255, 235});
        }
        // Rivers, when the world has them: blue lines under the roads, heavier for the bigger rivers.
        if (show_terrain) {
            for (const RiverLine& r : w.rivers) {
                const float thick = r.size >= 3 ? 3.0f : r.size == 2 ? 2.0f : 1.0f;
                for (std::size_t k = 1; k < r.points.size(); ++k)
                    DrawLineEx(cam.to_screen(static_cast<float>(r.points[k - 1].first), static_cast<float>(r.points[k - 1].second)),
                               cam.to_screen(static_cast<float>(r.points[k].first), static_cast<float>(r.points[k].second)), thick, {70, 120, 190, 200});
            }
            for (const CrossingSite& c : w.crossings) {   // a bridge, ferry or ford: a small mark on the river
                const Vector2 p = cam.to_screen(static_cast<float>(c.x), static_cast<float>(c.y));
                DrawRectangle(static_cast<int>(p.x) - 3, static_cast<int>(p.y) - 3, 6, 6, c.kind == "bridge" ? Color{60, 60, 60, 255} : Color{30, 90, 170, 255});
            }
        }
        for (const Edge& e : w.edges) {
            const Node& a = w.nodes[World::idx(e.a)];
            const Node& b = w.nodes[World::idx(e.b)];
            if ((!a.founded && !a.fixed_market) || (!b.founded && !b.fixed_market)) continue;
            // The road is drawn along its route over the terrain when it has one, a straight line otherwise.
            std::vector<Vector2> way;
            if (e.path.size() >= 2) for (const auto& pt : e.path) way.push_back(cam.to_screen(static_cast<float>(pt.first), static_cast<float>(pt.second)));
            else way = {cam.to_screen(static_cast<float>(a.x), static_cast<float>(a.y)), cam.to_screen(static_cast<float>(b.x), static_cast<float>(b.y))};
            if (!e.active) { for (std::size_t k = 1; k < way.size(); ++k) DrawLineEx(way[k - 1], way[k], 1.0f, {200, 120, 120, 120}); continue; }  // shut: a closed road or one not built yet
            const float danger = static_cast<float>(std::clamp(e.robbed_ema / 0.004, 0.0, 1.0));
            const float thick = e.mode == Mode::Rail ? 2.5f : e.mode == Mode::River ? 2.0f : 1.5f + 0.9f * static_cast<float>(e.road);   // worn trails thicken into wagon and graded roads
            for (std::size_t k = 1; k < way.size(); ++k) DrawLineEx(way[k - 1], way[k], thick, lerp(mode_color(e.mode), {200, 30, 20, 255}, danger));
        }
        // Camps: a tent on the trail, camp_miles out from the town the gang trades with (a label for the picture; the sim never reads it).
        for (const Gang& g : sim.gangs()) {
            if (!g.active || g.edge < 0 || g.camp_kind < 0 || g.hideout < 0) continue;
            const Edge& e = w.edges[World::idx(g.edge)];
            if (e.a != g.hideout && e.b != g.hideout) continue;
            const Vector2 m = point_along(w, e, g.hideout, std::clamp(g.camp_miles / std::max(1.0, e.miles), 0.0, 1.0));
            const Vector2 p = cam.to_screen(m.x, m.y);
            const float s = sim.is_band_gang(g) ? 4.0f : 6.0f;
            DrawTriangle({p.x, p.y - s}, {p.x - s, p.y + s * 0.7f}, {p.x + s, p.y + s * 0.7f}, {120, 80, 40, 255});
            DrawTriangleLines({p.x, p.y - s}, {p.x - s, p.y + s * 0.7f}, {p.x + s, p.y + s * 0.7f}, {40, 30, 20, 255});
            if (cam.zoom > 2.0f) DrawText(Simulation::camp_kind_name(g.camp_kind), static_cast<int>(p.x + s + 3), static_cast<int>(p.y - 6), 11, {110, 70, 30, 255});
        }
        for (const Gang& g : sim.gangs()) {
            if (!g.active || g.edge < 0) continue;
            const Edge& e = w.edges[World::idx(g.edge)];
            const Node& a = w.nodes[World::idx(e.a)];
            const Node& b = w.nodes[World::idx(e.b)];
            const Vector2 mid = point_along(w, e, e.a, 0.5);
            const Vector2 p = cam.to_screen(mid.x, mid.y);
            // A small band is a thin orange cross; a large gang a bigger, heavier red one with its size beside it.
            const bool band = sim.is_band_gang(g);
            const float s = band ? 4.0f : 7.0f;
            const Color gc = band ? Color{220, 120, 30, 255} : Color{190, 20, 20, 255};
            DrawLineEx({p.x - s, p.y - s}, {p.x + s, p.y + s}, band ? 1.8f : 3.2f, gc);
            DrawLineEx({p.x - s, p.y + s}, {p.x + s, p.y - s}, band ? 1.8f : 3.2f, gc);
            if (!band) DrawText(TextFormat("%d", g.men), static_cast<int>(p.x + s + 2), static_cast<int>(p.y - 6), 12, gc);
        }
        for (std::size_t i = 0; i < w.nodes.size(); ++i) {
            const Node& n = w.nodes[i];
            if (!n.founded && !n.fixed_market) continue;
            const Vector2 p = cam.to_screen(static_cast<float>(n.x), static_cast<float>(n.y));
            const float r = n.fixed_market ? 9.0f : 3.5f + std::sqrt(static_cast<float>(std::max(0.0, n.population))) / 9.0f;
            DrawCircleV(p, r, node_color(w, i, overlay, flour));
            DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), r, {40, 30, 20, 255});
            if (n.starving) DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), r + 3, {200, 30, 30, 255});
            if (n.garrison > 0) DrawRectangle(static_cast<int>(p.x - 2), static_cast<int>(p.y - r - 7), 4, 4, {30, 70, 140, 255});
            if (cam.zoom > 0.9f || n.fixed_market || n.population > 1500 || static_cast<int>(i) == selected)
                DrawText(n.name.c_str(), static_cast<int>(p.x + r + 3), static_cast<int>(p.y - 6), 13, {40, 30, 20, 255});
        }
        for (const Trader& t : sim.traders()) {
            Vector2 pos;
            if (!t.alive || !trader_position(w, t, pos)) continue;
            const Vector2 p = cam.to_screen(pos.x, pos.y);
            Color c = trader_color(t);
            if (!filter.shows(t)) c.a = 35;
            DrawCircleV(p, t.cargo >= 0 ? 3.0f : 2.2f, c);          // a loaded wagon is a little bigger
            if (static_cast<int>(&t - sim.traders().data()) == followed) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), 9, {20, 20, 20, 255});
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), 10, {250, 244, 228, 255});
            }
        }
        if (followed >= 0) draw_trader_panel(w, sim.traders()[static_cast<std::size_t>(followed)]);
        if (selected >= 0) {
            const Node& n = w.nodes[static_cast<std::size_t>(selected)];
            const Vector2 p = cam.to_screen(static_cast<float>(n.x), static_cast<float>(n.y));
            DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), 16, {20, 20, 20, 255});
            draw_town_panel(sim, static_cast<std::size_t>(selected));
        }
        headline_click = draw_headlines(sim, scenario.log_from, selected >= 0 ? 380 : followed >= 0 ? 300 : 40);

        // HUD: date, speed, counts.
        panel(10, 8, 520, 26);
        int alive = 0, loaded = 0;
        for (const Trader& t : sim.traders()) { alive += t.alive; loaded += t.alive && t.cargo >= 0; }
        int gangs = 0;
        for (const Gang& g : sim.gangs()) gangs += g.active;
        char hud[220];
        std::snprintf(hud, sizeof hud, "%s   %s   outfits %d (%d loaded)   gangs %d   overlay: %s", format_date(w.today).c_str(),
                      paused || w.today >= end_day ? "paused" : (std::string(TextFormat("%.0f d/s", speeds[speed]))).c_str(), alive, loaded, gangs, overlay_name(overlay));
        DrawText(hud, 18, 13, 15, {40, 30, 20, 255});

        // Legend.
        panel(10, 40, 190, 262);
        int ly = 48;
        auto key = [&](Color c, const char* label) { DrawCircle(24, ly + 7, 4, c); DrawText(label, 36, ly, 13, {40, 30, 20, 255}); ly += 18; };
        key({40, 40, 40, 255}, "freelancer");
        key({30, 150, 70, 255}, "house outfit");
        key({40, 80, 200, 255}, "eastern firm");
        key({230, 130, 20, 255}, "freight job");
        DrawLineEx({20, static_cast<float>(ly + 4)}, {28, static_cast<float>(ly + 10)}, 1.8f, {220, 120, 30, 255});
        DrawLineEx({20, static_cast<float>(ly + 10)}, {28, static_cast<float>(ly + 4)}, 1.8f, {220, 120, 30, 255});
        DrawText("small band (2-5 men)", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawLineEx({18, static_cast<float>(ly + 2)}, {30, static_cast<float>(ly + 12)}, 3.2f, {190, 20, 20, 255});
        DrawLineEx({18, static_cast<float>(ly + 12)}, {30, static_cast<float>(ly + 2)}, 3.2f, {190, 20, 20, 255});
        DrawText("large gang (men shown)", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawCircleLines(24, ly + 7, 5, {200, 30, 30, 255});
        DrawText("food shortage", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawTriangle({24, static_cast<float>(ly + 1)}, {19, static_cast<float>(ly + 11)}, {29, static_cast<float>(ly + 11)}, {120, 80, 40, 255});
        DrawText("gang camp", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawText("big dot: loaded wagon", 18, ly, 12, {90, 70, 40, 255});
        ly += 18;
        DrawText("thick road: worn in by traffic", 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        DrawText("B price board", 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        DrawText("G play: start at selected town", 18, ly, 12, {90, 70, 40, 255});
        ly += 18;
        DrawText(TextFormat("C show: %s", filter.carrier < 0 ? "all" : kCarrierNames[filter.carrier]), 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        DrawText(TextFormat("L loaded only: %s", filter.loaded_only ? "on" : "off"), 18, ly, 12, {90, 70, 40, 255});

        if (show_board) draw_price_board(w);
        if (player.active()) {
            bool wait = false;
            draw_player_panel(sim, player, wait);
            if (wait && player_holding(sim, player) && w.today < end_day) sim.step();
        }

        // Event feed: the latest events that are not routine, newest at the bottom, with the cause beneath.
        const auto& events = sim.log().events();
        std::vector<const Event*> recent;
        for (std::size_t i = events.size(); i-- > 0 && recent.size() < 6;)
            if (!events[i].minor && events[i].day >= scenario.log_from) recent.push_back(&events[i]);
        const int fh = static_cast<int>(recent.size()) * 34 + 10;
        panel(10, GetScreenHeight() - fh - 10, 760, fh);
        int ey = GetScreenHeight() - fh - 4;
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            const Event& e = **it;
            DrawText(format_date(e.day).c_str(), 18, ey, 14, {110, 90, 60, 255});
            DrawText(e.text.substr(0, 84).c_str(), 112, ey, 14, event_color(e.type));
            DrawText(e.cause.substr(0, 98).c_str(), 112, ey + 15, 12, {110, 100, 90, 255});
            ey += 34;
        }
        EndDrawing();

        if (!shot.empty() && ++frames == 3) { TakeScreenshot(shot.c_str()); break; }
    }
    CloseWindow();
    return 0;
}
