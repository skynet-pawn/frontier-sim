#include "frontier/terrain.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <queue>
#include <stdexcept>

#include "nlohmann/json.hpp"

namespace frontier {

double TerrainGrid::at(double x, double y) const {
    const double gx = std::clamp((x - x_min) / cell - 0.5, 0.0, w - 1.001);
    const double gy = std::clamp((y - y_min) / cell - 0.5, 0.0, h - 1.001);
    const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
    const double fx = gx - x0, fy = gy - y0;
    auto v = [&](int cx, int cy) { return static_cast<double>(m[static_cast<std::size_t>(cy) * w + cx]); };
    return (v(x0, y0) * (1 - fx) + v(x0 + 1, y0) * fx) * (1 - fy) + (v(x0, y0 + 1) * (1 - fx) + v(x0 + 1, y0 + 1) * fx) * fy;
}

TerrainGrid read_terrain(const std::string& dir, const std::string& name) {
    TerrainGrid t;
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    const std::string tdir = d + "/../../terrain/" + name;
    std::ifstream hf(tdir + "/terrain.json");
    if (!hf) throw std::runtime_error("terrain '" + name + "': cannot open " + tdir + "/terrain.json");
    const nlohmann::json hj = nlohmann::json::parse(hf, nullptr, true, true);
    t.cell = hj.at("cell_miles").get<double>();
    t.w = hj.at("width").get<int>();
    t.h = hj.at("height").get<int>();
    t.x_min = hj.at("x_min").get<double>();
    t.y_min = hj.at("y_min").get<double>();
    std::ifstream bf(tdir + "/heights.i16", std::ios::binary);
    if (!bf) throw std::runtime_error("terrain '" + name + "': cannot open " + tdir + "/heights.i16");
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    if (raw.size() != static_cast<std::size_t>(t.w) * t.h * 2) throw std::runtime_error("terrain '" + name + "': heights.i16 has the wrong size");
    t.m.resize(static_cast<std::size_t>(t.w) * t.h);
    for (std::size_t i = 0; i < t.m.size(); ++i)   // little-endian int16, whatever the host
        t.m[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(raw[2 * i] | (raw[2 * i + 1] << 8)));
    t.ok = t.w > 1 && t.h > 1;
    return t;
}

std::vector<RiverLine> read_rivers(const std::string& dir, const std::string& name) {
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    const std::string path = d + "/../../rivers/" + name + "/rivers.json";
    std::ifstream f(path);
    if (!f) throw std::runtime_error("rivers '" + name + "': cannot open " + path);
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, true, true);
    std::vector<RiverLine> out;
    for (const auto& jr : j.at("rivers")) {
        RiverLine r;
        r.name = jr.value("name", "");
        r.size = std::clamp(jr.value("size", 1), 1, 3);
        for (const auto& pt : jr.at("points")) r.points.push_back({pt.at(0).get<double>(), pt.at(1).get<double>()});
        if (r.points.size() >= 2) out.push_back(std::move(r));
    }
    return out;
}

void add_rivers(TerrainGrid& g, const std::vector<RiverLine>& rivers) {
    g.river.assign(static_cast<std::size_t>(g.w) * g.h, 0);
    for (const RiverLine& r : rivers)
        for (std::size_t i = 1; i < r.points.size(); ++i) {
            const double ax = r.points[i - 1].first, ay = r.points[i - 1].second, bx = r.points[i].first, by = r.points[i].second;
            const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(bx - ax, by - ay) / (g.cell * 0.5))));
            for (int k = 0; k <= steps; ++k) {
                const double f = static_cast<double>(k) / steps;
                const int cx = static_cast<int>(std::floor((ax + (bx - ax) * f - g.x_min) / g.cell));
                const int cy = static_cast<int>(std::floor((ay + (by - ay) * f - g.y_min) / g.cell));
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int x = cx + dx, y = cy + dy;
                        if (x < 0 || y < 0 || x >= g.w || y >= g.h) continue;
                        std::uint8_t& cell = g.river[static_cast<std::size_t>(y) * g.w + x];
                        cell = std::max<std::uint8_t>(cell, static_cast<std::uint8_t>(r.size));
                    }
            }
        }
}

namespace {

// Douglas-Peucker: drops points that lie within `tol` miles of the line between the kept ones.
void simplify(const std::vector<std::pair<double, double>>& p, std::size_t lo, std::size_t hi, double tol, std::vector<bool>& keep) {
    if (hi <= lo + 1) return;
    const double dx = p[hi].first - p[lo].first, dy = p[hi].second - p[lo].second;
    const double len = std::hypot(dx, dy);
    double worst = 0;
    std::size_t at = lo;
    for (std::size_t i = lo + 1; i < hi; ++i) {
        const double d = len > 0 ? std::fabs(dx * (p[i].second - p[lo].second) - dy * (p[i].first - p[lo].first)) / len
                                 : std::hypot(p[i].first - p[lo].first, p[i].second - p[lo].second);
        if (d > worst) { worst = d; at = i; }
    }
    if (worst > tol) {
        keep[at] = true;
        simplify(p, lo, at, tol, keep);
        simplify(p, at, hi, tol, keep);
    }
}

}  // namespace

TerrainPath least_cost_path(const TerrainGrid& g, double ax, double ay, double bx, double by, double climb_per_100m, double descent_share,
                            const double cross[3], double margin) {
    TerrainPath out;
    auto cell_of = [&](double x, double y, int& cx, int& cy) {
        cx = std::clamp(static_cast<int>(std::floor((x - g.x_min) / g.cell)), 0, g.w - 1);
        cy = std::clamp(static_cast<int>(std::floor((y - g.y_min) / g.cell)), 0, g.h - 1);
    };
    int sx, sy, tx, ty;
    cell_of(ax, ay, sx, sy);
    cell_of(bx, by, tx, ty);
    const int pad = static_cast<int>(std::ceil(margin / g.cell));
    const int x0 = std::max(0, std::min(sx, tx) - pad), x1 = std::min(g.w - 1, std::max(sx, tx) + pad);
    const int y0 = std::max(0, std::min(sy, ty) - pad), y1 = std::min(g.h - 1, std::max(sy, ty) + pad);
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    auto id = [&](int cx, int cy) { return static_cast<std::size_t>(cy - y0) * W + (cx - x0); };
    auto height = [&](int cx, int cy) { return static_cast<double>(g.m[static_cast<std::size_t>(cy) * g.w + cx]); };

    // Sixteen neighbours (the knight's moves as well) keep a straight run within about 1% of its true length.
    static const int dxs[16] = {1, -1, 0, 0, 1, 1, -1, -1, 2, 2, -2, -2, 1, 1, -1, -1};
    static const int dys[16] = {0, 0, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 2, -2, 2, -2};
    std::vector<double> dist(static_cast<std::size_t>(W) * H, 1e18);
    std::vector<int> from(static_cast<std::size_t>(W) * H, -1);
    using Item = std::pair<double, std::size_t>;
    // A*: the straight distance to the goal never overstates the cost left (every step costs at least its length), so the search
    // heads for the goal instead of flooding the grid. Items are (distance so far + that estimate, cell).
    const double goal_x = g.x_min + (tx + 0.5) * g.cell, goal_y = g.y_min + (ty + 0.5) * g.cell;
    auto estimate = [&](int cx, int cy) { return std::hypot(g.x_min + (cx + 0.5) * g.cell - goal_x, g.y_min + (cy + 0.5) * g.cell - goal_y); };
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    dist[id(sx, sy)] = 0;
    open.push({estimate(sx, sy), id(sx, sy)});
    const std::size_t goal = id(tx, ty);
    while (!open.empty()) {
        const auto [f, u] = open.top();
        open.pop();
        const double d = dist[u];
        if (f > d + estimate(x0 + static_cast<int>(u % W), y0 + static_cast<int>(u / W)) + 1e-9) continue;   // a stale entry
        if (u == goal) break;
        const int ux = x0 + static_cast<int>(u % W), uy = y0 + static_cast<int>(u / W);
        const double hu = height(ux, uy);
        for (int k = 0; k < 16; ++k) {
            const int vx = ux + dxs[k], vy = uy + dys[k];
            if (vx < x0 || vx > x1 || vy < y0 || vy > y1) continue;
            const double step = std::hypot(dxs[k], dys[k]) * g.cell;
            const double dh = height(vx, vy) - hu;
            double extra = (dh > 0 ? dh : -dh * descent_share) / 100.0 * climb_per_100m;
            if (!g.river.empty()) {   // entering a river's band is a crossing; moving along or out of it is free
                const int into = g.river[static_cast<std::size_t>(vy) * g.w + vx], here = g.river[static_cast<std::size_t>(uy) * g.w + ux];
                if (into > 0 && here == 0) extra += cross[into - 1];
            }
            const std::size_t v = id(vx, vy);
            const double nd = d + step + extra;
            if (nd < dist[v]) { dist[v] = nd; from[v] = static_cast<int>(u); open.push({nd + estimate(vx, vy), v}); }
        }
    }
    if (dist[goal] >= 1e17) { out.cost = std::hypot(bx - ax, by - ay); out.points = {{ax, ay}, {bx, by}}; return out; }
    out.cost = dist[goal];
    std::vector<std::pair<double, double>> pts;
    int prev_river = 0;
    for (int u = static_cast<int>(goal); u >= 0; u = from[static_cast<std::size_t>(u)]) {
        if (!g.river.empty()) {
            const int here = g.river[static_cast<std::size_t>(y0 + u / W) * g.w + (x0 + u % W)];
            if (prev_river > 0 && here == 0) { ++out.crossings; out.biggest = std::max(out.biggest, prev_river); }   // walking back: leaving a band = it was entered going forward
            prev_river = here > 0 ? std::max(prev_river, here) : 0;
        }
        pts.push_back({g.x_min + (x0 + u % W + 0.5) * g.cell, g.y_min + (y0 + u / W + 0.5) * g.cell});
    }
    std::reverse(pts.begin(), pts.end());
    pts.front() = {ax, ay};
    pts.back() = {bx, by};
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    simplify(pts, 0, pts.size() - 1, 1.5, keep);
    for (std::size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.points.push_back(pts[i]);
    return out;
}

}  // namespace frontier
