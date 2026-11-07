# 开发：方法拆解、为 Blade II 改了什么、完整构建

README 讲结果与最短用法；本页讲怎么工作：[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 方法的拆解、为 Blade II 改了什么、从 XCI 到成品包的完整命令。工程记录（分析、根因表、现状）见 [blade_port.md](blade_port.md)。`dm/` 是 d2r-mobile 的 `dm/` 目录原样拷贝（MIT，见 `LICENSE.d2r-mobile`）加上 Blade 的改动；改动都能用 `git log -p` 看到。

## 工作区与环境

- 工作区 `~/Documents/game_port/Blade/work`：`data/`（ExeFS / RomFS）、`build/`（改写产物 `out*`、`cache/metal_shader_cache/b-<链版本>`）、`run/`（日志、存档副本）、`shdump/`、`shpak/`（pak 抽出的着色器）、`shpack/`、`texfacts.tsv`、`pso_manifest.tsv`、`icon/icon.jpg`、`venv/`、`out/macapp/`（成品）。游戏内容只放这里。
- 着色器翻译器是 .NET apphost：`DOTNET_ROOT=/opt/homebrew/opt/dotnet/libexec`（`tools/shaders_iterate.sh` 自己设；直接调 `dmsh_build.py` 要手动设）。`shaders_iterate.sh` 用 `bash` 跑。
- 测试规矩（无声、不点亮显示器、不让玩家为诊断玩游戏、根因要有计数）见 [porting_playbook.md](porting_playbook.md) 第 1 节。

---

## d2r-mobile 的核心思想

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

## 为 Blade II 改了什么（举一反三的部分）

### 改写器（`dm/tools/patch_nso.py`）

直接拿 d2r 的改写器跑 Blade 的三个模块：`sdk` 一次通过；`subsdk0` 和 `main` 撞上三个 D2R 二进制里没有的情况，逐个补上：

1. **第二种跳转表写法**（UE4 / 较新 clang）：`adrp xB; add xB; ldrsw xT,[xB,xI,lsl #2]; add x18,xT,xB; br x18`，表项是 32 位相对偏移，到 `br` 时槽里已是绝对目标。新增 `site_br_abs`：离线按守门的 `cmp wI,#K` 读出 K+1 个目标，补丁块把槽值与每个目标比对后落到对应尾块恢复寄存器再跳。`sites.tsv` 以 `JA` 行登记。
2. **`blr x18`**（函数指针在 x18 里）：新增 `site_blr`。调用点上 x16/x17 本就可破坏（AAPCS64 IP0/IP1），直接借用。
3. **`ldr x18, <literal>`**：新增 `site_ldr_lit`。
4. **映像超过 128 MB**：`b` 的射程是 ±128 MB，补丁区再接在映像后面就够不着。改成**两遍发射**（第一遍只量长度），映像太大时把补丁区放到映像**前面**（`PATCH_OFF < 0`，产物文件 = 补丁区 + 映像），装载器把映像基址放在补丁区之后（`loader.c`，`patch_off` 改为有符号）。小模块布局不变，`sdk` 的产物逐字节与原改写器相同。

### 宿主（`dm/host`）

- `Makefile.blade`：只编通用部分（装载 / SVC / 服务接管 / 图形层），不带任何 `dm_*.c` 暗黑功能文件、`diag.c`、`touch_mac.c`。
- `blade_stubs.c`：通用文件里残留的 100 个 D2R 功能钩子的中性实现（过滤器原样放行、查询答「没有」、D2R 剪口才会到的宿主调用一律中止）。清单由 `tools/host_trial_build.sh` 的链接缺符号列表得出。
- `hle_blade.c`：Blade 多用到的 SDK 接口——movie 媒体信息、hid 六轴 / 握持 / 手柄支持小程序、oe、time 时区规则、swkbd 配置、vi 缩放、web / err / pctl / diag，**音频渲染器**（在 SDK 接口层按 voice 混音：PcmInt16 / DSP-ADPCM 解码、线性插值重采样、音量 × 混音矩阵（含 BufferMixer 路由）叠成立体声，CoreAudio 出声或静音接收端按实时消耗；波形缓冲播完归还、已播样本数如实；`DM_AUDIO=1` 出声，`DM_AUDIO_OUT_DUMP=<文件>` 转储混音结果），联网一组一律「不可用」。
- `hle_hid.c`：Joy-Con 双持 / 左 / 右三种样式答「没接」；`hle_fs.c`：`DeleteDirectory`、`CommitSaveData`、`GetDirectoryEntryCount`。
- `tools/extract_game.sh`：按本游戏的 title / 版本从 XCI 取文件（prod.keys 里格式不对的行先过滤）。

---

## 完整构建（从 XCI 到成品包）

```bash
# 1. 取游戏文件（自己的 XCI + 自己的 prod.keys；hactool 自己从源码 make）
DM_ROOT=~/Documents/game_port/Blade/work tools/extract_game.sh "Blade II ... .xci" prod.keys

# 2. Python 环境 + 改写三个模块（main 两遍约 5 分钟）
python3 -m venv ~/Documents/game_port/Blade/work/venv && ~/Documents/game_port/Blade/work/venv/bin/pip install lz4 capstone unicorn pillow numpy
E=~/Documents/game_port/Blade/work/data/switch/01009cc00e224000/v131072/Update/ExeFS; O=~/Documents/game_port/Blade/work/build/out
for m in sdk subsdk0 main; do ~/Documents/game_port/Blade/work/venv/bin/python3 dm/tools/patch_nso.py $E/$m $O $m; done

# 3. Mac 宿主
make -C dm/host -f Makefile.blade
tools/run_mac.sh $O 120        # 日志在 ~/Documents/game_port/Blade/work/run/log/

# 4. 着色器（反复几轮直到没有「不在映射表里」）：带转储 / 配对 / 贴图类型记录跑一遍，再出包重编
W=~/Documents/game_port/Blade/work
tools/run_mac.sh $O 100 DM_PRESENT_LINK=0 DM_MSL_DIR=$W/build/cache/metal_shader_cache DM_NVN_SHDUMP=$W/shdump \
  DM_PSO_MANIFEST=off DM_NVN_TBDR=0 DM_KEYS="A@6,A@10,A@14,A@18,A@24,A@30,A@36,A@55" DM_NVN_SHOT=3000:$W/run/shot_%d.png
tools/pak_shaders.py extract $W/shpak/blobs $ROMFS/BladeII/Content/Paks/*.pak   # 全部着色器离线抽出（一次即可，约 3 分钟）
tools/pak_shaders.py derive $W/shpak/blobs $W/shpak/dump $W/shpak/facts.tsv     # → 转储格式 + 事实表；rsync 进 $W/shdump，facts 并进 $W/texfacts.tsv
tools/shaders_iterate.sh         # dotnet publish → dmsh_from_dump → dmsh_build → 重编宿主（全量 10 220 个约 14 分钟，J=12）

# 4b. Mac 成品包：tools/mac_app_blade.sh（图标 = 游戏自己的 Switch 图标：hactool 取控制 NCA 的 icon_AmericanEnglish.dat 放到 ~/Documents/game_port/Blade/work/icon/icon.jpg，游戏内容不进仓库；DM_MAC_DMG=1 再出 .dmg）
tools/mac_app_blade.sh ~/Documents/game_port/Blade/work/build/out2            # → ~/Documents/game_port/Blade/work/out/macapp/Blade II.app（AppIcon.icns、pso_manifest.tsv 预热清单、全量着色器库）
DM_MAC_DMG=1 tools/mac_app_blade.sh ~/Documents/game_port/Blade/work/build/out2   # 再压一个 Blade II Mobile 0.1 (<构建号>).dmg

# 5. iOS 壳（dm/ios/blade，不需要暗黑的 dm/ios 工程）：iphoneos 着色器库 → 生成工程 + 编译；没有签名身份时只做编译检查
IOSOUT=$W/build/ios/dmsh_b J=8 $W/venv/bin/python3 -I dm/gfx/tools/dmsh_build.py $W/shpack $W/build/cache/metal_shader_cache
cp dm/ios/local.env.example dm/ios/local.env   # 填 DM_TEAM_ID / DM_BUNDLE_ID / DM_DEVICE（见文件内说明）
DM_OUT=$O dm/ios/blade/run.sh build && dm/ios/blade/run.sh install && dm/ios/blade/run.sh launch
# RomFS：用「文件」App 把 Update/RomFS 的内容（含 BladeII/Content/Paks）拷进 App 的文件夹；日志在 App 的 Documents/boot.txt
# 不用真机验 iOS 的装载路径：make -C dm/host -f Makefile.blade embed EMBED_S=$PWD/dm/ios/blade/Gen/embed.S && DM_HOST_BIN=dmhost_blade_embed tools/run_mac.sh $O 60
```
