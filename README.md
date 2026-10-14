# Blade II Mobile — 把 Switch 版《Blade II: The Return of Evil》搬到 iOS / macOS

这是对 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（把 Switch 版《暗黑破坏神 II：狱火重生》原生跑在 iPhone 上的开源兼容层）的**方法论拆解**，以及把同一套方法**举一反三**用到本地另一款 Switch 游戏（Blade II，UE4 4.17）上的工程。
`dm/` 是 d2r-mobile 的 `dm/` 目录原样拷贝（MIT，见 `LICENSE.d2r-mobile`）加上 Blade 的改动；改动都能用 `git log -p` 看到。

状态（2026-10-14）：**分析完成、改写器已扩展到能改写 Blade 的全部三个模块、Mac 宿主编译通过**；运行阶段的进展见文末「现状与下一步」。
游戏内容（程序、资源、改写产物）一律不进仓库。

---

## 一、d2r-mobile 的核心思想

一句话：**不模拟 CPU，不翻译指令——Switch 和 iPhone 都是 AArch64，游戏程序本身可以原样执行；只把它对 Horizon 操作系统的依赖换成一个小宿主。**

它和模拟器（Ryujinx / yuzu）的根本区别：

| | 模拟器 | d2r-mobile |
|---|---|---|
| CPU | 解释 / JIT 翻译每条指令 | 原生执行，一字节不动（除几类不能执行的指令） |
| 内核 | 完整模拟 Horizon 内核 | 只把 SVC 换成宿主函数（线程 = pthread，内存 = mmap） |
| 系统服务 | 模拟 IPC 服务进程 | 在 **SDK 函数接口处**按名字接管（HLE_FORCE） |
| 图形 | 模拟 Maxwell GPU | 在 **NVN API 层**接管，录制命令、Metal 回放 |
| 着色器 | 运行时翻译 | 离线翻译（Maxwell 机器码 → SPIR-V → MSL → metallib） |
| iOS JIT | 需要（iOS 不允许） | 不需要：改写在离线做，代码段作为已签名段编进 App |

### 1. 程序改写（`dm/tools/patch_nso.py`）

AArch64 上只有四类东西在苹果内核下不能原样跑，改写器用线性扫描找到它们，每处换成一条 `b` 跳进映像后面的「补丁区」，补丁块做完等价动作再跳回：

| 原指令 | 为什么不能跑 | 换成 |
|---|---|---|
| `x18` 的读写 | 苹果内核把 x18 当平台寄存器随时清零 | 读写线程槽 K18（`tpidrro_el0` + 键页里的键号×8 = pthread TSD 槽） |
| `mrs/msr tpidr_el0`、`mrs tpidrro_el0` | 游戏用它找 TLS / Horizon 线程区；苹果内核另有用途 | 读写线程槽 KTLS / KTLR |
| `svc #n` | 没有 Horizon 内核 | 跳「宿主门」（`entry.S` 的 `dm_gate`），按返回点查表得知是哪号 SVC |
| `mrs cntpct_el0` | 没有 19.2 MHz 系统计数器 | 也走宿主门，宿主换算 |

关键设计：
- **键页**：补丁块不写死 TSD 键号，而是从映像末尾一页只读「键页」里取，装载器填入实际键号——运行时零代码改写，iOS 才能把代码段原样签名。
- **窗口**：`ldxr/stxr` 独占访问序列里的 x18 不能单条替换（会破坏独占监视器），整段作为窗口一起改写。
- **跳转表 `br x18`**：目标必须离线枚举，补丁块按索引落到「落地垫」再恢复借用的寄存器。
- **剪口（CUTS）/ 观测门（OBS）/ 参数位（PARAMS）**：按 NSO 构建号登记的、有意的语义改动（离线补丁、宿主回调、分辨率参数），每条带原字节断言。
- **验证**：`dm/verify/equiv.py` 用 Unicorn 对每个改写点做「原指令 vs 补丁块」随机状态等价试验；`structck.py` 用独立反汇编器核对结构。

### 2. 装载与启动（`dm/host/loader.c`、`main.c`）

宿主扮演 rtld：映射 `main` 与 `sdk` 两个 NSO 的改写映像，建线程槽、登记门、做 ELF 重定位（RELATIVE / ABS64 / GLOB_DAT / JUMP_SLOT），按名字解析导入；然后**照抄 `nn::init::Start` 的真实次序**初始化 SDK 运行时（`_init_libc0 → nnosInitialize → _init_libc1 → nninitInitializeSdkModule → nninitStartup → _init_libc2 → 各模块 DT_INIT`），最后进 `nnMain`。每个游戏线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。

### 3. 三层接管（`dm/host/hle*.c`）

| 层 | 做法 |
|---|---|
| 内核 SVC | `hle_svc.c`（地址空间 / 堆 / 查询）、`hle_thread.c`（线程、锁、条件变量、地址仲裁、事件——按 Atmosphère mesosphère 的语义） |
| C 库 | `hle_table.c`：调用约定一致、无结构布局依赖的函数直通苹果 libc |
| 系统服务 | `HLE_FORCE("<改编名>", 理由, ...)`：SDK 里走 IPC 的服务客户端（fs / hid / audio / oe / account / nifm / vi …）在 SDK 接口处整个替换，`nn::os` 原样运行。表里有、没人引用的条目装载即报错——清单必须精确 |

没实现的导入接「记名桩」：被调到打一行 `UNIMPL 名字` 返回 0。

### 4. 图形（`dm/gfx`）

`main` 只导入一个 `nvnBootstrapLoader`，宿主接管它，之后游戏按名字取的 523 个 NVN 函数全部指向宿主：有 CPU 可见结果的按语义实现，只改 GPU 状态的录成命令、提交时用 Metal 回放。内存池 = 零拷贝 `MTLBuffer`（GPU 地址 = CPU 地址，只支持统一内存）。贴图从 Tegra block-linear 布局反 swizzle。着色器离线翻译：Ryujinx 的翻译器（C#）出 SPIR-V → spirv-cross 出 MSL → `xcrun metal` 编译；运行时按控制段 sha256 查库。

### 5. iOS 壳（`dm/ios`）

改写映像用 `gen_embed.py` 变成汇编内嵌进可执行文件（原 text 与补丁区是 `__TEXT` 的节、可写部分是独立段），装载器 `vm_remap` 回原相对布局——不复制代码、不改页权限。触屏层画在原版 UI 之上，命中由宿主按游戏面板树作答。

---

## 二、Blade II 的分析（本地 XCI）

| 项 | 值 |
|---|---|
| 游戏 | Blade II: The Return of Evil，title `01009cc00e224000`，更新 v131072 |
| 引擎 | **Unreal Engine 4.17** + PhysX 3.4（`++UE4+Release-4.17`、`D:\ProXTest\extra\Blade2toNS\...` 构建路径） |
| SDK | NintendoSDK 7.4（本体 6.5.2） |
| 模块 | `main` 48 MB（text 70 MB / rodata 37 MB / data 5 MB / bss 24 MB，解压后 131 MB）、`sdk` 13 MB、`subsdk0` 7 MB（任天堂 movie 库 + android 缓冲队列 + mkvparser） |
| 资源 | RomFS 4.1 GB：`BladeII/Content/Paks/*.pak`（两个 1.9 GB）+ 559 MB mp4 过场 |
| NVN | 取址 512 个函数，**静态调用 181 个**；其中 **37 个 D2R 从未调用**（`nvnCommandBufferDrawElements`、`BindTexture`、`BindVertexBuffer`、`SetViewport`、`nvnBuffer*`、`nvnQueueAcquireTexture`、`FenceSync`…），d2r 图形层没有实现 |
| 导入 | `main` 导入 608 个符号：d2r 宿主已接 153 个；其余 455 里 `nn::os` / libc++ / `nn::mem` / `nn::crypto` 等 SDK 用户态部分原样运行，真要新接的是 `nn::audio` 音频**渲染器**（39 个，D2R 只用 AudioOut）、联网一组、`nn::hid` Joy-Con / 六轴、`nn::time` 时区规则、`nn::swkbd` 配置、`movie` 媒体信息 6 个 |
| SVC | `sdk` 里 60 处；d2r 实现 35 号，其余（IPC / 共享内存 / 端口类）在服务被接管后不会到达 |

机器可读的清单：`docs/main_imports_vs_d2r.tsv`（每个导入：命名空间、是否 d2r 已接、实现文件）。
分析脚本在 `tools/`：`nso_syms.py`（NSO 动态符号）、`import_coverage.py`、`main_imports_vs_d2r.py`、`br18_sites.py`（跳转表写法分类）、`dm/gfx/tools/nvn_slots.py`（已参数化，`NVN_LOADER=0x33e6400 NVN_TEXT_END=0x42bcef0`）。

---

## 三、为了 Blade II 改了什么（举一反三的部分）

### 改写器（`dm/tools/patch_nso.py`）

直接拿 d2r 的改写器跑 Blade 的三个模块：`sdk` 一次通过；`subsdk0` 和 `main` 撞上三个 D2R 二进制里没有的情况，逐个补上：

1. **第二种跳转表写法**（UE4 / 较新 clang）：`adrp xB; add xB; ldrsw xT,[xB,xI,lsl #2]; add x18,xT,xB; br x18`，表项是 32 位相对偏移，到 `br` 时槽里已是绝对目标。新增 `site_br_abs`：离线按守门的 `cmp wI,#K` 读出 K+1 个目标，补丁块把槽值与每个目标比对后落到对应尾块恢复寄存器再跳。`sites.tsv` 以 `JA` 行登记。
2. **`blr x18`**（函数指针在 x18 里）：新增 `site_blr`。调用点上 x16/x17 本就可破坏（AAPCS64 IP0/IP1），直接借用。
3. **`ldr x18, <literal>`**：新增 `site_ldr_lit`。
4. **映像超过 128 MB**：`b` 的射程是 ±128 MB，补丁区再接在映像后面就够不着。改成**两遍发射**（第一遍只量长度），映像太大时把补丁区放到映像**前面**（`PATCH_OFF < 0`，产物文件 = 补丁区 + 映像），装载器把映像基址放在补丁区之后（`loader.c`，`patch_off` 改为有符号）。小模块布局不变，`sdk` 的产物逐字节与原改写器相同。

### 宿主（`dm/host`）

- `Makefile.blade`：只编通用部分（装载 / SVC / 服务接管 / 图形层），不带任何 `dm_*.c` 暗黑功能文件、`diag.c`、`touch_mac.c`。
- `blade_stubs.c`：通用文件里残留的 100 个 D2R 功能钩子的中性实现（过滤器原样放行、查询答「没有」、D2R 剪口才会到的宿主调用一律中止）。清单由 `tools/host_trial_build.sh` 的链接缺符号列表得出。
- `hle_blade.c`：Blade 多用到的 SDK 接口——movie 媒体信息、hid 六轴 / 握持 / 手柄支持小程序、oe、time 时区规则、swkbd 配置、vi 缩放、web / err / pctl / diag，**音频渲染器**（第一版只维持协议：5 ms 定时触发系统事件、所有请求成功、不出声），联网一组一律「不可用」。
- `hle_hid.c`：Joy-Con 双持 / 左 / 右三种样式答「没接」；`hle_fs.c`：`DeleteDirectory`、`CommitSaveData`、`GetDirectoryEntryCount`。
- `tools/extract_game.sh`：按本游戏的 title / 版本从 XCI 取文件（prod.keys 里格式不对的行先过滤）。

---

## 四、怎么用

```bash
# 1. 取游戏文件（自己的 XCI + 自己的 prod.keys；hactool 自己从源码 make）
DM_ROOT=~/blade2-work tools/extract_game.sh "Blade II ... .xci" prod.keys

# 2. Python 环境 + 改写三个模块（main 两遍约 5 分钟）
python3 -m venv ~/blade2-work/venv && ~/blade2-work/venv/bin/pip install lz4 capstone unicorn pillow numpy
E=~/blade2-work/data/switch/01009cc00e224000/v131072/Update/ExeFS; O=~/blade2-work/build/out
for m in sdk subsdk0 main; do ~/blade2-work/venv/bin/python3 dm/tools/patch_nso.py $E/$m $O $m; done

# 3. Mac 宿主
make -C dm/host -f Makefile.blade
tools/run_mac.sh $O 120        # 日志在 ~/blade2-work/run/log/
```

---

## 五、现状与下一步

已做到：
- [x] d2r-mobile 方法论拆解（上文）
- [x] Blade II 全量分析：引擎 / SDK / 导入面 / NVN 调用面 / SVC 面（`docs/`）
- [x] 改写器扩展，三个模块全部改写成功
- [x] Mac 宿主编译链接通过（通用部分 + Blade 适配）
- [ ] 跑通到 `nnMain` → UE4 引擎初始化 → 第一帧（见提交记录里的运行日志）

接下来按依赖顺序：
1. **启动链**：按运行日志逐个补 `UNIMPL` / 红报的接口（UE4 的 FSwitchPlatformMisc、文件系统读 pak、线程）。
2. **图形层补 37 个 NVN 入口**：`nvnBuffer*`（UE4 的顶点 / 索引 / uniform 缓冲对象）、`DrawElements` 系列、`BindTexture/BindVertexBuffer/BindImage`、`SetViewport`、`QueueAcquireTexture`（UE4 自己管 swapchain）、`FenceSync/QueueWaitSync`、`TexturePoolRegisterImage`、`DeviceGetTextureHandle`——都在 d2r 的录制 / 回放框架内，有 Metal 对应物。
3. **着色器**：Blade 的 Maxwell 着色器在 .pak 里（UE4 4.17 内联在 uasset 里）。最省事的路线是宿主在 Mac 上先跑一次、把 `nvnProgramSetShaders` 收到的控制段 / 数据段全部转储，再用 d2r 的翻译链（Ryujinx 翻译器 → spirv-cross → metal）离线出库；`shader_map.tsv` / `shader_facts.tsv` 要为 Blade 重新生成。
4. **音频**：把渲染器的 voice / final mix 真混成 PCM 交给 CoreAudio（d2r 的 `hle_audio.c` 已有 AudioOut 到 CoreAudio 的一半）。
5. **iOS 壳**：`loader.c` 的 iOS 分支支持补丁区在前的布局（`gen_embed.py` 节顺序），触屏层换成通用虚拟手柄（Blade 是动作游戏，用摇杆 + 按键映射到 `nn::hid` 即可，不需要 D2R 那种面板命中层）。
6. **验证**：`structck.py` 认识 `JA` 行与负偏移补丁区；`equiv.py` 对 `br_abs / blr` 块的处理。
