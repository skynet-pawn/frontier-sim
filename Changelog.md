# Changelog

What has been built, newest first, in plain language. Each entry names the commit. For the design reasoning, results and open
gaps behind any of these, see `CLAUDE.md`; for how to build and run, see `README.md`.

Where things stand: the headless simulation (`sim/`, `cli/`) is mature and runs a 12-year history at 31 nodes in seconds and at
2,000 nodes in about 25-30 minutes. An optional raylib viewer (`viewer/`) draws the running world. There is no terrain and no
player yet; the next big pieces are real world generation and the first playable role.

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
