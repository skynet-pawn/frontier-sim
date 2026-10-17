// Frontier Sim viewer: a top-down map of the running simulation.
//
// The viewer only reads the sim (const accessors and the event log); it never changes world state, so another
// presentation layer can replace this file without touching sim/. The map is drawn in the sim's own coordinates
// (miles, x east, y north).
//
//   frontier_viewer [--data DIR] [--seed N] [--shot FILE --days N]
//   space pause/resume   1-5 speed   N one day (paused)   tab overlay   F fit map   click a town   wheel zoom
//   right-drag or middle-drag pan   P screenshot   Esc quit
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

// Where a wagon train is on its road, interpolated along the current leg. False if it is standing in a town.
bool trader_position(const World& w, const Trader& t, Vector2& out) {
    if (!t.travelling || t.leg >= t.route.size()) return false;
    const Edge& e = w.edges[World::idx(t.route[t.leg])];
    const Node& a = w.nodes[World::idx(t.at)];
    const Node& b = w.nodes[World::idx(e.other(t.at))];
    const double f = std::clamp(1.0 - t.days_left / std::max(0.1, e.travel_days), 0.0, 1.0);
    out = {static_cast<float>(a.x + (b.x - a.x) * f), static_cast<float>(a.y + (b.y - a.y) * f)};
    return true;
}

Color trader_color(const Trader& t) {
    if (t.job_town >= 0) return {230, 130, 20, 255};   // hauling a supplier's goods on a freight job
    if (t.firm) return {40, 80, 200, 255};              // an eastern firm's outfit
    if (t.house >= 0) return {30, 150, 70, 255};        // a freight house's outfit
    return {40, 40, 40, 255};                           // a freelancer
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
    panel(x, y, 320, 330);
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

    Camera2 cam;
    fit(cam, w);
    int speed = 1, overlay = 0;                       // speed index into days per second
    const double speeds[] = {1, 4, 16, 64, 256};
    bool paused = shot.empty() ? false : true;
    double accum = 0;
    int selected = -1;
    Vector2 drag_from{};

    if (!shot.empty()) for (int d = 0; d < shot_days; ++d) sim.step();
    int frames = 0;

    while (!WindowShouldClose()) {
        // ---- input
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        for (int k = 0; k < 5; ++k) if (IsKeyPressed(KEY_ONE + k)) { speed = k; paused = false; }
        if (IsKeyPressed(KEY_TAB)) overlay = (overlay + 1) % 3;
        if (IsKeyPressed(KEY_F)) fit(cam, w);
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
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            selected = -1;
            float best = 14;
            for (std::size_t i = 0; i < w.nodes.size(); ++i) {
                if (!w.nodes[i].founded && !w.nodes[i].fixed_market) continue;
                const Vector2 s = cam.to_screen(static_cast<float>(w.nodes[i].x), static_cast<float>(w.nodes[i].y));
                const float d = std::hypot(s.x - GetMouseX(), s.y - GetMouseY());
                if (d < best) { best = d; selected = static_cast<int>(i); }
            }
        }

        // ---- simulate
        if (!paused && w.today < end_day) {
            accum += GetFrameTime() * speeds[speed];
            for (int steps = 0; accum >= 1.0 && steps < 40 && w.today < end_day; ++steps, accum -= 1.0) sim.step();
            if (accum > 40) accum = 0;
        }

        // ---- draw
        BeginDrawing();
        ClearBackground({232, 222, 196, 255});
        for (const Edge& e : w.edges) {
            const Node& a = w.nodes[World::idx(e.a)];
            const Node& b = w.nodes[World::idx(e.b)];
            if ((!a.founded && !a.fixed_market) || (!b.founded && !b.fixed_market)) continue;
            const Vector2 pa = cam.to_screen(static_cast<float>(a.x), static_cast<float>(a.y));
            const Vector2 pb = cam.to_screen(static_cast<float>(b.x), static_cast<float>(b.y));
            if (!e.active) { DrawLineEx(pa, pb, 1.0f, {200, 120, 120, 120}); continue; }  // shut: a closed road or one not built yet
            const float danger = static_cast<float>(std::clamp(e.robbed_ema / 0.004, 0.0, 1.0));
            const float thick = e.mode == Mode::Rail ? 2.5f : e.mode == Mode::River ? 2.0f : 1.5f;
            DrawLineEx(pa, pb, thick, lerp(mode_color(e.mode), {200, 30, 20, 255}, danger));
        }
        for (const Gang& g : sim.gangs()) {
            if (!g.active || g.edge < 0) continue;
            const Edge& e = w.edges[World::idx(g.edge)];
            const Node& a = w.nodes[World::idx(e.a)];
            const Node& b = w.nodes[World::idx(e.b)];
            const Vector2 p = cam.to_screen(static_cast<float>((a.x + b.x) / 2), static_cast<float>((a.y + b.y) / 2));
            DrawLineEx({p.x - 5, p.y - 5}, {p.x + 5, p.y + 5}, 2.5f, {190, 20, 20, 255});
            DrawLineEx({p.x - 5, p.y + 5}, {p.x + 5, p.y - 5}, 2.5f, {190, 20, 20, 255});
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
            DrawCircleV(p, 2.6f, trader_color(t));
        }
        if (selected >= 0) {
            const Node& n = w.nodes[static_cast<std::size_t>(selected)];
            const Vector2 p = cam.to_screen(static_cast<float>(n.x), static_cast<float>(n.y));
            DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y), 16, {20, 20, 20, 255});
            draw_town_panel(sim, static_cast<std::size_t>(selected));
        }

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
        panel(10, 40, 150, 112);
        int ly = 48;
        auto key = [&](Color c, const char* label) { DrawCircle(24, ly + 7, 4, c); DrawText(label, 36, ly, 13, {40, 30, 20, 255}); ly += 18; };
        key({40, 40, 40, 255}, "freelancer");
        key({30, 150, 70, 255}, "house outfit");
        key({40, 80, 200, 255}, "eastern firm");
        key({230, 130, 20, 255}, "freight job");
        DrawLineEx({18, static_cast<float>(ly + 2)}, {30, static_cast<float>(ly + 12)}, 2.5f, {190, 20, 20, 255});
        DrawLineEx({18, static_cast<float>(ly + 12)}, {30, static_cast<float>(ly + 2)}, 2.5f, {190, 20, 20, 255});
        DrawText("gang", 36, ly, 13, {40, 30, 20, 255});
        ly += 18;
        DrawCircleLines(24, ly + 7, 5, {200, 30, 30, 255});
        DrawText("food shortage", 36, ly, 13, {40, 30, 20, 255});

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
