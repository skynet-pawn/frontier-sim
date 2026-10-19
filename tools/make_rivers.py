"""Rivers for a slice, from Natural Earth's river centrelines (public domain, https://www.naturalearthdata.com).

usage: python -I make_rivers.py SLICE NE_DIR            (NE_DIR holds the unzipped ne_10m_rivers_* shapefile folders)

Reads the polyline shapefiles directly (a small reader below: no GIS libraries needed), keeps the rivers that cross the slice's
terrain grid (data/terrain/SLICE/terrain.json), projects them into the sim's map miles with the same projection as
make_slice.py and make_terrain.py, and writes data/rivers/SLICE/rivers.json: for each river a name, a size class (1 a creek or
small river, 2 a river, 3 a great river) and its points.

Natural Earth rivers are generalised (positions good to a mile or two, small streams missing) and show the rivers as they are
today, not as the 1870s channel; reservoirs behind modern dams are not represented. Size comes from Natural Earth's own
`scalerank` (1 is the biggest; the lake-centreline file ranks 1-8, the North America supplement 10-11): 1-3 -> size 3, 4-8 -> size 2,
the rest size 1. A river listed in both files takes the larger size.
"""
import json, math, os, re, struct, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
slice_name, ne_dir = sys.argv[1], sys.argv[2]


def read_dbf(path):
    with open(path, 'rb') as f:
        data = f.read()
    n, hdr, rec = struct.unpack('<xxxxIHH', data[:12])
    fields, pos = [], 32
    while data[pos] != 0x0D:
        name = data[pos:pos + 11].split(b'\0')[0].decode('latin-1')
        flen = data[pos + 16]
        fields.append((name, flen))
        pos += 32
    rows = []
    for i in range(n):
        base = hdr + i * rec + 1
        row, off = {}, 0
        for name, flen in fields:
            row[name] = data[base + off:base + off + flen].decode('utf-8', 'replace').replace(chr(0), '').strip()   # fields are NUL-padded
            off += flen
        rows.append(row)
    return rows


def read_polylines(path):
    """Yields (record index, list of parts, each a list of (lon, lat))."""
    with open(path, 'rb') as f:
        data = f.read()
    pos, idx = 100, 0
    while pos + 8 <= len(data):
        _, clen = struct.unpack('>ii', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + clen * 2]
        pos += 8 + clen * 2
        shape_type = struct.unpack('<i', body[:4])[0]
        if shape_type in (3, 13, 23):
            nparts, npts = struct.unpack('<ii', body[36:44])
            parts = list(struct.unpack('<%di' % nparts, body[44:44 + 4 * nparts]))
            pts_at = 44 + 4 * nparts
            pts = struct.unpack('<%dd' % (2 * npts), body[pts_at:pts_at + 16 * npts])
            xy = [(pts[2 * i], pts[2 * i + 1]) for i in range(npts)]
            yield idx, [xy[a:b] for a, b in zip(parts, parts[1:] + [npts])]
        idx += 1


# the projection (as in make_terrain.py)
places = json.load(open(os.path.join(ROOT, 'data', 'history', slice_name, 'places.json'), encoding='utf-8'))['places']
cheyenne = next(p for p in places if p['id'] == 'cheyenne')
main = json.loads(re.sub(r'^\s*//.*$', '', open(os.path.join(ROOT, 'data', 'world.json'), encoding='utf-8').read(), flags=re.M))
cx = next(n for n in main['nodes'] if n['id'] == 'cheyenne')
AX, AY, COS = cx['x'], cx['y'], math.cos(math.radians(41.5))


def project(lon, lat):
    return AX + (lon - cheyenne['lon']) * COS * 69.17, AY + (lat - cheyenne['lat']) * 69.05


th = json.load(open(os.path.join(ROOT, 'data', 'terrain', slice_name, 'terrain.json'), encoding='utf-8'))
x0, y0 = th['x_min'], th['y_min']
x1, y1 = x0 + th['width'] * th['cell_miles'], y0 + th['height'] * th['cell_miles']


def size_of(scalerank):
    try:
        r = int(float(scalerank))
    except ValueError:
        r = 9
    return 3 if r <= 3 else 2 if r <= 8 else 1


rivers, seen = [], set()
for folder, base in (('lakes', 'ne_10m_rivers_lake_centerlines'), ('na', 'ne_10m_rivers_north_america')):
    d = os.path.join(ne_dir, folder)
    rows = read_dbf(os.path.join(d, base + '.dbf'))
    for idx, parts in read_polylines(os.path.join(d, base + '.shp')):
        row = rows[idx]
        if row.get('featurecla', 'River') not in ('River', 'Lake Centerline', ''):
            continue
        name = row.get('name', '') or row.get('NAME', '')
        for part in parts:
            pts = [project(lon, lat) for lon, lat in part]
            if not any(x0 <= x <= x1 and y0 <= y <= y1 for x, y in pts):
                continue
            # clip to the grid's extent, keep the pieces that stay inside
            piece = []
            for x, y in pts:
                if x0 <= x <= x1 and y0 <= y <= y1:
                    piece.append([round(x, 2), round(y, 2)])
                elif len(piece) > 1:
                    rivers.append({'name': name, 'size': size_of(row.get('scalerank', 9)), 'source': base, 'points': piece})
                    piece = []
                else:
                    piece = []
            if len(piece) > 1:
                rivers.append({'name': name, 'size': size_of(row.get('scalerank', 9)), 'source': base, 'points': piece})

# the two files overlap on the big rivers: keep the more detailed one (the North America supplement) where names match
by_name = {}
for r in rivers:
    by_name.setdefault(r['name'], []).append(r)
kept = []
for name, rs in by_name.items():
    sources = {r['source'] for r in rs}
    biggest = max(r['size'] for r in rs)
    pick = [r for r in rs if r['source'] == 'ne_10m_rivers_north_america'] if name and 'ne_10m_rivers_north_america' in sources else rs
    for r in pick:
        r['size'] = biggest
        kept.append(r)

out = os.path.join(ROOT, 'data', 'rivers', slice_name)
os.makedirs(out, exist_ok=True)
doc = {'_note': 'GENERATED by tools/make_rivers.py from Natural Earth river centrelines (public domain). Points are map miles; size 1 creek, 2 river, 3 great river.',
       'source': 'Natural Earth 1:10m rivers (ne_10m_rivers_lake_centerlines and ne_10m_rivers_north_america), naturalearthdata.com',
       'rivers': [{k: v for k, v in r.items() if k != 'source'} for r in kept]}
json.dump(doc, open(os.path.join(out, 'rivers.json'), 'w', encoding='utf-8'))
names = {}
for r in kept:
    length = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(r['points'], r['points'][1:]))
    names[(r['name'], r['size'])] = names.get((r['name'], r['size']), 0) + length
print('%d river pieces kept in the slice' % len(kept))
for (name, size), length in sorted(names.items(), key=lambda kv: -kv[1])[:40]:
    print('  size %d  %-28s %6.0f miles' % (size, name or '(unnamed)', length))
