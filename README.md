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

## How it works

- **World** (`data/world.json`, `data/goods.json`): nodes (towns, forts, camps, ranges, eastern markets) and
  edges (trail, river, rail, each with dates it opens or closes). Prices come from stock versus normal stock;
  freight cost comes from distance, weight and mode.
- **Traders** score every (destination, good, guards) option as expected profit minus risk, and remember
  where they were robbed. **Gangs** form from idle men, park on busy trails and fence loot in a town.
  Robbery angers towns; funded towns raise **posses**.
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
