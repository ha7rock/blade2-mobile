#!/usr/bin/env python3
"""Find every `br x18` in an NSO image's text and classify the jump-table idiom.
Usage: br18_sites.py <image.img> <text_end_hex>"""
import sys, struct
from capstone import *
img = open(sys.argv[1], 'rb').read(); text_end = int(sys.argv[2], 16)
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
sites = [o for o in range(0, text_end, 4) if struct.unpack_from('<I', img, o)[0] == 0xD61F0240]  # br x18
print('br x18 sites:', len(sites))
kinds = {}
for at in sites:
    lo = at - 4 * 14
    ctx = list(md.disasm(img[lo:at], lo))
    text = ' | '.join(f'{i.mnemonic} {i.op_str}' for i in ctx[-7:])
    if any(i.mnemonic == 'adr' and i.op_str.startswith('x18,') for i in ctx): k = 'adr-byte-table(d2r)'
    elif any(i.mnemonic == 'ldrsw' and i.op_str.startswith('x18,') for i in ctx): k = 'ldrsw-offset-table'
    else: k = 'other'
    kinds.setdefault(k, []).append(at)
    if k != 'adr-byte-table(d2r)': print(f'{at:#x} [{k}]  {text}')
for k, v in kinds.items(): print(k, len(v))
