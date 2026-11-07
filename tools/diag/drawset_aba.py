#!/usr/bin/env python3
"""Visibility A-B-A detector on DM_NVN_DRAWSET logs (one line per indexed draw: frame vskey ibuf count inst h64(cb1[0:64])).
An object = (vs key, index buffer, index count, instances, h64 world-matrix hash), h64 != 0.
  drop = object drawn in frames f-1 and f+1 but not in f;  pop = drawn in f but in neither f-1 nor f+1 while the same mesh (vs,ib,cnt)
  is drawn with another h64 in both f-1 and f+1 (i.e. an object of a persistent mesh appears/moves for one frame).
Usage: drawset_aba.py <drawset.txt> [--from F] [--to F] [--fps 60]"""
import sys, collections, argparse
ap = argparse.ArgumentParser(); ap.add_argument('f'); ap.add_argument('--from', dest='a', type=int, default=0); ap.add_argument('--to', dest='b', type=int, default=1 << 60)
ap.add_argument('--fps', type=float, default=60); ap.add_argument('--top', type=int, default=12)
a = ap.parse_args()
fr = collections.defaultdict(set)
for line in open(a.f, errors='replace'):
    p = line.split()
    if len(p) != 6 or p[5] == '0000000000000000': continue
    f = int(p[0])
    if a.a <= f <= a.b: fr[f].add((p[1], p[2], p[3], p[4], p[5]))
frames = sorted(fr)
byvs = collections.Counter(); drops = collections.Counter(); pops = collections.Counter(); perframe = {}; meshes = collections.Counter()
for f in frames:
    if f - 1 not in fr or f + 1 not in fr: continue
    A, B, C = fr[f - 1], fr[f], fr[f + 1]
    d = (A & C) - B
    def objs(S, m): return {k for k in S if k[:4] == m}
    pp = set()
    for k in B - A - C:
        oa, oc = objs(A, k[:4]), objs(C, k[:4])
        if oa and oa == oc: pp.add(k)   # mesh drawn with the identical object set before and after, but B has an extra / moved object
    if d or pp:
        perframe[f] = (len(d), len(pp)); drops[f] = len(d); pops[f] = len(pp)
        for k in d | pp: meshes[k[:4]] += 1; byvs[k[0]] += 1
nd, npop = sum(drops.values()), sum(pops.values())
print(f"[drawset] frames={len(frames)} drop_events={nd} pop_events={npop} frames_with_events={len(perframe)} frames_with>=3={sum(1 for v in perframe.values() if sum(v) >= 3)}")
hist = collections.Counter()
for f, (x, y) in perframe.items(): hist[int((f - frames[0]) / a.fps)] += x + y
print('  per-second (s since first frame: events):', ' '.join(f'{s}:{hist[s]}' for s in sorted(hist)))
print('  by vs key:', ' '.join(f'{k}:{v}' for k, v in byvs.most_common(12)))
print('  top meshes (vs, ib, count, inst): events')
for m, c in meshes.most_common(a.top): print('   ', m, c)
print('  per-frame (frame:drops/pops):', ' '.join(f'{f}:{x}/{y}' for f, (x, y) in sorted(perframe.items())))
