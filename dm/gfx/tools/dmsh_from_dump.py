#!/usr/bin/env python3
# 把宿主转储的着色器（DM_NVN_SHDUMP 目录里的 <键>.ctl / <键>.code）整理成 dmsh_build.py 认的输入：
#   <包目录>/{vs,ps,cs}_<游戏>.bin —— 与 Switch 着色器包同布局的记录流（每条：'BTCDBTCD' 标记 + 填充到 +0x38 + 代码区 + 控制段 0x880），
#   nvn/shader_map.tsv —— 每条一行（包名、包内序号、阶段词、键、占位 PC 哈希…），运行时只用「键 → 阶段词」，
#   nvn/shader_map.tsv.conflicts.tsv —— 空表头。
# 阶段：图形着色器从 SPH 头读（1 顶点、5 像素）；没有 0x30 头的是计算着色器。
# 用法：dmsh_from_dump.py <转储目录> <包输出目录> [游戏名=blade]   然后：dmsh_chain.py --header → 重编宿主 → dmsh_build.py <包输出目录> <缓存目录>
# Turns the host's shader dump (<key>.ctl / <key>.code in the DM_NVN_SHDUMP directory) into the input dmsh_build.py understands:
#   <pack dir>/{vs,ps,cs}_<game>.bin -- record streams laid out like a Switch shader package (each: 'BTCDBTCD' marker + padding to +0x38 + code area + 0x880 control section),
#   nvn/shader_map.tsv -- one row per record (package, index, stage word, key, placeholder PC hash ...); at runtime only "key -> stage word" is used,
#   nvn/shader_map.tsv.conflicts.tsv -- empty with a header.
# Stage: graphics shaders from the SPH header (1 vertex, 5 pixel); code without the 0x30 header is a compute shader.
# Usage: dmsh_from_dump.py <dump dir> <pack out dir> [game=blade]   then: dmsh_chain.py --header -> rebuild the host -> dmsh_build.py <pack out dir> <cache dir>
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from maxwell_refs import sph_stage
dump, packdir = sys.argv[1], sys.argv[2]; game = sys.argv[3] if len(sys.argv) > 3 else 'blade'
NVN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../nvn')
WORD = {'vs': '顶点', 'ps': '像素', 'cs': '计算', 'gs': '几何'}
# 几何着色器（阶段 4）也进包：Metal 没有几何着色器，dmsh_build.py 只把它翻成 GLSL 做离线分析（直通 + 写层号 → 融进配对的顶点着色器），宿主不加载
# Geometry shaders (stage 4) go into the package too: Metal has no geometry shaders, dmsh_build.py only translates them to GLSL for offline analysis (pass-through + layer write → fused into the paired vertex shader); the host does not load them
recs = {'vs': [], 'ps': [], 'cs': [], 'gs': []}; bad = 0
for fn in sorted(os.listdir(dump)):
    if not fn.endswith('.ctl'): continue
    key = fn[:-4]; ctl = open(os.path.join(dump, fn), 'rb').read(); code = open(os.path.join(dump, key + '.code'), 'rb').read()
    if len(ctl) != 0x880 or hashlib.sha256(ctl).hexdigest()[:16] != key or ctl[:4] != bytes.fromhex('34127698'): print('跳过（控制段不对）', key); bad += 1; continue
    st = sph_stage(code) or ('cs' if code[:4] != bytes.fromhex('78563412') else None)
    if not st: print('跳过（阶段不明）', key); bad += 1; continue
    recs[st].append((key, code, ctl))
os.makedirs(packdir, exist_ok=True)
rows = []
for st, lst in recs.items():
    if not lst: continue
    name = f'{st}_{game}.bin'; out = bytearray()
    for i, (key, code, ctl) in enumerate(lst):
        out += b'BTCDBTCD' + bytes(0x38 - 8) + code + ctl
        rows.append(f'{name}\t{i}\t{WORD[st]}\t{key}\t-\t-\t-\t-\t-\t-\t-\t-\t-\n')
    open(os.path.join(packdir, name), 'wb').write(out)
with open(os.path.join(NVN, 'shader_map.tsv'), 'w') as f:
    f.write(f'# kind: evidence  {game}：宿主转储的着色器（dm/gfx/tools/dmsh_from_dump.py）；运行时只用「键 → 阶段」 | shaders dumped by the host; at runtime only key -> stage is used\n')
    f.write('package\tindex\tstage\tdm_ctl_sha16\tpc_dxbc_sha16\tmetallib\tpc_outputs\tdm_outputs\tdm_ubo\tdm_tex\tdm_samp\tdm_ssbo\tsubset_ok\n')
    f.writelines(rows)
# 计算着色器的线程组大小 / 共享内存字节数在 NVN 控制段里（实测 Blade 的 4 个：+0x748 起 3 个 u32 = 线程组，+0x754 = 共享内存字节；图形着色器这里全 0）
# → 写成作答表 nvn/shader_facts.tsv（翻译器缺省的 48 KB 共享内存超过 Metal 上限，必须给真值）
# The compute threadgroup size / shared-memory bytes live in the NVN control section (measured on Blade's 4: 3 u32 from +0x748 = threadgroup, +0x754 = shared bytes; all 0 for graphics shaders)
# → written as the answer table nvn/shader_facts.tsv (the translator's default of 48 KB shared memory exceeds Metal's limit, so the real value is required)
import json
# 贴图类型作答：宿主 DM_NVN_TEXFACTS 记录的「键 \t 槽 \t 类型词」（缓冲 / 3D / 立方体…；翻译器缺省按 2D，类型不符 Metal 读到 0——UE4 蒙皮的骨骼矩阵就是缓冲贴图）
# 同一槽见过多种类型时取最后一种并提示
# Texture type answers: "key \t slot \t type word" recorded by the host via DM_NVN_TEXFACTS (buffer / 3D / cube ...; the translator defaults to 2D and Metal reads 0 when the type does not match -- UE4 skinning's bone matrices are a texture buffer)
# When one slot was seen with several types the last one wins with a notice
texfacts = {}
tf = os.environ.get('DM_TEXFACTS', os.path.join(os.environ.get('DM_ROOT', os.path.expanduser('~/blade2-work')), 'texfacts.tsv'))
if os.path.exists(tf):
    for ln in open(tf):
        c = ln.rstrip('\n').split('\t')
        if len(c) == 3:
            prev = texfacts.setdefault(c[0], {}).get(c[1])
            if prev and prev != c[2]: print(f'贴图类型作答：{c[0]} 槽 {c[1]} 既见 {prev} 又见 {c[2]}，取 {c[2]}')
            texfacts[c[0]][c[1]] = c[2]
with open(os.path.join(NVN, 'shader_facts.tsv'), 'w') as f:
    f.write('# 翻译查询的作答表（dmsh_from_dump.py：计算线程组 / 共享内存从控制段解出；贴图类型来自宿主 DM_NVN_TEXFACTS 记录）| answer table (dmsh_from_dump.py: compute threadgroup / shared memory decoded from the control sections; texture types from the host DM_NVN_TEXFACTS record)\n')
    for st in ('vs', 'ps', 'cs'):
        for key, code, ctl in recs[st]:
            facts = {'attr': {}, 'fout': {}, 'img': {}, 'tex': texfacts.get(key, {})}
            if st == 'cs':
                x, y, z, shared = struct.unpack_from('<3I', ctl, 0x748) + struct.unpack_from('<I', ctl, 0x754)
                facts.update({'local': [x, y, z], 'shared': (shared + 15) // 16 * 16})
            elif not facts['tex']: continue
            f.write(key + '\t' + json.dumps(facts, sort_keys=True, separators=(',', ':')) + '\n')
print(f'贴图类型作答：{sum(1 for k in texfacts if any(k == r[0] for st in recs for r in recs[st]))} 个着色器有记录（{tf}）')
open(os.path.join(NVN, 'shader_map.tsv.conflicts.tsv'), 'w').write('# 无键冲突（转储按键去重） | no key conflicts (the dump is deduplicated by key)\ndm_ctl_sha16\tchosen\treason\n')
print(f'{game}: 顶点 {len(recs["vs"])} 像素 {len(recs["ps"])} 计算 {len(recs["cs"])}，跳过 {bad}；包在 {packdir}，映射表已写到 nvn/shader_map.tsv')
