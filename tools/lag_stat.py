#!/usr/bin/env python3
"""Summarize a DM_NVN_LAGLOG run dir (tools/aba_run.sh with ABA_LAG=1): GPU lag at present (submits / unfinished presents),
fps and present-interval p99 from REGLUM timestamps, centre black frames (same rule as reglum_black.py) and, for each black
frame, the S/A/P lines of the frames around it.  Usage: lag_stat.py <run_dir> [--from 3000] [--detail]"""
import sys, os
def pct(v, p):
    if not v: return float('nan')
    v = sorted(v); return v[min(len(v)-1, int(p/100*len(v)))]
d = sys.argv[1]; F0 = 3000; detail = '--detail' in sys.argv
if '--from' in sys.argv: F0 = int(sys.argv[sys.argv.index('--from')+1])
# reglum
m, idx, ts = [], [], []
for l in open(os.path.join(d, 'reglum.txt'), errors='replace'):
    p = l.split('\t')
    if len(p) < 4 or len(p[3].strip()) != 4608: continue
    g = bytes.fromhex(p[3].strip()); s = 0
    for y in range(4, 32): s += sum(g[y*64+8:y*64+56])
    m.append(s/(28*48)); idx.append(int(p[0])); ts.append(int(p[1]))
# another BladeII instance overlapping this run (overlap.txt: "<epoch> other pid N") contaminates timing: keep only frames presented before it started
cut = None; ovf = os.path.join(d, 'overlap.txt'); lw = os.path.join(d, 't_launch_wall.txt')
if os.path.exists(ovf) and os.path.getsize(ovf) and os.path.exists(lw):
    t_ov = min(int(l.split()[0]) for l in open(ovf) if l.split() and l.split()[0].isdigit())
    t_l = int(open(lw).read()); t_lu = int(open(os.path.join(d, 't_launch.txt')).read())
    keep = [i for i in range(len(idx)) if t_l + (ts[i] - t_lu)/1e9 < t_ov - 1]
    cut = idx[keep[-1]] if keep else 0
    m, idx, ts = [m[i] for i in keep], [idx[i] for i in keep], [ts[i] for i in keep]
blk = [idx[i] for i in range(1, len(m)-1) if m[i-1] > 10 and m[i+1] > 10 and m[i] < 0.35*min(m[i-1], m[i+1])]
sel = [i for i in range(len(idx)) if idx[i] >= F0]
fps = (len(sel)-1)/((ts[sel[-1]]-ts[sel[0]])/1e9) if len(sel) > 1 else float('nan')
iv = [(ts[sel[k+1]]-ts[sel[k]])/1e6 for k in range(len(sel)-1)]
# lag
P, S, A, B, O, M = {}, {}, [], [], 0, 0
lf = os.path.join(d, 'lag.txt')
if os.path.exists(lf):
    for l in open(lf, errors='replace'):
        p = l.rstrip('\n').split('\t')
        if p[0] == 'P': P[int(p[1])] = (int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6]))
        elif p[0] == 'S': S.setdefault(int(p[1]), []).append(p[1:])
        elif p[0] == 'A': A.append(p[1:])
        elif p[0] == 'B': B.append(int(p[1]))
        elif p[0] == 'O': O += 1
        elif p[0] == 'M': M += 1
last = idx[-1] if idx else 0
ls = [v[3] for k, v in P.items() if F0 <= k <= last]; lp = [v[4] for k, v in P.items() if F0 <= k <= last]
ov = os.path.getsize(os.path.join(d, 'overlap.txt')) if os.path.exists(os.path.join(d, 'overlap.txt')) else 0
print(f"{os.path.basename(d.rstrip('/'))}: frames={len(m)} black={len(blk)} fps(>={F0})={fps:.1f} interval_p99={pct(iv,99):.1f}ms "
      f"lag_submits max={max(ls) if ls else 'na'} p99={pct(ls,99)} p50={pct(ls,50)} | unfinished_presents max={max(lp) if lp else 'na'} p99={pct(lp,99)} p50={pct(lp,50)} "
      f"| black_lag_submits={[P[b][3] if b in P else None for b in blk]} black_unfinished={[P[b][4] if b in P else None for b in blk]} ooo={O} mismatch={M} trig={len(B)} overlap={'no' if cut is None else f'YES(clean until present {cut})'}")
print(f"  black_at={blk[:40]}")
if detail and P:
    hdr = "frame fence done_begin done_commit alloc prevfence new arena_reuse arena_new arena_minmargin defer drained ub ub_bytes b2b upflush upblits reup"
    for b in blk[:12]:
        print(f"== black present {b}: centre A/B/C = " + "/".join(f"{m[idx.index(k)]:.1f}" for k in (b-1, b, b+1) if k in idx))
        for k in range(b-3, b+2):
            if k not in P: continue
            fr = P[k][0]
            print(f"  P{k}{'*' if k == b else ' '} frame={fr} fence={P[k][1]} done={P[k][2]} lag={P[k][3]} unfinished={P[k][4]}")
            for s in S.get(fr, []): print("     S " + " ".join(s[1:]))
            for a in A:
                if int(a[0]) == fr: print("     A " + " ".join(a))
    print("  S columns: " + hdr)
