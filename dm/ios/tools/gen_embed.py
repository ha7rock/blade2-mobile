# 生成内嵌汇编：把改写后的模块映像编进可执行文件，代码只进 __TEXT 一个执行段（苹果 ITMS-90999：只许一个带执行权限的段）。
#   __TEXT,__dm<名>x0   原 text：[0, 圆整16K(TEXT_END))（含 ro 首页的一部分，只读）
#   __DM<名>W,__img     RW 段：[圆整16K(TEXT_END), PATCH_OFF)，ro 余部 + data + bss（重定位只落这里）
#   __TEXT,__dm<名>x1   补丁区：[PATCH_OFF, 圆整16K(文件尾))
# PATCH_OFF < 0（映像超过 128 MB、b 够不到映像后面，改写器把补丁区放在映像前面；产物文件 = 补丁区 + 映像，见 dm/tools/patch_nso.py）：
#   __TEXT,__dm<名>x1   补丁区：文件 [0, -PATCH_OFF)
#   __TEXT,__dm<名>x0   原 text：文件 [-PATCH_OFF, -PATCH_OFF + 圆整16K(TEXT_END))
#   __DM<名>W,__img     RW 段：其余到 -PATCH_OFF + 圆整16K(映像长)（键页在映像末尾，落在这里）
#   装载器按 x1 的长度认出这种布局（z1 == -PATCH_OFF），把映像基址放在补丁区之后。
# 三块在可执行文件里不相邻；运行时由装载器（loader.c iOS 分支）保留一块连续地址，按原相对位置 vm_remap 回去（代码页共享已签名页，W 写时复制），
# 所以运行时布局与原映像逐字节相同：原代码与 data 之间的 PC 相对寻址、补丁区的相对跳转、宿主按「基址 + 原偏移」的引用都不用动。
# 用法：gen_embed.py <产物目录> <输出 .S> <模块名...>；链接参数打印到 stdout（W 段的 -segprot）
# Generate the embedded assembly: compile the rewritten module image into the executable, with code going into the single executable segment __TEXT only (Apple ITMS-90999: only one segment with execute permission is allowed).
#   __TEXT,__dm<name>x0   original text: [0, round16K(TEXT_END)) (includes part of the first ro page, read-only)
#   __DM<name>W,__img     RW segment: [round16K(TEXT_END), PATCH_OFF), rest of ro + data + bss (relocations land only here)
#   __TEXT,__dm<name>x1   patch area: [PATCH_OFF, round16K(end of file))
# PATCH_OFF < 0 (image over 128 MB, b cannot reach past it, the rewriter puts the patch area before the image; build output file = patch area + image, see dm/tools/patch_nso.py):
#   __TEXT,__dm<name>x1   patch area: file [0, -PATCH_OFF)
#   __TEXT,__dm<name>x0   original text: file [-PATCH_OFF, -PATCH_OFF + round16K(TEXT_END))
#   __DM<name>W,__img     RW segment: the rest up to -PATCH_OFF + round16K(image length) (the key page at the image end lands here)
#   The loader recognises this layout by the length of x1 (z1 == -PATCH_OFF) and places the image base after the patch area.
# The three pieces are not adjacent in the executable; at run time the loader (loader.c iOS branch) reserves one contiguous address range and vm_remaps them back at their original relative positions (code pages share the signed pages, W is copy-on-write),
# so the run-time layout is byte-identical to the original image: PC-relative addressing between original code and data, relative jumps in the patch area, and the host's "base + original offset" references all need no change.
# Usage: gen_embed.py <build output dir> <output .S> <module name...>; link flags are printed to stdout (-segprot for the W segment)
import os, sys
out, dst, names = sys.argv[1], sys.argv[2], sys.argv[3:]
PG = 0x4000
def r16(x): return (x + PG - 1) & ~(PG - 1)
asm = ['// 由 dm/ios/tools/gen_embed.py 生成，勿手改']
flags = []
for n in names:
    hdr = dict(l[2:].split() for l in open(f'{out}/{n}.fixups.tsv'))
    te, po = int(hdr['TEXT_END'], 16), int(hdr['PATCH_OFF'], 16)
    ko, ks = int(hdr['KEY_PAGE_OFF'], 16), int(hdr['KEY_PAGE_SIZE'], 16)
    bin_ = os.path.abspath(f'{out}/{n}.patched.bin'); sz = os.path.getsize(bin_)
    assert po % PG == 0 and ko % PG == 0 and ks == PG
    if po < 0:
        # 补丁区在映像前面：文件偏移 = 映像偏移 + pre | patch area before the image: file offset = image offset + pre
        pre = -po; a = r16(te); img = sz - pre; end = pre + r16(img)
        assert img > 0 and a <= ko and ko + ks <= r16(img), f'{n}: 键页 {ko:#x} 不在 W 段里 | key page not inside the W segment'
        pieces = ((f'__TEXT,__dm{n}x1', 0, pre), (f'__TEXT,__dm{n}x0', pre, pre + a), (f'__DM{n}W,__img', pre + a, end))
    else:
        a, end = r16(te), r16(sz)
        assert a <= ko and ko + ks <= po
        pieces = ((f'__TEXT,__dm{n}x0', 0, a), (f'__DM{n}W,__img', a, po), (f'__TEXT,__dm{n}x1', po, end))
    for sect, lo, hi in pieces:
        assert all(len(x) <= 16 for x in sect.split(','))
        fill = min(hi, sz) - lo
        asm += [f'    .section {sect}', '    .p2align 14', f'    .incbin "{bin_}", {lo:#x}, {fill:#x}']
        if hi - lo > fill: asm.append(f'    .space {hi - lo - fill:#x}')
        asm.append('    .p2align 14')   # 节尾补到 16K：vm_remap 按整页取 | Pad the section end to 16K: vm_remap takes whole pages
    flags.append(f'-Wl,-segprot,__DM{n}W,rw,rw')
open(dst, 'w').write('\n'.join(asm) + '\n')
print(' '.join(flags))
