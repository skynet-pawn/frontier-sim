# Frontier Sim: Project Brief

An open-source, top-down, systems-driven Old West game. The core is a simulated frontier economy that runs itself: settlers push west, find resources, sell them, and send goods back east. Money gets made, and other entities try to take it. Boom towns, ghost towns, dangerous trails, and bandit gangs emerge from simple rules.

## Purpose and success criteria

- This is a proof of concept for an idea, not a commercial product. It will be released open source so that someone with stronger art skills and assets can read the code, see how it's built, and potentially remake it at higher fidelity or scale.
- The demo works if a stranger can watch it for two minutes and see emergence: a town dying because the trail moved, a gang getting rich off one route until a posse forms.
- First measure of success is personal: does the simulated world produce stories worth watching?
- Code should be readable and well-structured, since other people are meant to study it. Prefer clarity over cleverness. Keep simulation logic separate from rendering so an artist or another developer can swap the presentation layer without touching the sim.

## Guiding principles

1. **Simulation first, game second.** The economy and agent systems are the hard, valuable part. Build and validate them headless before any graphics.
2. **Emergence from systems, not smart individuals.** Simple agents with needs, memory, and schedules interacting through shared world state.
3. **Traditional game AI, not ML.** Use utility AI (score actions by need and situation) and per-agent memory. Machine learning is only for offline parameter tuning, if at all. Every agent decision must be inspectable and debuggable.
4. **Experience shapes motivation.** Agents carry memory and adjust their own weights. A trader robbed twice on a route raises his risk aversion, hires an escort, or switches routes. A gang avoids a town that fought back. This is plain per-agent state feeding utility scores.
5. **Spirals are allowed when they would happen in the real world.** Boom/bust, overgrazing and the 1886-87 die-out, bison collapse, range-war escalation, and credit panics are all acceptable. Every collapse needs a traceable causal story rooted in the physical model, not numeric drift. Model the dampers too (scarcity pricing, migration, substitution, people giving up) so spirals run their course and sometimes recover.
6. **Log causes.** Keep an event log that records why things happened. This is the main tool for telling an emergent crash from a bug, and it later feeds player-facing rumors and headlines.
7. **Determinism.** Seeded generation and replayable runs, so bugs are reproducible.
8. **Legibility.** Emergence the player can't perceive might as well not exist. Plan to surface it through price boards, rumor feeds, visible caravans, and burned-out homesteads.

## Architecture: three layers

World size and simulation cost are separate. The world is large; detailed simulation is small and local.

1. **Strategic layer (whole map, always running).** A graph of nodes (towns, forts, railheads, mines, ranges) and edges (trails, rivers, rail). Each node holds aggregate state: population, stockpiles, local prices, cash balance, danger. Cheap enough to update daily across thousands of nodes. The economy lives here.
2. **Mobile entities on the graph.** Caravans, herds, gangs, and posses are single objects moving along edges, with a headcount and strength value. A gang of 12 is one object, not 12 simulated people.
3. **Tactical layer (near the player only, later).** Full individual detail and real-time encounters exist only in a bubble around the player. Off-screen events resolve statistically from strength, guards, and terrain, and the results are written to the event log. The aggregate model and local model must stay consistent so the player never returns to a town that contradicts what the sim said happened.

Use level-of-detail simulation: distant areas run as cheap aggregates, and only agents near the player get full behavior.

## Economic model (calibrate to history)

The frontier ran on arbitrage across distance plus heavy friction. Price gaps and freight costs should fall out of distance and bulk, so trade flows emerge without being scripted.

**Engines of activity**
- Extraction booms (mining, buffalo hides): sudden concentrated demand, sudden collapse when the resource thins. Supply merchants often out-earned the miners.
- Cattle: cheap in Texas, roughly ten times the value at northern railheads or stocking ranges after the Civil War. That gap paid for the drives.
- Government spending: forts and agency beef contracts anchor towns with steady cash and guaranteed buyers in remote places.
- Railheads: the biggest town-makers and town-killers. A town booms until the rails move on or quarantine lines push the trail west.
- Manufactured goods (flour, tools, whiskey, cloth, ammunition): cheap in the East, many times costlier far from rail.

**Frictions**
- Freight cost: ox-team wagons cover roughly 10-15 miles a day. Distance from rail translates directly into price. Steamboat to Fort Benton is a major northern link. Railroad arrival collapses local prices and kills old routes.
- Cash scarcity: credit, scrip, and IOUs carry much of the trade. Merchants and banks lend against cattle, land, and claims. This is where panics cascade.
- Time and risk: drives take months, with river crossings, stampedes, weather, disease (Texas fever quarantine shifts routes), and theft.
- Water and land control: whoever holds the water holds the range around it, the root of range wars and fencing disputes.

**Crime and law**
- Loot sources: cash payrolls, express shipments, stagecoaches, trains, herds. Rustling requires a market for the cattle.
- Law is funded by the economy: fee-based sheriffs and marshals, cattle associations paying detectives and brand inspectors. Where money flows, enforcement follows. Vigilantes appear where it doesn't.
- Crime follows value: violence clusters where cash, liquor, and transient men meet.

**Model layers**
1. Goods: start with 5-8 (beef cattle, flour/staples, tools, whiskey, ammunition, hides, ore, timber), each with base production cost and weight/bulk.
2. Nodes: local supply, demand, population, cash.
3. Routes: distance, terrain, danger, seasonal factor.
4. Agents: traders, drovers, settlers, bandits, lawmen, merchants. They move goods toward the biggest price gap, discounted by risk.
5. Shocks: weather and winter severity, disease, railroad extensions, strikes, resource depletion, as scheduled or random events on the physical layer.
6. Growth rules: settlements upgrade or attract population when food surplus, safety, and trade traffic cross thresholds, and decline when those fail.

Historical figures cited in design discussion were from memory and approximate. Verify prices, wages, freight rates, and trail distances against sources before calibrating.

**Time window (open decision).** 1866-1890 spans very different economies: early Texas-to-Kansas drives, the 1870s hide boom, and the 1880s northern bonanza ending in the 1886-87 winter. Either compress into one timeline with scheduled eras, or start with one slice. Suggested first slice: mid-1870s to mid-1880s drives and railheads.

## World generation

- Setting is real geography, from South Texas near the Mexican border to the Milk River in Montana. Use free elevation, river, and land-cover datasets (USGS or similar) for terrain.
- Generate the human layer procedurally: grazing quality from rainfall and terrain, ore from geology-flavored noise, timber and water from real data. Score locations for water, flat ground, resources, and trail proximity, and let the sim found towns where score and traffic justify them. Compute cheapest-path trails (river crossings are costs) and let traffic upgrade busy ones into roads.
- Hand-place a small number of key areas: real railheads, forts, major river crossings, sacred sites, and the map-edge weirdness. Procedural generation fills in between.
- Pre-simulate several decades of history before the player starts, so the world arrives with worn trails, ghost towns, and established ranches and gangs. This doubles as a tuning check: each generated history either looks plausible or it doesn't.
- Game time must be compressed relative to real travel time, with fast-forward through uneventful stretches. The cattle drive is a skippable delivery vehicle: random encounters and major stops only, wages drawn on arrival.

## Staged plan (each stage must be a complete, shareable thing)

1. **Headless sim.** Console program on the graph model from day one: 30-50 nodes, a few hundred aggregate agents, a handful of goods, traders and bandits, a price model, and an event log. Goal: see believable boom and bust in the log. Then scale node count to ~2,000 and check both performance and behavior.
2. **Overworld viewer.** Top-down map with towns and moving agents, placeholder art. Watching the world run is itself the first demo.
3. **One playable loop.** Simplest role first, probably cattle driver or trader, with real route and risk decisions (which route, which herd, whether to hire guards).
4. **Bandit and lawman roles.** New controllers over existing mechanics.
5. **Authored content layers.** Sacred sites, elders, map-edge weirdness, outlaw storyline. These are additive, not required for the core.
6. **Player-facing narrative systems.** Rumor feed and newspaper headlines generated from the event log. An LLM could write flavor text from the log, but only as description, never as control of world state.

If work stops after stage 2 or 3, something real still exists.

## Longer-term design intent (not for early stages)

- Roles: law enforcement, outlaw, cattle driver.
- Players can ride off from any stop for a few hours. Nearing the region edge gives a warning, then an Oregon Trail-style death cutscene. Far from towns and trails are secrets (buried treasure, mounds, weird or alien encounters) that evoke the settlers' sense of the strange. Music fades slowly to silence near the edge.
- Native American sacred sites act as safe spaces, with a rare, real, human elder or storyteller rather than a mystical apparition. Several distinct elder characters from different nations. These are hand-authored, never generated, and depiction should be culturally respectful and historically grounded.
- Outlaw storyline of aiding Native nations resisting expansion: a losing struggle in Red Dead Redemption 2's tone, where the player knows the outcome and the characters don't. The protagonist always has a self-serving or at least non-self-destructive reason to help. Cowboy-Native relations are shown as mutually beneficial rather than antagonistic.

## Ideas backlog (economy and agents)

- **Panic buying / hoarding.** A stock-out should itself raise demand (people stock up when shelves look empty), as the 2020 toilet-paper shortage showed. Model as a temporary demand multiplier at a node after a stock-out, with a traceable cause in the log and dampers (the surge exhausts itself, prices rise, traders respond).
- **State and federal forces** (Texas Rangers, US Marshals, the Army) as the top rung of the law escalation ladder; today the Army and state capitals only count as "backup" that blocks gang capture.
- **Private armies with a label** (Johnson County War): an association labels small ranchers "rustlers" and invades.
- **Credit depends on who you deal with.** A draft or IOU from a far-off stranger is worth something to the Army or a big town, little to a remote camp, and nothing to a Comanchero. Acceptance should depend on the counterparty's trust and ties.
- **Information lag.** Traders currently see all prices with small noise. Make price knowledge travel with traders and telegraph lines.
- **Seasons.** River freight, winter on the trail, drive season.
- **Banks, credit and panics.** Cash scarcity is modelled; credit is not.
- **Mining in stages (Deadwood).** Placer gold draws a crowd of individual miners and plays out fast; then a large company arrives with outside capital for deep-vein work (Homestake, from 1877): fewer workers, a steady payroll, an industrial company town. Model as staged production per mine (placer, then lode), with the second stage started by outside capital and paid as wages into the town. Gambling as the town's later economy is outside the 1874-1886 window.
- **Merchant imports.** Towns with cash should order staples ahead from the nearest source (railhead, East, or farm hub) and have them freighted in, instead of relying on speculative traders choosing a low-value-density cargo. Deadwood starves in every run for this reason: gold goes out, flour does not come in.
- **Farms and new towns (first pass built).** Farmers settle where the farm-gate price (best of local market and any outlet worth the freight) beats break-even; weather is drawn per zone per year; grain buyers ship surplus to the East by rail. Latent sites (Spearfish Valley, Garden City, Billings) become towns when settling pays for several months. Gaps: Texas has no outlet for grain (no Gulf market, so its farm districts shrink to local demand); farm numbers, capacities, opening dates and break-even price are rough and must be verified against sources; towns still cannot be founded by traffic alone (stage stops, crossings, mining strikes); farm districts do not yet feed an Indian agency or fort by direct contract with local farmers.

## Known risks

- Scope creep toward the full open-world vision, which is team-sized.
- Time is the real cost: systems-heavy solo games take years, and tuning never fully ends.
- Economic sims can run fine mechanically and still be boring, or collapse for unintended reasons.
- Interface work to make emergence legible is easy to underestimate.
- Staying motivated through the long middle, when the sim works but isn't fun yet. Mitigation: get a watchable version early.

## Decisions made

- **Language:** C++20 with CMake. `sim/` is a dependency-free static library; `cli/` is the headless runner; a future viewer (likely raylib) links only against `sim`. Config in JSON (nlohmann/json, header-only). Own seeded PRNG (xoshiro256**), not `<random>` distributions, for cross-compiler determinism.
- **License:** Apache 2.0 (art and authored content may be licensed separately later).

## Open decisions to settle at the start of the repo

- ~~Language and framework.~~ Settled, see above. Developer has years of C, C++, Java, and C# experience. Candidates: C++ with SDL or raylib, C# with a lightweight framework, or Godot. Pick based on which keeps the sim code cleanest and easiest for outside contributors to read.
- ~~Time window.~~ Started with the suggested slice: mid-1870s to mid-1880s (scenario starts 1874-01-01 as warm-up, log begins 1875-01-01).
- ~~Tick granularity.~~ One day per tick.

## Working agreements for Claude Code

- Keep the simulation core free of rendering and engine dependencies, so it runs headless and can be tested on its own.
- Make all randomness seeded and runs replayable.
- Every significant world event writes a cause-tagged entry to the event log.
- Prefer data-driven definitions (goods, routes, agent types, shock events) in config files over hard-coded values, so balance can be tuned without recompiling and contributors can read the model.
- Write short comments explaining why a rule exists in economic or historical terms, since others will study this code.
- When something behaves strangely, check the event log for the causal chain before changing numbers.
- Build in stages. Don't start a later stage until the earlier one produces believable behavior.
