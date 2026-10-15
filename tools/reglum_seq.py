#!/usr/bin/env python3
"""Sequence strip of REGLUM 64x36 thumbnails: reglum_seq.py <reglum.txt> <out.png> <first_present> <count> [cols]"""
import sys, subprocess
src, out, a, n = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]); cols = int(sys.argv[5]) if len(sys.argv) > 5 else 6
fr = {}
for line in open(src, errors='replace'):
    p = line.split('\t')
    if len(p) >= 4 and a <= int(p[0]) < a + n: fr[int(p[0])] = bytes.fromhex(p[3].strip())
raw = b''.join(fr.get(j, bytes(64*36)) for j in range(a, a + n))
rows = (n + cols - 1) // cols
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'gray', '-s', '64x36', '-i', '-',
                '-vf', f'scale=192:108:flags=neighbor,tile={cols}x{rows}', '-frames:v', '1', out], input=raw, check=True)
