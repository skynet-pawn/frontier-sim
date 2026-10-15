#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "frontier/types.h"

namespace frontier {

enum class EventType {
    Network,     // rails and trails opening or closing
    Production,  // strikes, depletion
    Population,  // towns growing, shrinking, dying
    Famine,
    Trader,
    Robbery,
    Gang,
    Law,
    Shock,
    Trade,       // individual trades (trace only)
};
inline constexpr int kEventTypeCount = 10;
const char* event_type_name(EventType t);

// One line of history. `cause` is the point of the log: it says why the thing
// happened, so an emergent crash can be told apart from a bug.
struct Event {
    long day = 0;
    EventType type = EventType::Network;
    NodeId node = -1;
    bool minor = false;     // routine events, hidden unless tracing
    std::string text;
    std::string cause;
};

class EventLog {
public:
    using Sink = std::function<void(const Event&)>;

    void add(long day, EventType type, NodeId node, std::string text, std::string cause, bool minor = false);
    void set_sink(Sink s) { sink_ = std::move(s); }

    const std::vector<Event>& events() const { return events_; }
    std::size_t count(EventType t) const { return counts_[static_cast<int>(t)]; }

    // FNV-1a over every event; two runs with the same seed must match.
    std::uint64_t hash() const { return hash_; }

private:
    std::vector<Event> events_;
    std::size_t counts_[kEventTypeCount] = {};
    std::uint64_t hash_ = 1469598103934665603ULL;
    Sink sink_;
};

}  // namespace frontier
