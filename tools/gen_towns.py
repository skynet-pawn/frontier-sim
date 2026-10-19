"""Procedural towns for the whole map: where settlements could have been, from the land itself.

usage: python -I gen_towns.py SLICE WORK_DIR [--preview] [--build] [--count 2000] [--seed 1]
  WORK_DIR holds the downloads and exports this tool reads (none are stored in the repo):
    prism/x/prism_ppt_us_25m_2020_avg_30y.tif      PRISM 30-year annual precipitation normal (millimetres, 2.5 arc-minute grid)
    ne_raw/admin1/ne_10m_admin_1_states_provinces.shp   Natural Earth states and provinces (public domain)
    routes.json                                     `route_probe DATA --paths` for the slice (each route's way over the terrain)
  and the repo's data/terrain/SLICE, data/rivers/SLICE and data/history/SLICE.

Stage 1: the suitability fields on a coarse grid (2-mile cells) and a picture of them (--preview writes docs/towns_fields_SLICE.png).
Stage 2 (--build --count N): places candidate sites by resource (farm land, grass, game for hides, furs along streams, minerals in the
mountains, timber) plus the railroad's stations, links them into the network and writes data/slices/SLICE_N. The candidates are latent: the
sim founds the ones that pay (farms.cpp), so the towns that exist at the end are the run's result, not the generator's.
"""
import json, math, os, re, sys
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proj, shp

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
slice_name, work = args[0], args[1]
PREVIEW = '--preview' in sys.argv
COARSE = 2.0   # miles per field cell

PROJ = proj.load(ROOT, slice_name)


def load_json(path):
    return json.loads(re.sub(r'^\s*//.*$', '', open(path, encoding='utf-8').read(), flags=re.M))


# ---------------------------------------------------------------- terrain on the coarse grid
th = load_json(os.path.join(ROOT, 'data', 'terrain', slice_name, 'terrain.json'))
fine = np.fromfile(os.path.join(ROOT, 'data', 'terrain', slice_name, 'heights.i16'), dtype='<i2').reshape(th['height'], th['width']).astype(np.float32)
k = int(round(COARSE / th['cell_miles']))
H0, W0 = (th['height'] // k) * k, (th['width'] // k) * k
Hm = fine[:H0, :W0].reshape(H0 // k, k, W0 // k, k).mean(axis=(1, 3))      # mean height of each coarse cell, row 0 = south
ny, nx = Hm.shape
x0, y0 = th['x_min'], th['y_min']
cx = x0 + (np.arange(nx) + 0.5) * COARSE
cy = y0 + (np.arange(ny) + 0.5) * COARSE
X, Y = np.meshgrid(cx, cy)
LAT, LON = PROJ.unproject(X, Y)
print('field grid %d x %d cells of %.0f miles' % (nx, ny, COARSE))


def box_mean(a, r):
    """Mean over a (2r+1)-square window (edges clamped)."""
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    n = 2 * r + 1
    return (c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n])[:a.shape[0], :a.shape[1]] / (n * n)


# slope as percent grade, and ruggedness (the height range in a 3 x 3 window, metres)
gy_, gx_ = np.gradient(Hm, COARSE * 1609.34)
grade = 100.0 * np.hypot(gx_, gy_)
pad = np.pad(Hm, 1, mode='edge')
stack = np.stack([pad[dy:dy + ny, dx:dx + nx] for dy in range(3) for dx in range(3)])
rugged = stack.max(axis=0) - stack.min(axis=0)
valley = Hm - box_mean(Hm, 3)    # negative where the ground lies below its surroundings: a drainage

# ---------------------------------------------------------------- precipitation (PRISM, millimetres a year)
tif = Image.open(os.path.join(work, 'prism', 'x', 'prism_ppt_us_25m_2020_avg_30y.tif'))
ppt = np.asarray(tif, dtype=np.float32)
t0_lon, t0_lat, step = -125.0208333333335, 49.9375000000005, 0.041666666667
ci = np.clip(np.floor((LON - t0_lon) / step).astype(int), 0, ppt.shape[1] - 1)
ri = np.clip(np.floor((t0_lat - LAT) / step).astype(int), 0, ppt.shape[0] - 1)
precip = ppt[ri, ci]
outside = (precip < 0) | (LON < t0_lon) | (LAT > t0_lat)
precip = np.where(outside, 0.0, precip)    # beyond the United States there is no data
print('precipitation: %d to %d mm, %d cells without data' % (precip[~outside].min(), precip.max(), outside.sum()))

# ---------------------------------------------------------------- raster helpers (PIL draws in pixel space, row 0 = north)


def to_pix(xm, ym):
    return ((np.asarray(xm) - x0) / COARSE, (y0 + ny * COARSE - np.asarray(ym)) / COARSE)


def raster_lines(lines, values, width=1):
    im = Image.new('L', (nx, ny), 0)
    d = ImageDraw.Draw(im)
    for pts, v in zip(lines, values):
        px, py = to_pix([p[0] for p in pts], [p[1] for p in pts])
        d.line(list(zip(px.tolist(), py.tolist())), fill=int(v), width=width)
    return np.asarray(im)[::-1].copy()   # back to row 0 = south


def chamfer(mask):
    """Distance in miles to the nearest True cell (a two-pass chamfer transform with 3-4 weights)."""
    INF = 1e9
    d = np.where(mask, 0.0, INF).astype(np.float64)
    a, b = 1.0, 1.4142
    for y in range(ny):
        row = d[y]
        if y > 0:
            up = d[y - 1]
            np.minimum(row, up + a, out=row)
            np.minimum(row[1:], up[:-1] + b, out=row[1:])
            np.minimum(row[:-1], up[1:] + b, out=row[:-1])
        for x in range(1, nx):
            v = row[x - 1] + a
            if v < row[x]: row[x] = v
    for y in range(ny - 1, -1, -1):
        row = d[y]
        if y < ny - 1:
            dn = d[y + 1]
            np.minimum(row, dn + a, out=row)
            np.minimum(row[1:], dn[:-1] + b, out=row[1:])
            np.minimum(row[:-1], dn[1:] + b, out=row[:-1])
        for x in range(nx - 2, -1, -1):
            v = row[x + 1] + a
            if v < row[x]: row[x] = v
    return d * COARSE


# ---------------------------------------------------------------- rivers and routes
rv = load_json(os.path.join(ROOT, 'data', 'rivers', slice_name, 'rivers.json'))
river_size = raster_lines([r['points'] for r in rv['rivers']], [r['size'] for r in rv['rivers']], 2)
d_river = chamfer(river_size > 0)
d_river_big = chamfer(river_size >= 2)
routes = json.load(open(os.path.join(work, 'routes.json'), encoding='utf-8'))
route_kind = raster_lines([r['path'] for r in routes], [2 if r['mode'] == 'rail' else 1 for r in routes], 2)
d_route = chamfer(route_kind > 0)
d_rail = chamfer(route_kind == 2)
print('distance fields done')

# ---------------------------------------------------------------- jurisdictions (modern state boundaries as the stand-in for territories)
STATE_REGION = {   # modern state -> the sim's region name; only the states this map's trade reaches (no Utah, Idaho, New Mexico, the Deep South)
    'Texas': 'texas', 'Oklahoma': 'indian_territory', 'Kansas': 'kansas', 'Nebraska': 'nebraska', 'Wyoming': 'wyoming', 'Montana': 'montana',
    'South Dakota': 'dakota', 'North Dakota': 'dakota', 'Minnesota': 'minnesota', 'Missouri': 'missouri', 'Iowa': 'iowa', 'Colorado': 'colorado'}
admin = os.path.join(work, 'ne_raw', 'admin1', 'ne_10m_admin_1_states_provinces')
rows = shp.read_dbf(admin + '.dbf')
region_names = sorted(set(STATE_REGION.values()))
region_img = Image.new('L', (nx, ny), 0)
rd = ImageDraw.Draw(region_img)
for idx, st, parts in shp.read_shapes(admin + '.shp'):
    row = rows[idx]
    if row.get('admin') != 'United States of America' or row.get('name') not in STATE_REGION:
        continue
    code = region_names.index(STATE_REGION[row['name']]) + 1
    for part in parts:
        lon = np.array([p[0] for p in part]); lat = np.array([p[1] for p in part])
        mx, my = PROJ.project(lat, lon)
        px, py = to_pix(mx, my)
        if px.max() < 0 or px.min() > nx or py.max() < 0 or py.min() > ny:
            continue
        rd.polygon(list(zip(px.tolist(), py.tolist())), fill=code)
region = np.asarray(region_img)[::-1].copy()
print('regions raster done:', {n: int((region == i + 1).sum()) for i, n in enumerate(region_names) if (region == i + 1).sum()})

# ---------------------------------------------------------------- suitability scores, 0 to 1
inch = precip / 25.4
rain_farm = np.clip((precip - 400.0) / 250.0, 0, 1)                       # dryland farming wants about 16 inches and gets sure at 26
rain_ranch = np.clip((precip - 220.0) / 150.0, 0, 1) * np.where(precip > 900, 0.7, 1.0)
flat = np.clip(1.0 - grade / 8.0, 0, 1)
flat_ranch = np.clip(1.0 - grade / 15.0, 0, 1)
water = np.maximum(np.exp(-d_river / 6.0), np.clip(-valley / 40.0, 0, 1) * 0.6)   # a river within a few miles, or a drainage
access = 0.25 + 0.75 * np.exp(-d_route / 40.0)                            # near a road or a railroad: a market
in_us = region > 0
farm = rain_farm * flat * (0.2 + 0.8 * water) * access * in_us * (Hm < 1800)
ranch = rain_ranch * flat_ranch * (0.3 + 0.7 * water) * (0.3 + 0.7 * access) * in_us
mining = np.clip((rugged - 250.0) / 400.0, 0, 1) * (Hm > 1300) * in_us
print('scores: farm mean %.3f, ranch mean %.3f, mining mean %.3f' % (farm[in_us].mean(), ranch[in_us].mean(), mining[in_us].mean()))

fields = dict(H=Hm, precip=precip, grade=grade, rugged=rugged, valley=valley, river_size=river_size, d_river=d_river, d_river_big=d_river_big,
              route_kind=route_kind, d_route=d_route, d_rail=d_rail, region=region, farm=farm, ranch=ranch, mining=mining, access=access, water=water)

if PREVIEW:
    real = load_json(os.path.join(ROOT, 'data', 'slices', slice_name, 'world.json'))['nodes']

    def panel(a, lo, hi, ramp):
        t = np.clip((a - lo) / (hi - lo), 0, 1)[::-1]
        rgb = np.zeros(t.shape + (3,), dtype=np.float32)
        stops = np.linspace(0, 1, len(ramp))
        for ch in range(3):
            rgb[:, :, ch] = np.interp(t, stops, [c[ch] for c in ramp])
        img = Image.fromarray((np.clip(rgb, 0, 255)).astype(np.uint8))
        d = ImageDraw.Draw(img)
        for r in routes:
            px, py = to_pix([p[0] for p in r['path']], [p[1] for p in r['path']])
            d.line(list(zip(px.tolist(), py.tolist())), fill=(40, 40, 40) if r['mode'] != 'rail' else (0, 0, 0), width=1)
        for n in real:
            px, py = to_pix(n['x'], n['y'])
            d.ellipse([float(px) - 3, float(py) - 3, float(px) + 3, float(py) + 3], fill=(255, 255, 255), outline=(0, 0, 0))
        return img

    panels = [panel(inch, 0, 45, [(255, 240, 200), (200, 220, 150), (90, 170, 120), (30, 90, 170), (20, 30, 120)]),
              panel(farm, 0, 0.6, [(235, 235, 220), (190, 220, 130), (80, 170, 60), (20, 100, 30)]),
              panel(ranch, 0, 0.6, [(235, 235, 220), (230, 210, 130), (190, 140, 60), (120, 70, 20)]),
              panel(mining, 0, 0.8, [(235, 235, 235), (240, 190, 170), (200, 60, 50), (110, 10, 20)])]
    w, h = panels[0].size
    sheet = Image.new('RGB', (w * 2 + 6, h * 2 + 6), (255, 255, 255))
    for i, p in enumerate(panels):
        sheet.paste(p, ((i % 2) * (w + 6), (i // 2) * (h + 6)))
    out = os.path.join(ROOT, 'docs', 'towns_fields_%s.png' % slice_name)
    sheet.save(out)
    print('wrote', out, sheet.size)


# ================================================================ Stage 2: candidate sites from the resources, the network, the world file
# Nothing here decides which sites become towns. Every candidate (except the 1874 settlements and the railroad's stations) is a latent
# site: the sim founds it when its resource pays (the net price of its good against its break-even, scaled by the site's quality) or,
# for a mining site, when prospectors strike it (a monthly chance weighted by the deposit). See farms.cpp.
BUILD = '--build' in sys.argv
if BUILD:
    import copy

    opts = {sys.argv[i][2:]: sys.argv[i + 1] for i in range(1, len(sys.argv) - 1) if sys.argv[i].startswith('--')}
    COUNT = int(opts.get('count', 4400))          # candidate sites (the original nodes and stations come on top)
    SEED = int(opts.get('seed', 1))
    START_SHARE = float(opts.get('start-share', 0.35))   # share of early farm and ranch sites already settled in 1874
    rng = np.random.default_rng(SEED)
    out_name = '%s_%d' % (slice_name, COUNT)

    base = load_json(os.path.join(ROOT, 'data', 'slices', slice_name, 'world.json'))
    scn = load_json(os.path.join(ROOT, 'data', 'slices', slice_name, 'scenario.json'))
    nodes = copy.deepcopy(base['nodes'])
    edges = copy.deepcopy(base['edges'])
    assert len(routes) == len(edges), 'routes.json must be the route_probe export of the same world'
    n_real = len(nodes)
    START = '1874-01-01'

    def cell_of(xm, ym):
        return (min(max(int((ym - y0) / COARSE), 0), ny - 1), min(max(int((xm - x0) / COARSE), 0), nx - 1))

    def region_at(xm, ym):
        r, c = cell_of(xm, ym)
        code = int(region[r, c])
        if code == 0:
            return None
        name = region_names[code - 1]
        lat, lon = PROJ.unproject(xm, ym)
        if name == 'dakota' and 43.2 <= float(lat) <= 45.1 and -104.1 <= float(lon) <= -103.0:
            return 'black_hills'
        return name

    def climate_at(xm, ym):
        lat, lon = PROJ.unproject(xm, ym)
        lat, lon = float(lat), float(lon)
        if lat < 35.2: return 'texas'
        if lat < 39.6: return 'central_plains'
        if lon < -108.5 and lat > 45.4: return 'northern_rockies'
        return 'northern_plains'

    def iso(year, month=1):
        return '%04d-%02d-01' % (year, month)

    taken = [(n['x'], n['y']) for n in nodes]
    taken_arr = np.array(taken, dtype=np.float64)

    def near_taken(xm, ym, d):
        return bool((np.hypot(taken_arr[:, 0] - xm, taken_arr[:, 1] - ym) < d).any())

    def mark_taken(xm, ym):
        global taken_arr
        taken_arr = np.vstack([taken_arr, [xm, ym]])

    def poly_len(path):
        return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(path, path[1:]))

    def along(path, dist):
        for a, b in zip(path, path[1:]):
            seg = math.hypot(b[0] - a[0], b[1] - a[1])
            if dist <= seg:
                t = dist / seg if seg > 0 else 0
                return a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            dist -= seg
        return path[-1]

    # ------------------------------------------------------------ names
    A = ['Cedar', 'Prairie', 'Willow', 'Elm', 'Buffalo', 'Antelope', 'Rock', 'Silver', 'Golden', 'Clear', 'Bitter', 'Salt', 'Sand', 'Red', 'White',
         'Painted', 'Hackberry', 'Cottonwood', 'Plum', 'Sage', 'Pine', 'Oak', 'Spring', 'Antler', 'Eagle', 'Bear', 'Coyote', 'Wolf', 'Pawnee', 'Sioux',
         'Lone', 'Twin', 'Round', 'Flat', 'Long', 'Dry', 'Big', 'Little', 'Mule', 'Elk', 'Beaver', 'Otter', 'Mesquite', 'Pecan', 'Blue', 'Green', 'Stone',
         'Mill', 'Ford', 'Grand', 'Mound', 'Smoky', 'Medicine', 'Lodgepole', 'Tin', 'Iron', 'Copper', 'Lead', 'Granite', 'Hickory', 'Walnut', 'Juniper',
         'Aspen', 'Birch', 'Maple', 'Ash', 'Sumac', 'Yucca', 'Cactus', 'Thunder', 'Rattlesnake', 'Badger', 'Prairie Dog', 'Skunk', 'Mink', 'Marten']
    B = ['Creek', 'Springs', 'Crossing', 'Ford', 'Bend', 'Grove', 'Butte', 'Flats', 'Hill', 'Wells', 'Fork', 'Junction', 'Bluff', 'Valley',
         'Point', 'Corners', 'Mills', 'Landing', 'City', 'Gulch', 'Draw', 'Divide', 'Lake', 'Run', 'Holler', 'Basin', 'Ranch', 'Station']
    used_names = {n['name'] for n in nodes}

    def new_name(style):
        for _ in range(300):
            if style == 'camp':
                nm = '%s %s' % (A[rng.integers(len(A))], ['Gulch', 'Camp', 'Diggings', 'City', 'Hill', 'Flat'][rng.integers(6)])
            elif style == 'post':
                nm = '%s %s' % (A[rng.integers(len(A))], ['Post', 'Trading Post', 'Crossing', 'Landing', 'Forks'][rng.integers(5)])
            elif style == 'mill':
                nm = '%s %s' % (A[rng.integers(len(A))], ['Mills', 'Mill', 'Sawmill', 'Pinery', 'Lumber Camp'][rng.integers(5)])
            else:
                nm = '%s %s' % (A[rng.integers(len(A))], B[rng.integers(len(B))])
            if nm not in used_names:
                used_names.add(nm)
                return nm
        nm = 'Town %d' % (len(used_names) + 1)
        used_names.add(nm)
        return nm

    # ------------------------------------------------------------ resource potentials (0 to 1), spread over the land by simple rules
    plains_rain = np.clip((precip - 250.0) / 200.0, 0, 1) * (precip < 1100)
    game = plains_rain * np.clip(1.0 - rugged / 300.0, 0, 1) * (0.4 + 0.6 * water) * (Hm < 2200) * in_us          # open grassland with water: bison, antelope, deer
    elev_band = np.clip((Hm - 600.0) / 400.0, 0, 1) * np.clip((3000.0 - Hm) / 600.0, 0, 1)
    fur = np.exp(-d_river / 3.0) * (0.25 + 0.75 * elev_band) * np.clip((precip - 300.0) / 250.0, 0, 1) * in_us   # beaver, mink and otter along wooded streams
    mineral = np.clip((rugged - 200.0) / 500.0, 0, 1) * (Hm > 1000) * in_us                                       # the mountains
    forest = np.clip((precip - 450.0) / 350.0, 0, 1) * np.clip((Hm - 800.0) / 700.0, 0, 1) * np.clip((3000.0 - Hm) / 500.0, 0, 1) * in_us
    forest = np.maximum(forest, np.clip((precip - 850.0) / 500.0, 0, 1) * (Hm < 800) * in_us * 0.6)              # eastern woodlands (east Texas, Missouri, Minnesota)
    ranch_n = ranch / max(float(ranch.max()), 1e-9)

    # ------------------------------------------------------------ when a place could open (guesses; see CLAUDE.md)
    def farm_open_year(lon, lat):
        """The line of settlement moves west: east of 96 W before 1870, 100 W about 1878, 102 W about 1887; Texas a little earlier."""
        year = 1860 + 4.5 * (-96.0 - lon) - (3 if lat < 34 else 0)
        return int(min(max(round(year), 1862), 1895))

    def ranch_open_year(lat):
        """The cattle boom: Texas in the 1870s, the northern ranges about 1880."""
        return int(round(1874 + max(0.0, min(1.0, (lat - 31.0) / 17.0)) * 8))

    # ------------------------------------------------------------ site templates (the same fields as the original towns)
    def base_site(kind, x, y, name, reg, pop, capacity, cash, stock):
        return {'id': None, 'region': reg, 'name': name, 'kind': kind, 'x': round(x), 'y': round(y), 'base_population': max(1, pop // 5),
                'population': pop, 'capacity': capacity, 'cash': cash, 'climate': climate_at(x, y), 'stock': stock}

    def farm_site(x, y, reg, r, c):
        pop = int(rng.choice([40, 70, 120, 200]))
        yld = round(0.55 + 0.45 * float(rain_farm[r, c]), 2)
        n = base_site('farm town', x, y, new_name('town'), reg, pop, int(rng.choice([800, 1500, 3000])), 1500,
                      {'beef_cattle': 20, 'flour': pop, 'tools': 15, 'whiskey': 30, 'ammunition': 15})
        n.update({'farmers': pop // 2, 'farm_capacity': int(rng.choice([800, 1500, 3000])), 'farm_yield': yld, 'game_capacity': round(250 * float(game[r, c]))})
        n['found_quality'] = round(yld / 0.8, 2)     # a better farm country is worth settling at a lower price
        return n

    def ranch_site(x, y, reg, lat, r, c):
        pop = int(rng.choice([30, 60, 100, 150]))
        n = base_site('ranch town', x, y, new_name('town'), reg, pop, int(rng.choice([300, 800, 1500])), 1000,
                      {'beef_cattle': 80, 'flour': pop // 2, 'whiskey': 25, 'ammunition': 15})
        n['production'] = {'beef_cattle': {'rate': int(rng.choice([8, 15, 30])), 'labor_pop': 25}}
        n['game_capacity'] = round(300 * float(game[r, c]))
        n['found_crop'] = 'beef_cattle'
        n['found_quality'] = round(0.6 + 0.8 * float(ranch_n[r, c]), 2)
        if lat > 41.5:
            n['tags'] = ['northern_range']   # the 1886-87 die-out reaches the northern ranges
        return n

    def hunters_site(x, y, reg, r, c):
        g = float(game[r, c])
        n = base_site('hunters camp', x, y, new_name('camp'), reg, 25, 600, 400, {'flour': 40, 'whiskey': 25, 'ammunition': 40})
        n['reserves'] = {'hides': int(g * rng.choice([60000, 120000, 250000]) + 5000)}
        n['production'] = {'hides': {'rate': int(rng.choice([150, 250, 350])), 'labor_pop': int(rng.choice([40, 80, 120]))}}
        n['found_crop'] = 'hides'
        n['found_quality'] = round(0.7 + 0.8 * g, 2)
        return n

    def fur_site(x, y, reg, r, c):
        f = float(fur[r, c])
        n = base_site('trapping post', x, y, new_name('post'), reg, 12, 300, 300, {'flour': 20, 'whiskey': 15, 'ammunition': 20, 'tools': 4})
        n['reserves'] = {'furs': int(f * rng.choice([5000, 12000, 25000]) + 800)}
        n['production'] = {'furs': {'rate': int(rng.choice([8, 15, 30])), 'labor_pop': int(rng.choice([8, 15, 25]))}}
        n['found_crop'] = 'furs'
        n['found_quality'] = round(0.8 + 0.6 * f, 2)
        return n

    def mining_site(x, y, reg, r, c):
        m = float(mineral[r, c])
        n = base_site('mining camp', x, y, new_name('camp'), reg, 12, 3000, 300, {'flour': 10, 'whiskey': 20, 'ammunition': 10})
        n['reserves'] = {'gold': int(rng.choice([80000, 200000, 500000]))}
        n['production'] = {'gold': {'rate': int(rng.choice([40, 80, 160])), 'labor_pop': int(rng.choice([60, 120, 250]))}}
        n['strike_chance'] = round(0.0025 + 0.006 * m, 4)        # per month once the country is open: a richer district is struck sooner
        n['strike_people'] = int(rng.choice([200, 600, 1500]))
        return n

    def mill_site(x, y, reg, r, c):
        f = float(forest[r, c])
        n = base_site('sawmill', x, y, new_name('mill'), reg, 15, 600, 400, {'flour': 25, 'tools': 10, 'whiskey': 15, 'ammunition': 10})
        n['reserves'] = {'timber': int(20000 * (0.4 + f))}
        n['production'] = {'timber': {'rate': int(rng.choice([1, 2, 3])), 'labor_pop': int(rng.choice([15, 25, 40]))}}
        n['found_crop'] = 'timber'
        n['found_quality'] = round(0.8 + 0.8 * f, 2)
        return n

    def station_site(x, y, reg):
        pop = int(rng.choice([6, 12, 25, 40]))
        return base_site('rail station', x, y, new_name('town'), reg, pop, 400, 500, {'flour': pop * 2, 'whiskey': 15, 'ammunition': 10, 'beef_cattle': 5})

    added = []

    def add(node, open_date, tag, latent_site=True, found_pop=None):
        node['id'] = 'p%04d' % (len(added) + 1)
        node['_open'] = open_date
        node['_tag'] = tag
        if latent_site:   # a place on the map; the sim founds it when it pays
            node['latent'] = True
            node['found_pop'] = found_pop if found_pop is not None else max(10, node['population'] // 2)
            node['farm_open'] = open_date
            node['population'] = 0
            node.pop('base_population', None)
            if 'farm_capacity' not in node:
                node['farm_capacity'] = 0
        added.append(node)
        nodes.append(node)
        mark_taken(node['x'], node['y'])

    # ------------------------------------------------------------ 1. the railroad's stations (platted on a schedule, not found by the sim)
    rail_stations = {}
    for ei, e in enumerate(edges):
        if e.get('mode') != 'rail' or poly_len(routes[ei]['path']) < 30:
            continue
        path = routes[ei]['path']
        total = poly_len(path)
        opened = max(e.get('from', START), START)
        chain = []
        d = 11.0
        while d < total - 6.0:
            px, py = along(path, d)
            d += 11.0
            if near_taken(px, py, 7.0):
                continue
            reg = region_at(px, py)
            if reg is None:
                continue
            nd = station_site(px, py, reg)
            nd['found_quality'] = 2.0     # the railroad platted it: it is settled as soon as the rails arrive
            if opened <= START:
                add(nd, START, 'station', latent_site=False)
            else:
                add(nd, opened, 'station', latent_site=True, found_pop=30)
                nd['farm_capacity'] = 200
                nd['farm_yield'] = 0.7
            chain.append(nd['id'])
        rail_stations[ei] = chain
    n_stations = len(added)
    print('rail stations placed:', n_stations)

    # ------------------------------------------------------------ 2. candidate sites for each resource, spread over the land
    shares = {'farm': 0.38, 'ranch': 0.23, 'mining': 0.12, 'fur': 0.10, 'hunt': 0.07, 'mill': 0.10}
    quota = {k: int(COUNT * v) for k, v in shares.items()}
    spacing = {'farm': 11.0, 'ranch': 15.0, 'mining': 11.0, 'fur': 14.0, 'hunt': 20.0, 'mill': 15.0}
    pot = {'farm': farm, 'ranch': ranch, 'mining': mineral, 'fur': fur, 'hunt': game, 'mill': forest}
    placed = {k: 0 for k in quota}
    started = 0
    for cat in ('mining', 'mill', 'fur', 'hunt', 'ranch', 'farm'):
        sc = pot[cat]
        w = np.where(sc > 0.03, sc ** 1.2, 0.0).ravel()
        if w.sum() <= 0:
            continue
        w = w / w.sum()
        tries = 0
        while placed[cat] < quota[cat] and tries < 60:
            tries += 1
            for flat_i in rng.choice(w.size, size=6000, p=w):
                if placed[cat] >= quota[cat]:
                    break
                r, c = divmod(int(flat_i), nx)
                px = x0 + (c + rng.random()) * COARSE
                py = y0 + (r + rng.random()) * COARSE
                if near_taken(px, py, spacing[cat]):
                    continue
                reg = region_at(px, py)
                if reg is None:
                    continue
                lat, lon = PROJ.unproject(px, py)
                lat, lon = float(lat), float(lon)
                if cat == 'farm':
                    year = farm_open_year(lon, lat)
                    nd = farm_site(px, py, reg, r, c)
                    early = year <= 1874 and rng.random() < START_SHARE
                    add(nd, iso(max(year, 1862)) if year > 1874 else START, 'farm', latent_site=not early)
                    started += early
                    if not early:
                        nd['farm_capacity'] = int(rng.choice([800, 1500, 3000]))
                elif cat == 'ranch':
                    year = ranch_open_year(lat)
                    nd = ranch_site(px, py, reg, lat, r, c)
                    early = year <= 1874 and rng.random() < START_SHARE
                    add(nd, iso(year) if year > 1874 else START, 'ranch', latent_site=not early)
                    started += early
                elif cat == 'hunt':
                    south = lat < 40.5    # the southern herd was shot out by about 1879, the northern one by about 1883
                    year = 1874 if south else int(rng.integers(1880, 1884))
                    add(hunters_site(px, py, reg, r, c), iso(year), 'hunt', found_pop=25)
                elif cat == 'fur':
                    add(fur_site(px, py, reg, r, c), iso(int(rng.integers(1874, 1880))), 'fur', found_pop=12)
                elif cat == 'mining':
                    add(mining_site(px, py, reg, r, c), iso(int(rng.integers(1874, 1883))), 'mining', found_pop=12)
                else:
                    add(mill_site(px, py, reg, r, c), iso(int(rng.integers(1874, 1884))), 'mill', found_pop=15)
                placed[cat] += 1
    print('candidate sites placed:', placed, '(of which settled at the start: %d)' % started)
    print('total nodes', len(nodes))

    # ------------------------------------------------------------ 3. the network: every site is linked to its nearest neighbours
    idx_of = {n['id']: i for i, n in enumerate(nodes)}
    P = np.array([[n['x'], n['y']] for n in nodes], dtype=np.float64)
    out_edges = []
    seen = set()
    deg = np.zeros(len(nodes), dtype=int)

    def link(a, b, mode='trail', frm=None, note=''):
        key = (min(a, b), max(a, b), mode)
        if a == b or key in seen:
            return
        seen.add(key)
        e = {'a': nodes[a]['id'], 'b': nodes[b]['id']}
        if mode != 'trail':
            e['mode'] = mode
        if frm and frm > START:
            e['from'] = frm
        if note:
            e['note'] = note
        out_edges.append(e)
        deg[a] += 1
        deg[b] += 1

    for ei, e in enumerate(edges):
        a, b = idx_of[e['a']], idx_of[e['b']]
        chain = rail_stations.get(ei)
        if chain:
            seq = [a] + [idx_of[c] for c in chain] + [b]
            for u, v in zip(seq, seq[1:]):
                link(u, v, 'rail', e.get('from'), 'railroad station spacing')
        else:
            seen.add((min(a, b), max(a, b), e.get('mode', 'trail')))
            out_edges.append(e)
            deg[a] += 1
            deg[b] += 1

    # Roads to latent sites are always open (no dates): a site that is not yet a town is just a place on a road, and dated routes would
    # force the whole route table to be rebuilt every time one opened.
    order = list(range(n_real, len(nodes)))
    for i in order:
        d = np.hypot(P[:, 0] - P[i, 0], P[:, 1] - P[i, 1])
        d[i] = 1e9
        want = 3 if nodes[i]['_tag'] in ('mining', 'fur', 'hunt') else 2
        reach = 150 if nodes[i]['_tag'] in ('mining', 'fur', 'hunt') else 80
        for j in np.argsort(d)[:want + 4]:
            if deg[i] >= want or d[j] > reach:
                break
            link(i, int(j), 'trail')
    parent = list(range(len(nodes)))

    def find(u):
        while parent[u] != u:
            parent[u] = parent[parent[u]]
            u = parent[u]
        return u

    for e in out_edges:
        parent[find(idx_of[e['a']])] = find(idx_of[e['b']])
    joins = 0
    for _ in range(len(nodes)):
        main_root = find(0)
        outside = [i for i in range(len(nodes)) if find(i) != main_root]
        if not outside:
            break
        inside = np.array([find(j) == main_root for j in range(len(nodes))])
        best = None
        for i in outside:
            d = np.where(inside, np.hypot(P[:, 0] - P[i, 0], P[:, 1] - P[i, 1]), 1e9)
            j = int(np.argmin(d))
            if best is None or d[j] < best[0]:
                best = (d[j], i, j)
        link(best[1], best[2], 'trail', None, 'joins an isolated place to the network')
        parent[find(best[1])] = find(best[2])
        joins += 1
    print('edges:', len(out_edges), '(joined %d islands)' % joins)

    # ------------------------------------------------------------ 4. write the world and the scenario
    for n in nodes:
        n.pop('_open', None)
        n.pop('_tag', None)
    world = dict(base)
    world['_note'] = ('GENERATED by tools/gen_towns.py from %s (seed %d): the %d original nodes plus railroad stations and %d candidate sites placed by '
                      'resource (farm land, grass, game, furs, minerals, timber). Candidates are latent: the sim founds the ones that pay.') % (slice_name, SEED, n_real, sum(placed.values()))
    world['nodes'] = nodes
    world['edges'] = out_edges
    regs = dict(world.get('regions', {}))
    for rn in region_names:
        regs.setdefault(rn, {})
    regs.setdefault('black_hills', {'organized': '1877-03-01'})
    world['regions'] = regs

    scenario = copy.deepcopy(scn)
    f = (COUNT * 0.5 + n_stations + n_real) / 31.0     # the traders an expected 2,000-odd towns would carry (as in gen_world.py)
    scenario['traders'] = int(scn['traders'] * f)
    scenario['max_traders'] = int(scn['max_traders'] * f)

    out = os.path.join(ROOT, 'data', 'slices', out_name)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'goods.json'), 'wb').write(open(os.path.join(ROOT, 'data', 'slices', slice_name, 'goods.json'), 'rb').read())
    json.dump(world, open(os.path.join(out, 'world.json'), 'w', encoding='utf-8'))
    json.dump(scenario, open(os.path.join(out, 'scenario.json'), 'w', encoding='utf-8'), indent=1)
    kinds = {}
    for n in nodes:
        kinds[n['kind']] = kinds.get(n['kind'], 0) + 1
    print('wrote %s: %d nodes, %d edges' % (out, len(nodes), len(out_edges)))
    print('by kind:', dict(sorted(kinds.items(), key=lambda kv: -kv[1])))

    if PREVIEW:
        try:
            base_img = Image.open(os.path.join(ROOT, 'data', 'terrain', slice_name, 'hillshade.jpg')).convert('RGB')
            sx = base_img.size[0] / (th['width'] * th['cell_miles'])
            sy = base_img.size[1] / (th['height'] * th['cell_miles'])
            img = base_img.copy()
            dr = ImageDraw.Draw(img)

            def P2(xm, ym):
                return ((xm - x0) * sx, img.size[1] - (ym - y0) * sy)
            colors = {'rail station': (30, 30, 30), 'farm town': (20, 140, 40), 'ranch town': (200, 140, 20), 'mining camp': (200, 30, 30),
                      'trapping post': (30, 90, 200), 'hunters camp': (140, 60, 20), 'sawmill': (110, 40, 150)}
            for n in nodes[n_real:]:
                px, py = P2(n['x'], n['y'])
                dr.ellipse([px - 2.5, py - 2.5, px + 2.5, py + 2.5], fill=colors.get(n['kind'], (0, 0, 0)))
            for n in nodes[:n_real]:
                px, py = P2(n['x'], n['y'])
                dr.ellipse([px - 5, py - 5, px + 5, py + 5], fill=(255, 255, 255), outline=(0, 0, 0))
            img = img.resize((img.size[0] // 2, img.size[1] // 2), Image.LANCZOS)
            img.save(os.path.join(ROOT, 'docs', 'towns_candidates_%s.png' % out_name))
            print('wrote docs/towns_candidates_%s.png' % out_name)
        except Exception as ex:   # the picture is a convenience
            print('preview failed:', ex)
