"""Turns downloaded elevation tiles into a heightfield in the sim's own map coordinates.

usage: python -I make_terrain.py SLICE RAWDIR [--zoom 10] [--cell 0.25] [--margin MILES]      (e.g. black_hills ../scratch/terrain_raw10)

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
MARGIN = float(opts['margin']) if 'margin' in opts else None   # trim the grid to the places' extent plus this many miles

# The slice's projection (tools/proj.py), the same one make_slice.py and make_rivers.py use.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proj
PROJ = proj.load(ROOT, slice_name)
places = json.load(open(os.path.join(ROOT, 'data', 'history', slice_name, 'places.json'), encoding='utf-8'))['places']


def project(lat, lon):
    return PROJ.project(lat, lon)


def unproject(x, y):
    return PROJ.unproject(x, y)


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
if MARGIN is not None:   # keep the part of the tile rectangle that holds the places, not the empty corners of the projection
    pxs, pys = zip(*[(float(project(p['lat'], p['lon'])[0]), float(project(p['lat'], p['lon'])[1])) for p in places])
    xmin, xmax = max(xmin, math.floor(min(pxs) - MARGIN)), min(xmax, math.ceil(max(pxs) + MARGIN))
    ymin, ymax = max(ymin, math.floor(min(pys) - MARGIN)), min(ymax, math.ceil(max(pys) + MARGIN))
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
SPIKE = 700   # the glitches seen were 800 to 4,800 m off; real mountainsides at half-mile spacing can differ by a few hundred
pad = np.pad(grid.astype(np.float32), 1, mode='edge')
around = np.stack([pad[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3) if (dy, dx) != (1, 1)])
med = np.median(around, axis=0)
spikes = np.abs(grid - med) > SPIKE
grid = np.where(spikes, np.rint(med), grid).astype(np.int16)
print('despiked %d cells (more than %d m from the median of their neighbours)' % (spikes.sum(), SPIKE))
# Below sea level is the Gulf floor and a few pits: for routing the sea is flat, so clamp it to 0.
grid = np.maximum(grid, 0).astype(np.int16)
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
    'projection': PROJ.description,
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
          'custer_city': 1650, 'fort_laramie': 1260, 'casper': 1550, 'chadron': 1000, 'fort_robinson': 1250,
          'kansas_city': 270, 'st_paul': 215, 'galveston': 2, 'san_antonio': 200, 'austin': 150, 'fort_worth': 200, 'dodge_city': 790,
          'wichita': 400, 'helena': 1240, 'bozeman': 1450, 'miles_city': 720, 'fort_benton': 790, 'abilene': 350, 'caldwell': 350,
          'ellsworth': 500, 'great_bend': 560, 'garden_city': 880, 'gainesville': 220, 'fort_sill': 360, 'fort_griffin': 400}
print('town elevations (model / rough expectation, m):')
for p in places:
    v = float(sample(np.array([p['lat']]), np.array([p['lon']]))[0])
    print('  %-14s %5s / %s' % (p['id'], 'n/a' if math.isnan(v) else int(round(v)), expect.get(p['id'], '?')))
