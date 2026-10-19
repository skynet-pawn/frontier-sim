// Dev tool: prints every route's mode, straight distance, miles (including climbing and river crossings) and river crossings.
// usage: route_probe [DATA_DIR]
#include <cmath>
#include <cstdio>
#include <string>

#include "frontier/simulation.h"

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : FRONTIER_DATA_DIR;
    const auto world = frontier::load_world(dir);
    auto scenario = frontier::load_scenario(dir, world);
    frontier::Simulation sim(world, scenario);
    std::printf("%-34s %-5s %8s %8s %6s %s\n", "route", "mode", "straight", "miles", "extra", "rivers");
    for (const auto& e : sim.world().edges) {
        const auto& a = sim.world().nodes[frontier::World::idx(e.a)];
        const auto& b = sim.world().nodes[frontier::World::idx(e.b)];
        const double straight = std::hypot(a.x - b.x, a.y - b.y);
        const std::string name = a.name + " - " + b.name;
        std::printf("%-34s %-5s %8.0f %8.0f %5.0f%% %d (biggest %d)\n", name.c_str(), frontier::mode_name(e.mode), straight, e.miles,
                    straight > 0 ? 100.0 * (e.miles / straight - 1.0) : 0.0, e.river_crossings, e.river_biggest);
    }
    return 0;
}
