#include "frontier/rng.h"

namespace frontier {

namespace {
std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// SplitMix64 expands one seed into the 256 bits of xoshiro state.
std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
}  // namespace

Rng::Rng(std::uint64_t seed) {
    for (auto& word : s_) word = splitmix64(seed);
}

std::uint64_t Rng::next_u64() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double Rng::next_double() {
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::uint64_t Rng::next_below(std::uint64_t bound) {
    // Rejection sampling avoids modulo bias.
    const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
    std::uint64_t r;
    do { r = next_u64(); } while (r >= limit);
    return r % bound;
}

}  // namespace frontier
