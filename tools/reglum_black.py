#!/usr/bin/env python3
"""Count one-frame scene-wide dropouts in REGLUM game-frame thumbnails: central region (cols 8-55, rows 4-31 of 64x36, avoiding most UI)
mean of frame B < 0.35 x min(A, C) with A, C > 10 ('black'), or B > 1.8 x max(A, C) with B > 60 ('bright').
Usage: reglum_black.py <reglum.txt>..."""
import sys
for fn in sys.argv[1:]:
    m, idx = [], []
    for l in open(fn, errors='replace'):
        p = l.split('\t')
        if len(p) < 4 or len(p[3].strip()) != 4608: continue
        g = bytes.fromhex(p[3].strip()); s = 0
        for y in range(4, 32): s += sum(g[y*64+8:y*64+56])
        m.append(s / (28*48)); idx.append(int(p[0]))
    blk = [idx[i] for i in range(1, len(m)-1) if m[i-1] > 10 and m[i+1] > 10 and m[i] < 0.35*min(m[i-1], m[i+1])]
    brt = [idx[i] for i in range(1, len(m)-1) if m[i] > 60 and m[i] > 1.8*max(m[i-1], m[i+1])]
    print(f"{fn}: frames={len(m)} black_ABA={len(blk)} bright_ABA={len(brt)} black_at={blk[:30]} bright_at={brt[:15]}")
