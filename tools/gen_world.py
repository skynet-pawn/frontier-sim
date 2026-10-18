"""Scale test world: the real 30 nodes plus synthetic farm and ranch towns, to N nodes in all.
usage: gen_world.py N OUTDIR      (scenario trader counts are scaled by N/30)"""
import json, math, os, random, re, shutil, sys
N = int(sys.argv[1]); out = sys.argv[2]
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data') + '/'
rng = random.Random(1234)
world = json.loads(re.sub(r'^\s*//.*$', '', open(src + 'world.json').read(), flags=re.M))
nodes = world['nodes']
real = [n for n in nodes if 'x' in n]
xs = [n['x'] for n in real]; ys = [n['y'] for n in real]
x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
def climate(x, y):
    if y < 650: return 'texas'
    if y < 950: return 'central_plains'
    if x < -500 and y > 1300: return 'northern_rockies'
    return 'northern_plains'
pos = [(n['x'], n['y']) for n in real]
from_region = lambda x, y: min(real, key=lambda n: math.hypot(n['x'] - x, n['y'] - y)).get('region', '')
k = 0
while len(nodes) < N:
    x = rng.uniform(x0, x1); y = rng.uniform(y0, y1)
    if min(math.hypot(x - a, y - b) for a, b in pos) < 12: continue
    k += 1
    reg = from_region(x, y)
    pop = rng.choice([60, 100, 200, 400])
    if rng.random() < 0.7:
        nodes.append({'id': 'syn_farm_%d' % k, 'name': 'Farm Town %d' % k, 'region': reg, 'kind': 'farm town', 'x': round(x), 'y': round(y),
                      'base_population': pop // 5, 'farmers': pop // 2, 'farm_capacity': rng.choice([1500, 2500, 4000]),
                      'farm_yield': round(rng.uniform(0.6, 1.0), 2), 'climate': climate(x, y), 'population': pop, 'capacity': 4000,
                      'cash': 1500, 'transient_inflow': True,
                      'stock': {'beef_cattle': 30, 'flour': pop, 'tools': 15, 'whiskey': 40, 'ammunition': 20}})
    else:
        nodes.append({'id': 'syn_ranch_%d' % k, 'name': 'Ranch Town %d' % k, 'region': reg, 'kind': 'ranch town', 'x': round(x), 'y': round(y),
                      'base_population': pop // 5, 'population': pop, 'capacity': 4000, 'cash': 1000, 'transient_inflow': True,
                      'stock': {'beef_cattle': 100, 'flour': pop // 2, 'whiskey': 30, 'ammunition': 20},
                      'production': {'beef_cattle': {'rate': rng.choice([10, 20, 40]), 'labor_pop': 25}}})
    pos.append((x, y))
# each synthetic node links to its three nearest neighbours (any node placed before it, so the graph stays connected)
ids = [n['id'] for n in nodes]
for i in range(len(real), len(nodes)):
    d = sorted(range(i), key=lambda j: math.hypot(pos[i][0] - pos[j][0], pos[i][1] - pos[j][1]))[:3]
    for j in d:
        world['edges'].append({'a': nodes[i]['id'], 'b': nodes[j]['id']})
os.makedirs(out, exist_ok=True)
shutil.copy(src + 'goods.json', out + '/goods.json')
json.dump(world, open(out + '/world.json', 'w'))
sc = json.load(open(src + 'scenario.json'))
f = N / 30.0
sc['traders'] = int(sc['traders'] * f); sc['max_traders'] = int(sc['max_traders'] * f)
json.dump(sc, open(out + '/scenario.json', 'w'), indent=1)
print('nodes', len(nodes), 'edges', len(world['edges']), 'traders', sc['traders'], 'max', sc['max_traders'])
