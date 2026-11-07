#!/usr/bin/env python3
"""For every symbol main.img imports (its API surface into sdk/subsdk0/host), say whether d2r-mobile's host
already registers it (HLE / HLE_FORCE / libc L()). Usage: main_imports_vs_d2r.py <d2r dm dir> <main.img> [sdk.img] [subsdk0.img]"""
import sys, os, re, struct, collections
repo, mainimg = sys.argv[1:3]; others = sys.argv[3:]
def syms(path):
    img = open(path, 'rb').read()
    mod0 = struct.unpack_from('<I', img, 4)[0]; dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    symtab = strtab = None; off = dyn
    while True:
        tag, val = struct.unpack_from('<qQ', img, off); off += 16
        if tag == 0: break
        if tag == 6: symtab = val
        if tag == 5: strtab = val
    out = {}
    for i in range(1, (strtab - symtab) // 24):
        name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, symtab + i*24)
        e = img.index(b'\0', strtab + name); out[img[strtab+name:e].decode('latin1')] = shndx != 0
    return out
main = syms(mainimg)
defined_by = {}
for p in others:
    for n, d in syms(p).items():
        if d: defined_by.setdefault(n, os.path.basename(p).split('.')[0])
prov = {}
for root, _, files in os.walk(repo):
    for f in files:
        if not f.endswith(('.c', '.m', '.h', '.S')): continue
        s = open(os.path.join(root, f), encoding='utf-8', errors='replace').read()
        for n in re.findall(r'HLE(?:_FORCE)?\(\s*"([^"]+)"', s): prov.setdefault(n, os.path.relpath(os.path.join(root, f), repo))
        for n in re.findall(r'\bL\((\w+)\)', s): prov.setdefault(n, 'hle_table.c')
def ns(n):
    m = re.match(r'_ZN(?:K)?(\d+)([a-z]+)(\d+)([A-Za-z]+)', n)
    if m and m.group(2)[:int(m.group(1))] == 'nn': return 'nn::' + m.group(4)[:int(m.group(3))]
    m = re.match(r'_ZN(?:K)?(\d+)([A-Za-z]+)', n)
    if m: return m.group(2)[:int(m.group(1))]
    m = re.match(r'(nn[a-z]+|nvn)', n)
    return 'C:' + m.group(1) if m else 'C'
imports = sorted(n for n, d in main.items() if not d)
byns = collections.defaultdict(list)
for n in imports: byns[ns(n)].append(n)
tot = covered = 0
print('namespace\ttotal\tin_d2r_host\tnot_in_d2r')
rows = []
for k in sorted(byns, key=lambda k: -len(byns[k])):
    c = sum(1 for n in byns[k] if n in prov); rows.append((k, len(byns[k]), c)); tot += len(byns[k]); covered += c
    print(f'{k}\t{len(byns[k])}\t{c}\t{len(byns[k]) - c}')
print(f'TOTAL\t{tot}\t{covered}\t{tot - covered}\n')
for k in sorted(byns, key=lambda k: -len(byns[k])):
    print(f'## {k}')
    for n in byns[k]:
        print(f'  {"HOST:" + prov[n] if n in prov else "sdk:" + defined_by.get(n, "?")}\t{n}')
