#!/usr/bin/env python3
"""A-B-A one-frame glitch detector (accepted baseline definition; do not change).

Screen recording -> window crop -> 128x70 gray -> blocks -> event when, for frames a,b,c
(consecutive), a block's mean satisfies |b-a|>20 and |c-b|>20 with opposite signs and |c-a|<8.

Usage:
  aba_detect.py <mov> <crop w:h:x:y> [--fps 60] [--raw out.raw]
  aba_detect.py --raw-in <file.raw> [--w 128 --h 70]
  aba_detect.py --reglum <reglum.txt>     (64x36 game-frame thumbnails, 16x9 blocks of 4x4)
Block grid for the screen path: --bx 16 --by 10 (block counts) by default.
"""
import sys, subprocess, argparse, collections

def blocks_of(frame, W, H, BX, BY):
    bw, bh = W // BX, H // BY
    out = []
    for by in range(BY):
        rows = [frame[(by*bh + r)*W:(by*bh + r)*W + W] for r in range(bh)]
        for bx in range(BX):
            s = 0
            for row in rows:
                s += sum(row[bx*bw:(bx+1)*bw])
            out.append(s / (bw*bh))
    return out

def detect(blocks, BX, minblocks=1):
    ev = []  # (frame index of b, list of block idx)
    for i in range(1, len(blocks)-1):
        a, b, c = blocks[i-1], blocks[i], blocks[i+1]
        hit = []
        for k in range(len(b)):
            d1 = b[k]-a[k]; d2 = c[k]-b[k]
            if abs(d1) > 20 and abs(d2) > 20 and (d1 > 0) != (d2 > 0) and abs(c[k]-a[k]) < 8:
                hit.append(k)
        if len(hit) >= minblocks:
            ev.append((i, hit))
    return ev

def report(ev, n, BX, fps, label, times=None, idx=None, toff=0.0):
    tot = sum(len(h) for _, h in ev)
    big = {k: sum(1 for _, h in ev if len(h) >= k) for k in (3, 10, 20)}
    print(f"[{label}] frames={n} event_frames={len(ev)} block_events={tot} frames>=3blk={big[3]} >=10blk={big[10]} >=20blk={big[20]}")
    pos = collections.Counter()
    for _, h in ev:
        for k in h: pos[(k % BX, k // BX)] += 1
    print("  per-frame:", " ".join(f"{(idx[i] if idx else i)}:{len(h)}" for i, h in ev))
    print("  block positions (x,y):count top:", ", ".join(f"{p}:{c}" for p, c in pos.most_common(20)))
    hist = collections.Counter()
    for i, h in ev:
        t = (times[i] if times else i / fps + toff)
        hist[int(t)] += len(h)
    print("  per-second histogram (s:block_events):", " ".join(f"{s}:{hist[s]}" for s in sorted(hist)))
    return tot

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mov', nargs='?'); ap.add_argument('crop', nargs='?')
    ap.add_argument('--fps', type=float, default=60); ap.add_argument('--raw')
    ap.add_argument('--raw-in'); ap.add_argument('--w', type=int, default=128); ap.add_argument('--h', type=int, default=70)
    ap.add_argument('--bx', type=int, default=16); ap.add_argument('--by', type=int, default=10)
    ap.add_argument('--reglum'); ap.add_argument('--minblocks', type=int, default=1)
    ap.add_argument('--toff', type=float, default=0.0, help='screen: seconds to add to frame times (recording start since launch)')
    ap.add_argument('--t0', type=int, help='reglum: time origin in ns (CLOCK_UPTIME_RAW, e.g. app launch)')
    a = ap.parse_args()
    if a.reglum:
        W, H, BX, BY = 64, 36, 16, 9
        frames, times, idx = [], [], []
        t0 = a.t0
        for line in open(a.reglum, errors='replace'):
            p = line.split()
            # format: present_index time_ns mean hex
            if len(p) < 4 or len(p[-1]) != W*H*2: continue
            try: fr = bytes.fromhex(p[-1])
            except ValueError: continue
            idx.append(int(p[0])); t = int(p[1]); t0 = t if t0 is None else t0
            times.append((t - t0) / 1e9); frames.append(fr)
        bl = [blocks_of(f, W, H, BX, BY) for f in frames]
        ev = detect(bl, BX, a.minblocks)
        report(ev, len(frames), BX, 60, 'reglum', times, idx)
        return
    W, H = a.w, a.h
    if a.raw_in:
        data = open(a.raw_in, 'rb').read()
    else:
        cmd = ['ffmpeg', '-v', 'error', '-i', a.mov, '-vf', f'crop={a.crop},scale={W}:{H},format=gray',
               '-r', str(a.fps), '-f', 'rawvideo', '-']
        data = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
        if a.raw: open(a.raw, 'wb').write(data)
    fs = W*H; n = len(data) // fs
    bl = [blocks_of(data[i*fs:(i+1)*fs], W, H, a.bx, a.by) for i in range(n)]
    ev = detect(bl, a.bx, a.minblocks)
    report(ev, n, a.bx, a.fps, 'screen', toff=a.toff)

if __name__ == '__main__':
    main()
