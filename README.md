# Frontier Sim

An open-source, top-down, systems-driven Old West game. The core is a simulated
frontier economy that runs itself: settlers push west, find resources, sell them,
and send goods back east. Boom towns, ghost towns, dangerous trails, and bandit
gangs emerge from simple rules.

This is a proof of concept meant to be read, studied, and remade. See
[CLAUDE.md](CLAUDE.md) for the full design brief.

**Status:** Stage 1 (headless sim) is running end to end: 24 nodes, traders and bandit gangs on a price-driven
economy, a posse system, scheduled railroads and shocks, and a cause-tagged event log. Calibration is rough
(see "Known issues"). Nothing here is historically tuned yet.

## Layout

- `sim/`  simulation core (static library, no rendering or engine dependencies)
- `cli/`  headless runner
- `data/` goods, routes, and events as config files
- `docs/` design notes

## Build

```
cmake -S . -B build
cmake --build build
./build/cli/frontier_cli 42      # argument is the seed
```

Runs are deterministic: the same seed produces the same history (`ctest` checks this).

```
frontier_cli --seed 7 --days 2000 --report-days 500   # town table every 500 days
frontier_cli --quiet                                  # tables and summary only
frontier_cli --trace                                  # also minor events and every trade
```

Each event prints with a `because:` line giving its cause. When something looks wrong, read the chain of
causes before changing any numbers.

## Tools

```
frontier_batch --seeds 20 --runs        # same scenario under 20 seeds: distributions, per-town outcomes, problems
frontier_batch --seeds 50 --csv out.csv # one row per run
ctest                                   # determinism, money audit and invariants, scripted-action tests
```

The **money audit** reconciles every cash balance against recorded flows in and out of the frontier economy
(eastern markets, government pay, freight, emigrants and so on). Its residual must be exactly zero; the batch
runner and `money_test` fail if it is not. The batch runner also checks invariants every day (no negative
stock or cash, no NaNs).

**Actions.** Traders change the world only through `act_buy`, `act_sell` and `act_travel` (and gangs through
`act_gang_relocate`). The AI planner calls exactly these, so anything else, a script or later a player, gets the same
costs, risks and log entries. `add_trader(..., controlled=true)` makes an outfit that waits for orders.

**Events** carry a stable `kind` and typed `fields` (who, where, how much) next to the human-readable text and
cause, ready for rumors, headlines or a viewer. Eastern markets have finite depth: a big sale depresses the price.

## How it works

- **World** (`data/world.json`, `data/goods.json`): nodes (towns, forts, camps, ranges, eastern markets) and
  edges (trail, river, rail, each with dates it opens or closes). Prices come from stock versus normal stock;
  freight cost comes from distance, weight and mode.
- **Traders** score every (destination, good, guards) option as expected profit minus risk, and remember
  where they were robbed. **Gangs** form from idle men, park on busy trails and fence loot in a town.
  Robbery angers towns; funded towns raise **posses**, and can hire a **leader for hire**
  (a skill multiplier with a fee who gains experience, can die, and sometimes emerges from a winning posse).
- **Escalation.** Railroads, express companies and stock associations (`interests` in `world.json`) keep a fund,
  count robberies on trails touching their stations, and hire detectives who travel far and hit hard.
  A town with no interest, Army post or state capital nearby has no backup.
- **Capture.** A gang that spends heavily in a small, poorly backed town gains influence and takes it over:
  tribute, a toll on all trade, taxes diverted. A competent boss lets the town thrive; a clumsy one squeezes
  harder as trade dries up, until the gang starves, splits, or the townspeople hang its boss.
- **Towns** hold as many people as their trade and government income supports, so a trail moving away kills them.
- All tunables are in `Params` (`sim/include/frontier/config.h`) and overridable in `data/scenario.json`.

## Known issues

- Gangs routinely hit their 30-man cap and beat posses; law is too weak.
- Cash piles up in some towns (Helena, Tascosa) while others starve for it.
- Frontier towns are mostly smaller than their real counterparts; support income and trader count need calibration.
- Freight rates, prices and populations are from memory and unverified.

## Third-party code

`sim/third_party/nlohmann/json.hpp`: [nlohmann/json](https://github.com/nlohmann/json) 3.11.3, MIT license.

## License

Code: Apache License 2.0 (see [LICENSE](LICENSE)). Art and authored content may
be licensed separately when they are added.
