"""Procedural towns for the whole map: where settlements could have been, from the land itself.

usage: python -I gen_towns.py SLICE WORK_DIR [--preview] [--build] [--count 2000] [--seed 1]
  WORK_DIR holds the downloads and exports this tool reads (none are stored in the repo):
    prism/x/prism_ppt_us_25m_2020_avg_30y.tif      PRISM 30-year annual precipitation normal (millimetres, 2.5 arc-minute grid)
    ne_raw/admin1/ne_10m_admin_1_states_provinces.shp   Natural Earth states and provinces (public domain)
    routes.json                                     `route_probe DATA --paths` for the slice (each route's way over the terrain)
  and the repo's data/terrain/SLICE, data/rivers/SLICE and data/history/SLICE.

Stage 1: the suitability fields on a coarse grid (2-mile cells) and a picture of them (--preview writes docs/towns_fields_SLICE.png).
Stage 2 (--build): places towns on the fields (stations along the railroads and roads, farm towns, ranch headquarters, mining camps, trading
posts), links them into the network and writes data/slices/SLICE_N: a world of N nodes (the original ones plus the new ones).
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


# ================================================================ Stage 2: place towns, link them, write the world
BUILD = '--build' in sys.argv
if BUILD:
    import copy, datetime

    opts = {sys.argv[i][2:]: sys.argv[i + 1] for i in range(1, len(sys.argv) - 1) if sys.argv[i].startswith('--')}
    COUNT = int(opts.get('count', 2000))
    SEED = int(opts.get('seed', 1))
    rng = np.random.default_rng(SEED)
    out_name = '%s%d' % (slice_name, COUNT)

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

    pos = np.array([[n['x'], n['y']] for n in nodes], dtype=np.float64)
    taken = [tuple(p) for p in pos]

    def near_taken(xm, ym, d):
        a = np.array(taken)
        return bool((np.hypot(a[:, 0] - xm, a[:, 1] - ym) < d).any())

    def poly_len(path):
        return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(path, path[1:]))

    def along(path, dist):
        """The point `dist` miles along a polyline."""
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
         'Fort', 'Mill', 'Ford', 'Grand', 'Mound', 'Smoky', 'Medicine', 'Lodgepole', 'Tin', 'Iron', 'Copper', 'Lead', 'Granite']
    B = ['Creek', 'Springs', 'Crossing', 'Ford', 'Station', 'Bend', 'Grove', 'Butte', 'Flats', 'Hill', 'Wells', 'Fork', 'Junction', 'Bluff', 'Valley',
         'Point', 'Corners', 'Mills', 'Landing', 'City', 'Camp', 'Gulch', 'Draw', 'Divide', 'Lake', 'Run', 'Siding', 'Holler', 'Basin']
    used_names = {n['name'] for n in nodes}

    def new_name(kind):
        for _ in range(200):
            if kind == 'mining camp':
                nm = '%s %s' % (A[rng.integers(len(A))], ['Gulch', 'Camp', 'Diggings', 'City', 'Hill', 'Flat'][rng.integers(6)])
            else:
                nm = '%s %s' % (A[rng.integers(len(A))], B[rng.integers(len(B))])
            if nm not in used_names:
                used_names.add(nm)
                return nm
        nm = 'Town %d' % (len(used_names) + 1)
        used_names.add(nm)
        return nm

    # ------------------------------------------------------------ when a place could open (guesses; see CLAUDE.md)
    def farm_open_year(lon, lat):
        """The line of settlement moves west: east of 96 W before 1870, 100 W about 1878, 102 W about 1887; Texas a little earlier."""
        year = 1860 + 4.5 * (-96.0 - lon) - (3 if lat < 34 else 0)
        return int(min(max(round(year), 1862), 1895))

    def ranch_open_year(lat):
        """The cattle boom: Texas in the 1870s, the northern ranges about 1880."""
        return int(round(1874 + max(0.0, min(1.0, (lat - 31.0) / 17.0)) * 8))

    # ------------------------------------------------------------ templates (the same fields as the original towns)
    def farm_node(x, y, name, reg, lon, lat):
        r, c = cell_of(x, y)
        yld = round(0.55 + 0.45 * float(rain_farm[r, c]), 2)
        pop = int(rng.choice([40, 70, 120, 200]))
        return {'id': None, 'region': reg, 'name': name, 'kind': 'farm town', 'x': round(x), 'y': round(y),
                'base_population': pop // 5, 'farmers': pop // 2, 'farm_capacity': int(rng.choice([800, 1500, 3000])), 'farm_yield': yld,
                'climate': climate_at(x, y), 'population': pop, 'capacity': int(rng.choice([800, 1500, 3000])), 'cash': 1500,
                'stock': {'beef_cattle': 20, 'flour': pop, 'tools': 15, 'whiskey': 30, 'ammunition': 15}}

    def ranch_node(x, y, name, reg, lon, lat):
        pop = int(rng.choice([30, 60, 100, 150]))
        n = {'id': None, 'region': reg, 'name': name, 'kind': 'ranch town', 'x': round(x), 'y': round(y), 'base_population': pop // 5,
             'population': pop, 'capacity': int(rng.choice([300, 800, 1500])), 'cash': 1000, 'climate': climate_at(x, y),
             'stock': {'beef_cattle': 80, 'flour': pop // 2, 'whiskey': 25, 'ammunition': 15},
             'production': {'beef_cattle': {'rate': int(rng.choice([8, 15, 30])), 'labor_pop': 25}}}
        if lat > 41.5:
            n['tags'] = ['northern_range']   # the 1886-87 die-out reaches the northern ranges
        return n

    def mining_node(x, y, name, reg, year):
        reserve = int(rng.choice([80000, 200000, 500000]))
        return {'id': None, 'region': reg, 'name': name, 'kind': 'mining camp', 'x': round(x), 'y': round(y), 'population': 12, 'capacity': 3000, 'cash': 300,
                'stock': {'flour': 10, 'whiskey': 20, 'ammunition': 10}, 'reserves': {'gold': reserve},
                'production': {'gold': {'rate': int(rng.choice([40, 80, 160])), 'labor_pop': int(rng.choice([60, 120, 250])), 'from': iso(year, int(rng.integers(1, 13)))}}}

    def depot_node(x, y, name, reg, kind):
        pop = int(rng.choice([6, 12, 25, 40]))
        return {'id': None, 'region': reg, 'name': name, 'kind': kind, 'x': round(x), 'y': round(y), 'base_population': pop // 2, 'population': pop,
                'capacity': 400, 'cash': 500, 'climate': climate_at(x, y), 'stock': {'flour': pop * 2, 'whiskey': 15, 'ammunition': 10, 'beef_cattle': 5}}

    def post_node(x, y, name, reg):
        pop = int(rng.choice([8, 15, 30]))
        return {'id': None, 'region': reg, 'name': name, 'kind': 'trading post', 'x': round(x), 'y': round(y), 'base_population': pop // 2, 'population': pop,
                'capacity': 300, 'cash': 400, 'climate': climate_at(x, y), 'stock': {'flour': pop, 'whiskey': 20, 'ammunition': 15, 'tools': 5}}

    added = []   # (node dict, open date or None, rail edge index and order or None)

    def add(node, open_date, kind_tag):
        node['id'] = 'p%04d' % (len(added) + 1)
        node['_open'] = open_date
        node['_tag'] = kind_tag
        added.append(node)
        nodes.append(node)
        taken.append((node['x'], node['y']))

    def latent(node, open_year_date, found_pop):
        """A place that does not exist yet becomes a town when settling pays after `farm_open`."""
        node['latent'] = True
        node['found_pop'] = found_pop
        node['farm_open'] = open_year_date
        if 'farm_capacity' not in node:
            node['farm_capacity'] = 200
            node['farm_yield'] = 0.7
        node['population'] = 0
        node.pop('base_population', None)

    # ------------------------------------------------------------ 1. stations along the railroads (and a chain replaces each rail edge)
    new_edges = []
    rail_stations = {}
    for ei, e in enumerate(edges):
        if e.get('mode') != 'rail' or poly_len(routes[ei]['path']) < 30:
            continue
        path = routes[ei]['path']
        total = poly_len(path)
        opened = e.get('from', START)
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
            lat, lon = PROJ.unproject(px, py)
            nd = depot_node(px, py, new_name('depot'), reg, 'rail station')
            add(nd, max(opened, START), 'station')
            if opened > START:
                latent(nd, opened, 30)
            chain.append(nd['id'])
        rail_stations[ei] = chain
    print('rail stations placed:', sum(len(v) for v in rail_stations.values()))

    # ------------------------------------------------------------ 2. stage stations and road ranches along the trails
    for ei, e in enumerate(edges):
        if e.get('mode', 'trail') != 'trail' or e.get('miles', 0) == 0 and routes[ei]['miles'] < 45:
            continue
        if routes[ei]['miles'] < 45:
            continue
        path = routes[ei]['path']
        total = poly_len(path)
        opened = e.get('from', START)
        d = 22.0
        while d < total - 10.0:
            px, py = along(path, d)
            d += 22.0
            if near_taken(px, py, 12.0):
                continue
            reg = region_at(px, py)
            if reg is None:
                continue
            nd = depot_node(px, py, new_name('station'), reg, 'stage station')
            add(nd, max(opened, START), 'station')
            if opened > START:
                latent(nd, opened, 15)
    print('with stage stations:', len(added))

    # ------------------------------------------------------------ 3. farm towns, ranch headquarters, mining camps and trading posts, by suitability
    remaining = max(0, COUNT - len(nodes))
    quota = {'mining': int(remaining * 0.12), 'farm': int(remaining * 0.50), 'ranch': int(remaining * 0.33)}
    quota['post'] = remaining - sum(quota.values())
    spacing = {'mining': 14.0, 'farm': 13.0, 'ranch': 18.0, 'post': 28.0}
    post_score = access * np.maximum(water, 0.2) * in_us * (Hm < 2200) * (d_river_big < 12)
    score_of = {'mining': mining, 'farm': farm, 'ranch': ranch, 'post': post_score}
    placed = {k: 0 for k in quota}
    for cat in ('mining', 'farm', 'ranch', 'post'):
        sc = score_of[cat]
        w = np.where(sc > 0.04, sc ** 1.5, 0.0).ravel()
        if w.sum() <= 0:
            continue
        w = w / w.sum()
        tries = 0
        while placed[cat] < quota[cat] and tries < 40:
            tries += 1
            picks = rng.choice(w.size, size=min(4000, max(200, quota[cat] * 8)), p=w)
            for flat_i in picks:
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
                if cat == 'mining':
                    year = int(rng.integers(1874, 1887))
                    nd = mining_node(px, py, new_name('mining camp'), reg, year)
                    add(nd, iso(year), 'mining')   # a tiny camp from the start; its production and the rush begin on the date
                elif cat == 'farm':
                    year = farm_open_year(lon, lat)
                    nd = farm_node(px, py, new_name('farm'), reg, lon, lat)
                    add(nd, iso(year), 'farm')
                    if year > 1874:
                        latent(nd, iso(year), nd['population'] // 2 or 20)
                        nd['farm_capacity'] = int(rng.choice([800, 1500, 3000]))
                elif cat == 'ranch':
                    year = ranch_open_year(lat)
                    nd = ranch_node(px, py, new_name('ranch'), reg, lon, lat)
                    add(nd, iso(year), 'ranch')
                    if year > 1874:
                        latent(nd, iso(year), nd['population'] // 2 or 20)
                else:
                    nd = post_node(px, py, new_name('post'), reg)
                    add(nd, START, 'post')
                placed[cat] += 1
    # the trading posts under-fill (they need a big river nearby), so the shortfall goes to farm towns and ranch headquarters
    short = COUNT - len(nodes)
    if short > 0:
        for cat, share in (('farm', 0.6), ('ranch', 0.4)):
            quota[cat] = placed[cat] + int(short * share)
            sc = score_of[cat]
            w = np.where(sc > 0.04, sc ** 1.5, 0.0).ravel()
            w = w / w.sum()
            for _ in range(40):
                if placed[cat] >= quota[cat]:
                    break
                for flat_i in rng.choice(w.size, size=4000, p=w):
                    if placed[cat] >= quota[cat]:
                        break
                    r, c = divmod(int(flat_i), nx)
                    px, py = x0 + (c + rng.random()) * COARSE, y0 + (r + rng.random()) * COARSE
                    if near_taken(px, py, spacing[cat] * 0.8):
                        continue
                    reg = region_at(px, py)
                    if reg is None:
                        continue
                    lat, lon = PROJ.unproject(px, py)
                    lat, lon = float(lat), float(lon)
                    if cat == 'farm':
                        year = farm_open_year(lon, lat)
                        nd = farm_node(px, py, new_name('farm'), reg, lon, lat)
                    else:
                        year = ranch_open_year(lat)
                        nd = ranch_node(px, py, new_name('ranch'), reg, lon, lat)
                    add(nd, iso(year), cat)
                    if year > 1874:
                        latent(nd, iso(year), nd['population'] // 2 or 20)
                        if cat == 'farm':
                            nd['farm_capacity'] = int(rng.choice([800, 1500, 3000]))
                    placed[cat] += 1
    print('placed:', placed, '-> total nodes', len(nodes))

    # ------------------------------------------------------------ 4. the network
    idx_of = {n['id']: i for i, n in enumerate(nodes)}
    P = np.array([[n['x'], n['y']] for n in nodes], dtype=np.float64)
    out_edges = []
    seen = set()

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

    # the original routes: a rail edge with stations becomes a chain
    for ei, e in enumerate(edges):
        a, b = idx_of[e['a']], idx_of[e['b']]
        chain = rail_stations.get(ei)
        if chain:
            seq = [a] + [idx_of[c] for c in chain] + [b]
            for u, v in zip(seq, seq[1:]):
                link(u, v, 'rail', e.get('from'), 'railroad station spacing')
        else:
            key = (min(a, b), max(a, b), e.get('mode', 'trail'))
            seen.add(key)
            out_edges.append(e)

    def open_of(i):
        return nodes[i].get('_open') or START

    # everything else links to its nearest neighbours
    new_ids = [idx_of[n['id']] for n in added if n['_tag'] != 'station' or True]
    for i in new_ids:
        if any((nodes[i]['id'] in (e['a'], e['b'])) for e in out_edges[:0]):
            continue
        d = np.hypot(P[:, 0] - P[i, 0], P[:, 1] - P[i, 1])
        d[i] = 1e9
        kk = 2 if nodes[i]['_tag'] == 'station' else 3
        reach = 160 if nodes[i]['_tag'] == 'mining' else 80
        for j in np.argsort(d)[:kk + 2]:
            if d[j] > reach and kk == 0:
                break
            if d[j] > reach:
                continue
            if sum(1 for e in out_edges if nodes[i]['id'] in (e['a'], e['b'])) >= kk + 1:
                break
            link(i, int(j), 'trail', max(open_of(i), open_of(int(j))))
    # components: join every island to the main network by its nearest outside node
    parent = list(range(len(nodes)))

    def find(u):
        while parent[u] != u:
            parent[u] = parent[parent[u]]
            u = parent[u]
        return u

    for e in out_edges:
        parent[find(idx_of[e['a']])] = find(idx_of[e['b']])
    main_root = find(0)
    joins = 0
    for _ in range(len(nodes)):
        roots = {find(i) for i in range(len(nodes))}
        if len(roots) == 1:
            break
        best = None
        for i in range(len(nodes)):
            if find(i) == main_root:
                continue
            d = np.hypot(P[:, 0] - P[i, 0], P[:, 1] - P[i, 1])
            mask = np.array([find(j) == main_root for j in range(len(nodes))])
            d = np.where(mask, d, 1e9)
            j = int(np.argmin(d))
            if best is None or d[j] < best[0]:
                best = (d[j], i, j)
        link(best[1], best[2], 'trail', max(open_of(best[1]), open_of(best[2])), 'joins an isolated place to the network')
        parent[find(best[1])] = find(best[2])
        joins += 1
    print('edges:', len(out_edges), '(joined %d islands)' % joins)

    # ------------------------------------------------------------ 5. write the world and the scenario
    for n in nodes:
        n.pop('_open', None)
        n.pop('_tag', None)
    world = dict(base)
    world['_note'] = 'GENERATED by tools/gen_towns.py from %s (seed %d): the %d original nodes plus procedural towns placed on suitability fields from rainfall (PRISM), slope, rivers and trade routes.' % (slice_name, SEED, n_real)
    world['nodes'] = nodes
    world['edges'] = out_edges
    regs = dict(world.get('regions', {}))
    for rn in region_names:
        regs.setdefault(rn, {})
    regs.setdefault('black_hills', {'organized': '1877-03-01'})
    world['regions'] = regs

    scenario = copy.deepcopy(scn)
    f = COUNT / 31.0
    scenario['traders'] = int(scn['traders'] * f)
    scenario['max_traders'] = int(scn['max_traders'] * f)
    shocks = list(scenario.get('shocks', []))
    for nd in added:
        if nd.get('kind') == 'mining camp' and nd['production']['gold']['from'] > '1874-06-01':
            shocks.append({'name': 'Rush to %s' % nd['name'], 'kind': 'rush', 'date': nd['production']['gold']['from'], 'node': nd['id'],
                           'people': int(rng.choice([200, 600, 1500])), 'cash_per_person': 40, 'cause': 'a strike (a procedural camp; the date is a guess)'})
    scenario['shocks'] = shocks

    out = os.path.join(ROOT, 'data', 'slices', out_name)
    os.makedirs(out, exist_ok=True)
    for fn in ('goods.json',):
        open(os.path.join(out, fn), 'wb').write(open(os.path.join(ROOT, 'data', 'slices', slice_name, fn), 'rb').read())
    json.dump(world, open(os.path.join(out, 'world.json'), 'w', encoding='utf-8'))
    json.dump(scenario, open(os.path.join(out, 'scenario.json'), 'w', encoding='utf-8'), indent=1)
    kinds = {}
    for n in nodes:
        kinds[n['kind']] = kinds.get(n['kind'], 0) + 1
    print('wrote %s: %d nodes, %d edges, %d scenario shocks' % (out, len(nodes), len(out_edges), len(shocks)))
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
            colors = {'rail station': (30, 30, 30), 'stage station': (120, 60, 20), 'farm town': (20, 140, 40), 'ranch town': (200, 140, 20),
                      'mining camp': (200, 30, 30), 'trading post': (30, 90, 200)}
            for n in nodes[n_real:]:
                px, py = P2(n['x'], n['y'])
                col = colors.get(n['kind'], (0, 0, 0))
                dr.ellipse([px - 2.5, py - 2.5, px + 2.5, py + 2.5], fill=col)
            for n in nodes[:n_real]:
                px, py = P2(n['x'], n['y'])
                dr.ellipse([px - 5, py - 5, px + 5, py + 5], fill=(255, 255, 255), outline=(0, 0, 0))
            img = img.resize((img.size[0] // 2, img.size[1] // 2), Image.LANCZOS)
            img.save(os.path.join(ROOT, 'docs', 'towns_placed_%s.png' % out_name))
            print('wrote docs/towns_placed_%s.png' % out_name)
        except Exception as ex:   # the picture is a convenience
            print('preview failed:', ex)
