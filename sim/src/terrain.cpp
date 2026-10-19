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
                            double margin) {
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
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    dist[id(sx, sy)] = 0;
    open.push({0, id(sx, sy)});
    const std::size_t goal = id(tx, ty);
    while (!open.empty()) {
        const auto [d, u] = open.top();
        open.pop();
        if (d > dist[u]) continue;
        if (u == goal) break;
        const int ux = x0 + static_cast<int>(u % W), uy = y0 + static_cast<int>(u / W);
        const double hu = height(ux, uy);
        for (int k = 0; k < 16; ++k) {
            const int vx = ux + dxs[k], vy = uy + dys[k];
            if (vx < x0 || vx > x1 || vy < y0 || vy > y1) continue;
            const double step = std::hypot(dxs[k], dys[k]) * g.cell;
            const double dh = height(vx, vy) - hu;
            const double extra = (dh > 0 ? dh : -dh * descent_share) / 100.0 * climb_per_100m;
            const std::size_t v = id(vx, vy);
            const double nd = d + step + extra;
            if (nd < dist[v]) { dist[v] = nd; from[v] = static_cast<int>(u); open.push({nd, v}); }
        }
    }
    if (dist[goal] >= 1e17) { out.cost = std::hypot(bx - ax, by - ay); out.points = {{ax, ay}, {bx, by}}; return out; }
    out.cost = dist[goal];
    std::vector<std::pair<double, double>> pts;
    for (int u = static_cast<int>(goal); u >= 0; u = from[static_cast<std::size_t>(u)]) {
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
