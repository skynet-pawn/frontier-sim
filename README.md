# Frontier Sim

An open-source, top-down, systems-driven Old West game. The core is a simulated
frontier economy that runs itself: settlers push west, find resources, sell them,
and send goods back east. Boom towns, ghost towns, dangerous trails, and bandit
gangs emerge from simple rules.

This is a proof of concept meant to be read, studied, and remade. See
[CLAUDE.md](CLAUDE.md) for the full design brief.

**Status:** Stage 1 (headless sim), just started.

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

Runs are deterministic: the same seed produces the same history.

## License

Code: Apache License 2.0 (see [LICENSE](LICENSE)). Art and authored content may
be licensed separately when they are added.
