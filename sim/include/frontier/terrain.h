#pragma once
// Terrain under a slice: a grid of metres above sea level in the sim's own map miles (data/terrain/NAME, written by
// tools/make_terrain.py), and the cheapest way across it between two towns. Optional: a world with no terrain behaves as before.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace frontier {

struct TerrainGrid {
    bool ok = false;
    double x_min = 0, y_min = 0, cell = 2;
    int w = 0, h = 0;
    std::vector<std::int16_t> m;   // row-major from the south edge

    double at(double x, double y) const;   // bilinear, clamped to the grid
};

// Reads data/terrain/NAME next to the data directory `dir` (dir/../../terrain/NAME). Throws if it is missing or malformed.
TerrainGrid read_terrain(const std::string& dir, const std::string& name);

struct TerrainPath {
    double cost = 0;                                   // miles of travel including the climbing, along the cheapest way
    std::vector<std::pair<double, double>> points;     // the way, from a to b, simplified (map miles)
};

// The cheapest way from (ax, ay) to (bx, by) over the grid. Each step costs its length plus `climb_per_100m` extra miles for
// every 100 m climbed and `descent_share` of that for every 100 m descended, so a road goes round a ridge when that is cheaper
// than going over it. The search stays within `margin` miles of the straight line's bounding box.
TerrainPath least_cost_path(const TerrainGrid& g, double ax, double ay, double bx, double by, double climb_per_100m, double descent_share,
                            double margin = 40.0);

}  // namespace frontier
