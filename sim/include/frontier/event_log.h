#pragma once
#include <cstdint>
#include <functional>
#include <map>
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

// One typed value attached to an event: who, where, how much. Rumors, headlines and a viewer read these
// instead of parsing the human-readable text.
struct Field {
    std::string key;
    std::string value;
    bool numeric = false;
};
using Fields = std::vector<Field>;
Field field(std::string key, double v);
Field field(std::string key, int v);
Field field(std::string key, long v);
Field field(std::string key, const std::string& v);
Field field(std::string key, const char* v);

// One line of history. `cause` is the point of the log: it says why the thing
// happened, so an emergent crash can be told apart from a bug.
struct Event {
    long day = 0;
    EventType type = EventType::Network;
    std::string kind;       // stable machine name, e.g. "robbery", "gang_takeover"
    NodeId node = -1;
    bool minor = false;     // routine events, hidden unless tracing
    std::string text;
    std::string cause;
    Fields fields;
};

struct StateIo;   // saves and loads the log (save.cpp)

class EventLog {
public:
    using Sink = std::function<void(const Event&)>;

    // Fields for the next add(); consumed by it.
    void with(Fields f) { pending_ = std::move(f); }
    void add(long day, EventType type, std::string kind, NodeId node, std::string text, std::string cause,
             bool minor = false);
    void set_sink(Sink s) { sink_ = std::move(s); }

    const std::vector<Event>& events() const { return events_; }
    std::size_t count(EventType t) const { return counts_[static_cast<int>(t)]; }
    std::size_t count_kind(const std::string& kind) const {
        const auto it = kind_counts_.find(kind);
        return it == kind_counts_.end() ? 0 : it->second;
    }
    const std::map<std::string, std::size_t>& kind_counts() const { return kind_counts_; }

    // FNV-1a over every event; two runs with the same seed must match.
    std::uint64_t hash() const { return hash_; }

private:
    friend struct StateIo;
    std::vector<Event> events_;
    std::size_t counts_[kEventTypeCount] = {};
    std::map<std::string, std::size_t> kind_counts_;
    std::uint64_t hash_ = 1469598103934665603ULL;
    Fields pending_;
    Sink sink_;
};

}  // namespace frontier
