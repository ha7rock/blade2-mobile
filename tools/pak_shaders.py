#!/usr/bin/env python3
# 从 UE4 pak 里离线抽出全部 NVN 着色器 + 推出翻译所需事实，不用靠运行时自录（Blade II：着色器 zlib 压缩后内联在材质 uasset / GlobalShaderCache 里）。
# Offline extraction of every NVN shader from the UE4 paks + derivation of the facts the translator needs, instead of relying on
# runtime self-recording (Blade II: shaders are zlib-compressed and inlined in material uassets / GlobalShaderCache-GLSL_SWITCH.bin).
#
# 用法 | usage:
#   pak_shaders.py extract <blob dir> <pak>...        pak（v4，zlib 分块）→ 每个资源里的 zlib 流 → 含 NVN 控制段（魔数 0x98761234）的 blob 存为 <key>.blob
#   pak_shaders.py derive  <blob dir> <dump dir> <facts.tsv> [<runtime dump dir> <runtime facts.tsv>]
#        blob → <key>.ctl（0x880）/ <key>.code（长度 = 控制段 +0x704，与宿主 DM_NVN_SHDUMP 同格式）+ 事实表（与 DM_NVN_TEXFACTS 同格式：
#        「键 \t 槽 \t 类型词」「键 \t attr:N \t float|uint」），可选与运行时转储 / 事实逐条核对。
# blob 布局（glslc 输出）：'LSLGSP' 头 … 代码区（0x30 NVN 头 + SPH + 指令）… 4 字节 … 控制段 0x880 … NV_gpu_program5 汇编文本（到 END）。
# 事实来源：汇编 `LDC.U64 Dn, buf14[off]` + `TEX… handle(Dn), 2D|3D|CUBE|BUFFER|…` → 槽 = off/8（对运行时记录 1412/1412 吻合）；
#            顶点属性被 .S/.U/.S32/.U32 指令读取 → uint（对运行时记录 699/699 吻合）。
# Blob layout (glslc output): 'LSLGSP' header … code area (0x30 NVN header + SPH + instructions) … 4 bytes … 0x880 control section … NV_gpu_program5 text (to END).
# Facts: `LDC.U64 Dn, buf14[off]` + `TEX… handle(Dn), <target>` -> slot = off/8 (matches 1412/1412 runtime records);
#        a vertex attribute read by a .S/.U/.S32/.U32 instruction -> uint (matches 699/699 runtime records).
import sys, os, re, struct, zlib, hashlib, collections
CTL = struct.pack('<I', 0x98761234); CODE = struct.pack('<I', 0x12345678); PAK_MAGIC = struct.pack('<I', 0x5A6F12E1)
TARGET = {'2D': '2d', '3D': '3d', 'CUBE': 'cube', 'ARRAY2D': '2darray', 'ARRAYCUBE': 'cubearray', 'BUFFER': 'buffer', '2DMS': '2dms', 'ARRAY2DMS': '2dmsarray',
          'SHADOW2D': '2d', 'SHADOWCUBE': 'cube', 'SHADOWARRAY2D': '2darray', 'SHADOWARRAYCUBE': 'cubearray', '1D': '1d', 'ARRAY1D': '1darray', 'RECT': '2d'}

def pak_entries(f):
    f.seek(0, 2); sz = f.tell(); f.seek(sz - 256); tail = f.read(256); i = tail.rfind(PAK_MAGIC)
    if i < 0: raise SystemExit('不是 UE4 pak（找不到尾部魔数）')
    ver, = struct.unpack_from('<I', tail, i + 4); off, isz = struct.unpack_from('<qq', tail, i + 8)
    if ver > 4: raise SystemExit(f'pak 版本 {ver}：块偏移语义不同（≥5 为相对偏移），本工具只按 ≤4 写')
    f.seek(off); idx = f.read(isz); p = 0
    ml, = struct.unpack_from('<i', idx, p); p += 4 + ml; n, = struct.unpack_from('<i', idx, p); p += 4
    for _ in range(n):
        l, = struct.unpack_from('<i', idx, p); p += 4; name = idx[p:p + l - 1].decode('latin1'); p += l
        o, s, u, c = struct.unpack_from('<qqqI', idx, p); hdr = 48; p += 48; blocks = []
        if c:
            nb, = struct.unpack_from('<I', idx, p); p += 4; hdr += 4 + 16 * nb
            for _ in range(nb): blocks.append(struct.unpack_from('<qq', idx, p)); p += 16
        p += 5; hdr += 5
        yield name, o, s, c, blocks, hdr

def entry_data(f, o, s, c, blocks, hdr):
    if c == 0: f.seek(o + hdr); return f.read(s)
    if c != 1: return None
    parts = []
    for a, b in blocks: f.seek(a); parts.append(zlib.decompress(f.read(b - a)))
    return b''.join(parts)

def extract(out, paks):
    os.makedirs(out, exist_ok=True); seen = set(os.listdir(out)); st = collections.Counter(); rows = []
    for pak in paks:
        with open(pak, 'rb') as f:
            for name, o, s, c, blocks, hdr in pak_entries(f):
                try: buf = entry_data(f, o, s, c, blocks, hdr)
                except Exception as e: print('解压失败', name, e); continue
                if buf is None or b'\x78\x9c' not in buf: continue
                st['files'] += 1
                for m in re.finditer(b'\x78\x9c', buf):
                    d = zlib.decompressobj()
                    try: blob = d.decompress(buf[m.start():m.start() + 16 * 1024 * 1024])
                    except zlib.error: continue
                    if not d.eof or len(blob) < 0x900: continue
                    for cm in re.finditer(re.escape(CTL), blob):
                        ctl = blob[cm.start():cm.start() + 0x880]
                        if len(ctl) < 0x880: continue
                        key = hashlib.sha256(ctl).hexdigest()[:16]; st['refs'] += 1
                        rows.append(f'{key}\t{os.path.basename(pak)}:{name}\n')
                        if key + '.blob' in seen: continue
                        seen.add(key + '.blob'); st['new'] += 1
                        with open(os.path.join(out, key + '.blob'), 'wb') as g: g.write(blob)
                if st['files'] % 5000 == 0: print(dict(st), flush=True)
    with open(os.path.join(out, 'index.tsv'), 'a') as g: g.writelines(rows)
    print('抽取完成', dict(st), '→', out)

LD_RE = re.compile(r'^\s*LDC\.U64\s+(D\d+)\.([xy]),\s*buf(\d+)\[(\d+)\]')
TEX_RE = re.compile(r'^\s*(TEX|TXL|TXD|TXG|TXB|TXF|TXQ|TXP|LOD|TXFMS)[\.\w]*\s+.*?,\s*handle\((D\d+)\.([xy])\),\s*([A-Z0-9]+)')
INT_OP = re.compile(r'^[A-Z0-9]+(\.[A-Z0-9]+)*?\.(S|U|S32|U32|S16|U16|S8|U8)(\.|$)')
ATTR_RE = re.compile(r'vertex\.attrib\[(\d+)\]')

def facts_from_asm(text):
    tex, attr, src = {}, {}, {}
    for ln in text.split('\n'):
        m = LD_RE.match(ln)
        if m: src[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4))); continue
        m = TEX_RE.match(ln)
        if m:
            s = src.get((m.group(2), m.group(3))); w = TARGET.get(m.group(4))
            if s and s[0] == 14 and s[1] % 8 == 0 and w: tex.setdefault(s[1] // 8, w)
        for a in ATTR_RE.finditer(ln):
            n = int(a.group(1)); op = ln.strip().split(' ')[0]
            if INT_OP.match(op) or op.startswith(('I2F', 'I2I', 'CVT.F32.S', 'CVT.F32.U')): attr[n] = 'uint'
            else: attr.setdefault(n, 'float')
    return tex, attr

def derive(blobs, dump, facts_path, rt_dump=None, rt_facts=None):
    os.makedirs(dump, exist_ok=True); st = collections.Counter(); lines = []
    rt = collections.defaultdict(dict)
    if rt_facts and os.path.exists(rt_facts):
        for ln in open(rt_facts):
            c = ln.rstrip('\n').split('\t')
            if len(c) == 3: rt[c[0]][c[1]] = c[2]
    for fn in sorted(os.listdir(blobs)):
        if not fn.endswith('.blob'): continue
        key = fn[:-5]; b = open(os.path.join(blobs, fn), 'rb').read(); o = b.find(CTL)
        while o >= 0 and hashlib.sha256(b[o:o + 0x880]).hexdigest()[:16] != key: o = b.find(CTL, o + 1)
        if o < 0: st['bad'] += 1; continue
        ctl = b[o:o + 0x880]; total = struct.unpack_from('<I', ctl, 0x704)[0]
        if not total or total > 0x400000 or o - 4 - total < 0: st['bad'] += 1; continue
        code = None
        for gap in (4, 0, 8):
            s = o - gap - total
            if s >= 0 and b[s:s + 4] == CODE: code = b[s:s + total]; break
        if code is None: code = b[o - 4 - total:o - 4]; st['code_no_magic'] += 1   # 计算着色器无 0x30 头 | compute shaders carry no 0x30 header
        if rt_dump and os.path.exists(os.path.join(rt_dump, key + '.code')):
            same = open(os.path.join(rt_dump, key + '.code'), 'rb').read() == code and open(os.path.join(rt_dump, key + '.ctl'), 'rb').read() == ctl
            st['rt_same' if same else 'rt_DIFF'] += 1
            if not same: print('与运行时转储不一致', key)
        with open(os.path.join(dump, key + '.ctl'), 'wb') as g: g.write(ctl)
        with open(os.path.join(dump, key + '.code'), 'wb') as g: g.write(code)
        st['shaders'] += 1
        end = b.find(b'\nEND', o + 0x880); text = b[o + 0x880:end if end > 0 else len(b)].decode('latin1')
        tex, attr = facts_from_asm(text)
        for slot, w in sorted(tex.items()):
            lines.append(f'{key}\t{slot}\t{w}\n'); st['tex_facts'] += 1
            r = rt.get(key, {}).get(str(slot))
            if r is not None: st['tex_rt_same' if r == w else 'tex_rt_DIFF'] += 1
            if r is not None and r != w: print('贴图事实与运行时不一致', key, slot, '运行时', r, '汇编', w)
        for n, w in sorted(attr.items()):
            lines.append(f'{key}\tattr:{n}\t{w}\n'); st['attr_facts'] += 1
            r = rt.get(key, {}).get(f'attr:{n}')
            if r is not None: st['attr_rt_same' if r == w else 'attr_rt_DIFF'] += 1
            if r is not None and r != w: print('属性事实与运行时不一致', key, n, '运行时', r, '汇编', w)
    with open(facts_path, 'w') as g: g.writelines(lines)
    print('推导完成', dict(st), '→', dump, facts_path)

if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'extract': extract(a[1], a[2:])
    elif len(a) in (4, 6) and a[0] == 'derive': derive(*a[1:])
    else: print(__doc__ or open(__file__).read().split('\n\n')[0]); sys.exit(2)
