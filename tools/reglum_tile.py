#!/usr/bin/env python3
"""Contact sheet of REGLUM 64x36 game-frame thumbnails around given present indices (b-1, b, b+1 per row).
Usage: reglum_tile.py <reglum.txt> <out.png> <present_idx> [present_idx ...]"""
import sys, subprocess
src, out, idxs = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3:]]
want = set(); [want.update((i-1, i, i+1)) for i in idxs]
fr = {}
for line in open(src, errors='replace'):
    p = line.split('\t')
    if len(p) >= 4 and int(p[0]) in want: fr[int(p[0])] = bytes.fromhex(p[3].strip())
raw = b''.join(fr.get(j, bytes(64*36)) for i in idxs for j in (i-1, i, i+1))
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'gray', '-s', '64x36', '-i', '-',
                '-vf', f'scale=256:144:flags=neighbor,tile=3x{len(idxs)}', '-frames:v', '1', out], input=raw, check=True)
