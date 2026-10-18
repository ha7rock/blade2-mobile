# iOS 触摸虚拟手柄图层（设计，2026-10-18）

代码：`dm/ios/blade/Sources/pad_layout.h`（几何 + 绘制，纯 C / CoreGraphics / CoreText）、`dm/ios/blade/Sources/front_blade.m`（触点归类、淡出、震感）、`tools/pad_mockup.m`（Mac 上出效果图，用的是同一份几何与绘制代码）。

## 效果图（Mac 渲染，真机画的就是这份）

| | |
|---|---|
| ![15 Pro 按着](screenshots/ios_pad_15pro.jpg) iPhone 15 Pro 横屏 852×393 点，安全区 左右 59 / 下 21（黄虚线）；左摇杆推右上、A 与 ZL 按下 | ![15 Pro 空闲](screenshots/ios_pad_15pro_idle.jpg) 同机型，松手 2.5 s 后的空闲态（整层透明度 0.45） |
| ![SE](screenshots/ios_pad_se.jpg) iPhone SE 级 667×375 点、无安全区、16:9 铺满；B 按下 | |

## 依据

- 游戏的输入映射（pak 里的 `BladeII/Config/DefaultInput.ini`）：全部手柄键都映射成动作——面键 4、L/R、ZL/ZR、十字键 4、＋/−、L3/R3；左摇杆是移动轴（轴死区 0.25），右摇杆只在背包里转角色（`InvenRotateMove`）。
- 战斗 HUD 右下 4 个技能格的键提示是 L / ZL / R / ZR，面键菱形是主攻击 / 闪避区。
- 出图是 16:9 居中留黑边（`nvn_mtl.m` 出图视口），iPhone 19.5:9 两侧各有约 0.09 H 的黑柱。

## 布局

尺寸基准 `r = H × 0.075`，夹在 20–40 点（iPhone 横屏 393 点高 → 29.5 点，直径 59 点 ≥ 44 点可点最小值）。全部相对安全区。

| 控件 | 位置 | 半径 / 命中 |
|---|---|---|
| A / B / X / Y 菱形 | 中心离右下安全角 (2.8r, 2.8r)，键距 1.55r | r / 1.2r（相邻命中圈不重叠） |
| ZL L R ZR 技能弧 | 绕菱形中心半径 3.3r，185° → 266°（从左到上），都在右拇指转动半径内 | 0.68r / 0.95r |
| − ＋ | 右上角 | 0.52r / 0.8r |
| L3 R3 | 左上角（极少用，最小最淡） | 0.48r / 0.75r |
| 十字键 | 左侧中部、摇杆空闲圈之上（矮屏自动让开 L3 R3） | 0.58r / 1.1× |
| 左摇杆 | 左 45% 屏空处浮动，落点为中心，满偏 2.0r；空闲时左下 (2.6r, 2.6r) 画 0.7 倍的淡提示圈 | 8% 死区（游戏自己还有 0.25） |
| 右摇杆 | 右 55% 屏空处（背包转角色），不画空闲圈 | 同上 |

## 行为

- 每根手指按落点归类：命中键圆 → 该键；否则按 x 分左右摇杆。手指在键之间滑动即换键（A→B 连按手法），换键有震感。
- 松手 2.5 s 后整层淡到 `DM_PAD_ALPHA`（缺省 0.45），再碰立即回 1。
- 实体手柄接上时整层隐藏、触点只当摇杆；全部断开再显示。
- 环境变量：`DM_PAD_OVERLAY=0` 不画；`DM_PAD_SCALE` 0.8–1.4 整体缩放；`DM_PAD_ALPHA` 空闲透明度；`DM_PAD_HAPTIC=0` 关震感。

## 改布局的流程

改 `pad_layout.h` → `clang -fobjc-arc -framework AppKit -framework CoreText -I dm/host -I dm/ios/blade/Sources tools/pad_mockup.m -o /tmp/pad_mockup` → `/tmp/pad_mockup docs/screenshots/03_skeletons.jpg out.png 852 393 0 59 21 59 3 A,ZL 0.7,0.5 1` 看图（工具还会打印每个键的中心 / 半径 / 命中半径）→ 真机 `dm/ios/blade/run.sh build && install && launch`。

## 真机上要验的

1. 启动日志 `虚拟手柄：窗口 … 安全区 …` 一行的数值与机型相符（横屏左右 59、下 21）。
2. 拇指盲按菱形与技能弧不误触、不漏触；摇杆落点区与十字键不打架。
3. 空闲淡出不遮 HUD；背包里右半屏滑动能转角色。
4. 整层 `drawRect` 的重绘开销（每次触点变化重画一次 Core Graphics）对帧时间的影响——如可见，改成 CALayer 分层只动变化的键。
