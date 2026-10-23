# 把下一款 Switch 游戏搬到 iOS / macOS：Blade II 踩过的坑与做法

给**下一个移植任务**看的总结。每条都是 Blade II（UE4 4.17，NintendoSDK 7.4）上真实踩过、有提交记录的问题，按工程阶段排列；每条写明「现象 → 根因 → 处理 → 在哪」。引擎相关的条目标了【UE4】，换引擎时只借思路；没标的是方法论层面的，任何游戏都会遇到。

方法论本身见仓库 [README](../README.md) 第一节；本文只讲坑。

---

## 0. 新游戏第一天清单

1. **提取**：`tools/extract_game.sh <xci/nsp> prod.keys`（按 title / 版本取 ExeFS + RomFS；prod.keys 里格式不对的行要先过滤，hactool 自己从源码 make）。nsp 本体 + 升级档的情况要分别解出再按「Update 覆盖 base」取文件。
2. **认引擎与 SDK**：在 `main` 解压映像里搜构建字符串（`++UE4+Release-x.y`、`Unity`、构建路径）；npdm 看 `SystemResourceSize`（0 → `DM_SYSRES_MB=0`，游戏走普通堆）。
3. **导入清单**：`tools/nso_syms.py`、`tools/import_coverage.py`、`tools/main_imports_vs_d2r.py` → 哪些 SDK 接口 d2r 已接、哪些要新接（Blade 的新接是音频渲染器、hid 六轴、time 时区、swkbd、movie）。`HLE_FORCE` 表必须精确：表里有、没人引用的条目装载即报错。
4. **图形 API 面**：`dm/gfx/tools/nvn_slots.py`（`NVN_LOADER` / `NVN_TEXT_END` 按本游戏填）数一下静态调用了哪些 NVN 函数、哪些 d2r 没实现。UE4 用了 37 个 D2R 从未调的入口（DrawElements、BindTexture、nvnBuffer*、FenceSync…）。
5. **跳转表写法**：`tools/br18_sites.py` 分类 `br x18` 的形态；新 clang / 新引擎会出现改写器不认识的写法（见 §2）。
6. **工作区**：`~/<game>-work`（data / build / run / shdump / shpack / texfacts.tsv / venv），游戏内容永远不进仓库。

---

## 1. 测试方法（先立规矩，再开工）

| 规矩 | 原因 |
|---|---|
| **无声、不点亮显示器**：`tools/aba_run.sh`（`DM_AUDIO=0`、`caffeinate -ims` 不带 `-d/-u`、`DM_WINDOW_BACK=1` 小窗在后面、截图走 `DM_NVN_SHOT=帧号:路径` 而不是屏幕录制） | 玩家的电视和 Mac 在同一条 HDMI-CEC 链上，点亮显示器会把电视切过来 |
| **不让玩家为诊断玩游戏**；要抓活实例用 `tools/dump_req.sh shot/draws/rts/texraw/texlist`（写 `dump.req`，宿主下一帧响应） | 玩家明确要求；而且抓活实例比复现快 |
| **根因要有计数**：探测器行（`[帧账]`、`[长帧]`、`画面触发器`、`【红】`）数出来再说「根因」；A/B 用环境开关对比，开关验证完就删 | 猜过的根因（寄存器 8 是 2D 贴图、驻留提交导致卡顿）都被计数否定 |
| **一个方向失败 2–3 次就停**，换路或报告 | 脚本走标题→大厅三次没通就停了，交给玩家下次顺手看 |
| **每次改动最多启动一次**；别让实例留着 | 启动会污染玩家的存档 / host.log（所有运行写同一个 host.log） |
| 着色器链有版本号（`dmsh_chain.h`），出包前后库数要对得上 | 链文件一改 CAS 全量重编（约 25 分钟），库数变了就是翻译器退化 |

---

## 2. 改写器（`dm/tools/patch_nso.py`）

| 现象 | 根因 | 处理 |
|---|---|---|
| 拒绝 `br x18`（第二种写法）、`blr x18`、`ldr x18,=lit` | UE4 / 新 clang 的代码形态，D2R 二进制里没有 | 新增 `site_br_abs`（按守门 `cmp wI,#K` 枚举目标）、`site_blr`（借 x16/x17）、`site_ldr_lit`；`sites.tsv` 以 `JA` 行登记 |
| `b` 跳不到补丁区 | 映像 > ±128 MB 射程 | 两遍发射，映像太大时补丁区放映像**前面**（`PATCH_OFF < 0`），装载器与 iOS `gen_embed.py` 都认负偏移 |
| 改完不放心 | — | `dm/verify/equiv.py`（Unicorn 随机状态等价试验）+ `structck.py`，每个改写点都跑 |

---

## 3. 装载、启动、系统服务

| 现象 | 根因 | 处理 |
|---|---|---|
| 构造函数里 `FMemory::Free` 空指针崩 | SDK 内 libc++ 走 SDK 自己的 `operator new`，释放走游戏的分配器；真机 rtld 是**全局插入**（main 的定义优先） | 装载器按装载顺序全局解析符号 |
| 接管崩在固定地址 | d2r 写死了 `sdk+偏移` | 全部改按符号名取 |
| 目录枚举栈溢出 | `telldir/seekdir` 搅乱目录流 | 另开目录流计数 |
| `GetSparseTileLayout` 后 memset 负长度 | 仓内 `nvn.h` 把结构留成占位 | 按真实布局清零 |
| 按真机走普通堆而不是 16 GB 虚拟内存 | npdm `SystemResourceSize = 0` | `DM_SYSRES_MB=0`（通用宿主缺省） |
| 整帧稳定 200 ms | macOS **App Nap**：窗口不在前台时 `SleepThread(10 ms)` 要 200 ms 才醒 | `NSProcessInfo beginActivity`（LatencyCritical） |
| 【UE4】读取画面永远不结束 / 进关卡崩 / 开场视频 Tick 崩 | 被接管的子系统（movie 播放器）对**从未打开**的解码器调 Stop、遍历空流对象 | 产品剪口（CUTS，按构建号登记、带原字节断言）；通用教训：**接管掉一个子系统后，要查它的关闭 / 停止路径对未初始化对象怎么办** |
| 没声音 | D2R 只用 AudioOut，UE4 用 `nn::audio` 渲染器（39 个接口） | `hle_blade.c` 在 SDK 接口层按 voice 混音（ADPCM 解码、重采样、混音矩阵）→ CoreAudio；渲染器事件按内核事件对象触发而不按句柄 |

逆向套路（任何游戏通用）：在解压映像找 UTF-32 字面量 → `adrp+add` 引用 → `dm/tools/sdis.py` 反汇编 → `DM_PROBE` 观测门确认。

---

## 4. 图形层：渲染错误一览（最费时间的部分）

### 4.1 着色器翻译链离线不知道的东西——用「事实表」补

翻译器（Ryujinx）离线只看到机器码，**贴图类型、属性格式、颜色目标格式**都要宿主运行时记下来（`DM_NVN_TEXFACTS` → `texfacts.tsv` → `dmsh_from_dump.py` → `shader_facts.tsv`）。每种事实都是一次「人物黑 / 不显示」排查出来的：

| 现象 | 缺的事实 | 事实行 |
|---|---|---|
| 主角不显示（蒙皮顶点着色器从**缓冲贴图**读骨骼矩阵，按 2D 声明读到 0） | 贴图目标类型 | `key\t<槽>\tbuffer/3d/cube/cubearray…` |
| 只剩武器，身体不显示（骨骼索引属性 RGBA8UI 按 float4 声明） | 顶点属性分量类型 | `key\tattr:<槽>\tuint/sint` |
| 【UE4】装备界面角色全黑（光照通道：R16Uint 目标 + 整数贴图读） | 颜色目标整数格式、贴图整数格式 | `key\tfout:<目标>\tuint/sint`、`key\ttexfmt:<槽>\tuint/sint`（`dmsh_build.py int_tex_rewrite` 改声明并 `as_type<float4>` 按位转回） |

**全量离线抽取**胜过「走到哪补到哪」：`tools/pak_shaders.py extract/derive` 从 pak 里抽出全部着色器并从汇编推出事实（UE4 专用；别的引擎要另写抽取器，但思路一样：找到打包格式里的 NVN 着色器 blob）。

### 4.2 Metal 与 NVN 的语义差

| 现象 | 根因 | 处理 |
|---|---|---|
| 画面全蓝 / 全黑、光照像没有 | **几何着色器**写 `gl_Layer`（LUT、半透明光照体、立方体阴影）；Metal 没有几何着色器 | 离线认出「直通 + 写层号」族，融进顶点着色器变体（`<顶点键>+<几何键>_b`，`[[render_target_array_index]]`）；立方体阴影按 `instance_id % 6` 选面 |
| 地板成块缺失、筛孔纹 | 深度预通道 + GEQUAL 要两套顶点着色器算出**逐位相同**的位置；fast-math 下 fma 合并不同 | `xcrun metal -fpreserve-invariance`（Mac 与 iOS 两条编译口都要） |
| 读模板位得到 0 | STENCIL8 视图只能以 `X32_Stencil8` 读；HLSL 读 `.g`，宿主不记 NVN 分量调换 | 视图四分量全调成模板（swizzle rrrr） |
| 立方体数组 | NVN 的 depth 数**面**，Metal 的 arrayLength 数**立方体** | 建图 `depth/6`，上传按面数——**两处要一起改**（原来两处相乘抵消，单改一处 → GPU 错误恢复 ×4/局） |
| 火焰成方块、大贴图 2×2 错位条纹 | 贴图按驱动缺省块线性布局进池；deko3d 的「高度×1.5」挑块高，压缩格式真机最多 8 GOB | 压缩格式块高封顶 8；`DM_NVN_POOLDUMP` + `tools/pooltex_th.py` 按块间连续性核对 |
| 金属全黑（无反射） | ① spirv-cross 给缓冲贴图原子操作生成 `device atomic_uint*` 成员，宿主填了贴图号而不是 GPU 地址 → 光照网格全零；② GPU 拷贝填充的池内贴图被脏页跟踪用**陈旧池字节**重传（环境 BRDF 查找表被冲掉） | ① `nvn_rbind.ptr` 按 MSL 类型含 `*` 判断传地址；② 拷贝目的地也标 GPU 拥有，免重传 |
| 一帧错画 / 黑帧 | 宿主资源跨帧复用 | `DM_NVN_SERIAL=1`（present 时等本帧 GPU 工作）+ 上传暂存块复用；`ACQ_DEPTH=2` 更糟，别再试 |
| 闪烁（60 / 120 Hz） | 出图节拍 | 刷新率分档 + `DM_PRESENT_LINK=0` 缺省 |
| 失焦后显存每秒 +300 MB | 每批新建 32 MB 上传暂存块走延迟释放，一张每帧重写的贴图 → 1.5 × 32 MB/帧 | 暂存块复用；常开的分配账（`分配账` 行）盯住 |
| 局内偶尔一顿（20–63 ms） | 首次遇到的渲染管线在提交线程同步编；通用宿主没有种子预热清单时预热整个没开 | 预热按本机记录 `pso_user.tsv` 也跑；打包时把游玩记录汇总成 `pso_manifest.tsv` 带进包。**新游戏第一包就该带上**，先自己跑几局把清单攒出来 |
| 零星卡顿 | 诊断本身：画面触发器每次命中同步存 2–3 张全尺寸 PNG（40–60 ms） | 触发器缺省关（`DM_NVN_GLITCH=1` 开）——**诊断工具上线前先量它自己的开销** |

### 4.3 D2R 专属缺省

d2r 宿主里有 D2R 调过的缺省（TBDR 写回前瞻、固定地址、暗黑功能钩子）。通用宿主用 `-DDM_GENERIC_HOST=1` 关掉；`blade_stubs.c` 的中性桩清单由 `tools/host_trial_build.sh` 的链接缺符号列表得出。TBDR 优化（`DM_NVN_TBDR=0`）要在新游戏上重新验证再开。

---

## 5. 着色器链操作细节

- 一轮迭代：`bash tools/shaders_iterate.sh`（文件没有可执行位，直接运行会 Permission denied）。
- 翻译器是 .NET apphost：直接调 `dmsh_build.py` 要 `DOTNET_ROOT=/opt/homebrew/opt/dotnet/libexec`，锁文件变了用 `--update-lock`。
- CAS 以工具指纹 + `dmsh_build.py` 源码指纹为键：改链文件 = 全量重编（10 220 个约 14 分钟 J=12，25 分钟 J=4 nice）。只加事实不改链 → 增量。
- 13 个 SPIRV-Cross 失败是历史遗留，库数基线 10 205；每次重编对库数。
- Mac App 的 `shdump/` 是「每个着色器见过一次」的记录，**不是**缺失清单；缺失看日志「不在映射表里」。

---

## 6. iOS 壳（`dm/ios/blade`）

- 改写映像用 `gen_embed.py` 内嵌成已签名段，装载器 `vm_remap` 回原布局；补丁区在前的布局（`PATCH_OFF < 0`）已支持。
- UE4 的 16 GB 虚拟地址区只在 `IsVirtualAddressMemoryEnabled()` 为真时才走；npdm `SystemResourceSize = 0` 的游戏不需要。
- 工程由 XcodeGen `project.yml` 生成，宿主源文件由 `gen_build.py` 从 `Makefile.blade` 展开；`dm/ios/local.env` 填 Team ID / Bundle ID / 设备（真机一步目前卡在这里）。
- 实体手柄 `gamepad_generic.m` Mac / iOS 共用；触摸虚拟手柄按游戏自己的 HUD 槽位叠半透明按钮（玩家否决过「手游扇形」布局）。

---

## 7. 给下一款游戏的预期

能直接复用：改写器（含三种新写法与负偏移）、装载器、SVC / 线程、服务接管框架与 Blade 新接的 SDK 接口、图形层（含 37 个 UE4 用到的 NVN 入口、事实表机制、几何着色器融合、块高规则、整数格式）、着色器链、Mac App 打包、无声测试脚本、iOS 壳骨架。

一定要重做：提取（title / 版本）、导入清单与 `HLE_FORCE` 表、`nvn_slots.py` 参数、产品剪口（全部按构建号，换游戏全失效）、离线着色器抽取器（如果不是 UE4 的 pak）、触摸手柄布局（按新游戏的 HUD）。

新引擎最可能新踩的坑：几何 / 曲面细分着色器的别的用法、Metal 没有的贴图格式、我们没见过的 NVN 入口（先用 `nvn_slots.py` 数）、别的音频接口（AudioOut vs 渲染器）。
