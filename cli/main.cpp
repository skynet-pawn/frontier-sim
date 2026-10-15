#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "frontier/rng.h"

int main(int argc, char** argv) {
    const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    frontier::Rng rng(seed);
    std::cout << "Frontier Sim (seed " << seed << ")\n"
              << "first roll: " << rng.next_below(100) << "\n";
    return 0;
}
