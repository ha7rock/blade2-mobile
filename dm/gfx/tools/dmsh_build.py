#!/usr/bin/env python3
# B 路线离线出包：Switch 原版着色器 → SPIR-V（dmsh_xlate，Ryujinx 翻译器）→ spirv-cross MSL → metallib + 回放反射 JSON。
# 运行时不翻译（iOS 无 JIT）：产物进着色器缓存目录，文件名 = Switch 控制段键（dm_ctl_sha16，与 shader_map.tsv 第 4 列同）+「_b」。
# 查询作答来源：仓内的作答表 nvn/shader_facts.tsv（dmsh_facts.py）——贴图类型、顶点属性 / 片元输出分量类型、计算线程组大小与共享内存；出包不需要 PC 版文件。
# 用法：dmsh_build.py <Switch 包目录> <输出缓存目录> [键…]（不给键 = 全部）；产物实际写进 <输出缓存目录>/b-<链版本>/（IOSOUT 同），链版本见 dmsh_chain.py   环境：XLATE=翻译工具路径，DMSH_CAS=内容寻址缓存目录（缺省为输出缓存目录/cas），J=并发
# 程序本体内嵌的图形着色器（映射表 main.rodata@偏移 行）从 MAIN（Switch main NSO，缺省 3.3.94012 Update/ExeFS/main）解出；解 NSO 要 lz4，用 dm 的 venv python 跑
# 产物：<键>_b.metallib、<键>_b.json（回放反射，含驱动段 c0 / c2 的运行时读取偏移 drv_reads）、<键>_b.metal（MSL）
# 环境 IOSOUT=目录：同一份 MSL 经 dm/ios/tools/ios_metal.compile 同时出 iphoneos metallib + 反射（chain_metal 记 iOS 编译口径）
# 末尾汇总：成功 / 失败数与原因分布；驱动段读取的偏移分布（落在回放层能填的范围外的单列）
# Route B offline build: original Switch shaders → SPIR-V (dmsh_xlate, the Ryujinx translator) → spirv-cross MSL → metallib + replay reflection JSON.
# No translation at runtime (iOS has no JIT): build output goes into the shader cache directory, file name = Switch control segment key (dm_ctl_sha16, same as column 4 of shader_map.tsv) + "_b".
# Source of query answers: the in-repo answer table nvn/shader_facts.tsv (dmsh_facts.py) - texture types, vertex attribute / fragment output component types, compute thread group size and shared memory; the build needs no PC version files.
# Usage: dmsh_build.py <Switch package dir> <output cache dir> [key...] (no keys = all); build output is actually written to <output cache dir>/b-<chain version>/ (same for IOSOUT), for the chain version see dmsh_chain.py   Environment: XLATE=path to the translation tool, DMSH_CAS=content-addressed cache dir (default <output cache dir>/cas), J=concurrency
# Graphics shaders embedded in the executable itself (mapping table rows main.rodata@offset) are extracted from MAIN (Switch main NSO, default 3.3.94012 Update/ExeFS/main); extracting the NSO needs lz4, run with dm's venv python
# Build output: <key>_b.metallib, <key>_b.json (replay reflection, including drv_reads, the runtime read offsets into driver segments c0 / c2), <key>_b.metal (MSL)
# Environment IOSOUT=dir: the same MSL also goes through dm/ios/tools/ios_metal.compile to produce an iphoneos metallib + reflection (chain_metal records the iOS compile convention)
# Final summary: success / failure counts and the distribution of reasons; distribution of driver segment read offsets (those outside the range the replay layer can fill are listed separately)
import ast, inspect, os, re, sys, json, struct, hashlib, shutil, subprocess, tempfile, collections, threading, concurrent.futures as cf
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作区根目录（游戏文件、产物、运行现场都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
HOME = os.path.expanduser('~')
dm_dir, out = sys.argv[1:3]; want = set(sys.argv[3:]); cache_root = out
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain, dmsh_fetch, dmsh_facts   # 翻译链版本：先核工具与锁文件，产物进 b-<V>/ | translation chain version: verify the tools against the lock file first; build output goes into b-<V>/
_bad = dmsh_chain.check_tools()
if _bad: sys.exit('\n'.join('【红】翻译链工具与锁文件不符：' + b for b in _bad) + '\n（换了工具就 dmsh_chain.py --update-lock 并入仓；只是没重编翻译器就重编）')
CHAIN = dmsh_chain.version(); out = os.path.join(out, 'b-' + CHAIN)
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'【红】nvn/dmsh_chain.h 不是当前翻译链版本 {CHAIN}：先跑 dmsh_chain.py --header（否则宿主找不到这批库）')
XLATE = os.environ.get('XLATE', f'{DM_ROOT}/build/gfx/dmsh_xlate/dmsh_xlate')
FACTS = dmsh_facts.load()
if not FACTS: sys.exit('【红】没有作答表 nvn/shader_facts.tsv（随仓库；维护者用 dmsh_facts.py --freeze 重出）')
IOSOUT = os.environ.get('IOSOUT')   # 设了就同时出 iOS 版（iphoneos metallib + 反射）到该目录 | if set, also build the iOS version (iphoneos metallib + reflection) into that directory
if IOSOUT:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); import ios_metal
    IOSOUT = os.path.join(IOSOUT, 'b-' + CHAIN); os.makedirs(IOSOUT, exist_ok=True)
MAIN = os.environ.get('MAIN', f'{DM_ROOT}/data/switch/3.3.94012/Update/ExeFS/main')
MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../nvn/shader_map.tsv')
BF_WANT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../nvn/bf_want.tsv')
BF_WANT = {line.strip() for line in open(BF_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(BF_WANT_FILE) else set()
XC = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
CAS = os.environ.get('DMSH_CAS', os.path.join(cache_root, 'cas'))
CAS_LOCKS, CAS_LOCKS_GUARD = {}, threading.Lock()
PRE_STATS = collections.Counter()
# -fpreserve-invariance：让 MSL 里 [[position, invariant]] 真的生效（Metal 默认 fast-math 下各着色器 fma 合并不同，UE4 深度预通道 + 基础通道 GEQUAL 测试要求裁剪位置逐位相同，否则地板按三角形成块丢失）
# -fpreserve-invariance: make [[position, invariant]] in the MSL actually take effect (under Metal's default fast-math each shader contracts fma differently; UE4's depth prepass + base pass GEQUAL test needs bit-identical clip positions, otherwise floor triangles drop out in blocks)
MAC_RECORD = 'xcrun -sdk macosx metal -c -fpreserve-invariance; xcrun -sdk macosx metallib'
TEX_BASE, SAMP_BASE, IMG_BASE, COMB_BASE = 0x168, 0x568, 0x120, 0x20   # c[2]：图像句柄表 0x120 + 8 r（8 槽，紧接贴图表之前；cs_lut_combine_c 写 u4 = 0x140 反推，u5 = 0x148 核对） | c[2]: image handle table 0x120 + 8 r (8 slots, immediately before the texture table; inferred from cs_lut_combine_c writing u4 = 0x140, checked with u5 = 0x148)

def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

def clone(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    r = subprocess.run(['cp', '-c', src, tmp], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60)
    if r.returncode:
        shutil.copyfile(src, tmp)
    os.replace(tmp, dst)

def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)

def source_without_rule_bodies():
    source = open(__file__, 'r').read(); lines = source.splitlines(True); tree = ast.parse(source)
    names = {f.__name__ for f, _ in RULES}
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in names and node.body:
            first = node.body[0].lineno - 1
            last = max(getattr(x, 'end_lineno', x.lineno) for x in ast.walk(node) if getattr(x, 'lineno', 0) >= node.body[0].lineno)
            indent = re.match(r'\s*', lines[first]).group(0); lines[first:last] = [indent + 'pass\n']
    return ''.join(lines)

def shader_rule_source(name):
    return b''.join(inspect.getsource(f).encode() for f, applies in RULES if applies(name))

def tool_fingerprint():
    sc = subprocess.run(['spirv-cross', '--version'], capture_output=True, text=True, timeout=30)
    return ('lock\0' + open(dmsh_chain.LOCK).read() + '\0options\0' + dmsh_chain.options() +
            '\0xlate\0' + dmsh_chain.xlate_source_sha() + '\0spirv-cross\0' + sc.stdout + sc.stderr +
            '\0xc\0' + repr(XC) + '\0mac\0' + MAC_RECORD + '\0ios\0' + (ios_metal.RECORD if IOSOUT else ''))

def shader_key(key, name, rec):
    pc = dmsh_facts.row(key, FACTS[key]).encode() if key in FACTS else '缺'.encode()
    map_row = MAP_ROWS.get(key, '缺'.encode()) + MAP_CONFLICT_ROWS.get(key, b'')
    embedded = rec if name.startswith('main.rodata@') else b''
    payload = (b'record\0' + rec + b'\0map\0' + map_row + b'\0pc\0' + pc + b'\0embedded\0' + embedded + b'\0pairs\0' + ','.join(sorted(PAIRS.get(key, ()))).encode() + b'\0gs\0' + ','.join(sorted(PAIRS_GS.get(key, ()))).encode() +
               b'\0tools\0' + TOOL_FP + b'\0code\0' + CODE_FP.encode() +
               b'\0rule\0' + shader_rule_source(name))
    return hashlib.sha256(payload).hexdigest()

def cache_meta_path(key): return os.path.join(CAS, 'index', key + '.json')
def cache_path(digest): return os.path.join(CAS, 'shader', digest)

def cache_outputs(key, digest, cached=False):
    root = cache_path(digest); files = {}
    for label, directory, names in (
            ('mac', out, [key + '_b.metallib', key + '_b.metal', key + '_b.json']),
            ('ios', IOSOUT, [key + '_b.metallib', key + '_b.json'])):
        if not directory: continue
        base = os.path.join(root, label) if cached else directory
        found = [n for n in names if os.path.exists(os.path.join(base, n))]
        if key + '_b.metallib' not in found or key + '_b.json' not in found: return None
        files[label] = found
    return root, files

def restore_cache(key, digest):
    found = cache_outputs(key, digest, cached=True)
    if not found: return False
    root, files = found
    for label, names in files.items():
        directory = out if label == 'mac' else IOSOUT
        for name in names:
            src = os.path.join(root, label, name); dst = os.path.join(directory, name)
            if name.endswith('.json'):
                tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
                with open(tmp, 'wb') as f: f.write(re.sub(rb'("chain"\s*:\s*")[^"]*"', rb'\g<1>' + CHAIN.encode() + b'"', open(src, 'rb').read(), count=1))
                os.replace(tmp, dst)
            else: clone(src, dst)
    return True

def cache_lookup(key, digest, rule_fp, code_fp):
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('shader', key), threading.Lock())
    with lock:
        if restore_cache(key, digest): PRE_STATS['hit'] += 1; return True, '命中'
        old = json.load(open(cache_meta_path(key))) if os.path.exists(cache_meta_path(key)) else None
        if old and old.get('code_fp') != code_fp: reason = '代码指纹变化：全部重出'
        elif old and old.get('rule_fp') != rule_fp: reason = '规则变'
        else: reason = '新键'
        PRE_STATS['miss'] += 1; PRE_STATS[reason] += 1; return False, reason

def publish_cache(key, digest, rule_fp, code_fp):
    found = cache_outputs(key, digest)
    if not found: return
    root, files = found
    for label, names in files.items():
        d = os.path.join(root, label); os.makedirs(d, exist_ok=True)
        for name in names: clone(os.path.join(out if label == 'mac' else IOSOUT, name), os.path.join(d, name))
        for name in names:
            if name.endswith('.json'):
                src = os.path.join(out if label == 'mac' else IOSOUT, name)
                value = json.load(open(src)); value.pop('fetch', None)
                atomic_json(os.path.join(d, name), value)
    atomic_json(cache_meta_path(key), {'digest': digest, 'rule_fp': rule_fp, 'code_fp': code_fp})

BF_STATS = collections.Counter()

def bf_digest(msl, key):
    h = hashlib.sha256()
    h.update(msl.encode())
    h.update(b'\0mac\0' + MAC_RECORD.encode())
    h.update(b'\0ios\0' + (ios_metal.RECORD if IOSOUT else '').encode())
    h.update(b'\0bf-want\0' + (b'1' if key in BF_WANT else b'0'))
    return h.hexdigest()

def bf_cache_root(digest): return os.path.join(CAS, 'variant', digest)

def bf_cache_restore(key, digest, fmeta, fmsl):
    root = bf_cache_root(digest)
    names = ['metal', 'mac.metallib'] + (['ios.metallib'] if IOSOUT else [])
    if not all(os.path.exists(os.path.join(root, n)) for n in names): return False
    clone(os.path.join(root, 'metal'), fmsl)
    clone(os.path.join(root, 'mac.metallib'), os.path.join(out, key + '_bf.metallib'))
    if IOSOUT: clone(os.path.join(root, 'ios.metallib'), os.path.join(IOSOUT, key + '_bf.metallib'))
    BF_STATS['hit'] += 1
    return True

def bf_cache_publish(digest, fmsl, maclib, ioslib, fmeta):
    root = bf_cache_root(digest); os.makedirs(root, exist_ok=True)
    clone(fmsl, os.path.join(root, 'metal'))
    clone(maclib, os.path.join(root, 'mac.metallib'))
    if IOSOUT: clone(ioslib, os.path.join(root, 'ios.metallib'))
    atomic_json(os.path.join(root, 'fetch.json'), fmeta)

def write_refl(key, refl):
    for d, rec in ((out, MAC_RECORD),) + (((IOSOUT, ios_metal.RECORD),) if IOSOUT else ()):
        value = dict(refl, chain_metal=rec)
        atomic_json(os.path.join(d, key + '_b.json'), value)

def fetch_finish(key, src, refl):
    if refl['stage'] != 'ps': return refl
    # 有可改采样的照旧出变体；没有可改采样的只给名单（bf_want.tsv，宿主规划器产出）里的键出仅输出映射变体
    # Those with rewritable samples get variants as before; those without get an output-mapping-only variant, and only for keys in the list (bf_want.tsv, produced by the host planner)
    fv, fmeta = dmsh_fetch.variant(src, refl['bindings'], om_only_ok=key in BF_WANT)
    if not fv:
        for path in (os.path.join(out, key + '_bf.metallib'), os.path.join(out, key + '_bf.metal')) + ((os.path.join(IOSOUT, key + '_bf.metallib'),) if IOSOUT else ()):
            try: os.unlink(path)
            except FileNotFoundError: pass
        write_refl(key, refl)
        return refl
    BF_STATS['generated'] += 1
    fmsl = os.path.join(out, key + '_bf.metal')
    digest = bf_digest(fv, key)
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('variant', digest), threading.Lock())
    with lock:
        try:
            if not bf_cache_restore(key, digest, fmeta, fmsl):
                BF_STATS['miss'] += 1
                open(fmsl, 'w').write(fv)
                fair = fmsl[:-6] + '.air'; flib = os.path.join(out, key + '_bf.metallib')
                try:
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fpreserve-invariance', fmsl, '-o', fair], check=True)
                    tmp_flib = flib + '.tmp-' + str(os.getpid())
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', fair, '-o', tmp_flib], check=True)
                    os.replace(tmp_flib, flib)
                    ilib = None
                    if IOSOUT:
                        ilib = os.path.join(IOSOUT, key + '_bf.metallib')
                        ios_metal.compile(fmsl, fmsl[:-6] + '.ios.air', ilib + '.tmp')
                        os.replace(ilib + '.tmp', ilib)
                except subprocess.CalledProcessError:
                    return refl
                BF_STATS['compile'] += 1
                bf_cache_publish(digest, fmsl, flib, ilib, fmeta)
        finally:
            for path in (fmsl, fmsl[:-6] + '.air', fmsl[:-6] + '.ios.air'):
                try: os.unlink(path)
                except FileNotFoundError: pass
    refl['fetch'] = fmeta
    write_refl(key, refl)
    return refl

pc_of, loc, stage_of = {}, {}, {}
MAP_ROWS, MAP_CONFLICT_ROWS = {}, {}
for raw in open(MAP, 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 4: MAP_ROWS.setdefault(c[3].decode(), raw)
for raw in open(MAP + '.conflicts.tsv', 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 1 and not raw.startswith((b'#', b'dm_')): MAP_CONFLICT_ROWS.setdefault(c[0].decode(), raw)
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
    if len(c) > 4 and c[0].startswith('main.rodata@'):   # 内嵌：位置 = main 映像偏移（控制段起点），阶段取映射表第 3 列 | embedded: location = main image offset (start of the control segment), stage taken from column 3 of the mapping table
        pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[0].split('@')[1], 16))); stage_of[c[3]] = {'顶点': 'vs', '像素': 'ps'}[c[2]]
for ln in open(MAP + '.conflicts.tsv'):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 1 and not ln.startswith('#') and not ln.startswith('dm_') and c[0] in pc_of: pc_of[c[0]] = c[1]

def main_rec(mi, o):
    """内嵌着色器 → 与包内记录同布局的字节：包内记录 +0x38 起是代码区（0x30 头 + SPH + 指令，标记 78563412），紧接控制段（34127698）；
    内嵌的代码区紧贴在控制段之前（核对 5 个：代码区长 0x100–0x300、c1 均为空，与同名包内记录布局一致）
    Embedded shader → bytes in the same layout as a package record: in a package record the code area starts at +0x38 (0x30 header + SPH + instructions, marker 78563412), immediately followed by the control segment (34127698);
        the embedded code area sits immediately before the control segment (checked 5: code area length 0x100–0x300, c1 empty in all, consistent with the layout of the same-named package records)"""
    g = mi.rfind(bytes.fromhex('78563412'), o - 0x1000, o)
    if g < 0 or mi[o:o + 4] != bytes.fromhex('34127698'): sys.exit(f'【红】main 映像 @{o:#x} 不是内嵌着色器控制段（MAIN={MAIN}）')
    return bytes(0x38) + mi[g:o + 0x880]

# 顶点 / 片元配对（宿主 DM_NVN_PAIRS 记录：每行 "0:<顶点键> 1:<片元键> …"）：顶点着色器的作业带上所有配过对的片元作业（翻译器 SetNextStage）
# Vertex / fragment pairing (recorded by the host via DM_NVN_PAIRS: each line "0:<vertex key> 1:<fragment key> ..."): a vertex shader's job carries the jobs of every fragment shader it was paired with (translator SetNextStage)
PAIRS, PAIRS_GS = {}, {}   # PAIRS_GS：顶点键 → 配过对的几何键（宿主记录 "2:<几何键>"；见 gs_layer） | PAIRS_GS: vertex key -> paired geometry keys (host records "2:<geometry key>"; see gs_layer)
_pf = os.environ.get('PAIRS', os.path.join(DM_ROOT, 'shpairs.txt'))
if os.path.exists(_pf):
    for ln in open(_pf):
        st = dict(x.split(':', 1) for x in ln.split() if ':' in x)
        if '0' in st and '1' in st: PAIRS.setdefault(st['0'], set()).add(st['1'])
        if '0' in st and '2' in st: PAIRS_GS.setdefault(st['0'], set()).add(st['2'])
def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698')); code = rec[0x38:ci]; stage = stage_of.get(key, name[:2])
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)   # 控制段 +0x700：c1 相对指令区起点、+0x704：指令区总长（c1 = 两者之间；全库长度 0 / 256 / 512 / 768） | control segment +0x700: c1 relative to the start of the instruction area, +0x704: total length of the instruction area (c1 = what lies between the two; lengths across the whole library are 0 / 256 / 512 / 768)
    c1 = rec[0x38 + c1o:0x38 + c1t]
    if stage != 'cs': code = code[0x30:]
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(),
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # 翻译选项（见 dmsh_xlate/Program.cs）；放宽精度缺省开（GPU 浮点末位误差不进游戏状态，mini 整帧 −26%）   # c1：翻译器读间接跳转表用 | translation options (see dmsh_xlate/Program.cs); relaxed precision is on by default (GPU float last-bit error does not enter game state, whole frame −26% on the second Mac)   # c1: used by the translator to read indirect jump tables
    if key in FACTS: j.update(FACTS[key])
    else: j['pc_missing'] = True   # 作答表里没有：查询按缺省（汇总里单列） | not in the answer table: queries use defaults (listed separately in the summary)
    if stage == 'vs' and key in PAIRS:
        j['next'] = []
        for fk in sorted(PAIRS[key]):
            if fk not in loc: continue
            fname, fi = loc[fk]
            if fname not in packs: packs[fname] = records(open(os.path.join(dm_dir, fname), 'rb').read())
            fj, _ = job_for(fk, fname, packs[fname][fi], w)
            j['next'].append(fj)
    return j, c1

def raster_facts(stage, src):
    """读入可省（全覆盖首绘）的运行时证明所需的静态事实（nvn_mtl.m first_draw_covers）：
    顶点：位置 x / y / z 是否原样取自同一个顶点属性的 .x / .y / .z（w 为常数 1 或同属性 .w），主函数无分支、无裁剪距离——是则 pos_attr = 属性槽；
    片元：是否可能丢弃（discard_fragment）、是否写 sample_mask、写出的颜色号。认不出一律按「不可证」（pos_attr = -1 / discard = 1）
    Static facts needed for the runtime proof that the load can be skipped (full-coverage first draw) (nvn_mtl.m first_draw_covers):
        Vertex: whether position x / y / z are taken unchanged from the .x / .y / .z of one and the same vertex attribute (w is the constant 1 or the same attribute's .w), with no branches and no clip distances in the main function - if so, pos_attr = attribute slot;
        Fragment: whether it may discard (discard_fragment), whether it writes sample_mask, and which color indices it writes. Anything unrecognized is treated as "unprovable" (pos_attr = -1 / discard = 1)"""
    if stage == 'ps':
        out = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
        return {'discard': int('discard_fragment' in src), 'sample_mask': int('sample_mask' in (out.group(1) if out else '')),
                'colors': sorted({int(x) for x in re.findall(r'\[\[color\((\d+)\)\]\]', out.group(1) if out else '')})}
    if stage != 'vs': return {}
    body = re.search(r'\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$', src, re.S)   # 主函数是文件里最后一个函数 | the main function is the last function in the file
    if not body or re.search(r'\b(if|for|while|switch|do)\b|clip_distance|ClipDistance', body.group(1)): return {'pos_attr': -1}
    b = body.group(1)
    attr = {m.group(2): int(m.group(3)) for m in re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src)}
    var = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);', b)}
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([xyzw])\s*=\s*([^;]+);', b): last[m.group(1)] = m.group(2).strip()
    src_of = {c: var.get(v) for c, v in last.items()}
    m0 = src_of.get('x')
    if not m0 or m0[0] not in attr or any(src_of.get(c) != (m0[0], c) for c in 'xyz'): return {'pos_attr': -1}
    if last.get('w') == '1.0': w = 'one'
    elif src_of.get('w') == (m0[0], 'w'): w = 'attr'
    else: return {'pos_attr': -1}
    # uv 直传（合并 B 第 2 步「本像素寻址」证明用）：插值输出 locn K 的 .x / .y 原样取自顶点属性 A 的 .x / .y → uv_from_attr[K] = A
    # uv pass-through (used for the "this-pixel addressing" proof in merge B step 2): the .x / .y of interpolated output locn K are taken unchanged from the .x / .y of vertex attribute A → uv_from_attr[K] = A
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'(m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    olast = {}
    for m in re.finditer(r'out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);', b): olast[(m.group(1), m.group(2))] = m.group(3).strip()
    uv, aff = {}, {}
    for mem, k in locn.items():
        sx, sy = var.get(olast.get((mem, 'x'), '')), var.get(olast.get((mem, 'y'), ''))
        if sx and sy and sx[0] == sy[0] and sx[0] in attr and sx[1] == 'x' and sy[1] == 'y': uv[str(k)] = attr[sx[0]]; continue
        # 仿射：分量 = fma(属性分量, uniform[槽].向量[i].分量, uniform[槽].向量[j].分量)（vp_cN 即 uniform 槽 N − 3）
        # Affine: component = fma(attribute component, uniform[slot].vector[i].component, uniform[slot].vector[j].component) (vp_cN is uniform slot N − 3)
        def fm(v_):
            m = re.search(r'float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);' % re.escape(v_), b)
            if not m or m.group(1) not in var: return None
            return var[m.group(1)], [int(m.group(2)) - 3, int(m.group(3)), 'xyzw'.index(m.group(4))], [int(m.group(5)) - 3, int(m.group(6)), 'xyzw'.index(m.group(7))]
        fx, fy = fm(olast.get((mem, 'x'), '')), fm(olast.get((mem, 'y'), ''))
        if fx and fy and fx[0][0] == fy[0][0] and fx[0][0] in attr and fx[0][1] == 'x' and fy[0][1] == 'y' and min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0:
            aff[str(k)] = {'attr': attr[fx[0][0]], 'sx': fx[1], 'ox': fx[2], 'sy': fy[1], 'oy': fy[2]}
    return {'pos_attr': attr[m0[0]], 'pos_w': w, 'uv_from_attr': uv, 'uv_affine': aff}

def decode_tex(name):
    """fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}：贴图槽、采样器槽（Ryujinx TextureHandle 编码；c[2] 句柄表） | fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}: texture slot, sampler slot (Ryujinx TextureHandle encoding; c[2] handle table)"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1; h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // 8 if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None
    words = [o0] + ([o1] if ty else [])
    if re.match(r'[a-z]+_i', name):   # 图像 | image
        return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < TEX_BASE and slot(x, IMG_BASE, tb) is not None), None), None, ty
    t = next((slot(x, TEX_BASE, tb) for x in words if slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    # 组合句柄表（nvnCommandBufferBindTexture，UE4 的 NVN RHI 用）：c[2][0x20 + 8r]，32 槽，紧接图像表之前；句柄类型 0 ⇒ 采样器取同槽句柄高位（samp_tex）
    # Combined handle table (nvnCommandBufferBindTexture, used by UE4's NVN RHI): c[2][0x20 + 8r], 32 slots, right before the image table; handle type 0 ⇒ the sampler comes from the same slot's high bits (samp_tex)
    if t is None: t = next((slot(x, COMB_BASE, tb) for x in words if COMB_BASE <= x * 4 < IMG_BASE and slot(x, COMB_BASE, tb) is not None), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty

def reflect(key, stage, msl, res):
    """MSL 参数缓冲结构体成员 → 回放反射：kind（同 nvn_refl.h）+ NVN 槽号 + 参数缓冲成员号 | MSL argument buffer struct member → replay reflection: kind (same as nvn_refl.h) + NVN slot number + argument buffer member index"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    sb_cb = {b['binding']: b for b in res['sbufs']}
    binds = []
    for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];', body.group(1) if body else '', re.M):
        ty, nm, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        e = {'msl_name': nm, 'msl_type': ty, 'arg_id': aid}
        if nm == 'support_buffer': e.update(kind='support')
        elif re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))
            e.update(kind='cbuf', reg=n - 3) if 3 <= n <= 16 else e.update(kind='c1' if n == 1 else 'drvcb', bank=n)
        elif re.fullmatch(r'[a-z]+_s(\d+)', nm):
            e.update(kind='sbuf', index=int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1)))
        elif nm.endswith('Smplr'):   # 句柄类型 0 = 贴图与采样器合在同一句柄：采样器号取该贴图槽句柄的高位 | handle type 0 = texture and sampler combined in one handle: the sampler index is taken from the high bits of that texture slot's handle
            t, s, ty = decode_tex(nm[:-5]); e.update(kind='samp', reg=s) if ty else e.update(kind='samp_tex', reg=t)
        else:
            d = decode_tex(nm)
            if not d: e.update(kind='?')
            else: e.update(kind='img' if re.match(r'[a-z]+_i', nm) else ('tbuf' if 'texture_buffer' in ty else 'tex'), reg=d[0])
        binds.append(e)
    # 存储缓冲：SPIR-V 绑定号 → c0 偏移 → NVN 存储缓冲槽（c0 基址：顶点 0x110、计算 0x310、像素 0x510，每槽 0x10）
    # Storage buffers: SPIR-V binding number → c0 offset → NVN storage buffer slot (c0 base: vertex 0x110, compute 0x310, pixel 0x510, 0x10 per slot)
    base = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}.get(stage)   # 外壳 / 域阶段的存储缓冲基址未实测：遇到存储缓冲按「无槽」报 | storage buffer base for the hull / domain stages has not been measured: a storage buffer there is reported as 「无槽」(no slot)
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] == 'sbuf':
            b = sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None
            off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
            if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0:
                e.update(kind='ubo_ptr', reg=(off - 0x210) // 16)   # 计算阶段 uniform 槽 r：地址在 c0[0x210 + 0x10 r]，经全局内存读（cs_lut_combine_c 读 cb10[66] 核对） | compute-stage uniform slot r: address at c0[0x210 + 0x10 r], read through global memory (checked against cs_lut_combine_c reading cb10[66])
            else: e['reg'] = (off - base) // 16 if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0 else None
            e['rw'] = bool(b and b['flags'] & 2)
    return binds

DRV_FILL = {0: [(0x110, 0x210), (0x310, 0x410), (0x510, 0x610)],   # c0：存储缓冲地址表（顶点 / 计算 / 像素，各 16 槽 × 0x10） | c0: storage buffer address tables (vertex / compute / pixel, 16 slots × 0x10 each)
            2: [(0x168, 0x568), (0x568, 0x968)]}                     # c2：贴图句柄表、采样器句柄表（各 128 槽 × 8） | c2: texture handle table, sampler handle table (128 slots × 8 each)

def drv_reads(src):
    """MSL 里对驱动段 c0 / c2 的读取：常量下标 → 字节区间；非常量下标记为动态 | Reads of driver segments c0 / c2 in the MSL: constant index → byte range; non-constant indices are recorded as dynamic"""
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit(): offs.add(int(idx) * 16)
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

def unfillable(dr):
    bad = []
    for bank, d in dr.items():
        if d['dynamic']: bad.append(f'c{bank} 动态下标')
        for o in d['vec4_offsets']:
            if not any(a <= o and o + 16 <= b or (a <= o < b) for a, b in DRV_FILL[int(bank)]): bad.append(f'c{bank}+{o:#x}')
    return bad

RULES = []   # 按着色器名挂的 MSL 改写规则（函数, 适用判定）；原有的暗部重映射 toe 已删（回到 Switch 原版亮度） | MSL rewrite rules attached by shader name (function, applicability test); the former dark-range remap toe has been removed (back to the original Switch brightness)
# 第一段代码指纹不包含 dmsh_fetch.py；可读 tile 变体单独按生成 MSL 做第二段缓存。
# The first-stage code fingerprint does not include dmsh_fetch.py; readable-tile variants get a separate second-stage cache keyed on the generated MSL.
CODE_FP = hashlib.sha256(source_without_rule_bodies().encode() + b''.join(b'\0' + open(m.__file__, 'rb').read() for m in (dmsh_chain,) + ((ios_metal,) if IOSOUT else ()))).hexdigest()
TOOL_FP = tool_fingerprint().encode()

# ---- 几何着色器直通 + 写层号 → 融进顶点着色器 ----
# UE4 的分层目标（32³ 调色 LUT、64³ 半透明光照体、立方体阴影）用 FWriteToSliceGS：几何着色器把三角形原样转发，只把顶点传来的层号写进 gl_Layer。
# Metal 没有几何着色器，但顶点函数可以直接输出 [[render_target_array_index]]：把几何着色器翻成 GLSL（Ryujinx → spirv-cross），认出
# "gl_Layer = <输入插值 N 的分量 c>"（按位或按值取整），再在配对顶点着色器的 MSL 里补一个 uint 输出 = 同一插值分量。认不出的几何着色器照旧不画（报在汇总里）。
# ---- Geometry shader pass-through + layer write → fused into the vertex shader ----
# UE4's layered targets (32³ color LUT, 64³ translucency lighting volumes, cube shadows) use FWriteToSliceGS: the geometry shader forwards the triangle unchanged and only writes the layer index passed from the vertex into gl_Layer.
# Metal has no geometry shaders, but a vertex function can output [[render_target_array_index]] directly: translate the geometry shader to GLSL (Ryujinx → spirv-cross), recognize
# "gl_Layer = <component c of input interpolant N>" (as bits or by value), then add a uint output = the same interpolant component to the paired vertex shader's MSL. Unrecognized geometry shaders are still not drawn (reported in the summary).
GS_CACHE, GS_LOCK = {}, threading.Lock()
def gs_glsl(key, w):
    spv = os.path.join(w, key + '.spv')
    if not os.path.exists(spv): return None, '几何着色器没有 spv（翻译失败？）'
    r = subprocess.run(['spirv-cross', '--version', '450', spv], capture_output=True, text=True)
    return (r.stdout, '') if r.returncode == 0 else (None, 'spirv-cross GLSL 失败：' + r.stderr.strip()[:120])
def gs_layer(key, w):
    """几何着色器的层号来源 {'locn': 输入插值号, 'comp': 分量, 'bits': 按位取整}；不是直通写层号则 None | Where the geometry shader's layer comes from; None unless it is a pass-through layer write"""
    with GS_LOCK:
        if key in GS_CACHE: return GS_CACHE[key][0]
        glsl, why = gs_glsl(key, w); info = None
        if glsl: open(os.path.join(out, key + '_b.glsl'), 'w').write(glsl)   # 留档：几何着色器的 GLSL（分析依据） | Kept for the record: the geometry shader's GLSL (basis of the analysis)
        if glsl and len(re.findall(r'EmitVertex\s*\(', glsl)) == 18:   # 6 面 × 3 顶点：立方体阴影 | 6 faces x 3 vertices: cube shadow
            cube, cwhy = gs_cube(glsl)
            if cube: info = dict(cube, kind='cube')
            else: why = '6 面几何着色器认不出：' + cwhy
        elif glsl:
            # spirv-cross 的 GLSL：输入插值 "layout(location = N) in vec4 NAME[3];"，层号多经临时量 "float T = NAME[i].c;" 再 "gl_Layer = floatBitsToInt(T);"（按位）或 "int(T)"（按值）
            # spirv-cross GLSL: input interpolants "layout(location = N) in vec4 NAME[3];", the layer usually goes through a temporary "float T = NAME[i].c;" then "gl_Layer = floatBitsToInt(T);" (bits) or "int(T)" (value)
            locs = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) in vec4 (\w+)\[', glsl)}
            temps = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float (\w+) = (\w+)\[\w+\]\.([xyzw]);', glsl)}
            m = re.search(r'gl_Layer\s*=\s*([^;]+);', glsl)
            emits = len(re.findall(r'EmitVertex\s*\(', glsl))
            src_m = re.match(r'\s*(floatBitsToInt|int)?\s*\(?\s*(\w+)(?:\[\w+\]\.([xyzw]))?\s*\)?\s*$', m.group(1)) if m else None
            arr = comp = None
            if src_m:
                v = src_m.group(2)
                if src_m.group(3): arr, comp = v, src_m.group(3)
                elif v in temps: arr, comp = temps[v]
            if not m: why = '没有写 gl_Layer'
            elif arr not in locs: why = 'gl_Layer 不是直接取自输入插值：' + m.group(1)[:80]
            elif emits != 3: why = f'EmitVertex {emits} 次（只认三角形直通；6 面 = 立方体阴影，待做实例化模拟）'
            else: info = {'kind': 'layer', 'locn': locs[arr], 'comp': comp, 'bits': src_m.group(1) != 'int'}
        GS_CACHE[key] = (info, why)
        return info
def gs_layer_why(key, w): gs_layer(key, w); return GS_CACHE[key][1]
def fuse_layer(src, info):
    """在顶点 MSL 里补写 out.gl_Layer（[[render_target_array_index]]）= 插值 locn 的分量 | Add out.gl_Layer ([[render_target_array_index]]) = component of interpolant locn to the vertex MSL"""
    out_s = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not out_s: return None
    mem = re.search(r'(\w+)\s+(\w+)\s+\[\[user\(locn%d\)\]\]' % info['locn'], out_s.group(1))
    if not mem: return None
    name = mem.group(2); comp = info['comp']
    body = out_s.group(1).rstrip() + '\n    uint gl_Layer [[render_target_array_index]];\n'
    src = src[:out_s.start(1)] + body + src[out_s.end(1):]
    # 分层管线带 inputPrimitiveTopology，Metal 不许顶点函数再写 [[point_size]]（三角形 / 线类别下报错）：融合变体去掉点大小输出
    # Layered pipelines carry inputPrimitiveTopology and Metal then forbids the vertex function from writing [[point_size]] (error under the triangle / line class): the fused variant drops the point size output
    src = re.sub(r'\n\s*float\s+\w+\s+\[\[point_size\]\];', '', src)
    src = re.sub(r'\n\s*out\.gl_PointSize\s*=\s*[^;]+;', '', src)
    expr = f'as_type<int>(out.{name}.{comp})' if info['bits'] else f'int(out.{name}.{comp})'
    i = src.rfind('    return out;')
    if i < 0: return None
    return src[:i] + f'    out.gl_Layer = uint(max({expr}, 0));\n' + src[i:]

# ---- 6 面几何着色器（UE4 单趟点光源阴影 FOnePassPointShadowDepthGS）→ 实例化 ×6 的顶点变体 ----
# 几何着色器把每个三角形发到立方体 6 个面：面 f 开关 = gp_c3._m0[f].x > 0，位置 = 输入插值（世界坐标）× gp_c3 第 base+stride·f 行起的 4 行（行向量乘法），
# gl_Layer = f，其余输出 = 输入直通 / 裁剪位置副本 / 面号常量（按位或按值）。Metal 没有几何着色器：顶点变体按 instance_id % 6 选面，
# 宿主把实例数与基实例 ×6（反射 gs_faces），几何阶段的 uniform 以 stage=2 的绑定挂进顶点参数块。每面的视锥剔除省掉（裁剪等价）。
# ---- 6-face geometry shader (UE4 one-pass point light shadow FOnePassPointShadowDepthGS) -> vertex variant instanced x6 ----
# The geometry shader emits every triangle to the 6 cube faces: face f enabled = gp_c3._m0[f].x > 0, position = input interpolant (world space) x the 4 rows of gp_c3 from row base+stride*f (row-vector product),
# gl_Layer = f, the other outputs = input pass-through / copy of the clip position / face constant (bits or value). Metal has no geometry shaders: the vertex variant selects the face by instance_id % 6,
# the host multiplies instance count and base instance by 6 (reflection gs_faces), and the geometry stage's uniform is bound into the vertex argument buffer as a stage=2 binding. Per-face frustum culling is dropped (clipping is equivalent).
def gs_cube(glsl):
    """返回 {'pos_in': 输入插值号或 None(gl_Position 直通), 'mat': (base, stride) 或 None, 'enable': 行号或 None, 'outs': {出位置号: {分量: ('in', 插值号, 分量) | ('pos', 分量) | ('face', bits)}}}；认不出返回 (None, 原因)"""
    ins = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) in vec4 (\w+)\[', glsl)}
    outs = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) out vec4 (\w+);', glsl)}
    defs = dict(re.findall(r'(?:float|int|bool) (_\d+) = ([^;]+);', glsl))
    layers = [int(x) for x in re.findall(r'gl_Layer = (\d+);', glsl)]
    if sorted(set(layers)) != [0, 1, 2, 3, 4, 5]: return None, f'层号不是 0–5 常量：{sorted(set(layers))}'
    def block(face):   # 面 face 第一个顶点的赋值（到该面第一次 EmitVertex 为止） | assignments of the first vertex of face `face` (up to that face's first EmitVertex)
        i = glsl.index(f'gl_Layer = {face};'); j = glsl.index('EmitVertex', i)
        # 往前取到上一次 EmitVertex / EndPrimitive / main 起点 | back to the previous EmitVertex / EndPrimitive / start of main
        k = max(glsl.rfind('EmitVertex', 0, i), glsl.rfind('EndPrimitive', 0, i), glsl.rfind('void main', 0, i))
        return dict(re.findall(r'(\w+\.[xyzw]|gl_Layer) = ([^;]+);', glsl[k:j]))
    b0, b1 = block(0), block(1)
    def src_of(expr, depth=0):   # 追到来源：输入插值 / gl_in 位置 / 矩阵乘积 | trace to the source: input interpolant / gl_in position / matrix product
        m = re.match(r'^(\w+)\[\w+\]\.([xyzw])$', expr)
        if m and m.group(1) in ins: return ('in', ins[m.group(1)], m.group(2))
        m = re.match(r'^gl_in\[\w+\]\.gl_Position\.([xyzw])$', expr)
        if m: return ('clip', m.group(1))
        m = re.search(r'gp_c3_1\._m0\[(\d+)\]\.([xyzw])', expr)
        if m: return ('mat', int(m.group(1)), m.group(2), expr)
        if expr in defs and depth < 8: return src_of(defs[expr], depth + 1)
        return None
    def lit(x):
        try: return float(x)
        except ValueError: return None
    def expand(expr, rows, ins_seen, depth=0, seen=None):   # 递归展开临时量：收集乘积链里用到的矩阵行与输入插值 | expand temporaries recursively: collect the matrix rows and input interpolants used in the product chain
        seen = seen if seen is not None else set()
        for r, c in re.findall(r'gp_c3_1\._m0\[(\d+)\]\.([xyzw])', expr): rows.add(int(r))
        for nm in re.findall(r'\b(\w+)\[\w+\]\.[xyzw]', expr):
            if nm in ins: ins_seen.add(ins[nm])
        if depth > 12: return
        for v in re.findall(r'_\d+', expr):
            if v in defs and v not in seen: seen.add(v); expand(defs[v], rows, ins_seen, depth + 1, seen)
    res = {'outs': {}, 'pos_in': None, 'mat': None, 'enable': None}
    rows0 = set()
    for tgt, val in b0.items():
        if tgt == 'gl_Layer': continue
        name, comp = tgt.split('.')
        src = src_of(val) if lit(val) is None else None
        if name == 'gl_Position':
            if src and src[0] == 'clip': continue   # 位置直通 | position pass-through
            if not src or src[0] != 'mat': return None, f'位置 {comp} 来源认不出：{val}'
            ins_seen = set(); expand(val, rows0, ins_seen)
            if len(ins_seen) == 1: res['pos_in'] = ins_seen.pop()
            continue
        if name not in outs: continue
        if lit(val) is not None:
            v1 = lit(b1.get(tgt, 'x')); bits = v1 is not None and 0 < v1 < 1e-30
            res['outs'].setdefault(outs[name], {})[comp] = ('face', bits)
        elif src and src[0] == 'in': res['outs'].setdefault(outs[name], {})[comp] = ('in', src[1], src[2])
        elif src and src[0] == 'mat': res['outs'].setdefault(outs[name], {})[comp] = ('pos', src[2])
        elif src and src[0] == 'clip': res['outs'].setdefault(outs[name], {})[comp] = ('pos', src[1])
        else: return None, f'输出 {tgt} 来源认不出：{val}'
    if rows0:
        rows1 = set()
        for tgt, val in b1.items():
            if tgt.startswith('gl_Position') and lit(val) is None: expand(val, rows1, set())
        if len(rows0) != 4 or len(rows1) != 4: return None, f'矩阵行数 {sorted(rows0)} / {sorted(rows1)}'
        base, stride = min(rows0), min(rows1) - min(rows0)
        if sorted(rows0) != [base, base + 1, base + 2, base + 3] or stride <= 0: return None, f'矩阵行 {sorted(rows0)} 不连续'
        res['mat'] = (base, stride)
        if res['pos_in'] is None: return None, '矩阵乘的输入插值认不出'
        m = re.search(r'0\.0 < gp_c3_1\._m0\[(\d+)\]\.x', glsl)
        if m: res['enable'] = int(m.group(1))
    return res, ''
def fuse_cube(src, info):
    """顶点 MSL → 实例化 ×6 的立方体阴影变体 | vertex MSL -> cube-shadow variant instanced x6"""
    out_s = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not out_s: return None
    members = {int(m.group(3)): m.group(2) for m in re.finditer(r'(\w+)\s+(\w+)\s+\[\[user\(locn(\d+)\)\]\]', out_s.group(1))}
    sig = re.search(r'vertex main0_out main0\((.*?)\)\s*\{', src, re.S)
    if not sig: return None
    params = sig.group(1)
    pre = ''
    if '[[instance_id]]' in params:
        params = params.replace('uint gl_InstanceIndex [[instance_id]]', 'uint gs_iid [[instance_id]]')
        if '[[base_instance]]' in params: pre = '    uint gl_InstanceIndex = gl_BaseInstance + (gs_iid - gl_BaseInstance) / 6u;\n'
        else: pre = '    uint gl_InstanceIndex = gs_iid / 6u;\n'
    else: params = params + ', uint gs_iid [[instance_id]]'
    ids = [int(x) for x in re.findall(r'\[\[id\((\d+)\)\]\]', src)]
    gid = max(ids) + 1 if ids else 0
    ds = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
    if not ds: return None
    if 'struct _16\n' not in src and 'struct _16 ' not in src: return None
    src = src[:ds.end(1)] + f'    constant _16* gs_c3 [[id({gid})]];\n' + src[ds.end(1):]
    # 融合输出结构 | fused output struct
    fo = 'struct main0_fused\n{\n    float4 gl_Position [[position, invariant]];\n    uint gl_Layer [[render_target_array_index]];\n'
    for loc in sorted(info['outs']): fo += f'    float4 g_{loc} [[user(locn{loc})]];\n'
    fo += '};\n\n'
    src = src.replace('vertex main0_out main0(', fo + 'vertex main0_fused main0(', 1)
    src = re.sub(r'vertex main0_fused main0\((.*?)\)\s*\{', lambda m: f'vertex main0_fused main0({params})\n{{\n{pre}', src, count=1, flags=re.S)
    src = re.sub(r'\n\s*float\s+\w+\s+\[\[point_size\]\];', '', src); src = re.sub(r'\n\s*out\.gl_PointSize\s*=\s*[^;]+;', '', src)
    body = '    main0_fused fo = {}; uint gs_face = gs_iid % 6u; constant float4* gs_m = (*spvDescriptorSet0.gs_c3)._m0;\n'
    if info['mat']:
        base, stride = info['mat']
        pm = members.get(info['pos_in'])
        if pm is None: return None
        # 顶点没写该插值的 .w（UE4 阴影顶点只写世界坐标 xyz）：硬件上未写的插值分量 w 缺省 1，翻译出的 MSL 却把输出清零 → 补 1，否则矩阵的平移行丢掉
        # The vertex stage does not write .w of that interpolant (UE4 shadow vertex shaders write only world xyz): on hardware an unwritten w defaults to 1 while the translated MSL zero-initialises outputs -> use 1, otherwise the matrix's translation row is lost
        pw = f'out.{pm}.w' if re.search(r'\bout\.' + pm + r'\.w\s*=', src) or re.search(r'\bout\.' + pm + r'\s*=', src) else '1.0'
        body += f'    {{ float4 p = float4(out.{pm}.xyz, {pw}); uint r = {base}u + {stride}u * gs_face; fo.gl_Position = p.x * gs_m[r] + p.y * gs_m[r + 1u] + p.z * gs_m[r + 2u] + p.w * gs_m[r + 3u]; }}\n'
        if info['enable'] is not None: body += f'    if (gs_m[{info["enable"]}u + gs_face].x <= 0.0) fo.gl_Position = float4(0.0, 0.0, 2.0, 1.0);\n'   # 该面关着：裁掉 | face disabled: clipped away
    else: body += '    fo.gl_Position = out.gl_Position;\n'
    for loc, comps in sorted(info['outs'].items()):
        for c, sv in sorted(comps.items()):
            if sv[0] == 'face': body += f'    fo.g_{loc}.{c} = {"as_type<float>(int(gs_face))" if sv[1] else "float(gs_face)"};\n'
            elif sv[0] == 'pos': body += f'    fo.g_{loc}.{c} = fo.gl_Position.{sv[1]};\n'
            else:
                m = members.get(sv[1]); body += f'    fo.g_{loc}.{c} = {"out." + m + "." + sv[2] if m else "0.0"};\n'
    body += '    fo.gl_Layer = gs_face;\n    return fo;\n'
    i = src.rfind('    return out;')
    if i < 0: return None
    return src[:i] + body + src[i + len('    return out;\n'):]

def finish(key, w, j, c1):
    name, k = loc[key]
    res = json.load(open(os.path.join(w, key + '.res.json')))
    msl = os.path.join(w, key + '.metal'); air = msl[:-6] + '.air'; lib = os.path.join(out, key + '_b.metallib')
    if j['stage'] == 'gs':   # 几何着色器：Metal 没有，只做离线分析（结果给配对的顶点着色器用；无 metallib） | Geometry shader: none on Metal, offline analysis only (result used by the paired vertex shader; no metallib)
        info = gs_layer(key, w)
        return ('ok', '', {'key': key + '_b', 'stage': 'gs', 'bindings': [], 'drv_unfillable': [], 'gs': info}) if info else ('几何着色器不是直通写层号', gs_layer_why(key, w), None)
    r = subprocess.run(['spirv-cross', *XC, os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
    if r.returncode: return 'MSL 失败', r.stderr.strip()[:160], None
    src = open(msl).read()
    fused = None
    if j['stage'] == 'vs' and key in PAIRS_GS:   # 顶点 + 直通写层号的几何：顶点着色器补写 [[render_target_array_index]]（分层目标），出成变体 <顶点键>+<几何键>_b
        for gk in sorted(PAIRS_GS[key]):
            info = gs_layer(gk, w)
            if not info: continue
            patched = fuse_cube(src, info) if info['kind'] == 'cube' else fuse_layer(src, info)
            if not patched: return '顶点融合几何失败', f'{gk}: {info}', None
            fused = (gk, patched, info['kind'])
            break
    if fused:
        gk, patched, gkind = fused
        fkey = key + '+' + gk
        fmsl = os.path.join(w, fkey + '.metal'); open(fmsl, 'w').write(patched)
        fair = fmsl[:-6] + '.air'; flib = os.path.join(out, fkey + '_b.metallib')
        try:
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fpreserve-invariance', fmsl, '-o', fair], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', fair, '-o', flib + '.tmp'], check=True); os.replace(flib + '.tmp', flib)
        except subprocess.CalledProcessError as e: return '融合变体 metal 失败', str(e)[:160], None
        clone(fmsl, os.path.join(out, fkey + '_b.metal'))
        fentry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', patched)
        frefl = {'key': fkey + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': 'vs',
                 'entry': fentry.group(2) if fentry else 'main0', 'msl_entry_kind': 'vertex', 'threadgroup_size': None, 'c1': c1.hex(), 'notes': res['notes'] + [f'融合几何着色器 {gk}（直通写层号）'],
                 'drv_reads': drv_reads(patched), 'drv_unfillable': unfillable(drv_reads(patched)), 'bindings': reflect(key, 'vs', patched, res),
                 'vertex_inputs': [{'location': int(m.group(2)), 'msl_type': m.group(1)} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', patched)],
                 'raster': raster_facts('vs', patched), 'writes_layer': 1, 'fused_gs': gk}
        if gkind == 'cube':   # 几何阶段的 uniform（gp_c3 = 几何阶段 uniform 槽 0）以 stage=2 挂进顶点参数块；宿主把实例数 ×6 | the geometry stage's uniform (gp_c3 = geometry stage uniform slot 0) bound into the vertex argument buffer as stage=2; the host multiplies instances by 6
            gid = int(re.search(r'gs_c3 \[\[id\((\d+)\)\]\]', patched).group(1))
            frefl['bindings'] = [b for b in frefl['bindings'] if b['msl_name'] != 'gs_c3'] + [{'msl_name': 'gs_c3', 'msl_type': 'constant _16*', 'arg_id': gid, 'kind': 'cbuf', 'reg': 0, 'stage': 2}]
            frefl['gs_faces'] = 6
        write_refl(fkey, frefl)
    try:
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fpreserve-invariance', msl, '-o', air], check=True)
        tmp_lib = lib + '.tmp-' + str(os.getpid())
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', tmp_lib], check=True)
        os.replace(tmp_lib, lib)
    except subprocess.CalledProcessError as e:
        return 'metal 失败', str(e)[:160], None
    clone(msl, os.path.join(out, key + '_b.metal'))
    if IOSOUT:   # iOS：同一份 MSL 走 iOS 的唯一编译口径（dm/ios/tools/ios_metal.py）出 iphoneos metallib | iOS: the same MSL goes through iOS's single compile convention (dm/ios/tools/ios_metal.py) to produce an iphoneos metallib
        ilib = os.path.join(IOSOUT, key + '_b.metallib')
        try:
            ios_metal.compile(msl, msl[:-6] + '.ios.air', ilib + '.tmp')
            os.replace(ilib + '.tmp', ilib)
        except subprocess.CalledProcessError as e: return 'iOS metal 失败', str(e)[:160], None
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src)
    dr = drv_reads(src)
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': j['stage'],
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None,
            'threadgroup_size': j.get('local'), 'c1': c1.hex(), 'notes': res['notes'] + (['无配对 PC 反射，查询按缺省'] if j.get('pc_missing') else []), 'drv_reads': dr, 'drv_unfillable': unfillable(dr),
            'bindings': reflect(key, j['stage'], src, res),
            'vertex_inputs': [{'location': int(m.group(2)), 'msl_type': m.group(1)} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', src)],
            'raster': raster_facts(j['stage'], src),
            # 顶点阶段写层号（分层目标：宿主把 3D / 数组贴图整体挂成 renderTargetArrayLength，管线带 inputPrimitiveTopology）
            # The vertex stage writes the layer index (layered targets: the host attaches the whole 3D / array texture as renderTargetArrayLength and the pipeline carries inputPrimitiveTopology)
            'writes_layer': int('[[render_target_array_index]]' in src)}
    write_refl(key, refl)
    fetch_finish(key, src, refl)
    return 'ok', '', refl

keys = sorted(want) if want else sorted(loc)
NJ = int(os.environ.get('J', '8'))
os.makedirs(out, exist_ok=True)
w = tempfile.mkdtemp(prefix='dmsh_', dir=os.environ.get('WORK'))
packs, jobs, c1s = {}, {}, {}
digests, rule_fps, cache_reasons, cache_hits = {}, {}, {}, set()
for key in keys:
    name, k = loc[key]
    if name.startswith('main.rodata@'):
        if 'main' not in packs:
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../verify'))
            try: import nsoimg
            except ImportError as e: sys.exit(f'【红】解 main NSO 要 lz4（{e}）：用 $DM_ROOT/build/cache/dm/venv/bin/python 跑')
            packs['main'] = nsoimg.load(MAIN)
        rec = main_rec(packs['main'], k)
        if hashlib.sha256(rec[rec.find(bytes.fromhex('34127698')):][:0x880]).hexdigest()[:16] != key: sys.exit(f'【红】{name} 控制段哈希与映射表键 {key} 不符（MAIN={MAIN}）')
    else:
        if name not in packs: packs[name] = records(open(os.path.join(dm_dir, name), 'rb').read())
        rec = packs[name][k]
    digests[key] = shader_key(key, name, rec)
    rule_fps[key] = hashlib.sha256(shader_rule_source(name)).hexdigest()
    hit, cache_reasons[key] = cache_lookup(key, digests[key], rule_fps[key], CODE_FP)
    if hit: cache_hits.add(key)
    else: jobs[key], c1s[key] = job_for(key, name, rec, w)
misses = [key for key in keys if key not in cache_hits]
chunks = [misses[i::NJ] for i in range(NJ)]
def xl(ch):
    jp = os.path.join(w, f'job_{abs(hash(ch[0]))}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
    return subprocess.run([XLATE, jp, w], capture_output=True, text=True)
with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, [c for c in chunks if c]))
xerr = {}
for r in xr:
    for ln in r.stderr.splitlines():
        m = re.match(r'【红】(\w+)：(.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]
def fin(key):
    if key in cache_hits:
        refl = json.load(open(os.path.join(out, key + '_b.json')))
        src = open(os.path.join(out, key + '_b.metal')).read() if refl['stage'] == 'ps' else ''
        return key, 'ok', '', fetch_finish(key, src, refl)
    if key in xerr or not os.path.exists(os.path.join(w, key + '.spv')): return key, '翻译失败', xerr.get(key, '无 spv'), None
    result = (key,) + finish(key, w, jobs[key], c1s[key])
    if result[1] == 'ok' and jobs[key]['stage'] != 'gs': publish_cache(key, digests[key], rule_fps[key], CODE_FP)
    return result
with cf.ThreadPoolExecutor(NJ) as ex: res = list(ex.map(fin, keys))
shutil.rmtree(w, ignore_errors=True)
st = collections.Counter(r[1] for r in res)
print(f'着色器 {len(res)}：成功 {st["ok"]}，' + '，'.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
print(f'前置缓存：命中 {PRE_STATS["hit"]}，重出 {PRE_STATS["miss"]}')
for reason in ('新键', '规则变', '代码指纹变化：全部重出'):
    if PRE_STATS[reason]: print(f'  {reason} {PRE_STATS[reason]}')
print(f'变体缓存：生成 {BF_STATS["generated"]}，命中 {BF_STATS["hit"]}，重出 {BF_STATS["miss"]}，编译 {BF_STATS["compile"]}')
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
ok = [r[3] for r in res if r[1] == 'ok']
rd = [x for x in ok if x['drv_reads']]
print(f'运行时读驱动段的着色器 {len(rd)}（c0 {sum("0" in x["drv_reads"] for x in rd)}，c2 {sum("2" in x["drv_reads"] for x in rd)}）；读到回放层填不了的 {sum(bool(x["drv_unfillable"]) for x in ok)}')
for e, n in collections.Counter(u for x in ok for u in x['drv_unfillable']).most_common(15): print(f'  填不了 {n:5d}  {e}')
SLOTK = {'cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf', 'ubo_ptr'}
noreg = [x for x in ok if any(b['kind'] in SLOTK and b.get('reg') is None for b in x['bindings'])]
unk = [x for x in ok if any(b['kind'] == '?' for b in x['bindings'])]
print(f'绑定没解出槽号的着色器 {len(noreg)}，种类认不出的 {len(unk)}（运行时加载会报红）')
for e, n in collections.Counter(f"{b['kind']} {b['msl_name'][:28]}" for x in noreg + unk for b in x['bindings'] if b['kind'] == '?' or (b['kind'] in SLOTK and b.get('reg') is None)).most_common(8): print(f'  {n:5d}  {e}')
nt = collections.Counter(nn.split('（')[0] for x in ok for nn in x['notes'])
for e, n in nt.most_common(8): print(f'  查询无 PC 作答 {n:5d}  {e}')
json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
