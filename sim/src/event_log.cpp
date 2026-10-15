#include "frontier/event_log.h"

namespace frontier {

const char* event_type_name(EventType t) {
    switch (t) {
        case EventType::Network:    return "NETWORK";
        case EventType::Production: return "PRODUCTION";
        case EventType::Population: return "POPULATION";
        case EventType::Famine:     return "FAMINE";
        case EventType::Trader:     return "TRADER";
        case EventType::Robbery:    return "ROBBERY";
        case EventType::Gang:       return "GANG";
        case EventType::Law:        return "LAW";
        case EventType::Shock:      return "SHOCK";
        case EventType::Trade:      return "TRADE";
    }
    return "?";
}

namespace {
void mix(std::uint64_t& h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= 0xff;
    h *= 1099511628211ULL;
}
}  // namespace

void EventLog::add(long day, EventType type, NodeId node, std::string text, std::string cause, bool minor) {
    Event e;
    e.day = day;
    e.type = type;
    e.node = node;
    e.minor = minor;
    e.text = std::move(text);
    e.cause = std::move(cause);
    ++counts_[static_cast<int>(type)];
    mix(hash_, std::to_string(day));
    mix(hash_, e.text);
    mix(hash_, e.cause);
    if (sink_) sink_(e);
    // Per-trade trace lines would swamp memory on long runs, so they are only streamed to the sink.
    if (type != EventType::Trade) events_.push_back(std::move(e));
}

}  // namespace frontier
