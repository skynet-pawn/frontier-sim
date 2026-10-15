#pragma once

namespace frontier {

// Indices into the World's vectors. Plain ints keep the sim easy to read and
// cheap to copy; names live in the definitions, not the ids.
using GoodId = int;
using NodeId = int;
using EdgeId = int;

// How goods move along an edge. Each mode has its own speed, freight rate and
// hazard (see Params). Railroad arrival collapsing freight cost is the central
// economic lever of the era.
enum class Mode : int { Trail = 0, River = 1, Rail = 2 };
inline constexpr int kModeCount = 3;

inline const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Trail: return "trail";
        case Mode::River: return "river";
        case Mode::Rail:  return "rail";
    }
    return "?";
}

}  // namespace frontier
