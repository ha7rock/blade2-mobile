# Blade II Mobile — 把 Switch 版《Blade II: The Return of Evil》搬到 iOS / macOS

这是对 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（把 Switch 版《暗黑破坏神 II：狱火重生》原生跑在 iPhone 上的开源兼容层）的**方法论拆解**，以及把同一套方法**举一反三**用到本地另一款 Switch 游戏（Blade II，UE4 4.17）上的工程。
`dm/` 是 d2r-mobile 的 `dm/` 目录原样拷贝（MIT，见 `LICENSE.d2r-mobile`）加上 Blade 的改动；改动都能用 `git log -p` 看到。

状态（2026-10-15）：**Mac 宿主能进关卡正常游玩并出声；iOS 壳（`dm/ios/blade`）能为 iphoneos 编出完整 App，待真机验证**；细节见文末「现状与下一步」。
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
- `hle_blade.c`：Blade 多用到的 SDK 接口——movie 媒体信息、hid 六轴 / 握持 / 手柄支持小程序、oe、time 时区规则、swkbd 配置、vi 缩放、web / err / pctl / diag，**音频渲染器**（在 SDK 接口层按 voice 混音：PcmInt16 / DSP-ADPCM 解码、线性插值重采样、音量 × 混音矩阵（含 BufferMixer 路由）叠成立体声，CoreAudio 出声或静音接收端按实时消耗；波形缓冲播完归还、已播样本数如实；`DM_AUDIO=1` 出声，`DM_AUDIO_OUT_DUMP=<文件>` 转储混音结果），联网一组一律「不可用」。
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

# 4. 着色器（反复几轮直到没有「不在映射表里」）：带转储 / 配对 / 贴图类型记录跑一遍，再出包重编
W=~/blade2-work
tools/run_mac.sh $O 100 DM_PRESENT_LINK=0 DM_MSL_DIR=$W/build/cache/metal_shader_cache DM_NVN_SHDUMP=$W/shdump \
  DM_PSO_MANIFEST=off DM_NVN_TBDR=0 DM_KEYS="A@6,A@10,A@14,A@18,A@24,A@30,A@36,A@55" DM_NVN_SHOT=3000:$W/run/shot_%d.png
tools/shaders_iterate.sh         # dotnet publish → dmsh_from_dump → dmsh_build → 重编宿主

# 5. iOS 壳（dm/ios/blade，不需要暗黑的 dm/ios 工程）：iphoneos 着色器库 → 生成工程 + 编译；没有签名身份时只做编译检查
IOSOUT=$W/build/ios/dmsh_b J=8 $W/venv/bin/python3 -I dm/gfx/tools/dmsh_build.py $W/shpack $W/build/cache/metal_shader_cache
cp dm/ios/local.env.example dm/ios/local.env   # 填 DM_TEAM_ID / DM_BUNDLE_ID / DM_DEVICE（见文件内说明）
DM_OUT=$O dm/ios/blade/run.sh build && dm/ios/blade/run.sh install && dm/ios/blade/run.sh launch
# RomFS：用「文件」App 把 Update/RomFS 的内容（含 BladeII/Content/Paks）拷进 App 的文件夹；日志在 App 的 Documents/boot.txt
# 不用真机验 iOS 的装载路径：make -C dm/host -f Makefile.blade embed EMBED_S=$PWD/dm/ios/blade/Gen/embed.S && DM_HOST_BIN=dmhost_blade_embed tools/run_mac.sh $O 60
```

---

## 五、现状与下一步

**2026-10-15 的结果（Mac 宿主，`tools/run_mac.sh`，60 fps）**：标题 → 读档确认（世界地图底图）→ 章节地图 → 关卡选择（角色立绘、关卡预览图）→ 读取画面 → Act 1「Darkness in Luemos」：带贴图的石板地、木桶、锁链、尖塔，雾、烛光、点光源，主角（白发角斗士）与敌人（骷髅兵）的完整模型、鹅、受击特效、连击数字、受伤红闪，画面与真机一致；不操作时主角被打死出「Defeat」画面。截图在 `~/blade2-work/run/shot_*.png`。

着色器链已通：宿主转储（`DM_NVN_SHDUMP`）→ `dmsh_from_dump.py` 合成包 + 映射表 + 作答表 → `dmsh_build.py`（Ryujinx → spirv-cross → metal）→ 285 个着色器 281 个出库（4 个是立方体阴影的几何着色器，见下）。一轮迭代：`tools/shaders_iterate.sh`。

画面之前「跑不出来」的根因（每条都有提交记录）：

| 问题 | 根因 | 处理 |
|---|---|---|
| 读取画面永远不结束 | 关卡加载关闭 UE4 视频播放器 → 对**从未打开**的 NVN 解码器调 Stop → 对未初始化的事件上内部锁，GameThread 死等 | 产品剪口：`0x3411998` 跳过 Stop |
| 进关卡崩在 `0x3a2de34` | 播放器关闭时遍历轨道「取消选轨」，流对象在宿主下恒空 | 产品剪口：`0x3a2aa70` 跳过轨道遍历 |
| 整帧稳定 200 ms | macOS **App Nap**：窗口不在前台时把进程定时器合并，`SleepThread(10 ms)` 要 200 ms 才醒 | `NSProcessInfo beginActivity`（LatencyCritical）退出 App Nap |
| 画面全蓝 / 全黑、光照像没有 | UE4 的 32³ 调色 LUT 与 64³ 半透明光照体用**几何着色器**写 `gl_Layer`；Metal 没有几何着色器，所有层都画进第 0 层，LUT 只有一层 → 色调映射后几乎全黑 | 离线把「直通 + 写层号」几何着色器翻成 GLSL 认出层号来源，融进配对顶点着色器（`<顶点键>+<几何键>_b` 变体，输出 `[[render_target_array_index]]`）；宿主按 `renderTargetArrayLength` 整体挂 3D 目标、管线带 `inputPrimitiveTopology`、去掉 `[[point_size]]` |
| 主角不显示 | 蒙皮顶点着色器从**缓冲贴图**读骨骼矩阵，翻译器离线不知道贴图类型，按 2D 声明 → Metal 读到 0 | 宿主运行时记每个着色器各槽实际绑定的贴图类型（`DM_NVN_TEXFACTS`，缓冲 / 3D / 立方体…），`dmsh_from_dump.py` 合进作答表 `shader_facts.tsv`；翻译器补上合并句柄表的槽号解码 |
| 火盆的火焰成方块、大贴图呈 2×2 错位条纹 | UE4 不走预排布参数，贴图按驱动**缺省块线性布局**写进池；宿主沿用 deko3d 的「高度×1.5」挑块高，对压缩格式挑到 16 GOB，真机驱动最多 8 GOB（Blade 实测 BC1/BC3 256²–1024² 全是 8，未压缩格式照旧到 16） | `nvn_tex.c`：压缩格式自动块高封顶 8；`DM_NVN_POOLDUMP` + `tools/pooltex_th.py` 按块间连续性给每张贴图打分核对块高 |
| 开局教程卡「Move Hero」关不掉（按什么键都没用） | 卡片在 PlayerController 的 InputComponent 上 BindAction("LeftStickUp/Down/Left/Right", bExecuteWhenPaused)，这些动作映射到 Gamepad_LeftStick_Up 等「摇杆当按键」；真机 hid 按摇杆位置自动置 NpadButton::StickL*（bit 16–23），宿主只填轴值不填这些位 → 动作永不触发（轴映射的移动正常，所以之前没暴露） | `hle_hid.c` fill()：按半程阈值补 StickL*/StickR* 位。查法：UTF-32 字面量 → adrp+add 引用 → 绑定处 → `DM_PROBE` 确认绑定/关闭函数（`sampler_blade.c`：探针、采样器、`DM_MEMSCAN`） |
| 顶点 / 片元管线建不出（6652 处插值不匹配） | UE4 顶点 / 片元是分开的程序，翻译器要知道配对 | 宿主记配对（`DM_NVN_PAIRS`，含几何阶段 `2:`），翻译器 `SetNextStage` 并集（Ryujinx 的 `MergeFromtNextStage` 改成按位或） |

| 2D 贴图全黑（读取画面底图、关卡预览、UI 图片），3D 材质平的 | d2r 只把带「打包布局」（`SetPackagedTextureLayout`）的贴图从内存池解排布上传；UE4 按 NVN 默认块线性布局直接 memcpy 进池、不带打包布局，于是采样贴图都建成了空的私有 MTLTexture | `tex_pool_src`：池里有存储、不是显示 / 图像 / 可压缩渲染目标 / 线性渲染目标 / 缓冲贴图的，一律按池内容上传（Blade 实测采样贴图 flags = 0x20 / 0，渲染目标 0x8 / 0xc）；脏页跟踪与重传照旧（一局首传 322 张、重传 8978 次） |
| 地板成块缺失、带筛孔纹 | UE4 深度预通道 + 基础通道 GEQUAL 深度测试要求两个不同顶点着色器算出**逐位相同**的裁剪位置；Metal 默认 fast-math 下各着色器 fma 合并不同，MSL 的 `[[position, invariant]]` 不加编译选项不生效 | `xcrun metal -fpreserve-invariance`（Mac 与 iOS 两条编译口径都加） |
| 主角 / 敌人只剩武器，身体不显示 | 蒙皮顶点着色器的骨骼索引属性是 RGBA8UI 整数格式，翻译器离线不知道属性类型、按 `float4` 声明；Metal 把整数转成浮点值，着色器再 `as_type<int>` 得到垃圾骨骼索引——只绑 0 号骨骼的部件（武器）碰巧正常 | 宿主在建管线时记录每个顶点属性的分量类型（`DM_NVN_TEXFACTS` 文件里的 `attr:<槽>` 行），`dmsh_from_dump.py` 合进作答表的 `attr`，翻译器按 `uint4` 声明 |
| 点光源立方体阴影不画 | 4 个几何着色器把三角形发到 6 个面（`EmitVertex` 18 次，层号 0–5 常量，每面乘 `gp_c3` 第 6+4f 行起的矩阵、`_m0[f].x` 为面开关） | 离线认出这一族（`dmsh_build.py gs_cube`），顶点变体按 `instance_id % 6` 选面与矩阵、输出层号与面号常量；几何阶段 uniform 以 `stage=2` 绑定挂进顶点参数块；宿主把实例数与基实例 ×6（反射 `gs_faces`）。坑：UE4 阴影顶点只写世界坐标 xyz，真机上未写的插值 w 缺省 1 而翻译出的 MSL 清零 → 融合时补 1，否则平移行丢失 |

2026-10-15 午后：**有声音了**——`hle_blade.c` 的音频渲染器按 SDK 接口真混音（游戏用 32 个 44.1 kHz 单声道 DSP-ADPCM 声音槽 → 32 kHz 渲染器 → CoreAudio，`DM_AUDIO=1`，`DM_AUDIO_GAIN` 调响度，`DM_AUDIO_OUT_DUMP` 存混音）；d2r 的暗黑专属固定地址缺省在通用宿主下全部失效（`DM_GENERIC_HOST`）；TBDR 优化开启验证通过（画面一致、GPU 时间持平，Mac 默认仍关）；200 秒脚本游玩（移动 / 攻击）无崩溃。

还没做 / 已知问题：
0. **偶发 GPU hang**（进关卡时出现过一次 `kIOGPUCommandBufferCallbackErrorHang`，后续帧恢复）；`MemoryPoolMapVirtual` 现已实现映射表但本游戏没用到。
1. **点光源阴影的画面验证**：Act 1 开头没有带阴影的点光源，只在标题场景（角色近景、无地面）用到；立方体图 6 个面已有几何、比较采样器正确，等遇到有接收面的场景再对比。
2. **音频**：已出声（Mac `DM_AUDIO=1`）。渲染器事件「写端失效」的根因是游戏 Detach 读端后 Destroy 临时 SystemEvent，宿主现在按内核事件对象触发而不按句柄。还没做：I3dl2 混响（只出干声）、ParseAdpcmHeader 之外的 SDK 纯计算都没动；音量偏大（开场曲与局内多 voice 叠加有削波），要对着真机录音核对增益口径。
3. **TBDR 优化**（写回前瞻 / 读入可省 / 延迟通道）现在全关（`DM_NVN_TBDR=0`），是 D2R 调过的，Blade 上要重新验证后再开；`MemoryPoolMapVirtual` / `Downsample` 仍只报红。
4. **iOS 壳**（`dm/ios/blade`，2026-10-15 晚）：已能为 iphoneos arm64 编出完整的 App（Debug / Release，`CODE_SIGNING_ALLOWED=NO`，152 MB），没有真机 / 签名身份，没在手机上跑过。做了的：
   - **补丁区在前的布局**：`gen_embed.py` 按 `PATCH_OFF < 0` 把文件切成 `__TEXT,__dm<名>x1`（补丁区，文件 [0, −PATCH_OFF)）/ `__TEXT,__dm<名>x0`（原 text）/ `__DM<名>W`（ro 余部 + data + bss + 键页）；`loader.c` iOS 分支按 x1 长度 == −PATCH_OFF 认出，保留「补丁区 + 映像」一块地址再 `vm_remap` 回原相对位置。Mac 上可用 `make -f Makefile.blade embed`（`-DDM_EMBED_IMAGE=1`）走同一条内嵌装载路径验证（映像编进 Mac 可执行文件，游戏照常进局）。
   - **16 GB 虚拟地址区——不需要**：UE4 的 `AllocateAddressRegion(16 GB)`（`main+0x12ccb94` 的缺省常数，`main+0x6c28c18` 可配置）只在 `nn::os::IsVirtualAddressMemoryEnabled()` 为真时走（`main+0x12cca88` 的分支），而 Blade 的 npdm `SystemResourceSize = 0` 让它为假，走 `SetMemoryHeapSize(3650 MB)` 的普通堆路线。通用宿主的 `DM_SYSRES_MB` 缺省改成 0（`hle_svc.c`），`run_mac.sh` 不再设 `DM_ALIAS_GB=18`：alias 512 MB、整块预留 5 GB，Mac 实测进局无红；5 GB 在 iOS 不签「扩展虚拟寻址」时也放得下（上限 6 GB）。工程仍签 `increased-memory-limit`（3650 MB 堆的常驻）与 `extended-virtual-addressing`（地址余量；免费账号删掉那一行即可），不做剪口。
   - **工程**：`dm/ios/blade/project.yml`（XcodeGen）+ `run.sh`；宿主源文件由 `gen_build.py` 从 `Makefile.blade` 展开（`-DDM_GENERIC_HOST=1` 等 `-D` 原样进 xcconfig），外壳只有 `Sources/main.m`（日志、找 RomFS、环境变量、提前装载、场景生命周期）与 `Sources/front_blade.m`（Metal 视图 + **通用虚拟手柄**：左摇杆浮动、右摇杆、A/B/X/Y/L/R/ZL/ZR/+/−/L3/R3/十字键，经 `dm_input_set` 进 `hle_hid.c`；接上实体手柄时自动隐藏，全部断开再显示）。`ENABLE_DEBUG_DYLIB=NO`（Xcode 的调试 dylib 会把内嵌段与 `_mh_execute_header` 搬出主可执行文件）。
   - **iphoneos 着色器库**：`dmsh_build.py` 的 `IOSOUT` 路径现在也出融合变体 `<顶点键>+<几何键>_b`（含 `-fpreserve-invariance`）并进内容缓存；301 个库 + 6 个融合变体 + 40 个采样变体。
   - **实体手柄**（`dm/host/gamepad_generic.m`，Mac 与 iOS 共用，GameController.framework，热插拔）：面键按位置排——下→B、右→A、左→Y、上→X（游戏画的是 Switch 键帽提示；`DM_PAD_FACE=letter` 改按字母），L/R、ZL/ZR、L3/R3、菜单→＋、选项→−、十字键；摇杆径向死区 12%（`DM_PAD_DEADZONE`）。Mac 上与键盘并存（`front_mac.m` 原有的 `dm_gamepad_start` 调用，不再是 `blade_stubs.c` 的空实现）。没有实体手柄实测，只验证了启动枚举（日志「手柄：GameController 启动，系统当前列出 N 个」）。
   - **iOS 音频会话**（`dm/host/audio_session.m`，`hle_blade.c` 建 RemoteIO 前调）：Playback 类别、不与别的 App 混音；打断开始 / 结束与进后台 / 回前台合成一个开关停 / 起渲染器的声卡单元（`dm_audren_suspend`，停着时节拍线程照实时静音消耗）；线路变化只记日志。音频渲染器缺省改成与 `hle_audio.c` 同口径：iOS 真出声、Mac 静音（`DM_AUDIO` 仍可覆盖）。没在真机上验证。
   - 还没做：真机上跑（发热 / 内存上限读数）、触摸屏（游戏只认手柄，没接 `dm_touch_set`）。
5. **验证工具**：`structck.py` / `equiv.py` 认识 `JA` 行、负偏移补丁区与新改写块。

**此前（同日白天）的结果**：引擎循环先跑起来（900 帧 / 60 秒，画面空）时修掉的通用问题：

| 问题 | 根因 | 处理 |
|---|---|---|
| 改写器拒绝 `br x18` / `blr x18` / `ldr x18,=lit` | UE4 / 新 clang 的代码形态 | 三种新改写块（见第三节） |
| `b` 跳不到补丁区 | 映像 131 MB > ±128 MB | 两遍发射，补丁区放映像前 |
| `StandardAllocator::Initialize` 中止 | UE4 要 16 GB 虚拟地址区，d2r 的 alias 区只有 512 MB | 当时用 `DM_ALIAS_GB` 放大；后来发现下一行的 `DM_SYSRES_MB=0` 让游戏根本不走这条路（见五.4），alias 又回到 512 MB |
| 按真机走普通堆而不是虚拟内存 | Blade 的 npdm `SystemResourceSize = 0` | `DM_SYSRES_MB=0`（d2r 写死 32 MB；通用宿主现在缺省 0） |
| 构造函数里 `FMemory::Free` 空指针崩 | sdk 内 libc++ 的 `basic_string` 走 sdk 自己的 `operator new`，释放却走 UE4 的 Binned 分配器；真机 rtld 是**全局插入**（main 的定义优先） | 装载器改成按装载顺序全局解析符号 |
| `StandardAllocator::Allocate` 接管崩 | d2r 写死了 `sdk+0x2f9974` | 改按符号名取 |
| 目录枚举递归到栈溢出 | 我写的 `GetDirectoryEntryCount` 用 telldir/seekdir 搅乱了目录流 | 另开目录流计数 |
| 图形层第一批调用报未实现 | UE4 的 NVN RHI 用缓冲对象 / 组合贴图句柄 / DrawElements 等 37 个入口 | 全部补上，落在 d2r 的录制 / 回放框架里 |
| `GetSparseTileLayout` 后 memset 负长度崩 | 仓内 `nvn.h` 把该结构留成 1 字节占位 | 按真实布局 0x1c 字节清零 |
| 开场视频 Tick 空指针 | movie 库在宿主下只空播，RHI 流对象从未建立 | 产品剪口：`0x3a29e40 → ret` |

---

## 版权与致谢 | Copyright and acknowledgements

**你必须拥有正版游戏。** 本仓库只包含兼容层的源码、工具和不含任何游戏内容的安装壳。我们不提供、不认可、也不对任何第三方分发游戏主程序、游戏资源、密钥、着色器转储 / 编译产物或改写产物的行为负责；请只配合你自己合法拥有的 Switch 版《Blade II: The Return of Evil》使用。仓库里与游戏相关的只有**事实表**（着色器键的哈希、绑定类型、改写器的指令偏移），不含任何游戏代码或资源。这是一个独立的非商业项目，与 Action Square、任天堂没有关联，也未获其认可或赞助；Blade II 是其权利人的商标，Nintendo Switch 是任天堂的商标。

**You must own the game.** This repository contains only the compatibility layer's source, tools and a content-free app shell. We do not provide, endorse, or take responsibility for any third-party distribution of the game program, game assets, keys, shader dumps / compiled shader libraries or rewrite outputs; use it only with your own legally owned Switch copy of *Blade II: The Return of Evil*. The only game-related files in the repository are **fact tables** (hashes of shader keys, binding types, instruction offsets for the rewriter); no game code or assets. This is an independent, non-commercial project, not affiliated with, endorsed by, or sponsored by Action Square or Nintendo; Blade II is a trademark of its owner and Nintendo Switch is a trademark of Nintendo.

致谢 | Acknowledgements
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)**（MIT）：本项目的全部基础。`dm/` 是它的 `dm/` 目录加上 Blade 的改动，许可证原样保留在 `LICENSE.d2r-mobile`；本项目的方法论、宿主、NVN→Metal 翻译层、离线着色器链、iOS 壳都来自它，许多修正（Metal 4 编码器屏障、按屏幕刷新率取出图档、内存池按需驻留）也直接吸收自它的后续提交。向作者致敬。| The whole foundation of this project. `dm/` is its `dm/` directory plus the Blade changes, with its license kept verbatim in `LICENSE.d2r-mobile`; the methodology, host, NVN→Metal translation layer, offline shader chain and iOS shell all come from it, and several fixes (Metal 4 encoder barriers, present pacing by screen refresh rate, on-demand memory-pool residency) were taken directly from its later commits. Our thanks to the author.
- **Ryujinx**（Ryubing，MIT）：`dm/third_party/ryujinx` 原样携带其着色器翻译器，把 Switch 着色器翻成 SPIR-V。| Its shader translator is bundled unchanged in `dm/third_party/ryujinx` to translate Switch shaders to SPIR-V.
- **deko3d**（zlib）：块线性贴图布局算法的出处。| Source of the block-linear texture layout algorithm.
- **spirv-cross**（Apache-2.0）、**libdeflate**（MIT）、**NVIDIA FLIP**（BSD）：按各自许可证使用。| Used under their respective licenses.
