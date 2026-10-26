// Frontier Sim viewer: a top-down map of the running simulation.
//
// The viewer reads the sim through const accessors and the event log. The one exception is play mode (G on a town),
// which drives a single outfit through the public action API (act_buy, act_travel, act_sell), exactly as the AI traders
// do, so another presentation layer can replace this file without touching sim/. The map is drawn in the sim's own
// coordinates (miles, x east, y north).
//
//   frontier_viewer [--data DIR] [--seed N] [--load SNAPSHOT] [--play TOWN] [--ground TOWN] [--shot FILE --days N]
//   --load starts from a state saved by frontier_cli --save (same data folder), so a pre-simulated world opens at once.
//   If DIR is data/slices/NAME and data/terrain/NAME exists, a shaded-relief terrain map is drawn under the towns (T toggles).
//   space pause/resume   1-5 speed   N one day (paused)   tab overlay   F fit map   click a town or a wagon   wheel zoom
//   right-drag or middle-drag pan   T terrain   C carrier filter   L loaded wagons only   X let go of a followed wagon
//   click a headline to jump to its town   P screenshot   B price board   Esc quit
//   V on a selected town (or --ground TOWN): the ground view, at about RimWorld scale, walking a drifter around the town (M returns).
//   Play mode: select a town, G starts you there as a freight wagon (shift-G as a drover). Pick a good, click a destination
//   town on the map, hire guards, set off. The world waits while you decide and runs while you travel; W waits a day.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "raylib.h"

#include "frontier/date.h"
#include "frontier/simulation.h"

using namespace frontier;

namespace {

// A real typeface instead of raylib's built-in bitmap font. Loaded once at start; falls back to the built-in if the file is missing.
Font g_font{};
bool g_font_ok = false;

bool g_measure = false;   // a layout pass: panels and text draw nothing, so a panel can learn its height before it is drawn

void Text(const char* s, int x, int y, int size, Color c) {
    if (g_measure) return;
    if (g_font_ok) DrawTextEx(g_font, s, {static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(size), 0, c);
    else DrawText(s, x, y, size, c);
}

int TextWidth(const char* s, int size) {
    return g_font_ok ? static_cast<int>(MeasureTextEx(g_font, s, static_cast<float>(size), 0).x) : MeasureText(s, size);
}


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
    static const char* const kinds[] = {"food_crisis", "food_crisis_over", "famine", "famine_over", "ghost_town", "town_founded", "gang_takeover", "town_freed", "shortage", "gang_war", "gang_merged",
                                        "leader_killed", "house_founded",
                                        "house_closed", "route_changed", "reserve_exhausted", "output_begins", "rush", "road_closure",
                                        "house_collapse", "firm_withdrawal", "rail_construction", "road_improved", "road_worn", "trader_settled", "crop_failure", "vigilance_formed", "repopulating", "law_established", "bushwhack_notable", "cavalry_sent"};
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
        if (!cur.empty() && TextWidth(t.c_str(), size) > width) { lines.push_back(cur); cur = word; }
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
    if (g_measure) return;
    DrawRectangle(x, y, w, h, {250, 244, 228, 230});
    DrawRectangleLines(x, y, w, h, {120, 100, 70, 255});
}

// One row of a table. A proportional font cannot be lined up with spaces, so each cell is placed at its own pixel offset
// from x: `at` is the cell's left edge, or its right edge when `right` is set (numbers).
struct Cell { std::string s; int at; bool right; };
void cells(int x, int y, int size, Color c, std::initializer_list<Cell> row) {
    for (const Cell& k : row) Text(k.s.c_str(), x + k.at - (k.right ? TextWidth(k.s.c_str(), size) : 0), y, size, c);
}

int draw_town_panel(const Simulation& sim, std::size_t i) {
    const World& w = sim.world();
    const Node& n = w.nodes[i];
    const int x = GetScreenWidth() - 330, y = 40;
    if (!g_measure) {   // lay it out once without drawing to learn how tall it is, so what sits below it never overlaps
        g_measure = true;
        const int bottom = draw_town_panel(sim, i);
        g_measure = false;
        panel(x, y, 320, bottom - y);
    }
    int ty = y + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}) { Text(s.c_str(), x + 10, ty, 15, c); ty += 19; };
    line(n.name + "  (" + n.kind + ")", {20, 20, 20, 255});
    if (n.fixed_market) { line("an eastern market: prices are fixed"); return ty + 4; }
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
    cells(x + 10, ty, 14, {90, 70, 40, 255}, {{"good", 0, false}, {"shelf", 135, true}, {"price", 205, true}, {"on order", 220, false}});
    ty += 18;
    for (const char* id : {"flour", "beef_cattle", "tools", "whiskey", "ammunition", "gold"}) {
        const GoodId g = find_good(w, id);
        if (g < 0) continue;
        const std::size_t gi = static_cast<std::size_t>(g);
        char order[40] = "";
        if (n.order_qty[gi] >= 1) std::snprintf(order, sizeof order, "%.0f @ $%.2f", n.order_qty[gi], n.order_price[gi]);
        char shelf[24], price[24];
        std::snprintf(shelf, sizeof shelf, "%.0f", n.stock[gi]);
        std::snprintf(price, sizeof price, "$%.2f", w.mid_price(static_cast<NodeId>(i), g));
        cells(x + 10, ty, 14, {40, 30, 20, 255}, {{w.goods[gi].name, 0, false}, {shelf, 135, true}, {price, 205, true}, {order, 220, false}});
        ty += 18;
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
            for (const std::string& l : lines) { Text(l.c_str(), x + 10, ty, 12, {40, 30, 20, 255}); ty += 14; }
            ty += 2;
        }
    } else {
        line("no newspaper here", {120, 100, 70, 255});
    }
    return ty + 4;
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
    bool departing = false;   // set at the freight office when he sets off: the ground view hands back to the map
    bool active() const { return id >= 0; }
    static Rectangle rect() { return {10, 312, 370, 340}; }
};

Trader* player_trader(Simulation& sim, const Player& p) { return p.active() ? sim.find_trader(p.id) : nullptr; }

double worth_of(const Trader& t) { return t.cash + (t.cargo >= 0 ? t.qty * t.unit_cost : 0.0); }

// A drifter who walks into town with a little money and a wagon is, to the sim, one more outfit that waits for orders.
constexpr double kDrifterCash = 1500;

void player_spawn(Simulation& sim, Player& p, NodeId at, bool drover, double cash = 3000) {
    p = Player{};
    p.id = sim.add_trader(at, cash, drover, /*controlled=*/true);
    p.drover = drover;
    p.start_day = sim.world().today;
    if (const Trader* t = sim.find_trader(p.id)) p.start_worth = worth_of(*t);
    char b[160];
    std::snprintf(b, sizeof b, "You have $%.0f and an empty wagon. Pick a good, click a town to haul it to.", cash);
    p.msg = b;
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
    Text(label, static_cast<int>(r.x) + 6, static_cast<int>(r.y) + 4, 13, enabled ? Color{40, 30, 20, 255} : Color{150, 140, 125, 255});
    return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// Draws the player's panel and carries out what he clicks. Sets `wait` when he asks to let a day pass.
void draw_player_panel(Simulation& sim, Player& p, bool& wait) {
    const World& w = sim.world();
    const Rectangle R = Player::rect();
    panel(static_cast<int>(R.x), static_cast<int>(R.y), static_cast<int>(R.width), static_cast<int>(R.height));
    const int x = static_cast<int>(R.x) + 10;
    int ty = static_cast<int>(R.y) + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}, int size = 14) { Text(s.c_str(), x, ty, size, c); ty += size + 4; };
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
        cells(x, ty, 13, {90, 70, 40, 255}, {{"good", 0, false}, {"price", 215, true}, {"shelf", 275, true}, {"est. profit", 350, true}});
        ty += 17;
        for (std::size_t gi = 0; gi < w.goods.size(); ++gi) {
            const GoodDef& g = w.goods[gi];
            const double stock = here.fixed_market ? 1e9 : here.stock[gi];   // an eastern market never runs out
            if (g.self_propelled != p.drover || stock < 1) continue;
            if (ty > static_cast<int>(R.y + R.height) - 90) break;
            const double units = std::min(t->capacity_lb / g.load_lb, stock);
            const double cost = w.quote_buy(t->at, static_cast<GoodId>(gi), units);
            std::string est = "-";
            if (ri.ok) {
                const double freight = (w.freight_unit_cost(static_cast<GoodId>(gi), ri.miles) * w.params.owner_haul_share) * units;
                const double sale = w.quote_sell(p.dest, static_cast<GoodId>(gi), units);
                char e[32];
                std::snprintf(e, sizeof e, "%+.0f", sale - cost - freight);
                est = e;
            }
            char shelf[16];
            if (here.fixed_market) std::snprintf(shelf, sizeof shelf, "plenty"); else std::snprintf(shelf, sizeof shelf, "%.0f", stock);
            char price[24];
            std::snprintf(price, sizeof price, "%.2f", cost / std::max(1.0, units));
            const Rectangle row{static_cast<float>(x - 4), static_cast<float>(ty - 1), R.width - 12, 17};
            const bool sel = p.sel == static_cast<GoodId>(gi);
            if (sel) DrawRectangleRec(row, {230, 210, 160, 255});
            else if (CheckCollisionPointRec(GetMousePosition(), row)) DrawRectangleRec(row, {240, 228, 200, 255});
            if (CheckCollisionPointRec(GetMousePosition(), row) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) p.sel = static_cast<GoodId>(gi);
            cells(x, ty, 13, {40, 30, 20, 255}, {{g.name, 0, false}, {price, 215, true}, {shelf, 275, true}, {est, 350, true}});
            ty += 17;
        }
        if (button({static_cast<float>(x), static_cast<float>(ty + 4), 150, 22}, "Buy a full load", p.sel >= 0)) {
            // Keep back the freight, the guards and a little spending money.
            double reserve = 100;
            if (ri.ok) reserve += (w.freight_unit_cost(p.sel, ri.miles) * w.params.owner_haul_share) * t->capacity_lb / w.goods[World::idx(p.sel)].load_lb + w.params.guard_wage * p.guards * ri.days;
            const Receipt r = sim.act_buy(*t, p.sel, 1e9, std::max(0.0, t->cash - reserve));
            if (r.result == ActionResult::Ok) std::snprintf(b, sizeof b, "Bought %.0f units for $%.0f.", r.units, r.money);
            else std::snprintf(b, sizeof b, "Could not buy (%s).", action_result_name(r.result));
            p.msg = b;
        }
        ty += 30;
    }
    // Guards, then off.
    std::snprintf(b, sizeof b, "guards: %d  ($%.1f a day)", p.guards, w.params.guard_wage * p.guards);
    Text(b, x, ty + 3, 13, {40, 30, 20, 255});
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
    panel(x, y, 430, 58 + rows * 17);
    Text("PRICE BOARD (B)", x + 8, y + 6, 13, {20, 20, 20, 255});
    cells(x + 8, y + 24, 13, {90, 70, 40, 255}, {{"town", 0, false}, {"pop", 235, true}, {"flour", 295, true}, {"beef", 355, true}, {"hunger", 412, true}});
    int ty = y + 42;
    for (std::size_t i : order) {
        const Node& n = w.nodes[i];
        char b[160];
        char pop[24], fl[24], bf[24], hu[24];
        std::snprintf(pop, sizeof pop, "%.0f", n.population);
        std::snprintf(fl, sizeof fl, "%.2f", flour >= 0 ? w.mid_price(static_cast<NodeId>(i), flour) : 0.0);
        std::snprintf(bf, sizeof bf, "%.2f", beef >= 0 ? w.mid_price(static_cast<NodeId>(i), beef) : 0.0);
        std::snprintf(hu, sizeof hu, "%.2f", n.hunger);
        cells(x + 8, ty, 13, n.starving ? Color{170, 30, 30, 255} : Color{40, 30, 20, 255}, {{n.name, 0, false}, {pop, 235, true}, {fl, 295, true}, {bf, 355, true}, {hu, 412, true}});
        ty += 17;
    }
}

void draw_trader_panel(const World& w, const Trader& t) {
    const int x = GetScreenWidth() - 330, y = 40;
    panel(x, y, 320, 250);
    int ty = y + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}) { Text(s.c_str(), x + 10, ty, 15, c); ty += 19; };
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
    if (!path.empty()) for (const std::string& l : wrap("ahead: " + path, 14, 296)) { Text(l.c_str(), x + 10, ty, 14, {40, 30, 20, 255}); ty += 16; }
    std::snprintf(b, sizeof b, "fear of the road ahead %.4f", fear_ahead);
    line(b);
    Text("click empty ground or press X to let go", x + 10, y + 232, 12, {110, 100, 90, 255});
}

// The big stories, newest first, wrapped. Click one to jump the map to its town.
// Returns the node of the clicked headline, or -1.
int draw_headlines(const Simulation& sim, long log_from, int top) {
    const int x = GetScreenWidth() - 330, h = GetScreenHeight() - top - 10;
    if (h < 60) return -1;
    panel(x, top, 320, h);
    Text("HEADLINES", x + 10, top + 6, 14, {90, 70, 40, 255});
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
        Text(format_date(e.day).c_str(), x + 10, ty, 12, {110, 90, 60, 255});
        int ly = ty + 14;
        for (std::size_t k = 0; k < lines.size() && k < 3; ++k, ly += 15)
            Text(lines[k].c_str(), x + 10, ly, 13, event_color(e.type));
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
    Color* px = nullptr;                  // the picture's pixels on the CPU, so the ground view can take its colour from it
    int iw = 0, ih = 0;
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
    Image img = LoadImage(pic.c_str());
    if (img.data) { t.px = LoadImageColors(img); t.iw = img.width; t.ih = img.height; t.tex = LoadTextureFromImage(img); UnloadImage(img); }
    SetTextureFilter(t.tex, TEXTURE_FILTER_BILINEAR);
    t.ok = t.tex.id != 0 && t.w > 0 && t.h > 0;
    return t;
}

// ---------------------------------------------------------------- ground view: the world at the scale it will be played
//
// The map above is the "world map"; this is the "local map", at about the scale of RimWorld: a person is a couple of feet
// across, a wagon about 12 feet, a screen shows roughly 200 feet. Time runs the same way too: a game day is about 17 real
// minutes at speed 1 (1,000 seconds, as in RimWorld), and the simulation steps once per game day. At this scale nobody
// crosses the map on foot (a day's walk is about 3 miles at this clock), so the trip between towns is the world map's job
// and this view is for a town, a camp or a stretch of road. Nothing drawn here decides an outcome: streets and buildings are
// laid out from the town's size and id, wagons shown are the ones the sim says are standing in town.

std::string g_ground_inside;   // --inside NAME: start in the first building of that name (for screenshots)
float g_ground_zoom = 9.0f;   // pixels per foot to start at (--zoom)
std::string g_ground_shot;   // with --shot, the ground view saves one frame and returns
constexpr double kFeet = 5280.0;   // feet in a mile

std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
std::uint64_t hash3(std::uint64_t a, std::int64_t b, std::int64_t c) {
    return mix64(a * 0x100000001b3ULL ^ mix64(static_cast<std::uint64_t>(b) * 31 + mix64(static_cast<std::uint64_t>(c))));
}
double unit_of(std::uint64_t h) { return static_cast<double>(h >> 11) / 9007199254740992.0; }

// A tileable greyscale-to-colour noise picture (value noise, summed octaves), drawn repeated over the ground.
Texture2D make_noise(int n, int cells, std::uint64_t seed, Color lo, Color hi, unsigned char alpha) {
    std::vector<float> lat(static_cast<std::size_t>(cells * cells));
    for (int i = 0; i < cells * cells; ++i) lat[static_cast<std::size_t>(i)] = static_cast<float>(unit_of(mix64(seed + static_cast<std::uint64_t>(i))));
    std::vector<Color> px(static_cast<std::size_t>(n * n));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float v = 0, amp = 0.6f, norm = 0;
            for (int o = 0; o < 3; ++o) {
                const int c = cells << o;
                const float fx = static_cast<float>(x) / n * c, fy = static_cast<float>(y) / n * c;
                const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                const float tx = fx - ix, ty = fy - iy;
                auto at = [&](int a, int b) {   // the lattice of this octave, hashed so the octaves differ and the picture wraps
                    return static_cast<float>(unit_of(mix64(seed + static_cast<std::uint64_t>(o) * 7919 + static_cast<std::uint64_t>((a % c) * c + (b % c)))));
                };
                const float a = at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx;
                const float b = at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx;
                v += (a * (1 - ty) + b * ty) * amp;
                norm += amp;
                amp *= 0.5f;
            }
            Color c = lerp(lo, hi, v / norm);
            c.a = alpha;
            px[static_cast<std::size_t>(y * n + x)] = c;
        }
    Image img{px.data(), n, n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(img);   // copies the pixels
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    return t;
}

Color terrain_color(const Terrain& t, double x, double y) {
    if (!t.ok || !t.px) return {128, 144, 84, 255};
    const int ix = std::clamp(static_cast<int>((x - t.x0) / t.w * t.iw), 0, t.iw - 1);
    const int iy = std::clamp(static_cast<int>((t.y0 + t.h - y) / t.h * t.ih), 0, t.ih - 1);
    return lerp(t.px[static_cast<std::size_t>(iy * t.iw + ix)], {150, 158, 96, 255}, 0.45f);   // the hillshade is dark; ground at this scale is sunlit grass
}

struct GroundCam {
    double cx = 0, cy = 0;   // miles
    float pxft = 9.0f;       // pixels per foot: 7 to 10 is about RimWorld's default zoom
    Vector2 to_screen(double x, double y) const {
        return {GetScreenWidth() * 0.5f + static_cast<float>((x - cx) * kFeet) * pxft, GetScreenHeight() * 0.5f - static_cast<float>((y - cy) * kFeet) * pxft};
    }
    Vector2 to_world(Vector2 s) const {
        return {static_cast<float>(cx + (s.x - GetScreenWidth() * 0.5f) / pxft / kFeet), static_cast<float>(cy - (s.y - GetScreenHeight() * 0.5f) / pxft / kFeet)};
    }
};

// The part of a segment (a to b, in pixels) inside the screen, as parameters; false if none (Liang-Barsky).
bool clip_to_screen(Vector2 a, Vector2 b, float margin, float& t0, float& t1) {
    t0 = 0; t1 = 1;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x + margin, GetScreenWidth() + margin - a.x, a.y + margin, GetScreenHeight() + margin - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) { if (q[i] < 0) return false; continue; }
        const float r = q[i] / p[i];
        if (p[i] < 0) { if (r > t1) return false; t0 = std::max(t0, r); }
        else { if (r < t0) return false; t1 = std::min(t1, r); }
    }
    return true;
}

void ground_road(const GroundCam& cam, Vector2 a, Vector2 b, Mode mode, int road) {
    float t0, t1;
    if (!clip_to_screen(a, b, 60, t0, t1)) return;
    const Vector2 p0{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0}, p1{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1};
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 0.01f) return;
    const Vector2 d{(b.x - a.x) / len, (b.y - a.y) / len}, nrm{-d.y, d.x};
    const float f = cam.pxft;
    if (mode == Mode::Rail) {
        DrawLineEx(p0, p1, 11 * f, {112, 106, 98, 255});   // ballast
        const float step = 2.2f * f;
        if (f >= 3.5f) {
            const float run = std::hypot(p1.x - p0.x, p1.y - p0.y);
            const float phase = std::fmod(t0 * len, step);   // ties stay put on the ground as the segment is clipped
            for (float s = step - phase; s < run; s += step) {
                const Vector2 c{p0.x + d.x * s, p0.y + d.y * s};
                DrawLineEx({c.x - nrm.x * 4.2f * f, c.y - nrm.y * 4.2f * f}, {c.x + nrm.x * 4.2f * f, c.y + nrm.y * 4.2f * f}, 0.9f * f, {74, 52, 36, 255});
            }
        }
        for (int side = -1; side <= 1; side += 2)
            DrawLineEx({p0.x + nrm.x * 2.35f * f * side, p0.y + nrm.y * 2.35f * f * side}, {p1.x + nrm.x * 2.35f * f * side, p1.y + nrm.y * 2.35f * f * side}, 0.45f * f, {170, 170, 176, 255});
        return;
    }
    // A worn trail is two ruts, a wagon road a packed band, a graded road a wider, smoother one.
    const float width = (road >= 2 ? 22.0f : road == 1 ? 15.0f : 9.0f) * f;
    const Color dust = road >= 2 ? Color{186, 162, 124, 255} : road == 1 ? Color{174, 148, 110, 255} : Color{160, 136, 100, 255};
    DrawLineEx(p0, p1, width, dust);
    if (road <= 1)
        for (int side = -1; side <= 1; side += 2)
            DrawLineEx({p0.x + nrm.x * width * 0.28f * side, p0.y + nrm.y * width * 0.28f * side}, {p1.x + nrm.x * width * 0.28f * side, p1.y + nrm.y * width * 0.28f * side}, std::max(1.0f, width * 0.09f), {128, 106, 76, 255});
}

// Where segment a-b crosses segment c-d (screen pixels), as the parameter along a-b; false if they do not cross.
bool seg_cross(Vector2 a, Vector2 b, Vector2 c, Vector2 d, float& t) {
    const float rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y;
    const float den = rx * sy - ry * sx;
    if (std::abs(den) < 1e-6f) return false;
    t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den;
    const float u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
    return t >= 0 && t <= 1 && u >= 0 && u <= 1;
}

// A rail line over a river rides a timber trestle: a deck on piers a little wider than the water, so the track does not
// float on the blue. `river_px` is the drawn width of the river including its banks.
void ground_trestle(const GroundCam& cam, Vector2 a, Vector2 b, Vector2 at, float river_px, Vector2 river_dir) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 0.01f) return;
    const Vector2 d{(b.x - a.x) / len, (b.y - a.y) / len}, nrm{-d.y, d.x};
    const float f = cam.pxft;
    const float across = std::max(0.25f, std::abs(d.x * river_dir.y - d.y * river_dir.x));   // a slanting crossing is longer than the river is wide
    const float half = std::min(river_px * 0.5f / across, 900.0f * f) + 5 * f;
    const Vector2 p0{at.x - d.x * half, at.y - d.y * half}, p1{at.x + d.x * half, at.y + d.y * half};
    DrawLineEx(p0, p1, 12.5f * f, {0, 0, 0, 50});
    for (float s = -half; s <= half; s += 6 * f) {   // piers: timber bents across the stream
        const Vector2 c{at.x + d.x * s, at.y + d.y * s};
        DrawLineEx({c.x - nrm.x * 5.2f * f, c.y - nrm.y * 5.2f * f}, {c.x + nrm.x * 5.2f * f, c.y + nrm.y * 5.2f * f}, 1.1f * f, {74, 52, 36, 255});
    }
    DrawLineEx(p0, p1, 11 * f, {120, 92, 62, 255});    // the deck
    DrawLineEx(p0, p1, 9.6f * f, {104, 78, 52, 255});
    for (int side = -1; side <= 1; side += 2) {
        DrawLineEx({p0.x + nrm.x * 5.4f * f * side, p0.y + nrm.y * 5.4f * f * side}, {p1.x + nrm.x * 5.4f * f * side, p1.y + nrm.y * 5.4f * f * side}, 0.6f * f, {70, 50, 34, 255});   // handrails
        DrawLineEx({p0.x + nrm.x * 2.35f * f * side, p0.y + nrm.y * 2.35f * f * side}, {p1.x + nrm.x * 2.35f * f * side, p1.y + nrm.y * 2.35f * f * side}, 0.45f * f, {170, 170, 176, 255});   // the rails
    }
}

// A covered wagon seen from above, drawn along `deg` (screen degrees, clockwise).
void draw_wagon(Vector2 c, float deg, float f, Color tint, bool loaded) {
    auto box = [&](float dx, float dy, float w, float h, Color col) {   // dx along the wagon, dy across it, in feet
        const float r = deg * 3.14159265f / 180;
        const Vector2 o{c.x + (std::cos(r) * dx - std::sin(r) * dy) * f, c.y + (std::sin(r) * dx + std::cos(r) * dy) * f};
        DrawRectanglePro({o.x, o.y, w * f, h * f}, {w * f / 2, h * f / 2}, deg, col);
    };
    box(1.2f, 1.2f, 12, 6, {0, 0, 0, 50});                              // shadow
    for (float dx : {-3.5f, 3.5f}) for (float dy : {-3.2f, 3.2f}) box(dx, dy, 2.6f, 0.5f, {40, 30, 24, 255});   // wheels
    box(0, 0, 12, 5, {112, 82, 52, 255});                              // bed
    box(-0.4f, 0, 9.6f, 4.4f, loaded ? Color{232, 222, 196, 255} : Color{205, 198, 178, 255});   // canvas
    box(-0.4f, 0, 9.6f, 0.5f, tint);                                   // a stripe for the kind of outfit
    for (int i = 0; i < 2; ++i) {                                      // a pair of oxen
        box(10 + i * 4.5f, -1.0f, 3.6f, 1.7f, {96, 70, 48, 255});
        box(10 + i * 4.5f, 1.0f, 3.6f, 1.7f, {110, 82, 58, 255});
    }
    box(7.5f, 0, 6, 0.3f, {70, 52, 36, 255});                          // tongue
}

struct Look { Color shirt{70, 82, 110, 255}, hat{92, 66, 44, 255}; bool hat_on = true; float scale = 1.0f; };

void draw_person(Vector2 c, float deg, float f, double stride, const Look& look = Look{}) {
    f *= look.scale;
    const float r = deg * 3.14159265f / 180;
    DrawEllipse(static_cast<int>(c.x + 0.9f * f), static_cast<int>(c.y + 1.0f * f), 1.5f * f, 1.1f * f, {0, 0, 0, 60});
    DrawRectanglePro({c.x, c.y, 1.2f * f, 2.5f * f}, {0.6f * f, 1.25f * f}, deg + 90, look.shirt);                // shoulders
    const float sw = static_cast<float>(std::sin(stride)) * 0.5f * f;                                           // arms swing a little
    DrawCircleV({c.x + std::cos(r) * sw, c.y + std::sin(r) * sw}, 0.45f * f, {196, 150, 116, 255});
    if (look.hat_on) {
        DrawCircleV(c, 1.35f * f, look.hat);                                                                      // hat brim
        DrawCircleV(c, 0.8f * f, lerp(look.hat, {0, 0, 0, 255}, 0.25f));                                         // crown
    } else {
        DrawCircleV(c, 0.85f * f, look.hat);                                                                      // bare head: hair or a bonnet
    }
}

// ---- the people of a town. They walk the streets the town was laid out with (so they never cross a house), stop to loiter,
// turn at corners, and thin out at night. Who they are comes from the town: soldiers at a post, miners in a camp, hands on a
// ranch. How many comes from its size. Nothing about them reaches the sim.
struct Folk {
    int street = 0;            // 0 is the main street, otherwise the cross street's index
    double u = 0, v = 0;       // feet along the street and across it
    double dir = 1, speed = 5, pause = 0, stride = 0;
    double bound = 100;        // how far along this street it may walk
    double lx = 0, ly = 0;     // where it stood last frame in the town's frame (feet), for stepping around others
    std::uint64_t h = 0;
    const char* role = "townsman";
    Look look;
};

// What a building is. `room` picks the inside (None means a shut door: a ghost town's shell, a tent). The door sits on the
// wall facing its street, (dx, dy) in the town's frame with an outward normal (nx, ny).
enum Room { kNone, kStore, kSaloon, kFreight, kHotel, kSmith, kBank, kLaw, kChurch, kHouse };
struct Building { double lx, ly, w, d; Color c; bool big; Room room = kNone; const char* name = ""; double dx = 0, dy = 0, nx = 0, ny = 0; };

// A piece of river near the town, in miles, with half its width (water and bank) in feet. Streets stop at the water and
// houses keep off the bank: a town sits beside its river, not in it.
struct RiverSeg { double ax, ay, bx, by, half; };

// Feet of dry ground between a point (miles) and the nearest bank; negative in the water.
double river_clearance(const std::vector<RiverSeg>& rs, double x, double y) {
    double best = 1e9;
    for (const RiverSeg& r : rs) {
        const double dx = r.bx - r.ax, dy = r.by - r.ay, len2 = dx * dx + dy * dy;
        const double t = len2 > 0 ? std::clamp(((x - r.ax) * dx + (y - r.ay) * dy) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (r.ax + dx * t), y - (r.ay + dy * t)) * kFeet - r.half);
    }
    return best;
}

// The town's streets and buildings, from its size and id: a main street toward its busiest road, cross streets, houses
// thinning outward. Local feet: x along the main street, y across it.
void ground_town(const Node& n, std::size_t idx, double theta, const std::vector<RiverSeg>& rivers, std::vector<Building>& out) {
    out.clear();
    const std::uint64_t id = mix64(static_cast<std::uint64_t>(idx) * 2654435761ULL + 17);
    const double pop = n.fixed_market ? 9000.0 : std::max(30.0, n.population);
    const double L = 220 + std::sqrt(pop) * 18;
    const bool tents = n.kind.find("camp") != std::string::npos || n.kind == "range";
    const double thin = n.ghost ? 0.35 : 1.0;
    const Color wood[] = {{128, 104, 78, 255}, {112, 92, 70, 255}, {146, 118, 84, 255}, {96, 82, 66, 255}, {160, 150, 132, 255}, {122, 70, 52, 255}};
    const Color tent[] = {{226, 216, 190, 255}, {210, 200, 172, 255}, {196, 186, 160, 255}};
    auto colour = [&](std::uint64_t h) {
        Color c = tents ? tent[h % 3] : wood[h % 6];
        if (n.ghost) c = lerp(c, {110, 108, 104, 255}, 0.6f);
        return c;
    };
    const int ns = static_cast<int>(L / 30);
    for (int k = -ns; k <= ns; ++k)
        for (int s = -1; s <= 1; s += 2) {
            const std::uint64_t h = hash3(id, k * 2 + (s > 0), 1);
            if (unit_of(h) >= 0.85 * thin * (1.0 - 0.65 * std::abs(k) / std::max(1, ns))) continue;
            const bool big = unit_of(mix64(h + 1)) < 0.12 && !tents;
            const double bw = (big ? 26 : 18) + unit_of(mix64(h + 2)) * 6, bd = (big ? 32 : 20) + unit_of(mix64(h + 3)) * 12;
            out.push_back({k * 30.0, s * (17 + bd / 2), bw, bd, colour(mix64(h + 4)), big});
        }
    const std::size_t nmain = out.size();
    const int nc = std::max(1, ns / 6);
    for (int j = -nc; j <= nc; ++j) {
        if (j == 0) continue;
        const double reach = (0.9 * L * (1.0 - static_cast<double>(std::abs(j)) / (nc + 1))) * 0.6 + 80;
        const int slots = static_cast<int>(reach / 26);
        for (int s = -1; s <= 1; s += 2)
            for (int m = 1; m <= slots; ++m)
                for (int side = -1; side <= 1; side += 2) {
                    const std::uint64_t h = hash3(id ^ 0xabcdef, j * 1000 + m * 2 + (side > 0), s + 5);
                    if (unit_of(h) >= 0.7 * thin * (1.0 - 0.5 * m / std::max(1, slots))) continue;
                    const double bw = 14 + unit_of(mix64(h + 2)) * 6, bd = 14 + unit_of(mix64(h + 3)) * 6;
                    out.push_back({j * 180.0 + side * (12 + bw / 2), s * (40 + m * 26.0), bd, bw, colour(mix64(h + 4)), false});
                }
    }
    const double c = std::cos(theta), s = std::sin(theta);   // keep houses off the water
    out.erase(std::remove_if(out.begin(), out.end(), [&](const Building& b) {
        return river_clearance(rivers, n.x + (b.lx * c - b.ly * s) / kFeet, n.y + (b.lx * s + b.ly * c) / kFeet) < 22;
    }), out.end());

    // Doors and trades. Main-street fronts nearest the middle get the town's trades in order of how soon a town needs them;
    // everything else on a cross street is a house. Camps have a trading tent and a saloon tent; ghost towns are shells.
    std::vector<Building*> mainst;
    for (Building& b : out) {
        if (std::fabs(b.ly) < 60) {
            mainst.push_back(&b);
            const double sg = b.ly < 0 ? -1 : 1;
            b.dx = b.lx; b.dy = b.ly - sg * b.d / 2; b.nx = 0; b.ny = -sg;
        } else {
            const double sg = b.lx - std::round(b.lx / 180.0) * 180.0 < 0 ? -1 : 1;   // which side of its cross street
            b.dx = b.lx - sg * b.w / 2; b.dy = b.ly; b.nx = -sg; b.ny = 0;
            if (!n.ghost && !tents) { b.room = kHouse; b.name = "house"; }
        }
    }
    if (n.ghost) return;
    std::sort(mainst.begin(), mainst.end(), [](const Building* a, const Building* b) { return std::fabs(a->lx) + (a->ly > 0) < std::fabs(b->lx) + (b->ly > 0); });
    if (tents) {
        const struct { Room r; const char* nm; } t[] = {{kStore, "trading tent"}, {kSaloon, "saloon tent"}};
        for (std::size_t i = 0; i < mainst.size() && i < 2; ++i) { mainst[i]->room = t[i].r; mainst[i]->name = t[i].nm; }
        return;
    }
    const bool post = n.kind == "fort";
    const struct { Room r; const char* nm; } seq[] = {
        {kStore, post ? "sutler's store" : "general store"}, {kSaloon, "saloon"}, {kFreight, "freight office"}, {kHotel, "hotel"},
        {kSmith, "blacksmith"}, {kBank, "bank"}, {kLaw, post ? "guardhouse" : "marshal's office"}, {kChurch, "church"}, {kSaloon, "saloon"}, {kStore, "dry goods"}};
    const std::size_t trades = std::min<std::size_t>({sizeof seq / sizeof seq[0], mainst.size(), static_cast<std::size_t>(2 + pop / 350)});
    for (std::size_t i = 0; i < mainst.size(); ++i) {
        if (i < trades) { mainst[i]->room = seq[i].r; mainst[i]->name = seq[i].nm; }
        else { mainst[i]->room = kHouse; mainst[i]->name = "house"; }
    }
}

void draw_town_ground(const Node& n, const GroundCam& cam, double theta, const std::vector<RiverSeg>& rivers, const std::vector<Building>& bs) {
    const double c = std::cos(theta), s = std::sin(theta);
    const float f = cam.pxft;
    auto at = [&](double lx, double ly) {
        return cam.to_screen(n.x + (lx * c - ly * s) / kFeet, n.y + (lx * s + ly * c) / kFeet);
    };
    const double pop = n.fixed_market ? 9000.0 : std::max(30.0, n.population);
    const double L = 220 + std::sqrt(pop) * 18;
    const int ns = static_cast<int>(L / 30), nc = std::max(1, ns / 6);
    const Color street{176, 152, 116, 255};
    // Streets are laid in 30-foot pieces so none is drawn over the water.
    auto lay = [&](double x0, double y0, double x1, double y1, double width) {
        const double len = std::hypot(x1 - x0, y1 - y0);
        const int pieces = std::max(1, static_cast<int>(len / 30));
        for (int i = 0; i < pieces; ++i) {
            const double a = static_cast<double>(i) / pieces, b = static_cast<double>(i + 1) / pieces;
            const double mx = x0 + (x1 - x0) * (a + b) / 2, my = y0 + (y1 - y0) * (a + b) / 2;
            if (river_clearance(rivers, n.x + (mx * c - my * s) / kFeet, n.y + (mx * s + my * c) / kFeet) < 0) continue;
            DrawLineEx(at(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a), at(x0 + (x1 - x0) * (b + 0.02), y0 + (y1 - y0) * (b + 0.02)), width * f, street);
        }
    };
    lay(-L - 40, 0, L + 40, 0, 34);                                                   // main street
    for (int j = -nc; j <= nc; ++j) {
        if (j == 0) continue;
        const double reach = (0.9 * L * (1.0 - static_cast<double>(std::abs(j)) / (nc + 1))) * 0.6 + 80;
        lay(j * 180.0, -reach - 40, j * 180.0, reach + 40, 22);
    }
    const float deg = static_cast<float>(-theta * 180.0 / 3.14159265);
    for (const Building& b : bs) {
        const Vector2 p = at(b.lx, b.ly);
        if (p.x < -80 || p.y < -80 || p.x > GetScreenWidth() + 80 || p.y > GetScreenHeight() + 80) continue;
        DrawRectanglePro({p.x + 0.9f * f, p.y + 1.1f * f, static_cast<float>(b.w) * f, static_cast<float>(b.d) * f}, {static_cast<float>(b.w) * f / 2, static_cast<float>(b.d) * f / 2}, deg, {0, 0, 0, 55});
        DrawRectanglePro({p.x, p.y, static_cast<float>(b.w) * f, static_cast<float>(b.d) * f}, {static_cast<float>(b.w) * f / 2, static_cast<float>(b.d) * f / 2}, deg, b.c);
        DrawRectanglePro({p.x, p.y, static_cast<float>(b.w) * f, 1.4f * f}, {static_cast<float>(b.w) * f / 2, 0.7f * f}, deg, lerp(b.c, {255, 255, 255, 255}, 0.22f));   // ridge
        if (b.room != kNone) {   // the door, on the wall that faces the street
            const Vector2 dp = at(b.dx, b.dy);
            const float dw = (b.nx == 0 ? 4.0f : 1.4f) * f, dh = (b.nx == 0 ? 1.4f : 4.0f) * f;
            DrawRectanglePro({dp.x, dp.y, dw, dh}, {dw / 2, dh / 2}, deg, {70, 44, 28, 255});
            if (f >= 4.5f && b.room != kHouse) {
                const int tw = TextWidth(b.name, 13);
                DrawRectangle(static_cast<int>(p.x) - tw / 2 - 4, static_cast<int>(p.y) - 10, tw + 8, 18, {40, 28, 18, 190});   // a sign board
                Text(b.name, static_cast<int>(p.x) - tw / 2, static_cast<int>(p.y) - 8, 13, {250, 240, 220, 235});
            }
        }
    }
    if (n.kind == "fort") {   // a stockade square beside the street, with a flagpole
        const double cx = 0, cy = 230, half = 150;
        const Vector2 q[4] = {at(cx - half, cy - half), at(cx + half, cy - half), at(cx + half, cy + half), at(cx - half, cy + half)};
        for (int i = 0; i < 4; ++i) DrawLineEx(q[i], q[(i + 1) % 4], 2.2f * f, {86, 62, 40, 255});
        for (int i = 0; i < 4; ++i) DrawCircleV(q[i], 5 * f, {96, 70, 46, 255});
        const Vector2 pole = at(cx, cy);
        DrawLineEx(pole, {pole.x, pole.y - 22 * f}, 0.5f * f, {60, 50, 40, 255});
        DrawRectangleV({pole.x, pole.y - 22 * f}, {7 * f, 4 * f}, {170, 40, 40, 255});
    }
}

double town_length(const Node& n) { return 220 + std::sqrt(n.fixed_market ? 9000.0 : std::max(30.0, n.population)) * 18; }

double street_reach(const Node& n, int street) {   // how far from the main street a cross street runs (feet)
    const double L = town_length(n);
    const int nc = std::max(1, static_cast<int>(L / 30) / 6);
    return street == 0 ? L : (0.9 * L * (1.0 - static_cast<double>(std::abs(street)) / (nc + 1))) * 0.6 + 80;
}

void make_folk(const Node& n, std::size_t idx, std::vector<Folk>& out) {
    out.clear();
    if (n.ghost) return;
    const std::uint64_t id = mix64(static_cast<std::uint64_t>(idx) * 40503ULL + 99);
    const double pop = n.fixed_market ? 9000.0 : std::max(30.0, n.population);
    const int count = std::clamp(static_cast<int>(std::sqrt(pop) * 2.4), 4, 160);
    const int nc = std::max(1, static_cast<int>(town_length(n) / 30) / 6);
    const bool camp = n.kind.find("camp") != std::string::npos || n.kind.find("mining") != std::string::npos;
    const bool ranchy = n.kind == "ranch town" || n.kind == "range" || n.kind == "farm town" || n.kind == "farm district";
    const Color shirts[] = {{92, 104, 140, 255}, {140, 96, 80, 255}, {96, 120, 92, 255}, {150, 140, 110, 255}, {110, 100, 120, 255}, {80, 80, 84, 255}, {160, 150, 140, 255}};
    const Color hats[] = {{92, 66, 44, 255}, {58, 48, 40, 255}, {120, 100, 74, 255}, {150, 130, 92, 255}};
    const Color hair[] = {{60, 40, 26, 255}, {150, 110, 60, 255}, {200, 190, 170, 255}, {34, 28, 24, 255}};
    for (int i = 0; i < count; ++i) {
        Folk f;
        f.h = mix64(id + static_cast<std::uint64_t>(i) * 7919);
        const double r = unit_of(f.h), q = unit_of(mix64(f.h + 1));
        // Most walk the main street; the rest turn up a cross street.
        if (q < 0.6) { f.street = 0; f.bound = town_length(n); f.u = (unit_of(mix64(f.h + 2)) - unit_of(mix64(f.h + 22))) * f.bound * 0.9; }   // thickest toward the middle
        else {
            int j = static_cast<int>(unit_of(mix64(f.h + 3)) * (2 * nc)) - nc;
            if (j >= 0) ++j;                                    // skip 0
            f.street = j; f.bound = street_reach(n, j); f.u = (unit_of(mix64(f.h + 2)) * 2 - 1) * f.bound * 0.8;
        }
        f.v = (unit_of(mix64(f.h + 4)) * 2 - 1) * (f.street == 0 ? 11.0 : 7.0);
        f.dir = unit_of(mix64(f.h + 5)) < 0.5 ? -1 : 1;
        f.speed = 3.8 + unit_of(mix64(f.h + 6)) * 2.6;
        f.pause = unit_of(mix64(f.h + 7)) < 0.4 ? unit_of(mix64(f.h + 8)) * 10 : 0;
        f.look.shirt = shirts[mix64(f.h + 9) % 7];
        f.look.hat = hats[mix64(f.h + 10) % 4];
        const double kind = unit_of(mix64(f.h + 11));
        if (n.garrison > 0 && kind < std::min(0.5, n.garrison / 80.0)) { f.role = "soldier"; f.look.shirt = {52, 66, 110, 255}; f.look.hat = {40, 50, 90, 255}; }
        else if (camp && kind < 0.7) { f.role = "miner"; f.look.shirt = {120, 100, 80, 255}; f.look.hat = {88, 76, 62, 255}; }
        else if (ranchy && kind < 0.55) { f.role = "hand"; f.look.hat = {176, 152, 104, 255}; }
        else if (n.kind == "railhead" && kind < 0.25) { f.role = "railman"; f.look.shirt = {54, 54, 62, 255}; f.look.hat = {40, 40, 46, 255}; }
        else if (kind < 0.16) { f.role = "child"; f.look.scale = 0.62f; f.look.hat_on = false; f.look.hat = hair[mix64(f.h + 12) % 4]; f.speed *= 1.2; }
        else if (kind < 0.42) { f.role = "woman"; f.look.hat_on = false; f.look.hat = unit_of(mix64(f.h + 13)) < 0.4 ? Color{214, 206, 188, 255} : hair[mix64(f.h + 12) % 4]; f.look.shirt = {120, 92, 112, 255}; }
        else if (r < 0.2) f.look.hat_on = false;
        out.push_back(f);
    }
}

// ---------------------------------------------------------------- inside a building

// A room drawn in feet, y down, the door in the middle of the bottom wall. The people in it are made from the town and
// the hour (a saloon fills at night); what the counter shows is read from the town's own shelf, orders and headlines, so
// the room never invents anything the sim does not hold.
struct Furn { float x, y, w, h; Color c; bool solid = true; };
struct Occupant { float x, y, deg; Look look; std::string role; };
struct Interior {
    Room room = kNone;
    const char* name = "";
    float rw = 30, rd = 24;
    Color floor{150, 118, 80, 255};
    std::vector<Furn> furn;
    std::vector<Occupant> people;
    float cx = 0, cy = 0;                 // where the counter is served from (the panel opens when the player is near)
    float px = 0, py = 0, heading = 90, stride = 0, dest_x = 0, dest_y = 0;
    bool walking = false;
    std::vector<NodeId> cand;             // the freight office: towns worth hauling to, nearest by road first
};

// The towns the freight board offers as destinations: the nearest founded ones that a road reaches, by days on the road, and
// the nearest eastern market at the end (the way out for cattle and hides).
void fill_candidates(const World& w, NodeId here, Interior& in) {
    in.cand.clear();
    const Node& h = w.nodes[World::idx(here)];
    std::vector<std::pair<double, NodeId>> v;
    NodeId east = -1;
    double east_d = 1e18;
    for (std::size_t i = 0; i < w.nodes.size(); ++i) {
        const Node& n = w.nodes[i];
        if (static_cast<NodeId>(i) == here || (!n.founded && !n.fixed_market) || n.ghost) continue;
        const double d = std::hypot(n.x - h.x, n.y - h.y);
        if (n.fixed_market) { if (d < east_d && route_info(w, here, static_cast<NodeId>(i)).ok) { east_d = d; east = static_cast<NodeId>(i); } continue; }
        v.push_back({d, static_cast<NodeId>(i)});
    }
    const std::size_t keep = std::min<std::size_t>(v.size(), 40);
    std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(keep), v.end());
    std::vector<std::pair<double, NodeId>> byroad;
    for (std::size_t i = 0; i < keep; ++i) {
        const RouteInfo r = route_info(w, here, v[i].second);
        if (r.ok) byroad.push_back({r.days, v[i].second});
    }
    std::sort(byroad.begin(), byroad.end());
    for (std::size_t i = 0; i < byroad.size() && i < 12; ++i) in.cand.push_back(byroad[i].second);
    if (east >= 0) in.cand.push_back(east);
}

Interior make_interior(const Node& n, std::size_t idx, const Building& b, std::size_t bi, double hour) {
    Interior in;
    in.room = b.room;
    in.name = b.name;
    const std::uint64_t h = hash3(mix64(static_cast<std::uint64_t>(idx) * 2654435761ULL + 17), static_cast<std::int64_t>(bi), 77);
    const Color wood{98, 66, 42, 255}, dark{70, 46, 30, 255}, iron{58, 58, 62, 255}, cloth{160, 60, 54, 255};
    const bool camp = n.kind.find("camp") != std::string::npos;
    const bool ranchy = n.kind == "range" || n.kind == "ranch" || n.kind == "farm";
    int serial = 0;
    auto person = [&](float x, float y, const char* role, Color shirt, Color hat, bool hat_on = true, float scale = 1.0f) {
        Occupant o{x, y, 90.0f, Look{}, role};
        o.look.shirt = shirt; o.look.hat = hat; o.look.hat_on = hat_on; o.look.scale = scale;
        in.people.push_back(o);
        ++serial;
    };
    // A customer or guest: what a town's people are, by its kind.
    auto patron = [&](float x, float y) {
        const std::uint64_t k = mix64(h + 100 + static_cast<std::uint64_t>(serial));
        const double u = unit_of(k);
        if (n.garrison > 0 && u < 0.4) person(x, y, "soldier", {52, 66, 110, 255}, {40, 50, 90, 255});
        else if (camp && u < 0.7) person(x, y, "miner", {120, 100, 80, 255}, {88, 76, 62, 255});
        else if (ranchy && u < 0.6) person(x, y, "hand", {110, 100, 90, 255}, {176, 152, 104, 255});
        else if (u > 0.9) person(x, y, "woman", {120, 92, 112, 255}, {214, 206, 188, 255}, false);
        else person(x, y, "townsman", {static_cast<unsigned char>(80 + unit_of(k + 1) * 60), 84, 96, 255}, {92, 66, 44, 255}, unit_of(k + 2) < 0.8);
    };
    const bool late = hour >= 17 || hour < 2;
    switch (b.room) {
    case kStore:
        in.rw = 34; in.rd = 26; in.floor = {160, 128, 90, 255};
        in.furn = {{1, 0.5f, in.rw - 2, 2, wood}, {5, 9, in.rw - 10, 2.5f, dark}, {2, 15, 3, 3, {110, 80, 50, 255}}, {in.rw - 5, 15, 3, 3, {110, 80, 50, 255}}, {in.rw - 9, 3.5f, 4, 3, {150, 120, 80, 255}}};
        person(in.rw / 2, 7, "storekeeper", {196, 190, 170, 255}, {60, 50, 40, 255}, false);
        in.cx = in.rw / 2; in.cy = 12.5f;
        for (int i = 0, k = static_cast<int>(unit_of(h) * 3); i < k; ++i) patron(8 + unit_of(mix64(h + 5 + i)) * (in.rw - 16), 15 + unit_of(mix64(h + 9 + i)) * 6);
        break;
    case kSaloon:
        in.rw = 38; in.rd = 28; in.floor = {138, 104, 70, 255};
        in.furn = {{4, 0.5f, in.rw - 12, 1.5f, wood}, {4, 5, in.rw - 12, 2.5f, dark}, {in.rw - 6, 1, 4, 5, iron}};
        person(in.rw / 2 - 2, 3.4f, "bartender", {230, 226, 214, 255}, {30, 30, 30, 255}, false);
        in.cx = in.rw / 2 - 2; in.cy = 9;
        for (int t = 0; t < 4; ++t) {
            const float tx = t % 2 ? in.rw - 11 : 6, ty = t < 2 ? 14.0f : 21.0f;
            in.furn.push_back({tx, ty, 3, 3, {120, 86, 54, 255}});
            if (unit_of(mix64(h + 20 + t)) < (late ? 0.9 : 0.35)) { patron(tx - 1.2f, ty + 1.5f); patron(tx + 4.2f, ty + 1.5f); }
        }
        for (int i = 0, k = late ? 2 : 0; i < k; ++i) patron(8 + i * 3.0f, 10.5f);   // leaning on the bar
        break;
    case kFreight:
        in.rw = 28; in.rd = 22; in.floor = {152, 126, 92, 255};
        in.furn = {{4, 7, in.rw - 8, 2.5f, dark}, {in.rw / 2 - 5, 0.3f, 10, 0.8f, {236, 228, 200, 255}, false}, {2, 13, 4, 4, {120, 92, 60, 255}}, {in.rw - 8, 12, 5, 5, {120, 92, 60, 255}}};
        person(in.rw / 2, 5.2f, "freight clerk", {200, 196, 180, 255}, {40, 40, 40, 255}, false);
        in.cx = in.rw / 2; in.cy = 11;
        break;
    case kHotel:
        in.rw = 32; in.rd = 26; in.floor = {146, 110, 78, 255};
        in.furn = {{in.rw - 14, 5, 10, 2.5f, dark}, {1, 1, 6, 13, {120, 90, 60, 255}}, {10, 14, 8, 5, cloth, false}, {in.rw - 6, 14, 3, 2, {120, 86, 54, 255}}};
        person(in.rw - 9, 3.5f, "desk clerk", {210, 206, 194, 255}, {40, 40, 40, 255}, false);
        in.cx = in.rw - 9; in.cy = 9;
        for (int i = 0, k = static_cast<int>(unit_of(h) * 3); i < k; ++i) patron(10 + unit_of(mix64(h + 5 + i)) * 12, 14 + unit_of(mix64(h + 9 + i)) * 7);
        break;
    case kSmith:
        in.rw = 30; in.rd = 24; in.floor = {112, 100, 88, 255};
        in.furn = {{in.rw - 8, 0.5f, 7, 4, {120, 52, 30, 255}}, {in.rw / 2 - 1, 10, 2, 1.5f, iron}, {0.5f, 2, 1.5f, 10, wood}, {4, 16, 6, 3, {96, 78, 58, 255}}};
        person(in.rw / 2, 8, "blacksmith", {110, 96, 84, 255}, {40, 30, 24, 255}, false);
        in.cx = in.rw / 2; in.cy = 12.5f;
        break;
    case kBank:
        in.rw = 26; in.rd = 22; in.floor = {172, 156, 128, 255};
        in.furn = {{2, 8, in.rw - 4, 2, dark}, {in.rw - 6, 1, 5, 4, iron}, {2, 8, 0.4f, 0.1f, dark, false}};
        person(in.rw / 2, 5.5f, "banker", {40, 40, 44, 255}, {30, 30, 30, 255});
        in.cx = in.rw / 2; in.cy = 12;
        break;
    case kLaw:
        in.rw = 26; in.rd = 22; in.floor = {148, 120, 86, 255};
        in.furn = {{4, 9, 8, 3, dark}, {in.rw - 9.5f, 0.5f, 9, 7.5f, {60, 60, 66, 255}}, {1.5f, 1.5f, 2.5f, 2.5f, iron}};
        person(8, 7.5f, n.law >= 2 ? "county deputy" : n.garrison > 0 ? "sergeant of the guard" : "town marshal", {70, 76, 96, 255}, {60, 46, 34, 255});
        in.cx = 8; in.cy = 14;
        break;
    case kChurch:
        in.rw = 30; in.rd = 36; in.floor = {170, 142, 106, 255};
        in.furn = {{in.rw / 2 - 2, 2, 4, 2, wood}};
        for (int r = 0; r < 4; ++r) { in.furn.push_back({4, 12.0f + r * 5, 8, 1.6f, {118, 84, 54, 255}}); in.furn.push_back({in.rw - 12, 12.0f + r * 5, 8, 1.6f, {118, 84, 54, 255}}); }
        person(in.rw / 2, 5, "preacher", {30, 30, 34, 255}, {30, 30, 30, 255}, false);
        in.cx = in.rw / 2; in.cy = 9;
        break;
    default:   // a house: a bed, a table and a stove
        in.rw = 18; in.rd = 16; in.floor = {156, 124, 90, 255};
        in.furn = {{1, 1, 6, 4, cloth}, {10, 8, 4, 3, {120, 86, 54, 255}}, {1, 10, 2.5f, 2.5f, iron}};
        in.cx = 8; in.cy = 8;
        if (unit_of(h + 1) < 0.7) person(11, 5, "woman", {120, 92, 112, 255}, {214, 206, 188, 255}, false);
        if (unit_of(h + 2) < 0.35) person(9, 11.5f, "child", {150, 120, 90, 255}, {110, 80, 50, 255}, false, 0.62f);
        break;
    }
    in.px = in.rw / 2; in.py = in.rd - 3.5f; in.dest_x = in.px; in.dest_y = in.py;
    return in;
}

bool interior_clear(const Interior& in, float x, float y) {
    if (x < 1.4f || x > in.rw - 1.4f || y < 1.4f || y > in.rd + 0.5f) return false;
    if (y > in.rd - 1.4f && std::abs(x - in.rw / 2) > 2.2f) return false;           // the wall either side of the door
    for (const Furn& f : in.furn)
        if (f.solid && x > f.x - 1.0f && x < f.x + f.w + 1.0f && y > f.y - 1.0f && y < f.y + f.h + 1.0f) return false;
    for (const Occupant& o : in.people)
        if (std::hypot(x - o.x, y - o.y) < 1.4f) return false;
    return true;
}

// Draws the room and its people, then the counter panel. Returns the labels (screen position, role) for the hover.
void draw_interior(Simulation& sim, Player& pl, std::size_t idx, const Interior& in, double hour, std::vector<std::pair<Vector2, std::string>>& labels) {
    const World& w = sim.world();
    const Node& n = w.nodes[idx];
    const int W = GetScreenWidth(), H = GetScreenHeight();
    const float area = static_cast<float>(W - 360);
    const float f = std::min((area - 40) / in.rw, (H - 130.0f) / in.rd);
    const float ox = (area - in.rw * f) / 2 + 10, oy = 76;
    ClearBackground({34, 26, 20, 255});
    DrawRectangle(0, 0, W, H, {34, 26, 20, 255});
    DrawRectangle(static_cast<int>(ox), static_cast<int>(oy), static_cast<int>(in.rw * f), static_cast<int>(in.rd * f), in.floor);
    for (float x = 1.2f; x < in.rw; x += 1.2f)   // floorboards
        DrawLineEx({ox + x * f, oy}, {ox + x * f, oy + in.rd * f}, 1.0f, lerp(in.floor, {0, 0, 0, 255}, 0.13f));
    const Color wall{82, 58, 40, 255};
    const float t = 1.1f * f;
    DrawRectangle(static_cast<int>(ox - t), static_cast<int>(oy - t), static_cast<int>(in.rw * f + 2 * t), static_cast<int>(t), wall);
    DrawRectangle(static_cast<int>(ox - t), static_cast<int>(oy), static_cast<int>(t), static_cast<int>(in.rd * f), wall);
    DrawRectangle(static_cast<int>(ox + in.rw * f), static_cast<int>(oy), static_cast<int>(t), static_cast<int>(in.rd * f), wall);
    DrawRectangle(static_cast<int>(ox - t), static_cast<int>(oy + in.rd * f), static_cast<int>((in.rw / 2 - 2.2f) * f + t), static_cast<int>(t), wall);
    DrawRectangle(static_cast<int>(ox + (in.rw / 2 + 2.2f) * f), static_cast<int>(oy + in.rd * f), static_cast<int>((in.rw / 2 - 2.2f) * f + t), static_cast<int>(t), wall);
    DrawRectangle(static_cast<int>(ox + (in.rw / 2 - 2.2f) * f), static_cast<int>(oy + (in.rd - 0.5f) * f), static_cast<int>(4.4f * f), static_cast<int>(0.5f * f), {120, 40, 36, 255});   // the mat
    for (const Furn& fu : in.furn) {
        const Rectangle r{ox + fu.x * f, oy + fu.y * f, fu.w * f, fu.h * f};
        DrawRectangleRec({r.x + 0.3f * f, r.y + 0.4f * f, r.width, r.height}, {0, 0, 0, 60});
        DrawRectangleRec(r, fu.c);
        DrawRectangleLinesEx(r, 1.0f, lerp(fu.c, {0, 0, 0, 255}, 0.4f));
    }
    if (in.room == kLaw) for (float x = in.rw - 9; x < in.rw - 0.5f; x += 1.2f) DrawLineEx({ox + x * f, oy + 0.5f * f}, {ox + x * f, oy + 8 * f}, 1.2f, {150, 150, 156, 255});
    if (in.room == kSmith) DrawCircleV({ox + (in.rw - 4.5f) * f, oy + 2.5f * f}, 1.3f * f, {255, 150, 50, 200});
    if (in.room == kBank) for (float x = 2.5f; x < in.rw - 2; x += 1.1f) DrawLineEx({ox + x * f, oy + 6.2f * f}, {ox + x * f, oy + 8 * f}, 1.0f, {190, 160, 90, 255});
    for (const Occupant& o : in.people) {
        const Vector2 sp{ox + o.x * f, oy + o.y * f};
        draw_person(sp, o.deg, f, 0, o.look);
        labels.push_back({sp, o.role});
    }
    // Lamplight after dark, less of the daylight tint than outside.
    const double day = std::clamp(0.5 + 0.9 * std::cos((hour - 13.0) / 12.0 * 3.14159265), 0.0, 1.0);
    const unsigned char a = static_cast<unsigned char>((1.0 - day) * 70);
    if (a > 0) DrawRectangle(0, 0, W - 360, H, {40, 24, 10, a});

    // The counter: what the shop holds, read from the town.
    const int x = W - 350, y = 70;
    const bool near = std::hypot(in.px - in.cx, in.py - in.cy) < 6.0f;
    panel(x, y, 340, near || in.room == kHouse ? 500 : 64);
    int ty = y + 8;
    auto line = [&](const std::string& s, Color c = {40, 30, 20, 255}) { Text(s.c_str(), x + 10, ty, 15, c); ty += 19; };
    auto para = [&](const std::string& s, Color c = {40, 30, 20, 255}) { for (const std::string& l : wrap(s, 13, 318)) { Text(l.c_str(), x + 10, ty, 13, c); ty += 16; } ty += 3; };
    char b[160];
    line(std::string(in.name) + ", " + n.name, {20, 20, 20, 255});
    if (!near && in.room != kHouse) { para("Step up to the counter. The door is the red mat."); }
    auto goods_table = [&](bool all) {
        cells(x + 10, ty, 14, {90, 70, 40, 255}, {{"good", 0, false}, {"on the shelf", 190, true}, {"price", 320, true}});
        ty += 18;
        int shown = 0;
        for (std::size_t g = 0; g < w.goods.size() && shown < 12; ++g) {
            if (n.stock[g] < 1 || w.goods[g].money) continue;
            if (!all && w.goods[g].name != "tools" && w.goods[g].name != "ammunition") continue;
            std::snprintf(b, sizeof b, "%.0f", n.stock[g]);
            char pr[24];
            std::snprintf(pr, sizeof pr, "$%.2f", w.mid_price(static_cast<NodeId>(idx), static_cast<GoodId>(g)));
            cells(x + 10, ty, 14, {40, 30, 20, 255}, {{w.goods[g].name, 0, false}, {b, 190, true}, {pr, 320, true}});
            ty += 18; ++shown;
        }
        if (!shown) para("The shelves are bare.", {150, 40, 30, 255});
    };
    if (!near && in.room != kHouse) return;
    switch (in.room) {
    case kStore: {
        Trader* t = player_trader(sim, pl);
        const bool here_ok = t && !t->travelling && t->at == static_cast<NodeId>(idx);
        if (!t) { para("You have no outfit. Leave and come back in to start again.", {150, 40, 30, 255}); break; }
        std::snprintf(b, sizeof b, "you: $%.0f in hand", t->cash);
        line(b, {30, 90, 50, 255});
        if (!here_ok) { para("Your wagon is not in this town."); break; }
        if (t->cargo >= 0) {
            std::snprintf(b, sizeof b, "on the wagon: %.0f %s (cost $%.2f a unit)", t->qty, w.goods[World::idx(t->cargo)].name.c_str(), t->unit_cost);
            line(b);
            std::snprintf(b, sizeof b, "here it would fetch about $%.0f", w.quote_sell(static_cast<NodeId>(idx), t->cargo, t->qty));
            line(b);
            if (button({static_cast<float>(x + 10), static_cast<float>(ty), 150, 22}, "Sell it here")) {
                const Receipt r = sim.act_sell(*t);
                if (r.result == ActionResult::Ok) std::snprintf(b, sizeof b, "Sold %.0f units for $%.0f.", r.units, r.money);
                else std::snprintf(b, sizeof b, "Could not sell (%s).", action_result_name(r.result));
                pl.msg = b;
            }
            ty += 30;
        } else {
            cells(x + 10, ty, 14, {90, 70, 40, 255}, {{"good (click to pick)", 0, false}, {"shelf", 235, true}, {"price", 320, true}});
            ty += 18;
            int shown = 0;
            for (std::size_t g = 0; g < w.goods.size() && shown < 9; ++g) {
                const GoodDef& gd = w.goods[g];
                const double stock = n.fixed_market ? 1e9 : n.stock[g];
                if (gd.self_propelled != pl.drover || stock < 1 || gd.money) continue;
                const double units = std::min(t->capacity_lb / gd.load_lb, stock);
                char sh[24], pr[24];
                if (n.fixed_market) std::snprintf(sh, sizeof sh, "plenty"); else std::snprintf(sh, sizeof sh, "%.0f", stock);
                std::snprintf(pr, sizeof pr, "$%.2f", w.quote_buy(static_cast<NodeId>(idx), static_cast<GoodId>(g), units) / std::max(1.0, units));
                const Rectangle row{static_cast<float>(x + 4), static_cast<float>(ty - 1), 332.0f, 17.0f};
                const bool hot = CheckCollisionPointRec(GetMousePosition(), row);
                if (pl.sel == static_cast<GoodId>(g)) DrawRectangleRec(row, {230, 210, 160, 255});
                else if (hot) DrawRectangleRec(row, {240, 228, 200, 255});
                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) pl.sel = static_cast<GoodId>(g);
                cells(x + 10, ty, 14, {40, 30, 20, 255}, {{gd.name, 0, false}, {sh, 235, true}, {pr, 320, true}});
                ty += 18; ++shown;
            }
            if (!shown) para("Nothing on the shelves you could haul.", {150, 40, 30, 255});
            if (button({static_cast<float>(x + 10), static_cast<float>(ty + 2), 160, 22}, "Buy a full load", pl.sel >= 0)) {
                // Keep back the freight and the guards for the road ahead, and a little spending money.
                const RouteInfo ri = pl.dest >= 0 ? route_info(w, t->at, pl.dest) : RouteInfo{};
                double reserve = std::max(150.0, 0.2 * t->cash);
                if (ri.ok) reserve = 100 + (w.freight_unit_cost(pl.sel, ri.miles) * w.params.owner_haul_share) * t->capacity_lb / w.goods[World::idx(pl.sel)].load_lb + w.params.guard_wage * pl.guards * ri.days;
                const Receipt r = sim.act_buy(*t, pl.sel, 1e9, std::max(0.0, t->cash - reserve));
                if (r.result == ActionResult::Ok) std::snprintf(b, sizeof b, "Bought %.0f units for $%.0f. Now see the freight office to haul it.", r.units, r.money);
                else std::snprintf(b, sizeof b, "Could not buy (%s).", action_result_name(r.result));
                pl.msg = b;
            }
            ty += 32;
        }
        if (!pl.msg.empty()) para(pl.msg, {90, 70, 40, 255});
        break;
    }
    case kSmith: para("The smith sells what he makes and mends what you bring."); goods_table(false); break;
    case kSaloon: {
        const GoodId wh = find_good(w, "whiskey");
        if (wh >= 0) { std::snprintf(b, sizeof b, "Whiskey, $%.2f a bottle.", w.mid_price(static_cast<NodeId>(idx), wh)); line(b); }
        ty += 4;
        line("What they are saying", {90, 70, 40, 255});
        int said = 0;
        const auto& news = sim.paper_headlines(static_cast<NodeId>(idx));
        for (std::size_t k = news.size(); k-- > 0 && said < 3; ++said) para(format_date(news[k]->day) + "  " + news[k]->text, {40, 30, 20, 255});
        if (n.starving) { para("\"The shelves have been bare and the children are thin.\"", {150, 40, 30, 255}); ++said; }
        if (n.law == 0) { para("\"No law here. The miners' meeting settles what it can.\""); ++said; }
        if (n.captor >= 0) { para("\"The gang runs this town and takes its cut.\"", {150, 40, 30, 255}); ++said; }
        if (!said) para("Nobody has much to say today.");
        break;
    }
    case kFreight: {
        Trader* t = player_trader(sim, pl);
        const bool here_ok = t && !t->travelling && t->at == static_cast<NodeId>(idx);
        if (!t) { para("You have no outfit. Leave and come back in to start again.", {150, 40, 30, 255}); break; }
        if (!here_ok) { para("Your wagon is not in this town."); break; }
        if (t->cargo >= 0) std::snprintf(b, sizeof b, "you: $%.0f, hauling %.0f %s", t->cash, t->qty, w.goods[World::idx(t->cargo)].name.c_str());
        else std::snprintf(b, sizeof b, "you: $%.0f, wagon empty", t->cash);
        line(b, {30, 90, 50, 255});
        // What other towns are asking for on their boards: a posted order pays the higher of its price and the shelf price.
        line("Wanted elsewhere (click to pick)", {90, 70, 40, 255});
        int open = 0;
        for (NodeId cn : in.cand) {
            if (open >= 4) break;
            const Node& c = w.nodes[World::idx(cn)];
            if (c.fixed_market) continue;
            for (std::size_t g = 0; g < w.goods.size() && open < 4; ++g) {
                if (c.order_qty[g] < 5 || w.goods[g].self_propelled != pl.drover) continue;
                if (t->cargo >= 0 && t->cargo != static_cast<GoodId>(g)) continue;
                const RouteInfo ri = route_info(w, t->at, cn);
                char row[120];
                std::snprintf(row, sizeof row, "%s: %.0f %s @ $%.2f  (%.0f d)", c.name.c_str(), c.order_qty[g], w.goods[g].name.c_str(), c.order_price[g], ri.days);
                const Rectangle rr{static_cast<float>(x + 4), static_cast<float>(ty - 1), 332.0f, 17.0f};
                const bool hot = CheckCollisionPointRec(GetMousePosition(), rr);
                if (pl.dest == cn) DrawRectangleRec(rr, {230, 210, 160, 255});
                else if (hot) DrawRectangleRec(rr, {240, 228, 200, 255});
                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { pl.dest = cn; if (t->cargo < 0) pl.sel = static_cast<GoodId>(g); }
                Text(row, x + 10, ty, 13, {40, 30, 20, 255});
                ty += 17; ++open;
            }
        }
        if (!open) { Text("no open orders near here for your load", x + 10, ty, 13, {110, 100, 90, 255}); ty += 17; }
        ty += 4;
        line("Where to (click to pick)", {90, 70, 40, 255});
        int rows = 0;
        for (NodeId cn : in.cand) {
            if (rows >= 6) break;
            const Node& c = w.nodes[World::idx(cn)];
            const RouteInfo ri = route_info(w, t->at, cn);
            if (!ri.ok) continue;
            char est[32] = "";
            if (t->cargo >= 0) {
                const double sale = w.quote_sell(cn, t->cargo, t->qty);
                std::snprintf(est, sizeof est, "%+.0f", sale - t->qty * t->unit_cost - (w.freight_unit_cost(t->cargo, ri.miles) * w.params.owner_haul_share) * t->qty);
            }
            char days[24], risk[24];
            std::snprintf(days, sizeof days, "%.0f d", ri.days);
            std::snprintf(risk, sizeof risk, "%.0f%% robbed", ri.robbed * 100);
            const Rectangle rr{static_cast<float>(x + 4), static_cast<float>(ty - 1), 332.0f, 17.0f};
            const bool hot = CheckCollisionPointRec(GetMousePosition(), rr);
            if (pl.dest == cn) DrawRectangleRec(rr, {230, 210, 160, 255});
            else if (hot) DrawRectangleRec(rr, {240, 228, 200, 255});
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) pl.dest = cn;
            cells(x + 10, ty, 13, ri.robbed > 0.25 ? Color{170, 30, 30, 255} : Color{40, 30, 20, 255}, {{c.name, 0, false}, {days, 190, true}, {risk, 270, true}, {est, 328, true}});
            ty += 17; ++rows;
        }
        ty += 6;
        std::snprintf(b, sizeof b, "guards: %d  ($%.1f a day)", pl.guards, w.params.guard_wage * pl.guards);
        Text(b, x + 10, ty + 3, 13, {40, 30, 20, 255});
        if (button({static_cast<float>(x + 200), static_cast<float>(ty), 26, 22}, "-", pl.guards > 0)) pl.guards = std::max(0, pl.guards - w.params.guard_step);
        if (button({static_cast<float>(x + 232), static_cast<float>(ty), 26, 22}, "+", pl.guards < w.params.max_guards)) pl.guards = std::min(w.params.max_guards, pl.guards + w.params.guard_step);
        ty += 28;
        const bool can_go = pl.dest >= 0 && pl.dest != t->at && route_info(w, t->at, pl.dest).ok;
        const std::string go_label = pl.dest >= 0 ? std::string("Set off for ") + w.nodes[World::idx(pl.dest)].name : std::string("Pick a destination");
        if (button({static_cast<float>(x + 10), static_cast<float>(ty), 200, 24}, go_label.c_str(), can_go)) {
            const ActionResult r = sim.act_travel(*t, pl.dest, pl.guards);
            if (r == ActionResult::Ok) { pl.msg = "On the road."; pl.departing = true; }
            else pl.msg = std::string("Cannot set off (") + action_result_name(r) + ").";
        }
        ty += 30;
        if (!pl.msg.empty()) para(pl.msg, {90, 70, 40, 255});
        break;
    }
    case kHotel:
        std::snprintf(b, sizeof b, "Rooms upstairs. %.0f people in town.", n.population);
        para(b);
        break;
    case kBank:
        std::snprintf(b, sizeof b, "The town holds $%.0f in cash.", n.cash);
        para(b);
        if (n.cash < 200) para("\"Money is tight. We pay out in dust.\"");
        break;
    case kLaw: {
        static const char* const kLaw[] = {"There is no officer; the miners' meeting rules.", "A town marshal keeps the peace here.", "The county sheriff keeps deputies here."};
        para(kLaw[std::clamp(n.law, 0, 2)]);
        if (n.garrison > 0) { std::snprintf(b, sizeof b, "Garrison: %.0f men.", n.garrison); para(b); }
        break;
    }
    case kChurch: para("A plain church. The preacher greets whoever comes in."); break;
    default: para("Someone's home. They look you over and say nothing."); break;
    }
}

// Runs the ground view until the player goes back to the map (returns false) or closes the window (returns true).
bool run_ground(Simulation& sim, const Terrain& terrain, Player& player, NodeId start_at, long end_day, bool& departed) {
    const World& w = sim.world();
    departed = false;
    // A drifter arriving on foot with no outfit of his own gets one: from here on he is a real trader the sim leaves alone.
    if (!player.active() || player.dead || !sim.find_trader(player.id)) player_spawn(sim, player, start_at, false, kDrifterCash);
    player.departing = false;
    const Texture2D patches = make_noise(256, 6, 11, {86, 110, 54, 255}, {176, 170, 106, 255}, 110);
    const Texture2D grain = make_noise(256, 48, 29, {50, 64, 34, 255}, {206, 204, 156, 255}, 70);
    GroundCam cam;
    cam.pxft = g_ground_zoom;
    const Node& home = w.nodes[World::idx(start_at)];
    // Step off the main street, as a stranger arriving at the edge of town would.
    double me_x = home.x, me_y = home.y - 60.0 / kFeet;
    cam.cx = me_x; cam.cy = me_y;
    double dest_x = me_x, dest_y = me_y, heading = 0, stride = 0;
    bool walking = false, show_town = false;
    double clock = 7 * 60;               // minutes since midnight; the drifter arrives at seven in the morning
    const double day_seconds = 1000.0;   // real seconds in a game day at speed 1 (RimWorld's pace)
    const double rates[] = {1, 2, 3, 6};
    int speed = 0;
    bool paused = false;
    int shot_frames = 0;
    std::vector<Building> buildings;
    std::vector<RiverSeg> near_rivers;
    std::vector<Folk> folk;
    Interior room;                    // the room the drifter is in, when inside >= 0 (an index into buildings)
    int inside = -1, enter_bi = -1;   // enter_bi: the door he is walking to
    std::vector<std::pair<Vector2, std::string>> labels;   // who is who, shown for the person under the mouse
    NodeId built_for = -1;
    double theta = 0;
    // Per-edge bounding boxes (miles), so the roads near the player can be found without walking every path.
    struct Box { double x0, y0, x1, y1; };
    std::vector<Box> ebox(w.edges.size()), rbox(w.rivers.size());
    for (std::size_t i = 0; i < w.edges.size(); ++i) {
        const Edge& e = w.edges[i];
        const Node& a = w.nodes[World::idx(e.a)];
        const Node& b = w.nodes[World::idx(e.b)];
        Box bx{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
        for (const auto& pt : e.path) { bx.x0 = std::min(bx.x0, pt.first); bx.x1 = std::max(bx.x1, pt.first); bx.y0 = std::min(bx.y0, pt.second); bx.y1 = std::max(bx.y1, pt.second); }
        ebox[i] = bx;
    }
    for (std::size_t i = 0; i < w.rivers.size(); ++i) {
        Box bx{1e18, 1e18, -1e18, -1e18};
        for (const auto& pt : w.rivers[i].points) { bx.x0 = std::min(bx.x0, pt.first); bx.x1 = std::max(bx.x1, pt.first); bx.y0 = std::min(bx.y0, pt.second); bx.y1 = std::max(bx.y1, pt.second); }
        rbox[i] = bx;
    }

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_M) || IsKeyPressed(KEY_BACKSPACE)) break;
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        for (int k = 0; k < 4; ++k) if (IsKeyPressed(KEY_ONE + k)) { speed = k; paused = false; }
        if (IsKeyPressed(KEY_E)) show_town = !show_town;
        const float wheel = GetMouseWheelMove();
        if (wheel != 0) cam.pxft = std::clamp(cam.pxft * (wheel > 0 ? 1.15f : 1 / 1.15f), 1.5f, 16.0f);
        const double dt = std::min(GetFrameTime(), 0.1f);
        const double mult = paused ? 0.0 : rates[speed];

        // ---- walk: click to go, or WASD
        if (inside < 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !(show_town && GetMouseX() > GetScreenWidth() - 340)) {
            const Vector2 t = cam.to_world(GetMousePosition());
            dest_x = t.x; dest_y = t.y; walking = true; enter_bi = -1;
            if (built_for >= 0) {   // a click on a building with a door means: go in
                const Node& tn = w.nodes[World::idx(built_for)];
                const double c = std::cos(theta), s = std::sin(theta);
                const double ddx = (t.x - tn.x) * kFeet, ddy = (t.y - tn.y) * kFeet;
                const double lx = ddx * c + ddy * s, ly = -ddx * s + ddy * c;
                for (std::size_t i = 0; i < buildings.size(); ++i) {
                    const Building& b = buildings[i];
                    if (b.room == kNone || std::abs(lx - b.lx) > b.w / 2 || std::abs(ly - b.ly) > b.d / 2) continue;
                    const double fx = b.dx + b.nx * 3.0, fy = b.dy + b.ny * 3.0;
                    dest_x = tn.x + (fx * c - fy * s) / kFeet; dest_y = tn.y + (fx * s + fy * c) / kFeet;
                    enter_bi = static_cast<int>(i);
                    break;
                }
            }
        }
        double ix = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1 : 0) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1 : 0);
        double iy = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1 : 0) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1 : 0);
        const double pace = 11.0 * mult * dt / kFeet;   // a steady walk is about 3 miles an hour: 4.4 ft/s; RimWorld pawns hurry, so 11 ft/s on screen
        // Solid ground test for the drifter: a house (with his own width added) or deep water stops him; a creek only slows him.
        const Node* town_now = built_for >= 0 ? &w.nodes[World::idx(built_for)] : nullptr;
        auto solid = [&](double x, double y) {
            if (!town_now) return false;
            const double c = std::cos(theta), s = std::sin(theta);
            const double dx = (x - town_now->x) * kFeet, dy = (y - town_now->y) * kFeet;
            const double lx = dx * c + dy * s, ly = -dx * s + dy * c;
            for (const Building& b : buildings)
                if (std::abs(lx - b.lx) < b.w / 2 + 1.4 && std::abs(ly - b.ly) < b.d / 2 + 1.4) return true;
            return false;
        };
        auto deep_water = [&](double x, double y) {
            for (const RiverSeg& r : near_rivers) {
                if (r.half < 40) continue;
                const double dx = r.bx - r.ax, dy = r.by - r.ay, len2 = dx * dx + dy * dy;
                const double t = len2 > 0 ? std::clamp(((x - r.ax) * dx + (y - r.ay) * dy) / len2, 0.0, 1.0) : 0.0;
                if (std::hypot(x - (r.ax + dx * t), y - (r.ay + dy * t)) * kFeet < r.half - 8) return true;
            }
            return false;
        };
        auto step_to = [&](double dx, double dy) {   // moves as far as the ground allows, sliding along walls
            const double nx = me_x + dx, ny = me_y + dy;
            auto ok = [&](double x, double y) { return !solid(x, y) && !deep_water(x, y); };
            if (ok(nx, ny)) { me_x = nx; me_y = ny; return true; }
            if (ok(nx, me_y)) { me_x = nx; return true; }
            if (ok(me_x, ny)) { me_y = ny; return true; }
            return false;
        };
        if (inside >= 0) {
            // ---- inside: his own small walk in feet, stopped by furniture and walls
            const double ipace = 6.0 * mult * dt;
            auto istep = [&](float dx, float dy) {
                if (interior_clear(room, room.px + dx, room.py + dy)) { room.px += dx; room.py += dy; return true; }
                if (interior_clear(room, room.px + dx, room.py)) { room.px += dx; return true; }
                if (interior_clear(room, room.px, room.py + dy)) { room.py += dy; return true; }
                return false;
            };
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && GetMouseX() < GetScreenWidth() - 360) {   // (a click on the counter panel is not a walk)
                const float area = static_cast<float>(GetScreenWidth() - 360);
                const float sc = std::min((area - 40) / room.rw, (GetScreenHeight() - 130.0f) / room.rd);
                room.dest_x = (GetMouseX() - ((area - room.rw * sc) / 2 + 10)) / sc;
                room.dest_y = (GetMouseY() - 76) / sc;
                room.walking = true;
            }
            if (ix != 0 || iy != 0) {
                const double len = std::hypot(ix, iy);
                istep(static_cast<float>(ix / len * ipace), static_cast<float>(-iy / len * ipace));
                room.heading = static_cast<float>(std::atan2(-iy, ix) * 180.0 / 3.14159265); room.stride += 9 * dt * mult; room.walking = false;
            } else if (room.walking) {
                const float dx = room.dest_x - room.px, dy = room.dest_y - room.py, dist = std::hypot(dx, dy);
                if (dist <= ipace) room.walking = false;
                else {
                    const float bx = room.px, by = room.py;
                    istep(static_cast<float>(dx / dist * ipace), static_cast<float>(dy / dist * ipace));
                    if (std::hypot(room.px - bx, room.py - by) < ipace * 0.2) room.walking = false;
                    room.heading = std::atan2(dy, dx) * 180.0f / 3.14159265f; room.stride += 9 * dt * mult;
                }
            }
            if (room.py > room.rd - 1.0f) {   // out the door: stand in the street in front of it
                const Building& b = buildings[static_cast<std::size_t>(inside)];
                const double c = std::cos(theta), s = std::sin(theta), fx = b.dx + b.nx * 4.0, fy = b.dy + b.ny * 4.0;
                const Node& tn = w.nodes[World::idx(built_for)];
                me_x = tn.x + (fx * c - fy * s) / kFeet; me_y = tn.y + (fx * s + fy * c) / kFeet;
                dest_x = me_x; dest_y = me_y; walking = false;
                cam.cx = me_x; cam.cy = me_y;
                inside = -1;
            }
        } else if (ix != 0 || iy != 0) {
            enter_bi = -1;
            const double len = std::hypot(ix, iy);
            step_to(ix / len * pace, iy / len * pace);
            heading = std::atan2(-iy, ix); stride += 9 * dt * mult; walking = false; dest_x = me_x; dest_y = me_y;
        } else if (walking) {
            const double dx = dest_x - me_x, dy = dest_y - me_y, dist = std::hypot(dx, dy);
            if (dist <= pace || dist < 1e-9) { me_x = dest_x; me_y = dest_y; walking = false; }
            else {
                const double bx = me_x, by = me_y;
                step_to(dx / dist * pace, dy / dist * pace);
                if (std::hypot(me_x - bx, me_y - by) < pace * 0.2) walking = false;   // walked into a wall: stop
                heading = std::atan2(-dy, dx); stride += 9 * dt * mult;
            }
        }
        if (inside < 0 && town_now) {   // arriving at the door he clicked on, or pressing F / Enter beside any door, goes in
            const double c = std::cos(theta), s = std::sin(theta);
            const double ddx = (me_x - town_now->x) * kFeet, ddy = (me_y - town_now->y) * kFeet;
            const double lx = ddx * c + ddy * s, ly = -ddx * s + ddy * c;
            int go = -1;
            for (std::size_t i = 0; i < buildings.size(); ++i) {
                const Building& b = buildings[i];
                if (b.room == kNone || std::hypot(lx - b.dx, ly - b.dy) > 6.5) continue;
                if ((enter_bi == static_cast<int>(i) && !walking) || IsKeyPressed(KEY_F) || IsKeyPressed(KEY_ENTER)) { go = static_cast<int>(i); break; }
            }
            if (!walking) enter_bi = -1;
            if (!g_ground_inside.empty())
                for (std::size_t i = 0; i < buildings.size(); ++i)
                    if (g_ground_inside == buildings[i].name) { go = static_cast<int>(i); g_ground_inside.clear(); break; }
            if (go >= 0) {
                inside = go;
                enter_bi = -1;
                room = make_interior(*town_now, static_cast<std::size_t>(built_for), buildings[static_cast<std::size_t>(go)], static_cast<std::size_t>(go), clock / 60.0);
                if (room.room == kFreight) fill_candidates(w, built_for, room);
                if (!g_ground_shot.empty()) { room.px = room.cx; room.py = room.cy + 1.0f; }   // screenshots start at the counter
            }
        }
        cam.cx += (me_x - cam.cx) * std::min(1.0, 6 * dt);   // the camera follows with a little lag
        cam.cy += (me_y - cam.cy) * std::min(1.0, 6 * dt);

        // ---- time: the simulation steps once per game day
        clock += dt * mult * 1440.0 / day_seconds;
        if (clock >= 1440) { clock -= 1440; if (w.today < end_day) sim.step(); }

        // The nearest founded town and how far off it is.
        int near = -1;
        double near_d = 1e18;
        for (std::size_t i = 0; i < w.nodes.size(); ++i) {
            const Node& n = w.nodes[i];
            if (!n.founded && !n.fixed_market) continue;
            const double d = std::hypot(n.x - me_x, n.y - me_y);
            if (d < near_d) { near_d = d; near = static_cast<int>(i); }
        }
        if (near >= 0 && static_cast<NodeId>(near) != built_for) {   // lay out the town and face its main street toward its busiest road
            built_for = static_cast<NodeId>(near);
            const Node& n = w.nodes[static_cast<std::size_t>(near)];
            double best = -1;
            theta = 0;
            for (EdgeId id : n.edges) {
                const Edge& e = w.edges[World::idx(id)];
                if (e.mode == Mode::River) continue;
                const Node& o = w.nodes[World::idx(e.other(built_for))];
                if (e.use_ema + 1e-6 > best) { best = e.use_ema + 1e-6; theta = std::atan2(o.y - n.y, o.x - n.x); }
            }
            near_rivers.clear();
            for (const RiverLine& r : w.rivers)
                for (std::size_t k = 1; k < r.points.size(); ++k) {
                    const auto &a = r.points[k - 1], &b = r.points[k];
                    if (std::min(a.first, b.first) > n.x + 3 || std::max(a.first, b.first) < n.x - 3 || std::min(a.second, b.second) > n.y + 3 || std::max(a.second, b.second) < n.y - 3) continue;
                    near_rivers.push_back({a.first, a.second, b.first, b.second, (r.size >= 3 ? 130.0 : r.size == 2 ? 45.0 : 7.0) + 8.0});
                }
            ground_town(n, static_cast<std::size_t>(near), theta, near_rivers, buildings);
            make_folk(n, static_cast<std::size_t>(near), folk);
        }

        // ---- draw
        BeginDrawing();
        labels.clear();
        const int W = GetScreenWidth(), H = GetScreenHeight();
        const float f = cam.pxft;
        const Vector2 tl = cam.to_world({0, 0}), br = cam.to_world({static_cast<float>(W), static_cast<float>(H)});
        const double vx0 = tl.x - 0.05, vx1 = br.x + 0.05, vy0 = br.y - 0.05, vy1 = tl.y + 0.05;   // the view in miles, with a margin
        DrawRectangleGradientEx({0, 0, static_cast<float>(W), static_cast<float>(H)},
                                terrain_color(terrain, tl.x, tl.y), terrain_color(terrain, tl.x, br.y), terrain_color(terrain, br.x, br.y), terrain_color(terrain, br.x, tl.y));
        // Texture over it: broad patches (about 250 ft across) and fine grain (about 25 ft), anchored to the ground.
        auto tile = [&](const Texture2D& t, double feet) {
            const double u = std::fmod(cam.cx * kFeet - W * 0.5 / f, feet) / feet * t.width;
            const double v = std::fmod(-cam.cy * kFeet - H * 0.5 / f, feet) / feet * t.height;
            DrawTexturePro(t, {static_cast<float>(u), static_cast<float>(v), static_cast<float>(W / (feet * f) * t.width), static_cast<float>(H / (feet * f) * t.height)},
                           {0, 0, static_cast<float>(W), static_cast<float>(H)}, {0, 0}, 0, WHITE);
        };
        tile(patches, 260);
        tile(grain, 26);
        // Tufts of grass and stones, one possible per 14-foot cell.
        {
            const double cell = 14;
            const std::int64_t i0 = static_cast<std::int64_t>(std::floor(tl.x * kFeet / cell)), i1 = static_cast<std::int64_t>(std::floor(br.x * kFeet / cell));
            const std::int64_t j0 = static_cast<std::int64_t>(std::floor(br.y * kFeet / cell)), j1 = static_cast<std::int64_t>(std::floor(tl.y * kFeet / cell));
            const Color base = terrain_color(terrain, cam.cx, cam.cy);
            const Color dark = lerp(base, {20, 30, 14, 255}, 0.45f);
            for (std::int64_t j = j0; j <= j1; ++j)
                for (std::int64_t i = i0; i <= i1; ++i) {
                    const std::uint64_t h = hash3(0x7f, i, j);
                    const double u = unit_of(h);
                    if (u > 0.34) continue;
                    const Vector2 p = cam.to_screen((i + unit_of(mix64(h + 1))) * cell / kFeet, (j + unit_of(mix64(h + 2))) * cell / kFeet);
                    if (u > 0.30) { DrawCircleV(p, 0.55f * f, {128, 124, 116, 255}); continue; }   // a stone
                    for (int b = 0; b < 3; ++b) {
                        const float a = static_cast<float>(-1.9 + b * 0.65 + unit_of(mix64(h + 3 + b)) * 0.3);
                        DrawLineEx(p, {p.x + std::cos(a) * 1.4f * f, p.y + std::sin(a) * 1.6f * f}, std::max(1.0f, 0.2f * f), dark);
                    }
                }
        }
        // Rivers.
        for (std::size_t i = 0; i < w.rivers.size(); ++i) {
            if (rbox[i].x1 < vx0 || rbox[i].x0 > vx1 || rbox[i].y1 < vy0 || rbox[i].y0 > vy1) continue;
            const RiverLine& r = w.rivers[i];
            const float width = (r.size >= 3 ? 260.0f : r.size == 2 ? 90.0f : 14.0f) * f;
            for (std::size_t k = 1; k < r.points.size(); ++k) {
                const Vector2 a = cam.to_screen(r.points[k - 1].first, r.points[k - 1].second), b = cam.to_screen(r.points[k].first, r.points[k].second);
                float t0, t1;
                if (!clip_to_screen(a, b, width, t0, t1)) continue;
                const Vector2 p0{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0}, p1{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1};
                DrawLineEx(p0, p1, width + 8 * f, {176, 160, 118, 255});     // muddy bank
                DrawLineEx(p0, p1, width, {64, 108, 150, 255});
                DrawLineEx(p0, p1, width * 0.6f, {80, 128, 170, 255});
            }
        }
        // Roads and rails near the view.
        for (std::size_t i = 0; i < w.edges.size(); ++i) {
            const Edge& e = w.edges[i];
            if (ebox[i].x1 < vx0 || ebox[i].x0 > vx1 || ebox[i].y1 < vy0 || ebox[i].y0 > vy1) continue;
            if (!e.active || e.mode == Mode::River || (e.latent && e.from == LONG_MAX)) continue;
            const Node& na = w.nodes[World::idx(e.a)];
            const Node& nb = w.nodes[World::idx(e.b)];
            if (e.path.size() >= 2) {
                for (std::size_t k = 1; k < e.path.size(); ++k)
                    ground_road(cam, cam.to_screen(e.path[k - 1].first, e.path[k - 1].second), cam.to_screen(e.path[k].first, e.path[k].second), e.mode, e.road);
            } else ground_road(cam, cam.to_screen(na.x, na.y), cam.to_screen(nb.x, nb.y), e.mode, e.road);
        }
        for (std::size_t i = 0; i < w.edges.size(); ++i) {   // trestles where a railway crosses a river
            const Edge& e = w.edges[i];
            if (e.mode != Mode::Rail || !e.active || (e.latent && e.from == LONG_MAX)) continue;
            if (ebox[i].x1 < vx0 || ebox[i].x0 > vx1 || ebox[i].y1 < vy0 || ebox[i].y0 > vy1) continue;
            std::vector<Vector2> way;
            if (e.path.size() >= 2) for (const auto& pt : e.path) way.push_back(cam.to_screen(pt.first, pt.second));
            else way = {cam.to_screen(w.nodes[World::idx(e.a)].x, w.nodes[World::idx(e.a)].y), cam.to_screen(w.nodes[World::idx(e.b)].x, w.nodes[World::idx(e.b)].y)};
            for (std::size_t r = 0; r < w.rivers.size(); ++r) {
                if (rbox[r].x1 < vx0 || rbox[r].x0 > vx1 || rbox[r].y1 < vy0 || rbox[r].y0 > vy1) continue;
                const RiverLine& rv = w.rivers[r];
                const float rpx = ((rv.size >= 3 ? 260.0f : rv.size == 2 ? 90.0f : 14.0f) + 8.0f) * f;
                for (std::size_t k = 1; k < way.size(); ++k)
                    for (std::size_t m = 1; m < rv.points.size(); ++m) {
                        const Vector2 c = cam.to_screen(rv.points[m - 1].first, rv.points[m - 1].second), d = cam.to_screen(rv.points[m].first, rv.points[m].second);
                        float t;
                        if (!seg_cross(way[k - 1], way[k], c, d, t)) continue;
                        const Vector2 at{way[k - 1].x + (way[k].x - way[k - 1].x) * t, way[k - 1].y + (way[k].y - way[k - 1].y) * t};
                        if (at.x < -400 || at.y < -400 || at.x > W + 400 || at.y > H + 400) continue;
                        const float rl = std::hypot(d.x - c.x, d.y - c.y);
                        if (rl > 0.01f) ground_trestle(cam, way[k - 1], way[k], at, rpx, {(d.x - c.x) / rl, (d.y - c.y) / rl});
                    }
            }
        }
        for (const CrossingSite& c : w.crossings) {   // a bridge: planks across the river
            if (c.x < vx0 || c.x > vx1 || c.y < vy0 || c.y > vy1) continue;
            const Vector2 p = cam.to_screen(c.x, c.y);
            DrawRectanglePro({p.x, p.y, 40 * f, 14 * f}, {20 * f, 7 * f}, 0, c.kind == "bridge" ? Color{96, 72, 50, 255} : Color{120, 100, 70, 160});
        }
        // Towns in view.
        for (std::size_t i = 0; i < w.nodes.size(); ++i) {
            const Node& n = w.nodes[i];
            if ((!n.founded && !n.fixed_market) || std::abs(n.x - cam.cx) > 2.5 || std::abs(n.y - cam.cy) > 2.5) continue;
            if (static_cast<NodeId>(i) != built_for) continue;   // only the town the player stands in has its layout built
            draw_town_ground(n, cam, theta, near_rivers, buildings);
        }
        // Wagons standing in the nearest town: the sim's own traders, parked along its main street.
        if (near >= 0 && near_d < 2.0) {
            const Node& n = w.nodes[static_cast<std::size_t>(near)];
            const double c = std::cos(theta), s = std::sin(theta);
            int shown = 0;
            for (const Trader& t : sim.traders()) {
                if (!t.alive || t.travelling || t.at != static_cast<NodeId>(near) || ++shown > 60) continue;
                const std::uint64_t h = mix64(static_cast<std::uint64_t>(t.id) * 7919 + 3);
                const double pop = std::max(30.0, n.population);
                const double L = 220 + std::sqrt(pop) * 18;
                const double lx = (unit_of(h) * 2 - 1) * L * 0.7, ly = (unit_of(mix64(h + 1)) < 0.5 ? -1 : 1) * 9.0;
                const Vector2 p = cam.to_screen(n.x + (lx * c - ly * s) / kFeet, n.y + (lx * s + ly * c) / kFeet);
                if (p.x < -120 || p.y < -120 || p.x > W + 120 || p.y > H + 120) continue;
                const float deg = static_cast<float>(-theta * 180.0 / 3.14159265) + (unit_of(mix64(h + 2)) < 0.5 ? 0.0f : 180.0f);
                draw_wagon(p, deg, f, trader_color(t), t.cargo >= 0);
                {   // the teamster stands by his wagon, a few feet toward the street
                    Look l; l.shirt = {110, 96, 80, 255}; l.hat = {70, 56, 42, 255};
                    const float side = ly < 0 ? -1.0f : 1.0f;
                    const Vector2 tp{p.x + static_cast<float>(std::cos(-theta + 1.5708)) * 9 * f * side, p.y + static_cast<float>(std::sin(-theta + 1.5708)) * 9 * f * side};
                    draw_person(tp, static_cast<float>(-theta * 180.0 / 3.14159265) + 90, f, 0, l);
                    labels.push_back({tp, t.firm ? "teamster, eastern firm" : t.house >= 0 ? "teamster, freight house" : t.job_town >= 0 ? "teamster on a freight job" : "freighter"});
                }
            }
        }
        // Outlaw camps: a tent and a fire, where the sim puts them.
        for (const Gang& g : sim.gangs()) {
            if (!g.active || g.edge < 0 || g.camp_kind < 0 || g.hideout < 0) continue;
            const Edge& e = w.edges[World::idx(g.edge)];
            if (e.a != g.hideout && e.b != g.hideout) continue;
            const Vector2 m = point_along(w, e, g.hideout, std::clamp(g.camp_miles / std::max(1.0, e.miles), 0.0, 1.0));
            if (m.x < vx0 || m.x > vx1 || m.y < vy0 || m.y > vy1) continue;
            const Vector2 p = cam.to_screen(m.x, m.y);
            DrawTriangle({p.x, p.y - 7 * f}, {p.x - 6 * f, p.y + 4 * f}, {p.x + 6 * f, p.y + 4 * f}, {206, 196, 168, 255});
            DrawCircleV({p.x + 9 * f, p.y + 3 * f}, 1.2f * f, {230, 110, 30, 255});
            // The band itself: its men sit and stand round the fire (up to a dozen drawn; the sim holds the real count).
            const int shown = std::min(g.men, 12);
            for (int k = 0; k < shown; ++k) {
                const std::uint64_t hh = mix64(static_cast<std::uint64_t>(g.id) * 31 + static_cast<std::uint64_t>(k) * 977 + 5);
                const double ang = unit_of(hh) * 6.2831853, rad = 5.0 + unit_of(mix64(hh + 1)) * 6.0;
                const Vector2 mp{p.x + 9 * f + static_cast<float>(std::cos(ang) * rad) * f, p.y + 3 * f + static_cast<float>(std::sin(ang) * rad) * f};
                Look l; l.shirt = {88, 70, 58, 255}; l.hat = {48, 40, 34, 255};
                draw_person(mp, static_cast<float>(ang * 180.0 / 3.14159265 + 180.0), f, 0, l);
                labels.push_back({mp, sim.is_band_gang(g) ? "outlaw, one of a small band" : "outlaw, one of a large gang"});
            }
        }
        // The town's people.
        if (near >= 0 && near_d < 1.0 && static_cast<NodeId>(near) == built_for) {
            const Node& n = w.nodes[static_cast<std::size_t>(near)];
            const double c = std::cos(theta), s = std::sin(theta);
            const double hr = clock / 60.0;
            const double day = std::clamp(0.5 + 0.9 * std::cos((hr - 13.0) / 12.0 * 3.14159265), 0.0, 1.0);
            const double out_and_about = 0.12 + 0.88 * day;                  // most are indoors at night
            const double ease = 1.0 - std::min(0.6, n.hunger);               // a hungry town moves slowly
            const std::uint64_t tick = static_cast<std::uint64_t>(GetTime() * 4);
            const double my_lx = ((me_x - n.x) * c + (me_y - n.y) * s) * kFeet, my_ly = (-(me_x - n.x) * s + (me_y - n.y) * c) * kFeet;   // the drifter in the town's frame
            for (Folk& p : folk) {
                if (unit_of(mix64(p.h + 99)) >= out_and_about) continue;
                if (p.pause > 0) p.pause -= dt * mult;
                else {
                    // Steer around the drifter and anyone close ahead: shift across the street, and wait a moment if blocked.
                    {
                        auto nudge = [&](double ox, double oy, double reach) {
                            const double dx = p.lx - ox, dy = p.ly - oy, d = std::hypot(dx, dy);
                            if (d >= reach || d < 1e-6) return;
                            const double push = (reach - d) / reach * 9.0 * dt * mult;
                            const double across = p.street == 0 ? dy : dx;   // the across-the-street part of the way apart
                            p.v += (across >= 0 ? 1.0 : -1.0) * push;
                            const double lim = p.street == 0 ? 11.0 : 7.0;
                            p.v = std::clamp(p.v, -lim, lim);
                            const double along = p.street == 0 ? dx : dy;    // standing in the way and heading at it: hold up
                            if (d < reach * 0.45 && along * p.dir < 0) p.pause = std::max(p.pause, 0.5);
                        };
                        nudge(my_lx, my_ly, 5.5);
                        for (const Folk& q : folk)
                            if (&q != &p && q.street == p.street && std::abs(q.u - p.u) < 5 ) nudge(q.lx, q.ly, 3.2);
                    }
                    const double step = p.speed * ease * dt * mult;
                    p.u += p.dir * step;
                    p.stride += 9 * dt * mult * p.speed / 11.0;
                    if (std::abs(p.u) > p.bound) { p.u = std::clamp(p.u, -p.bound, p.bound); p.dir = -p.dir; }
                    // Corners: crossing a street's mouth, sometimes turn into it (or back onto the main street at u = 0).
                    if (p.street == 0) {
                        const int nc = std::max(1, static_cast<int>(town_length(n) / 30) / 6);
                        const int j = static_cast<int>(std::lround(p.u / 180.0));
                        if (j != 0 && std::abs(j) <= nc && std::abs(p.u - j * 180.0) < step + 0.5 && unit_of(mix64(p.h + tick)) < 0.25) {
                            p.street = j; p.bound = street_reach(n, j); p.u = 0; p.v = std::clamp(p.v, -7.0, 7.0); p.dir = unit_of(mix64(p.h + 5 + tick)) < 0.5 ? -1 : 1;
                        }
                    } else if (std::abs(p.u) < step + 0.5 && unit_of(mix64(p.h + tick)) < 0.4) {
                        const int j = p.street;
                        p.street = 0; p.bound = town_length(n); p.u = j * 180.0; p.v = std::clamp(p.v, -11.0, 11.0);
                        p.dir = unit_of(mix64(p.h + 6 + tick)) < 0.5 ? -1 : 1;
                    }
                    if (unit_of(mix64(p.h + static_cast<std::uint64_t>(GetTime() * 60))) < 0.002) p.pause = 3 + unit_of(mix64(p.h + 21 + tick)) * 12;   // stop to talk or look about
                }
                const double lx = p.street == 0 ? p.u : p.street * 180.0 + p.v, ly = p.street == 0 ? p.v : p.u;
                p.lx = lx; p.ly = ly;
                const double wx = n.x + (lx * c - ly * s) / kFeet, wy = n.y + (lx * s + ly * c) / kFeet;
                if (river_clearance(near_rivers, wx, wy) < 2) { p.dir = -p.dir; p.u += p.dir * 4; continue; }   // turn back from the water
                const Vector2 sp = cam.to_screen(wx, wy);
                if (sp.x < -60 || sp.y < -60 || sp.x > W + 60 || sp.y > H + 60) continue;
                const double al = p.street == 0 ? (p.dir > 0 ? 0.0 : 3.14159265) : (p.dir > 0 ? 1.5707963 : -1.5707963);
                draw_person(sp, static_cast<float>(-(theta + al) * 180.0 / 3.14159265), f, p.stride, p.look);
                labels.push_back({sp, p.role});
            }
        }
        draw_person({static_cast<float>(GetScreenWidth() * 0.5f + (me_x - cam.cx) * kFeet * f), static_cast<float>(GetScreenHeight() * 0.5f - (me_y - cam.cy) * kFeet * f)},
                    static_cast<float>(heading * 180.0 / 3.14159265), f, stride);
        if (walking) {
            const Vector2 d = cam.to_screen(dest_x, dest_y);
            DrawCircleLines(static_cast<int>(d.x), static_cast<int>(d.y), 1.6f * f, {255, 255, 255, 170});
        }

        // Night: the day runs from dawn at about six to dusk at about eight.
        {
            const double hr = clock / 60.0;
            const double day = std::clamp(0.5 + 0.9 * std::cos((hr - 13.0) / 12.0 * 3.14159265), 0.0, 1.0);
            const unsigned char a = static_cast<unsigned char>((1.0 - day) * 150);
            if (a > 0) DrawRectangle(0, 0, W, H, {12, 20, 56, a});
        }

        if (inside >= 0) {   // the room replaces the street while he is in it
            labels.clear();
            draw_interior(sim, player, static_cast<std::size_t>(built_for), room, clock / 60.0, labels);
            const float area = static_cast<float>(W - 360);
            const float sc = std::min((area - 40) / room.rw, (H - 130.0f) / room.rd);
            draw_person({(area - room.rw * sc) / 2 + 10 + room.px * sc, 76 + room.py * sc}, room.heading, sc, room.stride);
            if (room.walking) DrawCircleLines(static_cast<int>((area - room.rw * sc) / 2 + 10 + room.dest_x * sc), static_cast<int>(76 + room.dest_y * sc), 0.8f * sc, {255, 255, 255, 170});
        }
        {   // name the person under the mouse
            const Vector2 m = GetMousePosition();
            const std::pair<Vector2, std::string>* best = nullptr;
            float bd = 14 * f / 7;
            for (const auto& l : labels) { const float d = std::hypot(l.first.x - m.x, l.first.y - m.y); if (d < bd) { bd = d; best = &l; } }
            if (best) {
                const int tw = TextWidth(best->second.c_str(), 14);
                DrawRectangle(static_cast<int>(best->first.x) - tw / 2 - 5, static_cast<int>(best->first.y) - 30, tw + 10, 20, {250, 244, 228, 235});
                Text(best->second.c_str(), static_cast<int>(best->first.x) - tw / 2, static_cast<int>(best->first.y) - 27, 14, {40, 30, 20, 255});
            }
        }
        // HUD
        panel(10, 8, 470, 68);
        char hud[200];
        std::snprintf(hud, sizeof hud, "%s   %02d:%02d   speed %s%.0fx", format_date(w.today).c_str(), static_cast<int>(clock / 60) % 24, static_cast<int>(clock) % 60, paused ? "paused " : "", rates[speed]);
        Text(hud, 20, 14, 16, {30, 24, 18, 255});
        if (near >= 0) {
            const Node& n = w.nodes[static_cast<std::size_t>(near)];
            const double ft = near_d * kFeet;
            std::snprintf(hud, sizeof hud, near_d < 0.25 ? "in %s  (%s, %.0f people)" : "%s is %.1f miles away", n.name.c_str(), n.kind.c_str(), n.population);
            if (near_d >= 0.25) std::snprintf(hud, sizeof hud, "%s is %.1f miles away", n.name.c_str(), near_d);
            (void)ft;
            Text(hud, 20, 34, 14, {90, 70, 40, 255});
        }
        if (const Trader* me = player_trader(sim, player)) {
            if (me->travelling) std::snprintf(hud, sizeof hud, "you: $%.0f, on the road", me->cash);
            else if (me->cargo >= 0) std::snprintf(hud, sizeof hud, "you: $%.0f, wagon carries %.0f %s", me->cash, me->qty, w.goods[World::idx(me->cargo)].name.c_str());
            else std::snprintf(hud, sizeof hud, "you: $%.0f, wagon empty (store to buy, freight office to haul)", me->cash);
            Text(hud, 20, 54, 14, {30, 90, 50, 255});
        }
        Text(inside >= 0 ? "click or WASD to walk   leave by the red mat at the door   1-4 speed   space pause"
                         : "click or WASD to walk   click a door (or F beside one) to go in   wheel zoom   1-4 speed   space pause   E town board   M world map",
             14, H - 24, 14, inside >= 0 ? Color{230, 220, 200, 230} : Color{60, 50, 40, 230});
        if (show_town && near >= 0) draw_town_panel(sim, static_cast<std::size_t>(near));
        EndDrawing();
        if (player.departing) { departed = true; break; }   // he set off at the freight office: back to the map, which runs while he travels
        if (!g_ground_shot.empty() && ++shot_frames == 4) { TakeScreenshot(g_ground_shot.c_str()); break; }
    }
    UnloadTexture(patches);
    UnloadTexture(grain);
    return WindowShouldClose();
}

// ---------------------------------------------------------------- the road: a loading screen over the days of a haul

struct TripResult {
    enum Kind { Arrived, Robbed, Killed, Stopped, Closed } kind = Stopped;
    NodeId at = -1;   // the town the wagon stands in when the trip ends
};

// The player never watches the world run. He sets off, the sim steps day by day behind this screen, and it ends when the
// wagon arrives, is held up (the gang takes the load and he is back at the town the leg started from), or he is killed.
// A toll (part of the load) only adds a line to the journal, and the wagon goes on.
TripResult run_trip(Simulation& sim, Player& player, long end_day) {
    const World& w = sim.world();
    TripResult res;
    const Trader* t0 = player_trader(sim, player);
    if (!t0) { res.kind = TripResult::Killed; return res; }
    const NodeId origin = t0->at, dest = t0->dest;
    const GoodId cargo0 = t0->cargo;
    const double value0 = t0->qty * t0->unit_cost;
    int robberies_seen = t0->robberies;
    double qty_seen = t0->qty;
    // Where the road passes, with the days to each stop, for the progress bar.
    std::vector<std::string> stops{w.nodes[World::idx(origin)].name};
    std::vector<double> stop_pop{w.nodes[World::idx(origin)].population};
    std::vector<double> stop_day{0};
    {
        NodeId cur = origin;
        double d = 0;
        for (EdgeId id : t0->route) {
            const Edge& e = w.edges[World::idx(id)];
            d += e.travel_days;
            cur = e.a == cur ? e.b : e.a;
            stops.push_back(w.nodes[World::idx(cur)].name);
            stop_pop.push_back(w.nodes[World::idx(cur)].population);
            stop_day.push_back(d);
        }
    }
    const double total_days = std::max(1.0, stop_day.back());
    const long start_day = w.today;
    std::vector<std::string> journal;
    bool finished = false;
    std::string headline, detail;

    while (!WindowShouldClose()) {
        const double t_start = GetTime();
        while (!finished && GetTime() - t_start < 0.05) {
            if (w.today >= end_day) { res.kind = TripResult::Stopped; finished = true; break; }
            sim.step();
            const Trader* t = player_trader(sim, player);
            if (!t) {
                res.kind = TripResult::Killed;
                res.at = origin;
                headline = cargo0 >= 0 ? "Killed on the road" : "Finished on the road";
                detail = cargo0 >= 0 ? "Riders stopped the wagon and shot you for the load. Nobody in " + w.nodes[World::idx(origin)].name + " will say much about it."
                                     : "The money ran out before the road did. Your outfit is finished.";
                player.dead = true;
                finished = true;
                break;
            }
            if (t->robberies > robberies_seen && t->travelling) {
                char b[160];
                std::snprintf(b, sizeof b, "Day %ld: a gang stopped the wagon and took a toll of %.0f %s.", w.today - start_day, qty_seen - t->qty,
                              cargo0 >= 0 ? w.goods[World::idx(cargo0)].unit.c_str() : "goods");
                journal.push_back(b);
                robberies_seen = t->robberies;
            }
            qty_seen = t->qty;
            if (!t->travelling) {
                res.at = t->at;
                if (t->at == dest) {
                    res.kind = TripResult::Arrived;
                } else {
                    res.kind = TripResult::Robbed;
                    headline = "Held up on the road";
                    char b[260];
                    std::snprintf(b, sizeof b, "A gang took the whole load%s%s. You walked back to %s with what was in your pockets.",
                                  cargo0 >= 0 ? ", worth about $" : "", cargo0 >= 0 ? std::to_string(static_cast<long>(value0)).c_str() : "",
                                  w.nodes[World::idx(t->at)].name.c_str());
                    detail = b;
                }
                finished = true;
            }
        }

        BeginDrawing();
        ClearBackground({226, 214, 184, 255});
        const int W = GetScreenWidth(), H = GetScreenHeight();
        const int bx = 90, bw = W - 180, by = H / 2;
        const double elapsed = static_cast<double>(w.today - start_day);
        const double frac = finished ? 1.0 : std::clamp(elapsed / total_days, 0.0, 1.0);
        Text(("On the road to " + w.nodes[World::idx(dest)].name).c_str(), bx, by - 120, 26, {50, 36, 22, 255});
        char b[160];
        std::snprintf(b, sizeof b, "day %.0f of about %.0f", std::min(elapsed, total_days), total_days);
        Text(b, bx, by - 84, 15, {90, 70, 40, 255});
        DrawRectangle(bx, by - 3, bw, 6, {150, 130, 95, 255});
        DrawRectangle(bx, by - 3, static_cast<int>(bw * frac), 6, {110, 80, 45, 255});
        for (std::size_t i = 0; i < stops.size(); ++i) {
            const int sx = bx + static_cast<int>(bw * stop_day[i] / total_days);
            DrawCircle(sx, by, 5, i == 0 || i + 1 == stops.size() ? Color{60, 40, 25, 255} : Color{120, 95, 65, 255});
            if (i == 0 || i + 1 == stops.size() || stop_pop[i] >= 500) Text(stops[i].c_str(), sx - 30, by + 14 + static_cast<int>(i % 2) * 18, 13, {60, 48, 32, 255});
        }
        DrawCircle(bx + static_cast<int>(bw * frac), by, 8, {170, 40, 30, 255});
        int jy = by + 80;
        for (const std::string& l : journal) { Text(l.c_str(), bx, jy, 14, {120, 40, 30, 255}); jy += 20; }

        bool go = false;
        if (finished && res.kind != TripResult::Arrived && res.kind != TripResult::Stopped) {
            const Rectangle card{static_cast<float>(W / 2 - 230), static_cast<float>(H / 2 - 200), 460, 150};
            panel(static_cast<int>(card.x), static_cast<int>(card.y), static_cast<int>(card.width), static_cast<int>(card.height));
            Text(headline.c_str(), static_cast<int>(card.x) + 14, static_cast<int>(card.y) + 12, 20, {150, 30, 25, 255});
            int ly = static_cast<int>(card.y) + 44;
            for (const std::string& l : wrap(detail, 14, 430)) { Text(l.c_str(), static_cast<int>(card.x) + 14, ly, 14, {40, 30, 20, 255}); ly += 18; }
            go = button({card.x + card.width - 110, card.y + card.height - 34, 96, 24}, "Continue") || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
        } else if (finished) {
            go = true;
        }
        EndDrawing();
        if (go) return res;
    }
    res.kind = TripResult::Closed;
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    std::string data = find_data_dir(), shot;
    std::uint64_t seed = 0;
    bool seed_set = false;
    std::string load_file, play_at, ground_at;
    int shot_days = 0;
    bool board_on = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") data = next();
        else if (a == "--load") load_file = next();
        else if (a == "--play") play_at = next();
        else if (a == "--ground") ground_at = next();
        else if (a == "--inside") g_ground_inside = next();
        else if (a == "--zoom") g_ground_zoom = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--seed") { seed = std::strtoull(next().c_str(), nullptr, 10); seed_set = true; }
        else if (a == "--shot") shot = next();
        else if (a == "--board") board_on = true;
        else if (a == "--days") shot_days = std::atoi(next().c_str());
    }
    const WorldDef world = load_world(data);
    Scenario scenario = load_scenario(data, world);
    if (seed_set) scenario.seed = seed;
    g_ground_shot = shot;
    Simulation sim(world, scenario);
    if (!load_file.empty()) sim.load_state(load_file);
    const World& w = sim.world();
    const long end_day = scenario.start + scenario.days;
    const GoodId flour = find_good(w, "flour");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1500, 900, "Frontier Sim");
    SetTargetFPS(60);
    {
        int n = 0;
        std::vector<int> cps;
        for (int c = 32; c < 256; ++c) cps.push_back(c);
        g_font = LoadFontEx(FRONTIER_FONT, 32, cps.data(), static_cast<int>(cps.size()));
        n = g_font.glyphCount;
        g_font_ok = n > 0 && g_font.texture.id != 0;
        if (g_font_ok) SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
    }
    SetExitKey(KEY_ESCAPE);
    const Terrain terrain = load_terrain(data);
    bool show_terrain = true, show_board = board_on;

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
    auto enter_ground = [&](NodeId at) {   // the local map: returns true if the window was closed there
        // Setting off hides the days behind a travel screen (the player never watches the world run); the trip ends in a
        // town, so the ground view opens again there, and the map is only seen when he asks for it.
        for (;;) {
            bool departed = false;
            const bool quit = run_ground(sim, terrain, player, at, end_day, departed) || !g_ground_shot.empty();
            fit(cam, w);
            paused = true;
            if (quit) return true;
            if (!departed) return false;
            selected = -1;
            player.departing = false;
            const TripResult r = run_trip(sim, player, end_day);
            if (r.kind == TripResult::Closed) return true;
            at = r.at >= 0 ? r.at : at;
            if (r.kind == TripResult::Arrived) player.msg = std::string("You have arrived in ") + w.nodes[World::idx(at)].name + ".";
            else if (r.kind == TripResult::Robbed) player.msg = "You were robbed on the road.";
            if (r.kind == TripResult::Stopped) return false;   // the history ran out
        }
    };
    bool was_travelling = false;
    bool ground_quit = false;
    if (!ground_at.empty())
        for (std::size_t i = 0; i < w.nodes.size(); ++i)
            if (w.nodes[i].id == ground_at || w.nodes[i].name == ground_at) { ground_quit = enter_ground(static_cast<NodeId>(i)); break; }
    int frames = 0;

    while (!ground_quit && !WindowShouldClose()) {
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
        if (IsKeyPressed(KEY_V) && selected >= 0 && (w.nodes[static_cast<std::size_t>(selected)].founded || w.nodes[static_cast<std::size_t>(selected)].fixed_market)) {
            if (enter_ground(static_cast<NodeId>(selected))) break;
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

        // Arriving in a town after a haul: the map hands over to the town, where the wagon stands.
        if (player.active() && !player.dead) {
            const Trader* me = player_trader(sim, player);
            const bool trav = me && me->travelling;
            if (was_travelling && me && !trav && me->at >= 0 && shot.empty()) {
                was_travelling = false;
                player.msg = std::string("You have arrived in ") + w.nodes[World::idx(me->at)].name + ".";
                if (enter_ground(me->at)) break;
                continue;
            }
            was_travelling = trav;
        } else was_travelling = false;

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
            if (e.latent && e.from == LONG_MAX) continue;   // a railroad nobody has started building: nothing to draw
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
            if (cam.zoom > 2.0f) Text(Simulation::camp_kind_name(g.camp_kind), static_cast<int>(p.x + s + 3), static_cast<int>(p.y - 6), 11, {110, 70, 30, 255});
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
            if (!band) Text(TextFormat("%d", g.men), static_cast<int>(p.x + s + 2), static_cast<int>(p.y - 6), 12, gc);
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
                Text(n.name.c_str(), static_cast<int>(p.x + r + 3), static_cast<int>(p.y - 6), 13, {40, 30, 20, 255});
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
        int town_bottom = 40;
        if (selected >= 0) {
            const Node& n = w.nodes[static_cast<std::size_t>(selected)];
            const Vector2 p = cam.to_screen(static_cast<float>(n.x), static_cast<float>(n.y));
            DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), 16, {20, 20, 20, 255});
            town_bottom = draw_town_panel(sim, static_cast<std::size_t>(selected)) + 6;
        }
        headline_click = draw_headlines(sim, scenario.log_from, selected >= 0 ? town_bottom : followed >= 0 ? 300 : 40);

        // HUD: date, speed, counts.
        panel(10, 8, 520, 26);
        int alive = 0, loaded = 0;
        for (const Trader& t : sim.traders()) { alive += t.alive; loaded += t.alive && t.cargo >= 0; }
        int gangs = 0;
        for (const Gang& g : sim.gangs()) gangs += g.active;
        char hud[220];
        std::snprintf(hud, sizeof hud, "%s   %s   outfits %d (%d loaded)   gangs %d   overlay: %s", format_date(w.today).c_str(),
                      paused || w.today >= end_day ? "paused" : (std::string(TextFormat("%.0f d/s", speeds[speed]))).c_str(), alive, loaded, gangs, overlay_name(overlay));
        Text(hud, 18, 13, 15, {40, 30, 20, 255});

        // Legend.
        panel(10, 40, 190, 262);
        int ly = 48;
        auto key = [&](Color c, const char* label) { DrawCircle(24, ly + 7, 4, c); Text(label, 36, ly, 13, {40, 30, 20, 255}); ly += 18; };
        key({40, 40, 40, 255}, "freelancer");
        key({30, 150, 70, 255}, "house outfit");
        key({40, 80, 200, 255}, "eastern firm");
        key({230, 130, 20, 255}, "freight job");
        DrawLineEx({20, static_cast<float>(ly + 4)}, {28, static_cast<float>(ly + 10)}, 1.8f, {220, 120, 30, 255});
        DrawLineEx({20, static_cast<float>(ly + 10)}, {28, static_cast<float>(ly + 4)}, 1.8f, {220, 120, 30, 255});
        Text("small band (2-5 men)", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawLineEx({18, static_cast<float>(ly + 2)}, {30, static_cast<float>(ly + 12)}, 3.2f, {190, 20, 20, 255});
        DrawLineEx({18, static_cast<float>(ly + 12)}, {30, static_cast<float>(ly + 2)}, 3.2f, {190, 20, 20, 255});
        Text("large gang (men shown)", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawCircleLines(24, ly + 7, 5, {200, 30, 30, 255});
        Text("food shortage", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawTriangle({24, static_cast<float>(ly + 1)}, {19, static_cast<float>(ly + 11)}, {29, static_cast<float>(ly + 11)}, {120, 80, 40, 255});
        Text("gang camp", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        Text("big dot: loaded wagon", 18, ly, 12, {90, 70, 40, 255});
        ly += 18;
        Text("thick road: worn in by traffic", 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        Text("B price board", 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        Text("G play: start at selected town", 18, ly, 12, {90, 70, 40, 255});
        ly += 18;
        Text(TextFormat("C show: %s", filter.carrier < 0 ? "all" : kCarrierNames[filter.carrier]), 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        Text(TextFormat("L loaded only: %s", filter.loaded_only ? "on" : "off"), 18, ly, 12, {90, 70, 40, 255});

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
            Text(format_date(e.day).c_str(), 18, ey, 14, {110, 90, 60, 255});
            Text(e.text.substr(0, 84).c_str(), 112, ey, 14, event_color(e.type));
            Text(e.cause.substr(0, 98).c_str(), 112, ey + 15, 12, {110, 100, 90, 255});
            ey += 34;
        }
        EndDrawing();

        if (!shot.empty() && ++frames == 3) { TakeScreenshot(shot.c_str()); break; }
    }
    CloseWindow();
    return 0;
}
