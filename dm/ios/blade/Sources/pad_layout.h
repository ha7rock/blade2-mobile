// Blade II 触摸虚拟手柄的图层：几何（放哪、多大、命中）与绘制（CoreGraphics），纯 C，不依赖 UIKit，
// 所以 iOS 壳（front_blade.m 的覆盖层）和 Mac 上的效果图工具（tools/pad_mockup.m）用的是同一份代码——效果图里看到的就是真机上画的。
// 键帽用系统自带的手柄图标（SF Symbols：a.circle、zl.button.roundedtop.horizontal、dpad、l.joystick…，正是 Switch 的键形），
// 由宿主通过 pad_glyph 回调提供位图（iOS：UIImage systemImageNamed；Mac：NSImage imageWithSystemSymbolName），这里只负责摆放与染色；
// 没有图标时退回画字。坐标系：原点左上、y 向下（UIKit drawRect 的上下文即如此；效果图工具自己翻转位图上下文）。
//
// 布局依据：这是原生手柄游戏，战斗 HUD 右下本来就把 8 个键摆成一块 7 格菱形（上排 L X R、中排 Y A、下排 ZL B ZR，格里是技能图和键名），
// 实际用法（作者真机确认）Y 普攻、B 格挡、A 技能、L 终结技、R 闪避。所以 iOS 上不另造手游布局，右手 8 键就半透明地叠在这些格子上、位置随 16:9 画面缩放，
// 玩家按的就是屏幕上看到的那个格子；左半屏浮动左摇杆（移动轴，死区 0.25）+ 一个十字键图标（方向键 = 摇杆方向）；顶边 L3 R3 / − ＋ 小图标；右半屏空处 = 右摇杆（只有背包用）。
// Layout rationale: this is a native controller game whose combat HUD already arranges the 8 keys as a 7-slot diamond block bottom right (top row L X R, middle Y A, bottom ZL B ZR, each slot showing the skill icon and key name);
// actual use (confirmed by the author on hardware): Y attack, B block, A skill, L finisher, R dodge. So on iOS no mobile-style layout is invented: the 8 right-hand keys sit translucently on those very slots, scaling with the 16:9 picture,
// and the player presses the slot they see; left half = floating left stick (move axes, dead zone 0.25) + one d-pad glyph (d-pad = stick directions); top edge L3 R3 / - + small glyphs; free right half = right stick (inventory only).
// 画面是 16:9 居中留黑边（nvn_mtl.m 的出图视口），iPhone 19.5:9 两侧各有约 H×0.09 的黑柱，摇杆提示落在黑柱里，少盖画面。
// 左侧 / 顶边控件以窗口高为基准：r = H×0.058（夹在 17–32 点，DM_PAD_SCALE 再乘）；右手 8 键以画面宽为基准，见 pad_layout_make。
//
// Touch gamepad layer for Blade II: geometry (where, how big, hit testing) and drawing (CoreGraphics), plain C with no UIKit dependency,
// so the iOS shell (overlay in front_blade.m) and the Mac mock-up tool (tools/pad_mockup.m) share one copy -- what the mock-up shows is what the device draws.
// Key caps are the system's own controller glyphs (SF Symbols: a.circle, zl.button.roundedtop.horizontal, dpad, l.joystick... -- the Switch shapes),
// supplied as bitmaps by the host through the pad_glyph callback (iOS: UIImage systemImageNamed; Mac: NSImage imageWithSystemSymbolName); this file only
// places and tints them, and falls back to text when no glyph is available. Coordinates: origin top left, y down (UIKit's drawRect context; the mock-up tool flips its own bitmap context).
//
// Layout rationale (BladeII/Config/DefaultInput.ini): the game maps every controller button to an action (4 face, L/R, ZL/ZR, 4 d-pad, + -, L3 R3),
// the left stick drives the move axes (dead zone 0.25) and the right stick is only used to rotate the character in the inventory (InvenRotateMove).
// The combat HUD's seven slots bottom right are labelled L / X / R (top row) and ZL / Y / A / B / ZR (bottom row): Y is the whip strike (primary attack), A the dash (dodge),
// Actual use (confirmed by the author on hardware): Y basic attack (most used), B block (second, perfect guard then counter), A skill, L finisher, R dodge, ZL ZR hardly ever. Hence, the mobile-game way:
//   - bottom right: one big basic-attack key Y in the corner; the rest fanned around it by frequency -- inner fan B R A, outer fan L X mid-arc, ZL ZR smallest at the ends; real gaps between keys (>= 0.3r ~ 7 pt), hit circles never overlap;
//   - left half: floating left stick (landing point = centre), idle = one faint joystick glyph bottom left; one d-pad glyph mid-left above the stick zone;
//   - top edge: L3 R3 top left (rarely used, smallest and faintest), - + top right;
//   - free space in the right half = right stick (inventory only, no idle hint).
// Left-side / top-edge controls scale with the window height: r = H*0.058 (clamped 17-32 pt, times DM_PAD_SCALE); the 8 right-hand keys scale with the picture width, see pad_layout_make.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <stdint.h>
#include "dm_input.h"

enum { PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_ZL, PB_ZR, PB_PLUS, PB_MINUS, PB_LS, PB_RS, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_N };
static const uint64_t k_bit[PB_N] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_PLUS, DM_BTN_MINUS, DM_BTN_LS, DM_BTN_RS, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT};
static const char *k_label[PB_N] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "−", "L3", "R3", "▲", "▼", "◀", "▶"};
// 图标名（松开 / 按下）；十字键四向共用一个 dpad 图标，按下画对应的 dpad.<向>.filled | glyph names (released / held); the four d-pad directions share one dpad glyph, a held direction draws dpad.<dir>.filled
static const char *k_glyph[PB_N][2] = {
    {"a.circle", "a.circle.fill"}, {"b.circle", "b.circle.fill"}, {"x.circle", "x.circle.fill"}, {"y.circle", "y.circle.fill"},
    {"l.button.roundedbottom.horizontal", "l.button.roundedbottom.horizontal.fill"}, {"r.button.roundedbottom.horizontal", "r.button.roundedbottom.horizontal.fill"},
    {"zl.button.roundedtop.horizontal", "zl.button.roundedtop.horizontal.fill"}, {"zr.button.roundedtop.horizontal", "zr.button.roundedtop.horizontal.fill"},
    {"plus.circle", "plus.circle.fill"}, {"minus.circle", "minus.circle.fill"},
    {"l.joystick.press.down", "l.joystick.press.down.fill"}, {"r.joystick.press.down", "r.joystick.press.down.fill"},
    {"dpad", "dpad.up.filled"}, {"dpad", "dpad.down.filled"}, {"dpad", "dpad.left.filled"}, {"dpad", "dpad.right.filled"}};
typedef CGImageRef (*pad_glyph_fn)(const char *name);   // 宿主提供：按名取图标位图（任意颜色，只用 alpha；可缓存，归宿主持有） | host-provided: glyph bitmap by name (any colour, only alpha is used; may be cached, owned by the host)
static pad_glyph_fn pad_glyph;

typedef struct { CGPoint c; CGFloat r, hit; } pad_circle;          // r：画的半径 / 图标半宽；hit：命中半径 | r: drawn radius / half glyph width; hit: hit-test radius
typedef struct {
    pad_circle btn[PB_N];
    CGFloat r, stick_r;             // 基准半径 / 摇杆满偏半径 | base radius / stick full-deflection radius
    CGPoint ls_idle, dpad_c;        // 左摇杆空闲提示位置 / 十字键图标中心 | idle left-stick hint position / d-pad glyph centre
    CGFloat dpad_box;               // 十字键图标边长 | d-pad glyph size
    CGFloat split_x;                // 左 / 右摇杆落点区分界 | boundary between the left- and right-stick landing zones
    CGSize size; CGRect pic;        // 窗口 / 16:9 画面矩形 | window / 16:9 picture rect
} pad_layout;
typedef struct { uint64_t held; int ls_on, rs_on; CGPoint ls_c, ls_p, rs_c, rs_p; } pad_state;

static inline CGRect pad_picture_rect(CGSize sz) {
    double s = fmin(sz.width / 1280.0, sz.height / 720.0), w = round(1280 * s), h = round(720 * s);
    if (fabs((sz.width / sz.height) / (16.0 / 9.0) - 1) < 0.01) return (CGRect){{0, 0}, sz};
    return CGRectMake(floor((sz.width - w) / 2), floor((sz.height - h) / 2), w, h);
}
static inline pad_circle pad_c(CGFloat x, CGFloat y, CGFloat r, CGFloat hit) { return (pad_circle){{x, y}, r, hit}; }
static inline void pad_layout_make(pad_layout *L, CGSize sz, CGFloat in_top, CGFloat in_left, CGFloat in_bottom, CGFloat in_right, CGFloat scale) {
    CGFloat H = sz.height, W = sz.width, r = fmin(32, fmax(17, H * 0.058)) * (scale > 0 ? scale : 1);
    CGFloat bot = H - in_bottom, top = in_top, left = in_left, right = W - in_right;
    L->size = sz; L->pic = pad_picture_rect(sz); L->r = r; L->stick_r = 1.8 * r; L->split_x = W * 0.45;
    // 右手 8 键：不另起布局，直接叠在游戏自己 HUD 的 7 格菱形块上（右下，UMG 锚定在 720p 画面，位置是 16:9 画面矩形的比例，
    // 从 960x540 截图量得：上排 L X R（v 0.787）、中排 Y A（0.847）、下排 ZL B ZR（0.917）；X 列 u 0.860，L R 距 X ±0.093，Y A 距 X ±0.037）。
    // 画的是半透明菱形框（格子对角半宽 0.027 画面宽，15 Pro 上 19 点），不画图标——游戏 HUD 里本来就有键名和技能图；命中半径 0.05 画面宽（35 点），
    // 相邻命中圈互相重叠，pad_hit 取最近者，整块等于按最近格子划分；DM_PAD_SCALE 不动这 8 键（位置由 HUD 定）。菜单里 HUD 不显示时仍有淡框指位。
    // right-hand 8 keys: no layout of their own -- they sit exactly on the game's own 7-slot HUD diamond block (bottom right; UMG anchored to the 720p frame, so positions are fractions of the 16:9 picture rect,
    // measured on 960x540 screenshots: top row L X R (v 0.787), middle Y A (0.847), bottom ZL B ZR (0.917); X column u 0.860, L R at ±0.093 from X, Y A at ±0.037).
    // Drawn as translucent diamond frames (slot half-diagonal 0.027 of the picture width, 19 pt on a 15 Pro), no glyphs -- the HUD already shows key names and skill icons; hit radius 0.05 of the picture width (35 pt),
    // neighbouring hit circles overlap and pad_hit takes the nearest, so the block is partitioned by nearest slot; DM_PAD_SCALE leaves these 8 alone (the HUD fixes them). In menus, where the HUD is hidden, the faint frames still mark the spots.
    { CGFloat pw = L->pic.size.width, ph = L->pic.size.height, px = L->pic.origin.x, py = L->pic.origin.y, sr = 0.027 * pw, sh = 0.05 * pw;
      static const int hud_btn[8] = {PB_L, PB_X, PB_R, PB_Y, PB_A, PB_ZL, PB_B, PB_ZR};
      static const double hud_u[8] = {0.7682, 0.8604, 0.9531, 0.8229, 0.8984, 0.7682, 0.8604, 0.9531}, hud_v[8] = {0.7870, 0.7870, 0.7870, 0.8472, 0.8472, 0.9167, 0.9167, 0.9167};
      for (int i = 0; i < 8; i++) L->btn[hud_btn[i]] = pad_c(px + hud_u[i] * pw, py + hud_v[i] * ph, sr, sh); }
    // 顶边：右上 − ＋，左上 L3 R3 | top edge: - + top right, L3 R3 top left
    L->btn[PB_PLUS] = pad_c(right - 0.8 * r, top + 0.8 * r, 0.5 * r, 0.75 * r); L->btn[PB_MINUS] = pad_c(right - 2.1 * r, top + 0.8 * r, 0.5 * r, 0.75 * r);
    L->btn[PB_LS] = pad_c(left + 0.8 * r, top + 0.8 * r, 0.5 * r, 0.75 * r); L->btn[PB_RS] = pad_c(left + 2.1 * r, top + 0.8 * r, 0.5 * r, 0.75 * r);
    // 左摇杆空闲提示：左下安全角内 2.2r；十字键图标在摇杆区上方 | idle left-stick hint 2.2r inside the bottom-left safe corner; d-pad glyph above the stick zone
    L->ls_idle = CGPointMake(left + 2.2 * r, bot - 2.2 * r);
    CGFloat q = 0.5 * r; L->dpad_box = 4.4 * q; CGPoint c = CGPointMake(left + 1.9 * r, L->ls_idle.y - L->stick_r - 2.0 * r);
    if (c.y - 2.2 * q < top + 1.7 * r) c.y = top + 1.7 * r + 2.2 * q;   // 矮屏：别压到 L3 R3 | short screens: stay clear of L3 R3
    L->dpad_c = c;
    L->btn[PB_UP] = pad_c(c.x, c.y - 1.3 * q, q, 1.25 * q); L->btn[PB_DOWN] = pad_c(c.x, c.y + 1.3 * q, q, 1.25 * q);
    L->btn[PB_LEFT] = pad_c(c.x - 1.3 * q, c.y, q, 1.25 * q); L->btn[PB_RIGHT] = pad_c(c.x + 1.3 * q, c.y, q, 1.25 * q);
}
static inline int pad_hit(const pad_layout *L, CGPoint p) {
    int best = -1; CGFloat bd = 1e9;
    for (int i = 0; i < PB_N; i++) { CGFloat dx = p.x - L->btn[i].c.x, dy = p.y - L->btn[i].c.y, d = sqrt(dx * dx + dy * dy); if (d < L->btn[i].hit && d < bd) { bd = d; best = i; } }
    return best;
}
// 摇杆值：以落点为中心，满偏 stick_r；8% 死区（游戏自己还有 0.25 的轴死区） | stick value: centred on the landing point, full deflection at stick_r; 8% dead zone (the game adds its own 0.25 axis dead zone)
static inline void pad_stick_value(const pad_layout *L, CGPoint c, CGPoint p, int32_t *x, int32_t *y) {
    CGFloat dx = (p.x - c.x) / L->stick_r, dy = (p.y - c.y) / L->stick_r, d = sqrt(dx * dx + dy * dy);
    if (d > 1) { dx /= d; dy /= d; } else if (d < 0.08) dx = dy = 0;
    *x = (int32_t)lrint(dx * DM_STICK_MAX); *y = (int32_t)lrint(-dy * DM_STICK_MAX);   // 屏幕 y 向下，Horizon 摇杆上为正 | screen y goes down, Horizon stick up is positive
}

// ---- 绘制 | drawing ----
static inline void pad_text(CGContextRef g, const char *s, CGPoint c, CGFloat px, CGFloat alpha) {
    CFStringRef str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); if (!str) return;
    CTFontRef font = CTFontCreateWithName(CFSTR("HelveticaNeue-Bold"), px, NULL);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB(); CGFloat comp[4] = {1, 1, 1, alpha}; CGColorRef col = CGColorCreate(cs, comp);
    CFTypeRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName}, vals[] = {font, col};
    CFDictionaryRef at = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, at); CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat asc = 0, desc = 0; double w = CTLineGetTypographicBounds(line, &asc, &desc, NULL);
    CGContextSaveGState(g);
    CGContextSetTextMatrix(g, CGAffineTransformMakeScale(1, -1));            // y 向下的上下文里字形要翻回来 | glyphs must be flipped back in a y-down context
    CGContextSetTextPosition(g, c.x - w / 2, c.y + (asc - desc) / 2); CTLineDraw(line, g);
    CGContextRestoreGState(g);
    CFRelease(line); CFRelease(as); CFRelease(at); CGColorRelease(col); CGColorSpaceRelease(cs); CFRelease(font); CFRelease(str);
}
// 图标：按最长边缩进 box 点的方框、居中，用白色按 alpha 染色（只取位图的形状） | glyph: fit the longer side into a box of `box` points, centred, tinted white at alpha (only the bitmap's shape is used)
static inline int pad_glyph_draw(CGContextRef g, const char *name, CGPoint c, CGFloat box, CGFloat alpha) {
    CGImageRef im = pad_glyph ? pad_glyph(name) : NULL; if (!im) return 0;
    CGFloat iw = CGImageGetWidth(im), ih = CGImageGetHeight(im), s = box / fmax(iw, ih), w = iw * s, h = ih * s;
    CGRect r = CGRectMake(c.x - w / 2, c.y - h / 2, w, h);
    CGContextSaveGState(g); CGContextBeginTransparencyLayerWithRect(g, r, NULL);
    CGContextTranslateCTM(g, 0, r.origin.y * 2 + h); CGContextScaleCTM(g, 1, -1);   // CGImage 在 y 向下的上下文里要翻回来 | a CGImage must be flipped back in a y-down context
    CGContextSetInterpolationQuality(g, kCGInterpolationHigh); CGContextDrawImage(g, r, im);
    CGContextSetBlendMode(g, kCGBlendModeSourceIn); CGContextSetRGBFillColor(g, 1, 1, 1, alpha); CGContextFillRect(g, r);
    CGContextEndTransparencyLayer(g); CGContextRestoreGState(g);
    return 1;
}
static inline void pad_disc(CGContextRef g, CGPoint c, CGFloat r, CGFloat alpha) { CGContextSetRGBFillColor(g, 0, 0, 0, alpha); CGContextFillEllipseInRect(g, CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r)); }
static inline void pad_draw_button(CGContextRef g, const pad_layout *L, int i, int on) {
    pad_circle c = L->btn[i];
    if (i <= PB_ZR) {   // HUD 格子上的 8 键：半透明菱形框，按下填白 | the 8 keys on the HUD slots: translucent diamond frame, filled white when held
        CGFloat d = c.r; CGContextBeginPath(g);
        CGContextMoveToPoint(g, c.c.x, c.c.y - d); CGContextAddLineToPoint(g, c.c.x + d, c.c.y); CGContextAddLineToPoint(g, c.c.x, c.c.y + d); CGContextAddLineToPoint(g, c.c.x - d, c.c.y); CGContextClosePath(g);
        CGContextSetRGBFillColor(g, 1, 1, 1, on ? 0.45 : 0.0); CGContextSetRGBStrokeColor(g, 1, 1, 1, on ? 0.95 : 0.35); CGContextSetLineWidth(g, on ? 2.0 : 1.2);
        CGContextDrawPath(g, on ? kCGPathFillStroke : kCGPathStroke);
        return;
    }
    if (!pad_glyph_draw(g, k_glyph[i][on ? 1 : 0], c.c, 2 * c.r, on ? 1.0 : 0.7)) {   // 其余键：系统手柄图标；没有图标：圈 + 字 | other keys: system controller glyph; no glyph: ring + text
        CGRect rr = CGRectMake(c.c.x - c.r, c.c.y - c.r, 2 * c.r, 2 * c.r);
        CGContextSetRGBStrokeColor(g, 1, 1, 1, on ? 0.9 : 0.5); CGContextSetLineWidth(g, 1.2); CGContextStrokeEllipseInRect(g, rr);
        pad_text(g, k_label[i], c.c, c.r * 0.85, on ? 1.0 : 0.8);
    }
}
static inline void pad_draw_dpad(CGContextRef g, const pad_layout *L, uint64_t held) {
    pad_disc(g, L->dpad_c, L->dpad_box * 0.5, 0.26);
    int any = 0; for (int i = PB_UP; i <= PB_RIGHT; i++) if (held & k_bit[i]) { any = 1; if (!pad_glyph_draw(g, k_glyph[i][1], L->dpad_c, L->dpad_box, 1.0)) break; }
    if (!any && !pad_glyph_draw(g, k_glyph[PB_UP][0], L->dpad_c, L->dpad_box, 0.8)) for (int i = PB_UP; i <= PB_RIGHT; i++) pad_draw_button(g, L, i, (held & k_bit[i]) != 0);
}
static inline void pad_draw_stick(CGContextRef g, const pad_layout *L, CGPoint c, CGPoint p) {
    CGFloat R = L->stick_r;
    pad_disc(g, c, R, 0.18);
    CGContextSetRGBStrokeColor(g, 1, 1, 1, 0.22); CGContextSetLineWidth(g, 1); CGContextStrokeEllipseInRect(g, CGRectMake(c.x - R, c.y - R, 2 * R, 2 * R));
    CGFloat dx = p.x - c.x, dy = p.y - c.y, d = sqrt(dx * dx + dy * dy); if (d > R) { dx *= R / d; dy *= R / d; }
    CGFloat k = R * 0.42; CGRect kr = CGRectMake(c.x + dx - k, c.y + dy - k, 2 * k, 2 * k);
    CGContextSetRGBFillColor(g, 1, 1, 1, 0.42); CGContextFillEllipseInRect(g, kr);
    CGContextSetRGBStrokeColor(g, 1, 1, 1, 0.7); CGContextSetLineWidth(g, 1); CGContextStrokeEllipseInRect(g, kr);
}
static inline void pad_draw(CGContextRef g, const pad_layout *L, const pad_state *S) {
    for (int i = 0; i < PB_UP; i++) pad_draw_button(g, L, i, (S->held & k_bit[i]) != 0);
    pad_draw_dpad(g, L, S->held);
    if (S->ls_on) pad_draw_stick(g, L, S->ls_c, S->ls_p);
    else if (!pad_glyph_draw(g, "l.joystick", L->ls_idle, 1.6 * L->r, 0.4)) { pad_disc(g, L->ls_idle, 0.7 * L->stick_r, 0.12); }   // 空闲：一个淡摇杆图标提示落点区 | idle: one faint joystick glyph marks the landing zone
    if (S->rs_on) pad_draw_stick(g, L, S->rs_c, S->rs_p);
}
