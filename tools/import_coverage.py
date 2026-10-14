#!/usr/bin/env python3
"""Which imports of the game modules must the host provide, and which does d2r-mobile already implement?
Usage: import_coverage.py <d2r repo dir> <img dir with main.img sdk.img subsdk0.img>"""
import sys, os, re, struct, collections
repo, imgdir = sys.argv[1:3]
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
mods = {m: syms(os.path.join(imgdir, m + '.img')) for m in ('main', 'sdk', 'subsdk0')}
exports = {n for m in mods.values() for n, d in m.items() if d}
imports = {n for m in mods.values() for n, d in m.items() if not d}
unresolved = sorted(imports - exports)
# names d2r provides: HLE("..."), HLE_FORCE("..."), L(name) libc table
prov = set()
for root, _, files in os.walk(repo):
    for f in files:
        if not f.endswith(('.c', '.m', '.h', '.S')): continue
        s = open(os.path.join(root, f), encoding='utf-8', errors='replace').read()
        prov |= set(re.findall(r'HLE(?:_FORCE)?\(\s*"([^"]+)"', s))
        prov |= set(re.findall(r'\bL\((\w+)\)', s))
print(f'# exports {len(exports)} imports {len(imports)} unresolved-by-modules {len(unresolved)}; d2r provides names: {len(prov)}')
have = [n for n in unresolved if n in prov]; miss = [n for n in unresolved if n not in prov]
print(f'# covered by d2r host: {len(have)}   missing: {len(miss)}')
def ns(n):
    m = re.match(r'_ZN(?:K)?(\d+)([a-z]+)(\d+)([A-Za-z]+)', n)
    if m and m.group(2)[:int(m.group(1))] == 'nn': return 'nn::' + m.group(4)[:int(m.group(3))]
    m = re.match(r'_ZN(?:K)?(\d+)([A-Za-z]+)', n)
    if m: return m.group(2)[:int(m.group(1))]
    return 'C'
byns = collections.defaultdict(list)
for n in miss: byns[ns(n)].append(n)
for k in sorted(byns, key=lambda k: -len(byns[k])):
    print(f'\n## missing {k}: {len(byns[k])}')
    for n in byns[k][:400]: print('  ', n)
print('\n## also-defined-in-modules but d2r force-takes-over (HLE_FORCE hits):', len([n for n in prov if n in exports]))
