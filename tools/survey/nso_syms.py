#!/usr/bin/env python3
"""List dynamic symbols of a decompressed NSO image (MOD0 → DT_SYMTAB/DT_STRTAB).
Usage: nso_syms.py <image.img> [imports|exports|all] [grep]
Prints: name, defined(1/0), value. Summary by namespace for imports."""
import struct, sys, re, collections
img = open(sys.argv[1], 'rb').read()
mode = sys.argv[2] if len(sys.argv) > 2 else 'imports'
pat = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
mod0 = struct.unpack_from('<I', img, 4)[0]
assert img[mod0:mod0+4] == b'MOD0', 'no MOD0'
dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
symtab = strtab = None; relasz = pltsz = rela = jmprel = 0
off = dyn
while True:
    tag, val = struct.unpack_from('<qQ', img, off); off += 16
    if tag == 0: break
    if tag == 6: symtab = val
    elif tag == 5: strtab = val
    elif tag == 7: rela = val
    elif tag == 8: relasz = val
    elif tag == 23: jmprel = val
    elif tag == 2: pltsz = val
nsym = (strtab - symtab) // 24
def cstr(o):
    e = img.index(b'\0', o); return img[o:e].decode('latin1')
syms = []
for i in range(1, nsym):
    name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, symtab + i*24)
    syms.append((cstr(strtab + name), shndx != 0, value, info & 0xf, info >> 4))
imports = [s for s in syms if not s[1]]
exports = [s for s in syms if s[1] and s[4] in (1, 2)]
print(f'# symbols {len(syms)}  imports {len(imports)}  exports(global/weak) {len(exports)}  rela {relasz//24} jmprel {pltsz//24}')
sel = imports if mode == 'imports' else exports if mode == 'exports' else syms
if mode == 'imports':
    ns = collections.Counter()
    for n, *_ in imports:
        m = re.match(r'_ZN(?:K)?(\d+)([a-z]+)', n)
        if m:
            top = m.group(2)[:int(m.group(1))]
            m2 = re.match(r'_ZN(?:K)?\d+[a-z]+(\d+)([A-Za-z]+)', n)
            sub = m2.group(2)[:int(m2.group(1))] if m2 and top == 'nn' else ''
            ns[top + ('::' + sub if sub else '')] += 1
        else:
            ns['<C: ' + re.sub(r'[0-9_].*', '', n)[:6] + '...>' if n.startswith(('nn', 'nv')) else '<C>'] += 1
    for k, v in ns.most_common(60): print(f'{v:6d}  {k}')
for n, d, v, t, b in sel:
    if pat and not pat.search(n): continue
    if mode != 'imports' or pat: print(f'{n}\t{int(d)}\t{v:#x}\t{t}')
