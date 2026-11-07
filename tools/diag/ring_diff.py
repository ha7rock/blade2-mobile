#!/usr/bin/env python3
"""Compare per-draw uniform fingerprints ('u' lines) of frames A, B, C in a pass-dump ring file.
Usage: ring_diff.py <ring.txt> <frameA> <frameB> <frameC> [--pass-agnostic]
Draw identity = (pass ordinal in frame, vs, fs, index buffer, vertex buffer, count, inst, occurrence).
Reports draws whose vertex-stage cbuf first-64-byte hash (h64, object world matrix rows) is equal in A and C
but different in B (uniform A-B-A), and draws present only in B / missing only in B."""
import sys, re, collections
path, fa, fb, fc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
agn = '--pass-agnostic' in sys.argv
fr = {f: [] for f in (fa, fb, fc)}
cur = {}
passord = {f: {} for f in fr}
for line in open(path, errors='replace'):
    m = re.match(r'\[pass\] F(\d+) (\w) (\d+) ?(.*)', line)
    if not m: continue
    f, t, p, rest = int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)
    if f not in fr: continue
    po = passord[f].setdefault(p, len(passord[f]))
    if t == 'd':
        kv = dict(x.split('=', 1) for x in rest.split() if '=' in x)
        cur[f] = {'pass': p, 'po': po, 'vs': kv.get('vs'), 'fs': kv.get('fs'), 'ib': kv.get('ib'), 'vc': kv.get('vc'), 'inst': kv.get('inst'), 'line': rest}
        fr[f].append(cur[f])
    elif t == 'u' and f in cur:
        cur[f]['u'] = {k: v for k, v in (x.split('=', 1) for x in rest.split() if '=' in x)}
    elif t == 'v' and f in cur:
        vb = re.findall(r'vb=(0x[0-9a-f]+)', rest); cur[f]['vb'] = vb[0] if vb else '-'
def keyed(lst):
    out = {}; occ = collections.Counter()
    for d in lst:
        k0 = (None if agn else d['po'], d['vs'], d['fs'], d['ib'], d.get('vb'), d['vc'], d['inst'])
        occ[k0] += 1; out[k0 + (occ[k0],)] = d
    return out
A, B, C = keyed(fr[fa]), keyed(fr[fb]), keyed(fr[fc])
print(f"draws: A={len(A)} B={len(B)} C={len(C)}; passes A={len(passord[fa])} B={len(passord[fb])} C={len(passord[fc])}")
aba = 0
for k in A:
    if k in B and k in C:
        ua, ub, uc = A[k].get('u', {}), B[k].get('u', {}), C[k].get('u', {})
        for r in ua:
            if r in ub and r in uc:
                ha, hb, hc = [x[r].split('/')[-1] for x in (ua, ub, uc)]
                if ha == hc and hb != ha:
                    aba += 1
                    print(f"UNIFORM-ABA pass#{k[0]} (B pass {B[k]['pass']}) {r} A/C={ha} B={hb} vs={k[1]} fs={k[2]} ib={k[3]} vc={k[5]} inst={k[6]} occ={k[7]}")
onlyB = [k for k in B if k not in A and k not in C]
missB = [k for k in A if k in C and k not in B]
print(f"uniform A-B-A draws: {aba}; draws only in B: {len(onlyB)}; draws in A and C but missing in B: {len(missB)}")
for k in onlyB[:30]: print('  onlyB', k, B[k].get('u'))
for k in missB[:30]: print('  missB', k, A[k].get('u'))

# ---- multiset view: (vs, fs, ib, vc, inst, h64 of each vertex cbuf) independent of pass order; static meshes keep ib, objects keep h64 (world matrix)
def ms(lst):
    c = collections.Counter()
    for d in lst:
        if d['ib'] in (None, '0', '0x0'): continue
        hs = tuple(sorted((r, v.split('/')[-1]) for r, v in d.get('u', {}).items() if r != 'r0'))
        c[(d['vs'], d['fs'], d['ib'], d['vc'], d['inst'], hs)] += 1
    return c
mA, mB, mC = ms(fr[fa]), ms(fr[fb]), ms(fr[fc])
newB = [k for k in mB if k not in mA and k not in mC]
gone = [k for k in mA if k in mC and k not in mB]
ibAC = {k[2] for k in mA} | {k[2] for k in mC}
print(f"\nmultiset: indexed draws A={sum(mA.values())} B={sum(mB.values())} C={sum(mC.values())}")
print(f"  (mesh,h64) in B but in neither A nor C: {len(newB)}  (of which mesh ib also drawn in A/C: {sum(1 for k in newB if k[2] in ibAC)})")
for k in newB[:40]: print('   NEW-IN-B', 'vs', k[0], 'fs', k[1], 'ib', k[2], 'vc', k[3], 'inst', k[4], 'h', k[5], 'x', mB[k])
print(f"  (mesh,h64) in both A and C but absent in B: {len(gone)}")
for k in gone[:40]: print('   GONE-IN-B', 'vs', k[0], 'fs', k[1], 'ib', k[2], 'vc', k[3], 'inst', k[4], 'h', k[5])
