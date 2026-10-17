// Batch runner: runs the same scenario under many seeds and reports distributions, so an emergent story
// can be told apart from a fluke or a bug. One run tells you what happened; twenty tell you what tends to.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "frontier/date.h"
#include "frontier/format.h"
#include "frontier/stats.h"

namespace {

using frontier::RunStats;

void usage() {
    std::puts("usage: frontier_batch [--seeds N] [--first-seed S] [--days N] [--threads T] [--data DIR]\n"
              "                      [--runs] [--csv FILE]\n"
              "  --runs   print one line per run\n"
              "  --csv    write one row per run");
}

std::string find_data_dir() {
    for (const char* c : {"data", "../data", "../../data", "../../../data", "../../../../data"}) {
        std::ifstream f(std::string(c) + "/goods.json");
        if (f) return c;
    }
    return "data";
}

// min / median / max of a metric across runs.
struct Spread {
    double min = 0, median = 0, mean = 0, max = 0;
};

Spread spread(std::vector<double> v) {
    Spread s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    return s;
}

template <class F>
void row(const char* label, const std::vector<RunStats>& runs, F get, const char* unit = "") {
    std::vector<double> v;
    for (const RunStats& r : runs) v.push_back(get(r));
    const Spread s = spread(v);
    std::printf("  %-34s %12.4g %12.4g %12.4g %12.4g %s\n", label, s.min, s.median, s.mean, s.max, unit);
}

}  // namespace

int main(int argc, char** argv) {
    int seeds = 20, days = -1, threads = static_cast<int>(std::thread::hardware_concurrency());
    std::uint64_t first = 1;
    bool print_runs = false;
    std::string dir, csv;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--seeds") seeds = std::atoi(next());
        else if (a == "--first-seed") first = std::strtoull(next(), nullptr, 10);
        else if (a == "--days") days = std::atoi(next());
        else if (a == "--threads") threads = std::atoi(next());
        else if (a == "--data") dir = next();
        else if (a == "--csv") csv = next();
        else if (a == "--runs") print_runs = true;
        else { usage(); return a == "--help" ? 0 : 2; }
    }
    if (dir.empty()) dir = find_data_dir();
    threads = std::max(1, std::min(threads, seeds));

    try {
        const frontier::WorldDef world = frontier::load_world(dir);
        frontier::Scenario scenario = frontier::load_scenario(dir, world);
        if (days >= 0) scenario.days = days;

        std::printf("Frontier batch: %d seeds (%llu..%llu), %d days from %s, %d threads\n", seeds,
                    static_cast<unsigned long long>(first), static_cast<unsigned long long>(first + seeds - 1),
                    scenario.days, frontier::format_date(scenario.start).c_str(), threads);

        // Seeds are handed out dynamically but results land in seed order, so output is deterministic.
        std::vector<RunStats> runs(static_cast<std::size_t>(seeds));
        std::atomic<int> next_job{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&]() {
                for (int j = next_job++; j < seeds; j = next_job++)
                    runs[static_cast<std::size_t>(j)] = frontier::run_and_collect(world, scenario, first + static_cast<std::uint64_t>(j));
            });
        for (std::thread& t : pool) t.join();

        if (print_runs) {
            std::printf("\n%5s %7s %7s %6s %6s %8s %6s %6s %6s %6s %9s %10s %3s\n", "seed", "tr.min", "tr.end", "robb", "gangs",
                        "posse w/r", "takeov", "freed", "detect", "ghosts", "pop end", "east $M", "ok");
            for (const RunStats& r : runs)
                std::printf("%5llu %7d %7d %6d %6d %4d/%-4d %6d %6d %6d %6d %9.0f %10.1f %3s\n",
                            static_cast<unsigned long long>(r.seed), r.traders_min, r.traders_end, r.robberies, r.gangs_formed,
                            r.posse_victories, r.posse_routs, r.takeovers, r.towns_freed, r.detective_parties, r.ghost_towns,
                            r.pop_end_total, r.east_buys / 1e6, r.invariants_ok && std::abs(r.residual) < 1.0 ? "ok" : "BAD");
        }

        std::printf("\n  %-34s %12s %12s %12s %12s\n", "metric", "min", "median", "mean", "max");
        row("traders at end", runs, [](const RunStats& r) { return double(r.traders_end); });
        row("  of them in freight houses", runs, [](const RunStats& r) { return double(r.house_outfits_end); });
        row("freight houses founded", runs, [](const RunStats& r) { return double(r.houses_founded); });
        row("  houses open at end", runs, [](const RunStats& r) { return double(r.houses_end); });
        row("  house drivers killed", runs, [](const RunStats& r) { return double(r.house_killed); });
        row("  house outfits broke (house could not cover)", runs, [](const RunStats& r) { return double(r.house_broke); });
        row("  broken outfits a house made good", runs, [](const RunStats& r) { return double(r.house_recaps); });
        row("traders, lowest point", runs, [](const RunStats& r) { return double(r.traders_min); });
        row("  of them eastern firms", runs, [](const RunStats& r) { return double(r.firms_end); });
        row("freelancers who arrived", runs, [](const RunStats& r) { return double(r.freelancers_entered); });
        row("firms who arrived", runs, [](const RunStats& r) { return double(r.firms_entered); });
        row("settled (made their pile)", runs, [](const RunStats& r) { return double(r.settled); });
        row("firms that withdrew", runs, [](const RunStats& r) { return double(r.firm_withdrawals); });
        row("traders ruined or killed", runs, [](const RunStats& r) { return double(r.trader_exits); });
        row("drover share at end", runs, [](const RunStats& r) { return r.drover_share_end; });
        row("robberies (whole load taken)", runs, [](const RunStats& r) { return double(r.robberies); });
        row("tolls (gang took part of a load)", runs, [](const RunStats& r) { return double(r.tolls); });
        row("  of them over $1,500", runs, [](const RunStats& r) { return double(r.robberies_over_1500); });
        row("gangs formed", runs, [](const RunStats& r) { return double(r.gangs_formed); });
        row("posses raised", runs, [](const RunStats& r) { return double(r.posses); });
        row("  posse victories", runs, [](const RunStats& r) { return double(r.posse_victories); });
        row("  posses routed", runs, [](const RunStats& r) { return double(r.posse_routs); });
        row("detective parties", runs, [](const RunStats& r) { return double(r.detective_parties); });
        row("town takeovers by gangs", runs, [](const RunStats& r) { return double(r.takeovers); });
        row("  towns freed", runs, [](const RunStats& r) { return double(r.towns_freed); });
        row("  vigilance committees", runs, [](const RunStats& r) { return double(r.vigilance); });
        row("sales made along the way", runs, [](const RunStats& r) { return double(r.waypoint_sales); });
        row("merchant orders posted", runs, [](const RunStats& r) { return double(r.merchant_orders); });
        row("  town hired its own freighters", runs, [](const RunStats& r) { return double(r.convoys_hired); });
        row("  withdrawn unfilled", runs, [](const RunStats& r) { return double(r.orders_expired); });
        row("standing contracts signed", runs, [](const RunStats& r) { return double(r.contracts_signed); });
        row("  dropped by the town", runs, [](const RunStats& r) { return double(r.contracts_cancelled); });
        row("  delivered under contract ($M at contract price)", runs, [](const RunStats& r) { return r.contract_revenue / 1e6; });
        row("  houses' margin on contracts ($M)", runs, [](const RunStats& r) { return (r.contract_revenue - r.contract_outlay) / 1e6; });
        row("  goods delivered ($M)", runs, [](const RunStats& r) { return r.merchant_delivered / 1e6; });
        row("  goods robbed on the way ($M)", runs, [](const RunStats& r) { return r.merchant_lost / 1e6; });
        row("food shortage episodes (merged)", runs, [](const RunStats& r) { return double(r.shortages); });
        row("  distinct towns that ran short", runs, [](const RunStats& r) { return double(r.shortage_towns); });
        row("  raw shortage log entries", runs, [](const RunStats& r) { return double(r.shortage_events); });
        row("people starved to death", runs, [](const RunStats& r) { return r.starved; });
        row("game hunted (head of beef-equivalent)", runs, [](const RunStats& r) { return r.game_taken; });
        row("  share of the herds left at the end", runs, [](const RunStats& r) { return r.game_left; });
        row("famine episodes (merged)", runs, [](const RunStats& r) { return double(r.famines); });
        row("  distinct towns with a famine", runs, [](const RunStats& r) { return double(r.famine_towns); });
        row("  raw famine log entries", runs, [](const RunStats& r) { return double(r.famine_events); });
        row("ghost towns (events)", runs, [](const RunStats& r) { return double(r.ghost_towns); });
        row("total population, start", runs, [](const RunStats& r) { return r.pop_start_total; });
        row("total population, end", runs, [](const RunStats& r) { return r.pop_end_total; });
        row("money in circulation, end ($M)", runs, [](const RunStats& r) { return r.money_end / 1e6; });
        row("east bought from frontier ($M)", runs, [](const RunStats& r) { return r.east_buys / 1e6; });
        row("east sold to frontier ($M)", runs, [](const RunStats& r) { return -r.east_sells / 1e6; });
        row("emigrants took away ($M)", runs, [](const RunStats& r) { return -r.emigrated / 1e6; });
        row("richest town's cash ($M)", runs, [](const RunStats& r) { return r.richest_cash / 1e6; });
        row("lowest eastern price multiplier", runs, [](const RunStats& r) { return r.east_price_low; });

        // What the East bought, by good: tells you what the whole economy is really built on.
        std::printf("\n  east purchases by good (median $M across runs):\n");
        for (std::size_t g = 0; g < world.goods.size(); ++g) {
            std::vector<double> bought, sold;
            for (const RunStats& r : runs) {
                bought.push_back(r.east_bought_by_good[g] / 1e6);
                sold.push_back(r.east_sold_by_good[g] / 1e6);
            }
            std::printf("    %-14s buys %8.2f   sells %8.2f\n", world.goods[g].id.c_str(), spread(bought).median, spread(sold).median);
        }

        // Who carries what: the specialization check. Median dollars of goods each kind of trader bought.
        std::printf("\n  goods bought by traders (median $M): freelancers | firms\n");
        for (std::size_t g = 0; g < world.goods.size(); ++g) {
            std::vector<double> fl, fi;
            for (const RunStats& r : runs) {
                fl.push_back(r.bought_by_freelancers[g] / 1e6);
                fi.push_back(r.bought_by_firms[g] / 1e6);
            }
            std::printf("    %-14s %8.2f | %8.2f\n", world.goods[g].id.c_str(), spread(fl).median, spread(fi).median);
        }

        // Which towns tend to live or die. Median end population against where they started.
        std::printf("\n  %-18s %8s %8s %8s %8s %8s %8s\n", "town", "start", "peak", "end", "ghost@end", "ever ghost", "ruled");
        for (std::size_t i = 0; i < world.nodes.size(); ++i) {
            if (world.nodes[i].fixed_market) continue;
            std::vector<double> peak, end, ruled;
            int ghost_end = 0, ever = 0;
            for (const RunStats& r : runs) {
                peak.push_back(r.towns[i].pop_peak);
                end.push_back(r.towns[i].pop_end);
                ruled.push_back(r.towns[i].days_ruled_by_gang);
                ghost_end += r.towns[i].ghost_end ? 1 : 0;
                ever += r.towns[i].ever_ghost ? 1 : 0;
            }
            std::printf("  %-18s %8.0f %8.0f %8.0f %7d%% %9d%% %7.0f d\n", world.nodes[i].name.c_str(), world.nodes[i].population,
                        spread(peak).median, spread(end).median, 100 * ghost_end / seeds, 100 * ever / seeds, spread(ruled).mean);
        }

        // Farms: how many farmers each farming district holds, and when the new districts were settled.
        std::printf("\n  farming districts (median farmers): %-8s %8s %8s   founded\n", "start", "peak", "end");
        for (std::size_t i = 0; i < world.nodes.size(); ++i) {
            const frontier::NodeDef& nd = world.nodes[i];
            if (nd.fixed_market || (nd.farm_capacity <= 0 && !nd.latent)) continue;
            std::vector<double> st, pk, en, fy;
            int founded = 0;
            for (const RunStats& r : runs) {
                st.push_back(r.towns[i].farmers_start);
                pk.push_back(r.towns[i].farmers_peak);
                en.push_back(r.towns[i].farmers_end);
                if (r.towns[i].founded_day >= 0) { ++founded; fy.push_back(1874.0 + r.towns[i].founded_day / 365.25); }
            }
            std::printf("  %-34s %8.0f %8.0f %8.0f", nd.name.c_str(), spread(st).median, spread(pk).median, spread(en).median);
            if (nd.latent) {
                if (founded) std::printf("   %3d%% of runs, median year %.1f\n", 100 * founded / seeds, spread(fy).median);
                else std::printf("   never\n");
            } else {
                std::printf("\n");
            }
        }

        // Where traders buy their flour: the East, or the farm districts nearest the hungry.
        std::printf("\n  flour traders bought, by place (mean $k per run):\n");
        for (std::size_t i = 0; i < world.nodes.size(); ++i) {
            double sum = 0;
            for (const RunStats& r : runs) sum += r.towns[i].flour_bought;
            if (sum / seeds >= 500) std::printf("    %-18s %8.1f\n", world.nodes[i].name.c_str(), sum / seeds / 1000);
        }

        std::printf("\n  goods ordered in by each town's merchants (mean $k per run):\n");
        for (std::size_t i = 0; i < world.nodes.size(); ++i) {
            double sum = 0;
            for (const RunStats& r : runs) sum += r.towns[i].imported;
            if (sum / seeds >= 500) std::printf("    %-18s %8.1f\n", world.nodes[i].name.c_str(), sum / seeds / 1000);
        }

        // What each town lives on: share of lifetime earnings, averaged over runs.
        std::printf("\n  what towns live on (share of lifetime earnings):\n");
        std::printf("  %-18s %8s %9s %9s %7s %12s\n", "town", "goods", "visitors", "govt", "other", "$/day (mean)");
        for (std::size_t i = 0; i < world.nodes.size(); ++i) {
            if (world.nodes[i].fixed_market) continue;
            double s = 0, v = 0, g = 0, t = 0;
            for (const RunStats& r : runs) {
                s += r.towns[i].earned_sales; v += r.towns[i].earned_visitors;
                g += r.towns[i].earned_government; t += r.towns[i].earned_total;
            }
            if (t <= 0) continue;
            std::printf("  %-18s %7.0f%% %8.0f%% %8.0f%% %6.0f%% %12.0f\n", world.nodes[i].name.c_str(), 100 * s / t, 100 * v / t,
                        100 * g / t, 100 * (t - s - v - g) / t, t / seeds / scenario.days);
        }

        // Problems.
        int bad = 0;
        for (const RunStats& r : runs) {
            if (!r.invariants_ok) {
                std::printf("\nINVARIANT VIOLATION seed %llu: %s\n", static_cast<unsigned long long>(r.seed), r.first_violation.c_str());
                ++bad;
            }
            if (std::abs(r.residual) >= 1.0) {
                std::printf("\nMONEY LEAK seed %llu: residual %s\n", static_cast<unsigned long long>(r.seed), frontier::money(r.residual).c_str());
                ++bad;
            }
        }
        std::printf("\n%s\n", bad ? "PROBLEMS FOUND (see above)" : "no invariant violations; money audit balanced in every run");

        if (!csv.empty()) {
            std::ofstream f(csv);
            f << "seed,hash,traders_min,traders_end,robberies,gangs_formed,posses,posse_victories,posse_routs,detectives,takeovers,freed,vigilance,famines,ghost_towns,pop_start,pop_end,money_end,east_buys,east_sells,emigrated,richest_cash,famine_towns,famine_events,shortages,shortage_events,shortage_towns,starved,game_taken,game_left,contracts_signed,contracts_cancelled,contract_revenue,contract_outlay\n";
            for (const RunStats& r : runs)
                f << r.seed << ',' << r.hash << ',' << r.traders_min << ',' << r.traders_end << ',' << r.robberies << ',' << r.gangs_formed << ',' << r.posses << ','
                  << r.posse_victories << ',' << r.posse_routs << ',' << r.detective_parties << ',' << r.takeovers << ',' << r.towns_freed << ',' << r.vigilance << ','
                  << r.famines << ',' << r.ghost_towns << ',' << r.pop_start_total << ',' << r.pop_end_total << ',' << r.money_end << ',' << r.east_buys << ','
                  << r.east_sells << ',' << r.emigrated << ',' << r.richest_cash << ',' << r.famine_towns << ',' << r.famine_events << ',' << r.shortages << ',' << r.shortage_events << ',' << r.shortage_towns << ',' << r.starved << ',' << r.game_taken << ',' << r.game_left << ',' << r.contracts_signed << ',' << r.contracts_cancelled << ',' << r.contract_revenue << ',' << r.contract_outlay << '\n';
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
