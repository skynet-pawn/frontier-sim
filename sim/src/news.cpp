// Newspapers: the frontier's way of knowing what happened somewhere else.
//
// A town that is big enough for long enough gets a paper. Each day the new entries in the event log that a paper would
// print become short headlines (no articles, just the fact), and the headline travels: it reaches the papers within its
// reach after a delay that grows with distance. Two things use it:
//   - a gang's name. A region hunts only the gangs it has heard of (crime.cpp, law.cpp), so a paper that prints a gang's
//     deeds raises its fame in that paper's region, and a gang that moves to country whose papers have never named it
//     starts fresh;
//   - the player, who reads a town's paper to learn where the trouble and the money are.
// Nothing here is a control: it only carries the log, never changes what happened.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

#include "frontier/format.h"
#include "frontier/simulation.h"

namespace frontier {

namespace {

double dist(double x1, double y1, double x2, double y2) { return std::hypot(x1 - x2, y1 - y2); }

std::string field_of(const Event& e, const char* key) {
    for (const Field& f : e.fields)
        if (f.key == key) return f.value;
    return std::string();
}

double number_of(const Event& e, const char* key) {
    const std::string s = field_of(e, key);
    return s.empty() ? 0.0 : std::atof(s.c_str());
}

}  // namespace

// Turns a log entry into the line a paper would print, or says no. `weight` is how far and how loudly it travels;
// `gang` is the outlaw gang it names (its fame rises where the story is read), or -1.
bool Simulation::make_headline(const Event& e, Headline& h) const {
    h.day = e.day;
    h.node = e.node;
    h.kind = e.kind;
    h.gang = -1;
    h.weight = 0.5;
    h.crime = false;
    const std::string where = e.node >= 0 ? nname(e.node) : std::string("the territory");
    if (e.kind == "robbery" && !e.minor) {
        h.text = strf("Highwaymen Rob Freight on the %s: %s Lost", field_of(e, "edge").c_str(), money(number_of(e, "value")).c_str());
        h.gang = static_cast<int>(number_of(e, "gang"));
        h.weight = number_of(e, "value") >= 10000 ? 1.2 : 0.7;
        h.crime = true;
    } else if (e.kind == "gang_takeover") {
        h.text = strf("Outlaws Seize %s: Gang of %d Now Rules the Town", where.c_str(), static_cast<int>(number_of(e, "men")));
        h.gang = static_cast<int>(number_of(e, "gang"));
        h.weight = 2.5;
        h.crime = true;
    } else if (e.kind == "town_freed") {
        h.text = strf("%s Throws Off Outlaw Rule", where.c_str());
        h.weight = 1.2;
    } else if (e.kind == "posse_victory") {
        h.text = strf("Posse From %s Destroys Outlaw Gang", field_of(e, "origin").c_str());
        h.weight = 0.8;
    } else if (e.kind == "posse_routed") {
        h.text = strf("Outlaws Rout %s Posse", field_of(e, "origin").c_str());
        h.gang = static_cast<int>(number_of(e, "gang"));
        h.weight = 1.0;
        h.crime = true;
    } else if (e.kind == "vigilance_formed") {
        h.text = strf("Vigilance Committee Organizes at %s", where.c_str());
        h.weight = 0.8;
    } else if (e.kind == "detectives_hired") {
        h.text = strf("%s Puts Detectives on the Trail of Outlaws", field_of(e, "interest").c_str());
        h.weight = 0.8;
    } else if (e.kind == "gang_war") {
        h.text = strf("Outlaw Gangs Fight It Out on the %s", field_of(e, "edge").c_str());
        h.weight = 0.6;
    } else if (e.kind == "law_established") {
        h.text = number_of(e, "law") >= 2 ? strf("Sheriff Opens Office at %s", where.c_str()) : strf("%s Hires a Marshal", where.c_str());
        h.weight = 0.6;
    } else if (e.kind == "band_hanged") {
        h.text = strf("Road Agents Hanged at %s", where.c_str());
        h.weight = 1.0;
    } else if (e.kind == "band_jailed" && !e.minor) {
        h.text = strf("Highwaymen Jailed at %s", where.c_str());
        h.weight = 0.6;
    } else if (e.kind == "shortage") {
        h.text = strf("%s Short of Provisions", where.c_str());
        h.weight = 0.6;
    } else if (e.kind == "food_crisis") {
        h.text = strf("Hunger Takes Its Toll At %s", where.c_str());
        h.weight = 1.6;
    } else if (e.kind == "ghost_town") {
        h.text = strf("%s Abandoned", where.c_str());
        h.weight = 1.2;
    } else if (e.kind == "town_founded") {
        h.text = strf("New Settlement Rises at %s", where.c_str());
        h.weight = 0.9;
    } else if (e.kind == "rush") {
        h.text = strf("Rush to %s", where.c_str());
        h.weight = 1.2;
    } else if (e.kind == "reserve_exhausted") {
        h.text = strf("Diggings at %s Play Out", where.c_str());
        h.weight = 0.9;
    } else if (e.kind == "road_closure") {
        h.text = strf("Roads Closed Near %s", where.c_str());
        h.weight = 0.8;
    } else if (e.kind == "crop_failure") {
        h.text = "Crops Fail on the Plains";
        h.weight = 1.0;
    } else if (e.kind == "bushwhack_notable") {
        h.text = e.text;
        h.gang = static_cast<int>(number_of(e, "gang"));
        h.weight = 2.0;
        h.crime = true;
    } else if (e.kind == "bushwhack_found") {
        h.text = e.text;
        h.gang = static_cast<int>(number_of(e, "gang"));
        h.weight = 0.9;
        h.crime = true;
    } else {
        return false;
    }
    return true;
}

void Simulation::news_day() {
    const Params& p = world_.params;
    if (!p.news_stages) return;

    // Papers open and close slowly: a town needs a few months at the size to support one.
    PhaseTimer papers_timer(phase_time_[11]);
    if (world_.today % 10 == 0) {
        paper_nodes_.clear();
        for (std::size_t i = 0; i < world_.nodes.size(); ++i) {
            Node& n = world_.nodes[i];
            if (n.fixed_market || !n.founded) continue;
            const double civ = n.market_population();
            if (!n.paper && !n.ghost && civ >= p.news_paper_pop) {
                if ((n.paper_streak += 10) >= p.news_paper_days) {
                    n.paper = 1;
                    n.paper_streak = 0;
                    log_.with({field("node", n.name), field("pop", civ)});
                    log_.add(world_.today, EventType::Population, "paper_founded", static_cast<NodeId>(i), strf("%s gets a newspaper", n.name.c_str()),
                             strf("%.0f townspeople can support a printer", civ));
                }
            } else if (n.paper && (n.ghost || civ < p.news_paper_pop / 3)) {
                if ((n.paper_streak += 10) >= p.news_paper_days) {
                    n.paper = 0;
                    n.paper_streak = 0;
                    log_.with({field("node", n.name), field("pop", civ)});
                    log_.add(world_.today, EventType::Population, "paper_closed", static_cast<NodeId>(i), strf("The %s paper closes", n.name.c_str()),
                             strf("only %.0f townspeople are left to buy it", civ));
                }
            } else {
                n.paper_streak = 0;
            }
            if (n.paper) paper_nodes_.push_back(static_cast<NodeId>(i));
        }
    }
    if (papers_.size() < world_.nodes.size()) papers_.resize(world_.nodes.size());

    // New log entries become headlines, queued for every paper in reach.
    const auto& events = log_.events();
    for (; news_cursor_ < events.size(); ++news_cursor_) {
        const Event& e = events[news_cursor_];
        Headline h;
        if (!make_headline(e, h)) continue;
        const auto shared = std::make_shared<const Headline>(h);
        const double reach = p.news_range * std::clamp(0.3 + 0.35 * h.weight, 0.3, 1.0);
        double ox = 0, oy = 0;
        bool has_origin = false;
        if (h.node >= 0) { ox = world_.nodes[World::idx(h.node)].x; oy = world_.nodes[World::idx(h.node)].y; has_origin = true; }
        const bool local_paper = has_origin && world_.nodes[World::idx(h.node)].paper;
        for (NodeId pn : paper_nodes_) {
            const Node& pnode = world_.nodes[World::idx(pn)];
            double d = 0;
            if (has_origin) d = dist(ox, oy, pnode.x, pnode.y);
            else if (h.kind == "crop_failure") d = 0;   // a regional shock: every paper prints it
            if (d > reach) continue;
            // The town's own paper has it next day; others wait for the wire or a rider, and a story from a place with no
            // paper has to be carried to one first.
            const long due = e.day + 1 + static_cast<long>(std::ceil(d / p.news_speed)) + (local_paper ? 0 : 2);
            news_queue_.push_back({due, pn, shared});
        }
    }

    // Deliver what is due. The queue is short-lived: stories a few days old at most.
    PhaseTimer delivery_timer(phase_time_[12]);
    std::size_t keep = 0;
    for (std::size_t i = 0; i < news_queue_.size(); ++i) {
        const NewsDelivery& d = news_queue_[i];
        if (d.due > world_.today) { news_queue_[keep++] = d; continue; }
        auto& shelf = papers_[World::idx(d.paper)];
        shelf.push_back(d.headline);
        if (shelf.size() > static_cast<std::size_t>(p.news_keep)) shelf.erase(shelf.begin());
        ++headlines_printed_;
        const Headline& h = *d.headline;
        if (h.crime && h.gang >= 0) {
            if (Gang* g = find_gang(h.gang)) {
                const int region = world_.nodes[World::idx(d.paper)].region;
                if (region >= 0 && g->active) {
                    if (g->known.size() < world_.region_names.size()) g->known.resize(world_.region_names.size(), 0.0f);
                    float& k = g->known[static_cast<std::size_t>(region)];
                    k += static_cast<float>(p.news_fame * h.weight * std::max(0.0, 1.0 - k / p.news_fame_cap));
                }
            }
        }
    }
    news_queue_.resize(keep);
}

}  // namespace frontier
