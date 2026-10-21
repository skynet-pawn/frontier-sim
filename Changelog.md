# Changelog

What has been built, newest first, in plain language. Each entry names the commit. For the design reasoning, results and open
gaps behind any of these, see `CLAUDE.md`; for how to build and run, see `README.md`.

Where things stand: the headless simulation (`sim/`, `cli/`) is mature and runs a 12-year history at 31 nodes in seconds and at
2,000 nodes in about 25-30 minutes. An optional raylib viewer (`viewer/`) draws the running world. There is no terrain and no
player yet; the next big pieces are real world generation and the first playable role.

## 2026-10-21 (roads form from traffic)

- (`49dd3dc`) **Trails become roads where wagons keep going.** Each trail edge has a road level (trail, wagon road, graded road). Wagons a day (`Edge::use_ema`, about a 140-day half-life) raise it past `road_up_wagon` 2.5 and `road_up_graded` 10; use below 40% of that lets it wear back. Reviewed every 90 days; the route table is rebuilt only if a road changed. A wagon road is 1.2 times as fast and bills 0.8 of the miles, a graded road 1.4 times and 0.6 (guesses); rail and rivers are fixed. Travel days and freight miles (`Edge::freight_miles`) are derived from the level, so saved state gains only `road` and `use_ema` (state version 3). Events `road_improved` and `road_worn` carry the cause; the viewer draws worn roads thicker. 31 nodes: the Black Hills and Montana roads are graded within a year or two of the rush, and the Austin-Fort Worth and San Antonio trails wear back after the railroads arrive (1878-79). No money moves (travellers and the towns at either end do the work). Not built: a cost paid by towns, bridges at crossings, roads for river and rail edges, new roads between towns the network does not link.

## 2026-10-21 (getting rich: houses as growth, settling as temptation)

- (`c3faa94`) **A house is a way to get rich faster, and a pile is a temptation to stop.** Nobody ever settled in the big world: 46 freelancers passed $20,000 in a year, but the settle roll ran only for a trader idle at the start of his day (one who sells and rebuys in one step never is: 4 eligible days against 2,230 rich days), and 33 of the 34 who passed $24,000 founded a house first. Now: (1) each freelancer has a pile he is after (`goal_min`-`goal_max`, $15,000 to $60,000, log-uniform) and a time he will wait for it (`patience_min_days`-`patience_max_days`, 1 to 5 years), both fixed from his id and the seed (`trait`, nothing stored); he tracks what he clears a day (`Trader::income`, from the daily change in `worth`, forgetting with a 60-day half-life). (2) With $16,000 in hand (his own float plus a second outfit's) he founds a house if he earns at least $3 a day and the pile is further off at that income than he will wait (`wants_house`): a fast earner does not need one, a man earning nothing would only multiply his losses. A house of fewer than 3 outfits hires with no reserve (`house_small_outfits`). Eastern firms always want to grow. (3) Settling is rolled when a load is sold (`maybe_settle`) from a $5,000 stake up, in proportion to the stake (`settle_chance` 0.05 per sale at $20,000, capped at 0.25). Saved state gains `worth` and `income` (state version 2: older snapshots do not load). 31 nodes (10 seeds, median): settled 0 to 44 a run, houses founded 110 to 98, traders at the end 1,415 to 1,208, population and robberies unchanged, money in circulation $32.8M to $28.5M. Not built: the owner stepping back from driving (the founder's wagon is just an outfit) and an owner cashing out of his house. New dev tool `wealth_probe`.

## 2026-10-20 (checks on lead stages)

- (no code) **Lead stages confirmed on 3 seeds** (famines 3, 3, 3 against 32 to 46 without; starved 2.6k to 4.1k against 15.6k to 18k; traders 5.4k to 5.7k against 1.5k; money $224M to $253M against $128M to $138M). The 3.7 times traders are mostly freight-house outfits following 2.3 times more orders and 27% more towns. A side finding: traders can go into debt for freight billed on departure, and retired traders' debts are written off against the East (+$127M of the $253M, +$40M in the baseline). See CLAUDE.md.

## 2026-10-21 (what traders really spend)

- (3354c66) **Trade ledger, stop spending in the planner, honest profit books.** The batch report now has a trade ledger
  (`TradeBook`): per kind of outfit, by year and by good, what traders bought, paid in freight, earned, spent on upkeep, guards and
  at every town stop, and was paid for freight jobs. It showed that lone traders lost about $148 a load (-$4.9M over six years)
  because the party spends $3 plus $1 a guard plus 1.5% of the cargo's value at every stop and the trip planner never counted it
  (it was $410 a load, more than the goods cost); that freight jobs (284,000 against 34,000 speculative loads, paying $15 each)
  were a loss for the carrier; and that the 'profit per trader' figure (-3 early, -9 late) included the running costs of town-hired
  convoys, which a town pays. Three switches, all on in the generated scenario and off elsewhere (the original maps behave as before):
  `plan_stop_costs` (traders and job-takers count stop spending when they judge a trip), and `books_stages` (convoy costs stay out
  of the profit total, which also feeds eastern firms' reported return; a carrier pays no value-based spending on a supplier's
  goods). From the day-2,200 checkpoint (seed 1, years 7 to 12): lone traders' net cash per load -$148 -> -$53 (planner only) ->
  +$14 (all three), total -$4.9M -> +$0.5M; the profit statistic -5 to -10 -> +2 to +9; eastern firms 3 -> 7; 5-year failure of later
  entrants 41.9% -> 45.0% (starting lone traders 50.4%). Noise to remember: famine episodes 3 / 1 / 5 across the three variants, from
  one checkpoint and seed.
- (3354c66) The generated world now starts with 1,500 lone traders instead of 9,654 (`tools/gen_towns.py` `start-traders`).

## 2026-10-20 (trader cohorts and the starting crowd)

- (5b6899b, with the notes in this commit) **Trader cohorts.** Each batch run now follows every trader by id and reports, for lone
  traders at the start, lone traders who entered later, freight-house outfits and eastern firms, the share gone within 1, 2, 3, 5
  and 10 years (only those old enough to be watched that long) and how they left. Finding: the generated world starts with 9,654
  lone traders, scaled from the 31-node map's 120 by the expected final town count (about 2,500) when only about 800 towns exist
  on day 1. They crash to about 1,500 by day 990 (81% of them gone within 5 years), which is where most of the roughly 10,000
  exits a run come from. Lone traders who enter later fail at about 48% within 5 years, 33% within 3. Starts of 3,900 and 2,000
  (scratch copies, one seed) crash less but settle at the same 700 to 1,000 lone traders; the late world barely changes (4,300 to
  5,600 traders, 5 or 6 famines). Overall left / ever is about 65% (51 to 75% is the user's target range). Nobody ever settles
  rich, and the average profit per trader keeps falling (-1 on day 30, -7 to -14 at the end): the trader problem is next. The
  generator now writes 1,500 (the user's call; to be confirmed on three seeds). The freight write-off (+$127M of $253M) is
  left as it is.

## 2026-10-20 (save and reload)

- (cd791d8) **Save and reload.** `Simulation::save_state(path)` and `load_state(path)` write and restore everything that changes while the
  sim runs (towns, routes' live state, traders, houses, contracts, gangs, posses, news, the random generator and the whole event log),
  from one field list per struct used for both directions, so they cannot disagree. Definitions still come from the data folder; the
  route table and trader horizons are rebuilt on load. `tests/save_test.cpp` runs a game straight through and again saved half-way
  and reloaded: same history hash, same money and byte-identical final snapshots (31 nodes for 3,650 days, the Black Hills slice,
  and the 4,694-node world for 600 days; from a day-550 snapshot of the big world the run reaches day 1,100 with the same hash as
  the unbroken run). A snapshot of the big world at day 1,100 is 53 MB. `frontier_cli --save FILE`, `--load FILE`,
  `--checkpoint-every N PREFIX`; `frontier_batch --load FILE`, `--checkpoint-every N PREFIX`; the viewer takes `--load FILE`.
- (5b6899b) **Time series.** `frontier_batch --series FILE N` writes one row every N days per seed: traders by kind, houses,
  gangs, money in circulation, profit per trader, population, towns, towns in shortage, running totals of shortages, famines,
  starved, robberies, trader exits, orders, jobs, towns founded, ghost towns, and every money flow. One run now shows how the economy
  moved, not only where it ended.

## 2026-10-19 (lead stages)

- `76fd8f8` **Lead stages: orders sized from each town's own lead time.** Found by stepping a generated gold camp (Marten Camp) day by day: it
  got food for 60 days, waited 14 days to hire a convoy, and the convoy took 86 days on the road; camps under 150 people never posted an
  order at all. Merchants now measure how long a town's orders really take (average and spread), track its growth, and size the safety
  stock, order cycle (one wagon load), hire wait, store reserve, supplier cover, expiry and rush provisions from that. Off by default;
  on in the generated 4,694-node scenario. 1 seed: famines 32 to 3, people starved 15,620 to 4,110, population 2.21M to 2.53M; but
  shortage episodes 343 to 611, traders 1,499 to 5,590 and money $128M to $253M (not yet explained). Keep off on the 31-node map
  (traders -20%). Also holds the gold express (`express_stages`, off: money in circulation grows 4.7 times).
- `20cc522` **CLI `--stop`, `--watch`, `--watch-from`.** Run to a day and print one town's shelves, cash, dust, orders and hunger daily.

## 2026-10-19 (mining-camp famine)

- `4be6696` **Gold camps pay for food in dust.** A gold camp's wealth is dust, but its open orders, freight jobs, standing contracts and hired
  convoys saw only cash, so a camp sitting on its own output looked broke and posted nothing. Found by following Twin Hill (a generated
  camp) day by day: the strike stake went to eight convoys, the food ran out after about 115 days, and from then on the town had $0 cash
  and no order open while its mine produced about $800 of gold a day. All of those now draw from cash plus dust. On the 4,694-node
  world (1 seed) famine episodes fell from 171 to 42, people starved from 40,000 to 18,000, population at the end rose from 1.57M to 2.2M.
- `783167f` **A strike brings working capital.** `rush_stake_days` (default 0; the generated scenario sets 90) gives a struck camp cash equal to
  that many days of its food bill. On its own it barely helped (famines 171 to 160); with dust payment it takes famines from 42 to 32.
- `1e58133` **Hired cattle convoys are drovers.** A town's hired convoy for beef could not buy cattle, because only a drover's outfit may.

## 2026-10-19 (settlement pace)

- `4182e08` **Settlement pace: founding is selective.** Farm and ranch sites that qualify now wait in line for settlers, who arrive at a set
  pace (2 to 5 new farm towns a month over the run) and take the best-served land first, so about half the farm sites and a third of
  the ranch sites become towns instead of nearly all. The hand-made maps are unchanged (no pace is set there). Strikes are smaller and
  generated camps have game. The result: farm towns and ranches no longer starve; the famine that remains (137) is almost all in
  mining camps, which are still too fragile. `frontier_batch` shows progress with `FRONTIER_PROGRESS=1`.

## 2026-10-19 (resource-driven towns)

- `513e5a7` **Towns the sim founds, from resources.** The generator no longer decides which towns exist. It lays out about 4,400 candidate sites
  by resource (farm land, grass, game, furs, minerals, timber) plus the railroad's stations, and the sim founds the ones that pay: a
  farm or ranch when its crop sells well enough, a hunters' camp or trapping post when hides or pelts do, a sawmill when timber can be
  freighted out, a mining camp when prospectors strike. Camps boom and go to ghost towns as the ore, bison and pelts run out. About
  1,950 towns with 30 or more people exist at the end. New good: furs. Farm and ranch founding is not selective enough yet, and
  famine and lawlessness are too high; see `CLAUDE.md`.

## 2026-10-19 (procedural towns)

- `d75a2a3` **A 2,000-node map, first pass.** `tools/gen_towns.py` places about 1,970 procedural towns on the real map from rainfall (PRISM),
  slope, rivers and trade routes: stations along the railroads and roads, farm towns in the river valleys east of the 100th
  meridian, ranch headquarters on the plains, mining camps in the ranges, with opening dates from a settlement frontier. The world
  (`data/slices/full2000`, 2,000 nodes, 4,359 routes) runs the full 15 years in 16 minutes with no crash or audit failure. It
  shows too much famine and lawlessness, which is the next thing to look at. See `docs/towns_placed_full2000.png`.

## 2026-10-19 (crash fix)

- `7a5a69c` **Crash fix.** A gang that a rival had just driven off its trail (it has no trail until its next review) could crash the sim when a
  town or a private interest looked for a gang to hunt. Both now skip such gangs. Found by running a 2,000-node world, which the
  smaller maps never triggered; a memory-checking build named the line.

## 2026-10-19 (more facts)

- `00c3e1e` **More sourced facts and the Waco bridge.** A second research pass added dated facts for St. Paul, Galveston, Gainesville, Garden
  City, the Nueces Strip and the Milk River country, so 30 of 31 towns now carry a sourced fact. One more crossing is recorded (the
  Waco Suspension Bridge over the Brazos, 1869-70). The Ogallala, Dodge City and Miles City crossings stayed unverified: nothing
  found.

## 2026-10-19 (facts and crossings)

- `dc6f7f4` **Dated facts and river crossings for the whole map.** Sourced dates for 24 of the 31 towns (rail arrivals, founding, forts,
  cattle seasons) now sit in `data/history/full/places.json` with a verified flag, and four rail dates in the routes were corrected
  from the sources. A new `crossings.json` records the real fords, ferries and bridges (Hannibal and Omaha bridges, the Fort Laramie
  bridge, the Red River Station ferry, Doan's Crossing and more); a route crosses a river cheaply near them and pays full
  price elsewhere. The viewer marks them.

## 2026-10-19 (the whole map)

- `5262a5a` **The whole map on real ground.** The original 31 towns, from Galveston to Fort Benton and St. Paul, now have real positions on an
  equal-area projection, with terrain from 220 m tiles, 611 river pieces and routes that follow the land and pay for river
  crossings. Behaviour matches the hand-placed map (traders, robberies, gangs, shortages, population within a few percent). Run
  it with `--data data/slices/full`. A new `route_probe` dev tool lists every route's miles and crossings. See `docs/full_map_1878.png`.

## 2026-10-19 (rivers)

- `c0f4e13` **Real-map layer 2, step 5: rivers.** Natural Earth river lines for the slice (Missouri, the Platte rivers, the Cheyenne, the
  White, the Niobrara and more). A route that crosses a river pays for the crossing (a ford, a ferry, a bridge for rail), so roads
  gain a few miles where they cross the North Platte or the Cheyenne. The viewer draws the rivers in blue. The USGS 30 m
  elevation download was tested and works (one tile in 5 seconds; the slice would be about 4 GB, not yet fetched).

## 2026-10-18 (final)

- `e43cdc1` **Real-map layer 2, step 4: finer terrain.** The slice's terrain is now built from 110 m tiles (684 tiles, 28 MB downloaded) on a
  quarter-mile grid, instead of 450 m tiles on a two-mile grid, with a despike step that removes the few bad pixels in the source.
  The path search is now A*, so the bigger grid stays fast. Routes come out a few miles longer because the finer ground has more
  small climbs. See `docs/black_hills_fine_1880.png`.

## 2026-10-18 (last)

- `c7e3094` **Real-map layer 2, step 3: routes follow the land.** A road between two towns now takes the cheapest way over the terrain
  (around a ridge when that costs less than climbing it) instead of a straight line. The viewer draws each road along that way and
  moves wagons and gangs along it. Miles on the Black Hills slice change by a few percent: the ground between its towns is gentle
  at this resolution. See `docs/black_hills_paths_1880.png`.

## 2026-10-18 (latest)

- `17df2cf` **Real-map layer 2, step 2: slope cost.** The sim now reads the slice's terrain: a route's miles include the climbing along it
  (1 extra mile per 100 m of ascent on a trail, a quarter of that for rail), so the Black Hills roads are 5 to 12% longer than the
  plains roads of the same straight length. Worlds without a terrain grid, including the 31-node map, are unchanged.

## 2026-10-18 (late)

- `1e32973` **Jail, a record and the rope.** A band the law catches is no longer simply gone. Where there is a marshal or sheriff its men
  go to jail: days for petty theft, months for hold-ups, longer for a man who has done time, never more than a year; killers or
  a long string of hold-ups risk hanging. They come out with a record and, as hardened men, form the next bands (a released
  man counts as a veteran in a fight). Where there is no law the town runs the men out. The frontier sources found are thin
  and noted in `CLAUDE.md`; the numbers are guesses within them. Large gangs on the road 1.65 to 1.79 at 31 nodes.

## 2026-10-18 (night)

- `a0edb1a` **More large gangs.** The funnel from small band to large gang was measured and fixed. Bands now live off the land (small
  cash needs, occasional petty theft, a recreation visit to town every two or three months with a real chance of being caught).
  Lawmen find a camp by following a trail of hold-ups (its heat), not by luck, and a far camp is harder to link. Most join attempts
  used to fail for lack of a partner; a successful band can now take in a quiet one. Large gangs on the road went from 0.6 to 1.65
  at 31 nodes, joins from 70 to 258 a run. Petty thefts are counted and shown in the log.

## 2026-10-18 (evening)

- `4533a12` **Real-map layer 1: the Cheyenne-Black Hills slice.** A first slice of the real map, reviewed and accepted, 16 places from Omaha to Deadwood, about
  1867-1890. Facts and guesses are kept apart. `data/history/black_hills/` holds the sourced FACTS (`places.json`, `routes.json`,
  `jurisdictions.json`: positions, founding and rail dates, forts, stage and freight roads, who had law where), each with a source
  and a verified flag, plus `game.json` with the GUESSES (population, cash, stock) to tweak. `tools/make_slice.py black_hills`
  builds a runnable world in `data/slices/black_hills/` (run it with `--data data/slices/black_hills`). Towns appear near their
  real dates and the Deadwood law story follows 1876-77. See `docs/black_hills_slice_1880.png`.

## 2026-10-18 (later)

- `bb24e97` **Gang camps.** A gang now lives at a camp off the roads (a dugout, a taken or abandoned cabin, a ruined blockhouse, a line
  shack or a canyon), a few miles up its trail from the nearest town, instead of in the town itself. It rides in only to spend
  after a big score. Each boss is careful (1 in 10), careless (1 in 10) or ordinary: a careless one often starts trouble that angers the town (30% of visits), an ordinary one sometimes (10%), a careful one rarely (2%). Lawmen find a
  band on the road, at the fence when it sells loot, on a spree in town, or (rarely) at the camp; a band caught mid-hold-up
  sometimes reaches its camp ahead of them. The camp type is a label for the viewer only. Being tuned against the 31-node map.

## 2026-10-18

- `f5a5694` **Outlaws come from money and thin law.** The hand-set "this town breeds drifters" flag is gone as the source of
  outlaws. Every frontier town draws idle men by population, and the men there turn to crime in proportion to the money moving
  through the town per head and how little law it has. Two test worlds of 300 towns that used to disagree by 2.7 times now agree.
- `3997ef6` **Profiling and a better test world.** `frontier_cli --profile N` prints where each simulated day's time goes
  (traders are 70% of the cost at 2,000 nodes; newspapers about 0.5%). Gang lookups use a binary search. The scale-test world
  generator now makes farm towns poorer and less crime-prone.
- `86f8f54` **Scale-test world generator** added at `tools/gen_world.py`.
- `fb094e6` Recorded the 2,000-node scale test of the new gang, law and news systems (30 minutes, no invariant failures).
- `9bda1e5` **Gangs, law, news and bushwhacking.**
  - *Two kinds of gang:* small bands of 2-5 men, and a few large gangs that bands can join; gang wars, veterans, and a soft cap on
    how many men a boss can hold.
  - *Law by stages:* no law, a marshal, then a sheriff, by town size and region; regions with their own jurisdictions and an
    "organized" date; thinner law than before, which is what lets large gangs last.
  - *Infamy and flight:* each gang has a reputation per region; a region hunts only gangs it has heard of; a gang whose name gets
    too hot rides to fresh country.
  - *Newspapers:* towns that grow big enough get a paper; events become one-line headlines that travel by distance and add to a
    gang's fame where they are read; the viewer shows a town's recent headlines.
  - *Bushwhacking:* bands waylay lone men; residents who vanish or turn up dead anger their town, drifters barely do, and a
    notable victim can bring in the Army.

## 2026-10-17

- `b03f110` **Overworld viewer** (optional raylib target): towns by size, roads tinted by robberies, wagon trains, gangs,
  shortages, a town panel, overlays, an event feed, a headline panel, wagon following, speed controls, and `--shot` for a
  headless frame.
- `b00ba52` **Trading horizon.** A trader considers only the nearest 64 towns plus the eastern markets, so a day's cost grows
  about linearly with the map instead of with its square: 2,000 nodes now run at about 0.26 s a day.
- `5f0f661` **Freight jobs, supply shocks, hunger-driven entry.** Orders can be carried as paid jobs; scenario shocks (road
  closures, a freight house collapsing, eastern firms leaving); new traders arrive when towns go short.
- `d90aec2` **Local game.** Hunters feed their own town and can hunt out the hills.
- `ec4f896` **Starvation and a death-based famine;** shortages are counted separately from famines.
- `2c54f36` **Standing supply contracts** between short towns and freight houses, a cause chain on each famine, and
  inbound-aware pricing (kept off).

## 2026-10-16

- `bfb4696` Government store for forts, growth gated by food cover, order sizing by lead time.
- `0ea6706` An open order is a floor, not a cap.
- `feaa7bc` **Freight houses** and a town order board with a convoy fallback.
- `60aae04` **Gangs take what they can carry:** value judged by the goods, weight-limited tolls, food only when desperate.
- `fe02915` Famine statistics merge flicker into episodes and count distinct towns.
- `92daed8` **Merchant imports:** towns order staples ahead and freight them in.
- `75992de` Trader entry signals count runs of legs; danger can't shut entry completely.
- `01e09b6` Round-trip trip scoring; dust payment for cash-short gold camps.
- `2c4658b` Towns recalibrated to census figures; Galveston, Texas rail and cotton; gold as camp money; rush provisions.
- `7b80414` **Farm sector:** weather, rail grain shipping, and latent sites that become towns when settling pays.
- `d1ad071` Traders sell part of their load in towns they pass; per-town income by source in the batch report.

## 2026-10-15

- `6df18c6` Two streams of new traders: freelancers on rumor, eastern firms on lagged reports.
- `ea42414` **Army posts:** garrisons, cost-plus federal contracts, Quartermaster trains, horses and forage.
- `fd6eaf5` A 1,000-seed baseline batch result added under `docs/`.
- `7c0c392` **Money audit, batch runner, action layer,** structured events, a finite eastern market.
- `4046bcb` Escalating law and town capture by gangs.
- `4568ae9` Posse cooldown 30 days (shorter when outrage is high); leaders for hire.
- `91d8ee5` **Stage 1 headless sim:** economy, traders, gangs, posses, an event log and the CLI.
- `72056a9` Line endings normalized with `.gitattributes`.
- `4899a2c` **Repo scaffold:** Apache 2.0, CMake, seeded RNG, CLI stub.
