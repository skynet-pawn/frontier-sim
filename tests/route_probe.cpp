// Dev tool: prints every route's mode, straight distance, miles (including climbing and river crossings) and river crossings.
// usage: route_probe [DATA_DIR] [--paths]
//   --paths prints JSON instead: every route's nodes, mode, miles and its way over the terrain, for tools/gen_towns.py
#include <cmath>
#include <cstdio>
#include <string>

#include "frontier/simulation.h"

int main(int argc, char** argv) {
    std::string dir = FRONTIER_DATA_DIR;
    bool paths = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--paths") paths = true;
        else dir = argv[i];
    }
    const auto world = frontier::load_world(dir);
    auto scenario = frontier::load_scenario(dir, world);
    frontier::Simulation sim(world, scenario);
    const auto& w = sim.world();

    if (paths) {
        std::printf("[");
        bool first = true;
        for (const auto& e : w.edges) {
            const auto& a = w.nodes[frontier::World::idx(e.a)];
            const auto& b = w.nodes[frontier::World::idx(e.b)];
            std::printf("%s{\"a\":\"%s\",\"b\":\"%s\",\"mode\":\"%s\",\"miles\":%.1f,\"path\":[", first ? "" : ",", a.name.c_str(), b.name.c_str(),
                        frontier::mode_name(e.mode), e.miles);
            first = false;
            if (e.path.empty()) std::printf("[%.1f,%.1f],[%.1f,%.1f]", a.x, a.y, b.x, b.y);
            for (std::size_t k = 0; k < e.path.size(); ++k) std::printf("%s[%.1f,%.1f]", k ? "," : "", e.path[k].first, e.path[k].second);
            std::printf("]}");
        }
        std::printf("]\n");
        return 0;
    }

    std::printf("%-34s %-5s %8s %8s %6s %s\n", "route", "mode", "straight", "miles", "extra", "rivers");
    for (const auto& e : w.edges) {
        const auto& a = w.nodes[frontier::World::idx(e.a)];
        const auto& b = w.nodes[frontier::World::idx(e.b)];
        const double straight = std::hypot(a.x - b.x, a.y - b.y);
        const std::string name = a.name + " - " + b.name;
        std::printf("%-34s %-5s %8.0f %8.0f %5.0f%% %d (biggest %d)\n", name.c_str(), frontier::mode_name(e.mode), straight, e.miles,
                    straight > 0 ? 100.0 * (e.miles / straight - 1.0) : 0.0, e.river_crossings, e.river_biggest);
    }
    return 0;
}
