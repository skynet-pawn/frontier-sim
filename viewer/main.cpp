// Frontier Sim viewer: a top-down map of the running simulation.
//
// The viewer only reads the sim (const accessors and the event log); it never changes world state, so another
// presentation layer can replace this file without touching sim/. The map is drawn in the sim's own coordinates
// (miles, x east, y north).
//
//   frontier_viewer [--data DIR] [--seed N] [--shot FILE --days N]
//   If DIR is data/slices/NAME and data/terrain/NAME exists, a shaded-relief terrain map is drawn under the towns (T toggles).
//   space pause/resume   1-5 speed   N one day (paused)   tab overlay   F fit map   click a town or a wagon   wheel zoom
//   right-drag or middle-drag pan   T terrain   C carrier filter   L loaded wagons only   X let go of a followed wagon
//   click a headline to jump to its town   P screenshot   Esc quit
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
                                        "house_collapse", "firm_withdrawal", "crop_failure", "vigilance_formed", "repopulating", "law_established", "bushwhack_notable", "cavalry_sent"};
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

// What one wagon train is doing. The sim does not store why a trader picked a road, so this shows what it carries,
// where it is bound, and the temperament and fear that weigh on that choice.
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
    const std::string dir = d + "/../../terrain/" + name;
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
    int shot_days = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") data = next();
        else if (a == "--seed") { seed = std::strtoull(next().c_str(), nullptr, 10); seed_set = true; }
        else if (a == "--shot") shot = next();
        else if (a == "--days") shot_days = std::atoi(next().c_str());
    }
    const WorldDef world = load_world(data);
    Scenario scenario = load_scenario(data, world);
    if (seed_set) scenario.seed = seed;
    Simulation sim(world, scenario);
    const World& w = sim.world();
    const long end_day = scenario.start + scenario.days;
    const GoodId flour = find_good(w, "flour");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1500, 900, "Frontier Sim");
    SetTargetFPS(60);
    SetExitKey(KEY_ESCAPE);
    const Terrain terrain = load_terrain(data);
    bool show_terrain = true;

    Camera2 cam;
    fit(cam, w);
    int speed = 1, overlay = 0;                       // speed index into days per second
    const double speeds[] = {1, 4, 16, 64, 256};
    bool paused = shot.empty() ? false : true;
    double accum = 0;
    int selected = -1, followed = -1;                  // a town index; a wagon (index into sim.traders())
    Filter filter;
    int headline_click = -1;
    Vector2 drag_from{};

    if (!shot.empty()) for (int d = 0; d < shot_days; ++d) sim.step();
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
        const bool over_panel = GetMouseX() > GetScreenWidth() - 340 && GetMouseY() > 40;
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
        if (headline_click >= 0) {                      // jump to the town a headline is about
            selected = headline_click;
            followed = -1;
            cam.cx = static_cast<float>(w.nodes[static_cast<std::size_t>(selected)].x);
            cam.cy = static_cast<float>(w.nodes[static_cast<std::size_t>(selected)].y);
            cam.zoom = std::max(cam.zoom, 1.5f);
            headline_click = -1;
        }

        // ---- simulate
        if (!paused && w.today < end_day) {
            accum += GetFrameTime() * speeds[speed];
            for (int steps = 0; accum >= 1.0 && steps < 40 && w.today < end_day; ++steps, accum -= 1.0) sim.step();
            if (accum > 40) accum = 0;
        }

        // A followed wagon keeps the camera on it; if it dies or leaves the road in a town, follow it to the town.
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
            const float thick = e.mode == Mode::Rail ? 2.5f : e.mode == Mode::River ? 2.0f : 1.5f;
            for (std::size_t k = 1; k < way.size(); ++k) DrawLineEx(way[k - 1], way[k], thick, lerp(mode_color(e.mode), {200, 30, 20, 255}, danger));
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
        panel(10, 40, 190, 196);
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
        DrawText("big dot: loaded wagon", 18, ly, 12, {90, 70, 40, 255});
        ly += 18;
        DrawText(TextFormat("C show: %s", filter.carrier < 0 ? "all" : kCarrierNames[filter.carrier]), 18, ly, 12, {90, 70, 40, 255});
        ly += 16;
        DrawText(TextFormat("L loaded only: %s", filter.loaded_only ? "on" : "off"), 18, ly, 12, {90, 70, 40, 255});

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
