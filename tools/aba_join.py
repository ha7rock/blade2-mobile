#!/usr/bin/env python3
"""Join game-frame (REGLUM) A-B-A frames with depth-prepass visibility A-B-A frames from DM_NVN_DRAWSET.
Usage: aba_join.py <run dir> [prepass vs key] ; frame f = present index - 1."""
import sys, gzip, collections, re
d = sys.argv[1]; pv = sys.argv[2] if len(sys.argv) > 2 else '499e06f881530aaa'
op = gzip.open if d and __import__('os').path.exists(d + '/drawset.txt.gz') else open
fn = d + ('/drawset.txt.gz' if op is gzip.open else '/drawset.txt')
fr = collections.defaultdict(set)
with op(fn, 'rt', errors='replace') as f:
    for line in f:
        p = line.split()
        if len(p) == 6 and p[1] == pv and p[5] != '0000000000000000': fr[int(p[0])].add((p[2], p[3], p[4], p[5]))
vis = {}
for f in fr:
    if f - 1 in fr and f + 1 in fr:
        A, B, C = fr[f - 1], fr[f], fr[f + 1]
        n = len((A & C) - B) + sum(1 for k in B - A - C if {x for x in A if x[:3] == k[:3]} == {x for x in C if x[:3] == k[:3]} and any(x[:3] == k[:3] for x in A))
        if n: vis[f] = n
# reglum A-B-A frames (recompute from detector output line 2: 'present:blocks')
ev = {}
for tok in open(d + '/reglum_aba.txt').read().split('\n')[1].split()[1:]:
    p, b = tok.split(':'); ev[int(p) - 1] = int(b)
fight = [f for f in ev if f >= 3000]
big = [f for f in fight if ev[f] >= 3]
hit = [f for f in big if f in vis]
print(f"{d.rstrip('/').split('/')[-1]}: prepass-vis-ABA frames(f>=3000)={sum(1 for f in vis if f >= 3000)} events={sum(n for f, n in vis.items() if f >= 3000)}; "
      f"image ABA frames>=3blk(f>=3000)={len(big)}, of which coincide with a prepass vis-ABA frame={len(hit)}; image block events in those={sum(ev[f] for f in hit)}")
