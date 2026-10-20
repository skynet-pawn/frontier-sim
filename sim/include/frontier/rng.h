#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace frontier {

struct StateIo;   // saves and loads the generator (save.cpp)

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

    double uniform(double lo, double hi) { return lo + (hi - lo) * next_double(); }
    bool chance(double p) { return next_double() < p; }

    template <class T>
    void shuffle(std::vector<T>& v) {
        for (std::size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[next_below(i)]);
    }

private:
    friend struct StateIo;
    std::uint64_t s_[4];
};

}  // namespace frontier
