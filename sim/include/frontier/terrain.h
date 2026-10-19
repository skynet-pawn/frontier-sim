#pragma once
// Terrain under a slice: a grid of metres above sea level in the sim's own map miles (data/terrain/NAME, written by
// tools/make_terrain.py), and the cheapest way across it between two towns. Optional: a world with no terrain behaves as before.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace frontier {

// A river as a line of map-mile points. size: 1 a creek or small river, 2 a river, 3 a great river.
struct RiverLine {
    std::string name;
    int size = 1;
    std::vector<std::pair<double, double>> points;
};

// A place where a river can be crossed cheaply: a bridge, a ferry, a known ford. Within `radius` miles of it the cost of crossing
// falls to `factor` times the usual.
struct CrossingSite {
    std::string name, kind;
    double x = 0, y = 0, radius = 3, factor = 1;
};

struct RiverSet {
    std::vector<RiverLine> rivers;
    std::vector<CrossingSite> crossings;
};

struct TerrainGrid {
    bool ok = false;
    double x_min = 0, y_min = 0, cell = 2;
    int w = 0, h = 0;
    std::vector<std::int16_t> m;   // row-major from the south edge
    std::vector<std::uint8_t> river;   // the same cells: 0 no river, else the size of the river there (a band about three cells wide)
    std::vector<std::uint8_t> river_factor;   // percent of the usual crossing cost at each cell (100 away from any crossing site)

    double at(double x, double y) const;   // bilinear, clamped to the grid
};

// Reads data/terrain/NAME next to the data directory `dir` (dir/../../terrain/NAME). Throws if it is missing or malformed.
TerrainGrid read_terrain(const std::string& dir, const std::string& name);

// Reads data/rivers/NAME/rivers.json next to the data directory (dir/../../rivers/NAME). Throws if it is missing or malformed.
RiverSet read_rivers(const std::string& dir, const std::string& name);

// Marks the grid cells the rivers run through (and one cell either side, so a step cannot jump a river) in `g.river`.
void add_rivers(TerrainGrid& g, const RiverSet& set);

struct TerrainPath {
    int crossings = 0;                                 // rivers the way crosses
    int biggest = 0;                                   // size of the biggest one
    double cost = 0;                                   // miles of travel including the climbing, along the cheapest way
    std::vector<std::pair<double, double>> points;     // the way, from a to b, simplified (map miles)
};

// The cheapest way from (ax, ay) to (bx, by) over the grid. Each step costs its length plus `climb_per_100m` extra miles for
// every 100 m climbed and `descent_share` of that for every 100 m descended, so a road goes round a ridge when that is cheaper
// than going over it. The search stays within `margin` miles of the straight line's bounding box.
// Entering a river costs `cross[size - 1]` extra miles (a ford, a ferry, a bridge), so a road crosses where the crossing is cheap.
TerrainPath least_cost_path(const TerrainGrid& g, double ax, double ay, double bx, double by, double climb_per_100m, double descent_share,
                            const double cross[3], double margin = 40.0);

}  // namespace frontier
