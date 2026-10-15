// Headless runner: loads data/, runs the simulation, prints the event log and a summary.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "frontier/date.h"
#include "frontier/format.h"
#include "frontier/simulation.h"

namespace {

void usage() {
    std::puts("usage: frontier_cli [--seed N] [--days N] [--data DIR] [--trace] [--quiet] [--report-days N]\n"
              "  --trace        also print minor events and every trade\n"
              "  --quiet        suppress the event log, print only reports and the summary\n"
              "  --report-days  print a town table every N days (0 = only at the end)");
}

std::string find_data_dir() {
    for (const char* c : {"data", "../data", "../../data", "../../../data", "../../../../data"}) {
        std::ifstream f(std::string(c) + "/goods.json");
        if (f) return c;
    }
    return "data";
}

void print_towns(const frontier::Simulation& sim) {
    using namespace frontier;
    const World& w = sim.world();
    std::printf("\n%s  traders %zu  gangs %zu  avg trader profit $%.1f/day\n", format_date(w.today).c_str(),
                sim.traders().size(), sim.gangs().size(), sim.avg_trader_profit());
    std::printf("%-18s %7s %7s %9s %9s %6s %7s %7s %7s\n", "town", "pop", "peak", "cash", "income/d", "hunger",
                "flour", "cattle", "gold");
    GoodId flour = -1, cattle = -1, gold = -1;
    for (std::size_t g = 0; g < w.goods.size(); ++g) {
        if (w.goods[g].id == "flour") flour = static_cast<GoodId>(g);
        if (w.goods[g].id == "beef_cattle") cattle = static_cast<GoodId>(g);
        if (w.goods[g].id == "gold") gold = static_cast<GoodId>(g);
    }
    for (std::size_t i = 0; i < w.nodes.size(); ++i) {
        const Node& n = w.nodes[i];
        if (n.fixed_market) continue;
        auto price = [&](GoodId g) { return g < 0 ? 0.0 : w.mid_price(static_cast<NodeId>(i), g); };
        std::printf("%-18s %7.0f %7.0f %9.0f %9.0f %6.2f %7.2f %7.2f %7.2f%s\n", n.name.c_str(), n.population,
                    n.peak_pop, n.cash, n.income_ema, n.hunger, price(flour), price(cattle), price(gold),
                    n.captor >= 0 ? strf("  (ruled by gang #%d, takes %.0f%%)", n.captor, n.toll * 100).c_str()
                    : n.ghost     ? "  (ghost town)"
                                  : "");
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace frontier;
    std::uint64_t seed = 0;
    bool seed_set = false, trace = false, quiet = false;
    int days = -1, report_days = 0;
    std::string dir;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--seed") { seed = std::strtoull(next(), nullptr, 10); seed_set = true; }
        else if (a == "--days") days = std::atoi(next());
        else if (a == "--data") dir = next();
        else if (a == "--report-days") report_days = std::atoi(next());
        else if (a == "--trace") trace = true;
        else if (a == "--quiet") quiet = true;
        else { usage(); return a == "--help" ? 0 : 2; }
    }
    if (dir.empty()) dir = find_data_dir();

    try {
        const WorldDef world = load_world(dir);
        Scenario scenario = load_scenario(dir, world);
        if (seed_set) scenario.seed = seed;
        if (days >= 0) scenario.days = days;

        Simulation sim(world, scenario);
        sim.trace = trace;
        if (!quiet)
            sim.log().set_sink([&](const Event& e) {
                if ((e.minor && !trace) || e.day < scenario.log_from) return;
                std::printf("%s [%s] %s\n", format_date(e.day).c_str(), event_type_name(e.type), e.text.c_str());
                if (!e.cause.empty()) std::printf("            because: %s\n", e.cause.c_str());
            });

        std::printf("Frontier Sim: seed %llu, %d days from %s, %zu nodes, %zu traders\n",
                    static_cast<unsigned long long>(scenario.seed), scenario.days, format_date(scenario.start).c_str(),
                    world.nodes.size(), sim.traders().size());
        for (int d = 1; d <= scenario.days; ++d) {
            sim.step();
            if (report_days > 0 && d % report_days == 0) print_towns(sim);
        }
        print_towns(sim);

        std::puts("\nleaders for hire:");
        for (const Leader& l : sim.leaders())
            std::printf("  %-34s %-16s skill %.2f  jobs %d  wins %d  earned %s%s\n", l.name.c_str(),
                        sim.world().nodes[static_cast<std::size_t>(l.at)].name.c_str(), l.skill, l.jobs, l.wins,
                        money(l.earned).c_str(), l.alive ? "" : "  (dead)");

        std::puts("\nprivate interests:");
        for (const Interest& in : sim.interests())
            std::printf("  %-34s fund %-10s losses to robbery %-10s detective parties %d (won %d)\n", in.name.c_str(),
                        money(in.fund).c_str(), money(in.losses_total).c_str(), in.ops, in.wins);

        const MoneyAudit audit = sim.money_audit();
        std::printf("\nmoney audit: started %s, now %s, residual %s (should be ~0)\n", money(audit.baseline).c_str(),
                    money(audit.actual).c_str(), money(audit.residual).c_str());
        for (int f = 0; f < kMoneyFlowCount; ++f)
            std::printf("  %-30s %14s\n", money_flow_name(static_cast<MoneyFlow>(f)), money(audit.flows[f]).c_str());

        std::puts("\nevent counts:");
        for (int t = 0; t < kEventTypeCount; ++t)
            std::printf("  %-11s %zu\n", event_type_name(static_cast<EventType>(t)),
                        sim.log().count(static_cast<EventType>(t)));
        std::printf("history hash: %016llx\n", static_cast<unsigned long long>(sim.log().hash()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
