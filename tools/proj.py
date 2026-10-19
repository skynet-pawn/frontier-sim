"""Map projections shared by make_slice.py, make_terrain.py and make_rivers.py (the sim only ever sees map miles).

A slice has either
  - data/history/SLICE/projection.json  {"type": "albers", "lat1": 29.5, "lat2": 45.5, "lat0": 37.5, "lon0": -98}
    (Albers equal-area conic: right distances and areas across a whole region; the standard for the lower 48), or
  - nothing: the default, a flat map anchored at Cheyenne (miles east and north of Cheyenne's place on the original hand-made map,
    69.05 miles a degree of latitude, 69.17 x cos(41.5 degrees) a degree of longitude), good for a slice a few hundred miles across.

Both work on numbers or numpy arrays.
"""
import json, math, os, re
import numpy as np

EARTH_MILES = 3958.8


class Flat:
    def __init__(self, anchor_lat, anchor_lon, ax, ay):
        self.anchor_lat, self.anchor_lon, self.ax, self.ay = anchor_lat, anchor_lon, ax, ay
        self.cos = math.cos(math.radians(41.5))
        self.description = {'type': 'flat', 'anchor_lat': anchor_lat, 'anchor_lon': anchor_lon, 'anchor_x': ax, 'anchor_y': ay,
                            'miles_per_degree_lat': 69.05, 'miles_per_degree_lon_at_41_5': round(self.cos * 69.17, 3)}

    def project(self, lat, lon):
        lat, lon = np.asarray(lat, dtype=np.float64), np.asarray(lon, dtype=np.float64)
        return self.ax + (lon - self.anchor_lon) * self.cos * 69.17, self.ay + (lat - self.anchor_lat) * 69.05

    def unproject(self, x, y):
        x, y = np.asarray(x, dtype=np.float64), np.asarray(y, dtype=np.float64)
        return self.anchor_lat + (y - self.ay) / 69.05, self.anchor_lon + (x - self.ax) / (self.cos * 69.17)


class Albers:
    def __init__(self, lat1, lat2, lat0, lon0):
        p1, p2, p0 = math.radians(lat1), math.radians(lat2), math.radians(lat0)
        self.n = (math.sin(p1) + math.sin(p2)) / 2
        self.c = math.cos(p1) ** 2 + 2 * self.n * math.sin(p1)
        self.rho0 = math.sqrt(self.c - 2 * self.n * math.sin(p0)) / self.n
        self.lon0 = lon0
        self.description = {'type': 'albers', 'lat1': lat1, 'lat2': lat2, 'lat0': lat0, 'lon0': lon0, 'earth_radius_miles': EARTH_MILES}

    def project(self, lat, lon):
        lat, lon = np.radians(np.asarray(lat, dtype=np.float64)), np.asarray(lon, dtype=np.float64)
        rho = np.sqrt(self.c - 2 * self.n * np.sin(lat)) / self.n
        theta = self.n * np.radians(lon - self.lon0)
        return EARTH_MILES * rho * np.sin(theta), EARTH_MILES * (self.rho0 - rho * np.cos(theta))

    def unproject(self, x, y):
        x, y = np.asarray(x, dtype=np.float64) / EARTH_MILES, np.asarray(y, dtype=np.float64) / EARTH_MILES
        dy = self.rho0 - y
        rho = np.sqrt(x * x + dy * dy)
        theta = np.arctan2(x, dy)
        lat = np.degrees(np.arcsin((self.c - (rho * self.n) ** 2) / (2 * self.n)))
        return lat, self.lon0 + np.degrees(theta) / self.n


def load(root, slice_name):
    path = os.path.join(root, 'data', 'history', slice_name, 'projection.json')
    if os.path.exists(path):
        j = json.load(open(path, encoding='utf-8'))
        if j.get('type') == 'albers':
            return Albers(j['lat1'], j['lat2'], j['lat0'], j['lon0'])
        raise SystemExit('unknown projection type in %s' % path)
    places = json.load(open(os.path.join(root, 'data', 'history', slice_name, 'places.json'), encoding='utf-8'))['places']
    cheyenne = next(p for p in places if p['id'] == 'cheyenne')
    main = json.loads(re.sub(r'^\s*//.*$', '', open(os.path.join(root, 'data', 'world.json'), encoding='utf-8').read(), flags=re.M))
    cx = next(n for n in main['nodes'] if n['id'] == 'cheyenne')
    return Flat(cheyenne['lat'], cheyenne['lon'], cx['x'], cx['y'])
