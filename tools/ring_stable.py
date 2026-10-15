#!/usr/bin/env python3
"""For a pass-dump ring (8 frames), find meshes whose per-frame set of (object h64) is identical in every frame except B.
Usage: ring_stable.py <ring.txt> <frameB>
Mesh identity = (vs, fs, index buffer, vertex count); object = r1 h64 (first 64 bytes of the per-object cbuf = world matrix rows).
Prints meshes stable in all other frames but different in B (objects missing / added / moved in B)."""
import sys, re, collections
path, fb = sys.argv[1], int(sys.argv[2])
fr = collections.defaultdict(lambda: collections.defaultdict(collections.Counter)); cur = {}
draws = collections.Counter()
for line in open(path, errors='replace'):
    m = re.match(r'\[pass\] F(\d+) (\w) (\d+) ?(.*)', line)
    if not m: continue
    f, t, rest = int(m.group(1)), m.group(2), m.group(4)
    if t == 'd':
        kv = dict(x.split('=', 1) for x in rest.split() if '=' in x); cur[f] = (kv.get('vs'), kv.get('fs'), kv.get('ib'), kv.get('vc')); draws[f] += 1
    elif t == 'u' and f in cur:
        kv = dict(x.split('=', 1) for x in rest.split() if '=' in x)
        h = kv.get('r1', '-').split('/')[-1]
        fr[f][cur[f]][h] += 1
frames = sorted(fr); others = [f for f in frames if f != fb]
# drop the partially recorded first/last ring frames (draw count much lower than median)
med = sorted(draws[f] for f in frames)[len(frames)//2]
others = [f for f in others if draws[f] > 0.8 * med]
print('frames', frames, 'draws', [draws[f] for f in frames], 'B', fb, 'reference frames', others)
n = 0
for mesh in set().union(*[fr[f].keys() for f in frames]):
    if mesh[2] in (None, '0'): continue
    sets = [frozenset(fr[f][mesh]) for f in others]
    if len(set(sets)) != 1: continue        # not stable outside B
    ref = sets[0]; b = frozenset(fr[fb][mesh])
    if b != ref:
        n += 1; print(f'MESH vs={mesh[0]} fs={mesh[1]} ib={mesh[2]} vc={mesh[3]}: other frames {sorted(ref)} -> B {sorted(b)}  (missing {sorted(ref-b)}, new {sorted(b-ref)})')
print('meshes stable elsewhere but different in B:', n)
