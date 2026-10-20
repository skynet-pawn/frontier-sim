// A run saved part-way and reloaded must continue exactly as an unbroken run: same history hash, same money, and a final
// snapshot that matches byte for byte. A field left out of the save list shows up here as a run that drifts.
//   save_test [DATA_DIR] [DAYS]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "frontier/simulation.h"

namespace {
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

int main(int argc, char** argv) {
    using namespace frontier;
    const std::string dir = argc > 1 ? argv[1] : FRONTIER_DATA_DIR;
    const int days = argc > 2 ? std::atoi(argv[2]) : 800;
    const int half = days / 2;
    const WorldDef world = load_world(dir);
    const Scenario scenario = load_scenario(dir, world);

    Simulation straight(world, scenario);
    straight.run(days);
    straight.save_state("save_test_straight.sav");

    Simulation first(world, scenario);
    first.run(half);
    first.save_state("save_test_mid.sav");

    Simulation second(world, scenario);   // a fresh simulation takes the state from the file
    second.load_state("save_test_mid.sav");
    second.save_state("save_test_mid_again.sav");
    int rc = 0;
    if (slurp("save_test_mid.sav") != slurp("save_test_mid_again.sav")) {
        std::puts("FAIL: a snapshot loaded and saved again differs from the original");
        rc = 1;
    }
    second.run(days - half);
    second.save_state("save_test_resumed.sav");

    std::printf("straight %016llx, resumed %016llx; money %.2f / %.2f; traders %zu / %zu\n",
                static_cast<unsigned long long>(straight.log().hash()), static_cast<unsigned long long>(second.log().hash()),
                straight.total_money(), second.total_money(), straight.traders().size(), second.traders().size());
    if (straight.log().hash() != second.log().hash()) { std::puts("FAIL: the resumed run's history differs"); rc = 1; }
    if (straight.total_money() != second.total_money()) { std::puts("FAIL: the resumed run's money differs"); rc = 1; }
    if (slurp("save_test_straight.sav") != slurp("save_test_resumed.sav")) { std::puts("FAIL: the final snapshots differ"); rc = 1; }
    for (const char* f : {"save_test_straight.sav", "save_test_mid.sav", "save_test_mid_again.sav", "save_test_resumed.sav"}) std::remove(f);
    if (rc == 0) std::puts("ok");
    return rc;
}
