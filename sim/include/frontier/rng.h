#pragma once
#include <cstdint>

namespace frontier {

// Seeded PRNG (xoshiro256**). We implement it ourselves instead of using
// <random> distributions because the standard does not guarantee identical
// output across compilers, and replayable runs are a project requirement.
class Rng {
public:
    explicit Rng(std::uint64_t seed);

    std::uint64_t next_u64();
    // Uniform double in [0, 1).
    double next_double();
    // Uniform integer in [0, bound). bound must be > 0.
    std::uint64_t next_below(std::uint64_t bound);

private:
    std::uint64_t s_[4];
};

}  // namespace frontier
