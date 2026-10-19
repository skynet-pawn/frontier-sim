"""Turns downloaded elevation tiles into a heightfield in the sim's own map coordinates.

usage: python -I make_terrain.py SLICE RAWDIR [--zoom 10] [--cell 0.25]      (e.g. black_hills ../scratch/terrain_raw10)

RAWDIR holds AWS Terrain Tiles in Terrarium format (https://registry.opendata.aws/terrain-tiles/, files named zZ_X_Y.png,
fetched from https://s3.amazonaws.com/elevation-tiles-prod/terrarium/Z/X/Y.png). A Terrarium pixel stores metres above sea
level as (R * 256 + G + B / 256) - 32768. The tiles blend USGS 3DEP, SRTM, GMTED and ETOPO1; attribution is required (see
data/terrain/ATTRIBUTION.md). Zoom 8 is about 450 m a pixel, zoom 9 about 220 m, zoom 10 about 110 m (at this latitude).

Writes data/terrain/SLICE/:
  terrain.json   header: grid origin and cell size in sim miles, size, the projection used, the sources
  heights.i16    raw little-endian int16, metres, row-major from the SOUTH edge (row 0 = lowest y), cells x fastest
  hillshade.jpg  a shaded-relief picture of the same grid (north up, one pixel a cell), for the viewer and for looking at
Run it on untrusted downloads with `python -I` and keep the raw tiles outside the repo.
"""
import glob, json, math, os, re, sys
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
slice_name, raw = args[0], args[1]
opts = {sys.argv[i][2:]: sys.argv[i + 1] for i in range(1, len(sys.argv) - 1) if sys.argv[i].startswith('--')}
Z = int(opts.get('zoom', 10))
CELL = float(opts.get('cell', 0.25))   # miles per cell

# The same projection as tools/make_slice.py: miles east and north, anchored at Cheyenne's position on the full map.
places = json.load(open(os.path.join(ROOT, 'data', 'history', slice_name, 'places.json'), encoding='utf-8'))['places']
cheyenne = next(p for p in places if p['id'] == 'cheyenne')
main = json.loads(re.sub(r'^\s*//.*$', '', open(os.path.join(ROOT, 'data', 'world.json'), encoding='utf-8').read(), flags=re.M))
cx = next(n for n in main['nodes'] if n['id'] == 'cheyenne')
AX, AY, COS = cx['x'], cx['y'], math.cos(math.radians(41.5))


def project(lat, lon):
    return AX + (lon - cheyenne['lon']) * COS * 69.17, AY + (lat - cheyenne['lat']) * 69.05


def unproject(x, y):
    return cheyenne['lat'] + (y - AY) / 69.05, cheyenne['lon'] + (x - AX) / (COS * 69.17)


# Stitch the tiles into one mosaic and remember which tile and pixel the top left is.
tiles = {}
for f in sorted(glob.glob(os.path.join(raw, 'z%d_*_*.png' % Z))):
    m = re.search(r'z%d_(\d+)_(\d+)\.png$' % Z, f)
    tiles[(int(m.group(1)), int(m.group(2)))] = f
if not tiles:
    sys.exit('no z%d tiles in %s' % (Z, raw))
xs = [k[0] for k in tiles]; ys = [k[1] for k in tiles]
tx0, tx1, ty0, ty1 = min(xs), max(xs), min(ys), max(ys)
mosaic = np.zeros(((ty1 - ty0 + 1) * 256, (tx1 - tx0 + 1) * 256), dtype=np.float32)
for (tx, ty), f in tiles.items():
    a = np.asarray(Image.open(f).convert('RGB'), dtype=np.float32)
    mosaic[(ty - ty0) * 256:(ty - ty0 + 1) * 256, (tx - tx0) * 256:(tx - tx0 + 1) * 256] = a[:, :, 0] * 256 + a[:, :, 1] + a[:, :, 2] / 256 - 32768
N = 2 ** Z * 256   # pixels around the world at this zoom


def mosaic_px(lat, lon):
    lat, lon = np.asarray(lat, dtype=np.float64), np.asarray(lon, dtype=np.float64)
    px = (lon + 180) / 360 * N
    py = (1 - np.arcsinh(np.tan(np.radians(lat))) / np.pi) / 2 * N
    return px - tx0 * 256, py - ty0 * 256


def sample(lat, lon):
    """Bilinear, vectorised; NaN off the mosaic."""
    px, py = mosaic_px(lat, lon)
    x0, y0 = np.floor(px).astype(np.int64), np.floor(py).astype(np.int64)
    off = (x0 < 0) | (y0 < 0) | (x0 + 1 >= mosaic.shape[1]) | (y0 + 1 >= mosaic.shape[0])
    x0c, y0c = np.clip(x0, 0, mosaic.shape[1] - 2), np.clip(y0, 0, mosaic.shape[0] - 2)
    fx, fy = px - x0c, py - y0c
    v = (mosaic[y0c, x0c] * (1 - fx) + mosaic[y0c, x0c + 1] * fx) * (1 - fy) + (mosaic[y0c + 1, x0c] * (1 - fx) + mosaic[y0c + 1, x0c + 1] * fx) * fy
    return np.where(off, np.nan, v)


# Grid covering the tile mosaic, in sim miles, kept a margin inside it so every cell has data.
corners = [(math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (ty / 2 ** Z))))), tx / 2 ** Z * 360 - 180) for tx, ty in ((tx0, ty0), (tx1 + 1, ty1 + 1))]
(top_lat, left_lon), (bot_lat, right_lon) = corners
xmin, ymin = project(bot_lat, left_lon)
xmax, ymax = project(top_lat, right_lon)
xmin, ymin, xmax, ymax = math.ceil(xmin + 10), math.ceil(ymin + 10), math.floor(xmax - 10), math.floor(ymax - 10)
w, h = int((xmax - xmin) / CELL), int((ymax - ymin) / CELL)
gx = xmin + (np.arange(w) + 0.5) * CELL
gy = ymin + (np.arange(h) + 0.5) * CELL
X, Y = np.meshgrid(gx, gy)
LAT, LON = unproject(X, Y)
vals = sample(LAT, LON)
missing = int(np.isnan(vals).sum())
grid = np.rint(np.nan_to_num(vals, nan=0.0)).astype(np.int16)
# The tiles hold a few single-pixel spikes (a 6,000 m cell beside the plains, a pit below sea level). Replace any cell that differs
# from the median of its eight neighbours by more than SPIKE metres with that median.
SPIKE = 300
pad = np.pad(grid.astype(np.float32), 1, mode='edge')
around = np.stack([pad[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3) if (dy, dx) != (1, 1)])
med = np.median(around, axis=0)
spikes = np.abs(grid - med) > SPIKE
grid = np.where(spikes, np.rint(med), grid).astype(np.int16)
print('despiked %d cells (more than %d m from the median of their neighbours)' % (spikes.sum(), SPIKE))
print('zoom %d, grid %d x %d cells of %.2f miles, x %d..%d y %d..%d, %d cells off the mosaic' % (Z, w, h, CELL, xmin, xmax, ymin, ymax, missing))
print('elevation: min %d m, max %d m, mean %d m' % (grid.min(), grid.max(), grid.mean()))

out = os.path.join(ROOT, 'data', 'terrain', slice_name)
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, 'heights.i16'), 'wb') as f:
    f.write(grid.astype('<i2').tobytes())
pixel_m = 156543.03 * math.cos(math.radians(43)) / 2 ** Z
header = {
    '_note': 'GENERATED by tools/make_terrain.py. heights.i16 is raw little-endian int16 metres, row-major from the south edge (row 0 = lowest y).',
    'slice': slice_name, 'cell_miles': CELL, 'width': w, 'height': h, 'x_min': xmin, 'y_min': ymin,
    'projection': {'anchor_place': 'cheyenne', 'anchor_lat': cheyenne['lat'], 'anchor_lon': cheyenne['lon'], 'anchor_x': AX, 'anchor_y': AY,
                   'miles_per_degree_lat': 69.05, 'miles_per_degree_lon_at_41_5': round(COS * 69.17, 3)},
    'source': 'AWS Terrain Tiles, Terrarium encoding, zoom %d (about %d m a pixel); blended from USGS 3DEP, SRTM, GMTED2010, ETOPO1' % (Z, round(pixel_m)),
    'source_url': 'https://registry.opendata.aws/terrain-tiles/',
    'tiles': len(tiles),
}
json.dump(header, open(os.path.join(out, 'terrain.json'), 'w', encoding='utf-8'), indent=1)

# Hillshade (light from the northwest), north up, one pixel a cell.
g = grid.astype(np.float32)[::-1]
dy, dx = np.gradient(g, CELL * 1609.34)
slope = np.arctan(np.hypot(dx, dy) * 6.0)   # exaggerate relief so the plains read
aspect = np.arctan2(-dx, dy)
az, alt = math.radians(315), math.radians(40)
shade = np.clip(np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect), 0, 1)
# elevation ramp: river bottoms and plains green, then tan, brown, and pale rock above the timber line
stops = [(200, (0.36, 0.52, 0.30)), (700, (0.52, 0.60, 0.32)), (1200, (0.74, 0.70, 0.45)), (1700, (0.66, 0.52, 0.32)),
         (2300, (0.52, 0.40, 0.30)), (3000, (0.80, 0.78, 0.74)), (3900, (0.97, 0.97, 0.97))]
rgb = np.zeros(g.shape + (3,), dtype=np.float32)
for ch in range(3):
    rgb[:, :, ch] = np.interp(g, [e for e, _ in stops], [c[ch] for _, c in stops])
rgb = rgb * (0.30 + 0.85 * shade)[:, :, None]
Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(os.path.join(out, 'hillshade.jpg'), quality=88)   # a JPEG keeps it near 1 MB (the PNG was 5)

# Sanity check at the slice's towns against what the places should be (from memory, in metres).
expect = {'cheyenne': 1860, 'laramie': 2195, 'deadwood': 1380, 'omaha': 320, 'sidney': 1250, 'ogallala': 1010, 'rapid_city': 980,
          'custer_city': 1650, 'fort_laramie': 1260, 'casper': 1550, 'chadron': 1000, 'fort_robinson': 1250}
print('town elevations (model / rough expectation, m):')
for p in places:
    v = float(sample(np.array([p['lat']]), np.array([p['lon']]))[0])
    print('  %-14s %5s / %s' % (p['id'], 'n/a' if math.isnan(v) else int(round(v)), expect.get(p['id'], '?')))
