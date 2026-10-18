// Blade II 触摸虚拟手柄的图层：几何（放哪、多大、命中）与绘制（CoreGraphics），纯 C，不依赖 UIKit，
// 所以 iOS 壳（front_blade.m 的覆盖层）和 Mac 上的效果图工具（tools/pad_mockup.m）用的是同一份代码——效果图里看到的就是真机上画的。
// 键帽用系统自带的手柄图标（SF Symbols：a.circle、zl.button.roundedtop.horizontal、dpad、l.joystick…，正是 Switch 的键形），
// 由宿主通过 pad_glyph 回调提供位图（iOS：UIImage systemImageNamed；Mac：NSImage imageWithSystemSymbolName），这里只负责摆放与染色；
// 没有图标时退回画字。坐标系：原点左上、y 向下（UIKit drawRect 的上下文即如此；效果图工具自己翻转位图上下文）。
//
// 布局依据（BladeII/Config/DefaultInput.ini）：游戏把全部手柄键都映射成动作（面键 4、肩键 L/R、扳机 ZL/ZR、十字键 4、＋ −、L3 R3），
// 左摇杆走移动轴（死区 0.25），右摇杆只在背包里旋转角色（InvenRotateMove）。战斗 HUD 右下角 4 个技能格的键提示是 L / ZL / R / ZR，所以：
//   · 右下：A/B/X/Y 菱形（Switch 排法：X 上 Y 左 A 右 B 下；主攻击 / 闪避区，最大）；外圈 4 个技能键按 ZL L R ZR 从左到上排成弧，都在右手拇指转动半径内；
//   · 左半屏：浮动左摇杆（落点为中心），空闲时左下只画一个淡的摇杆图标提示；十字键一个图标放左侧中部、摇杆区之上；
//   · 顶边：左上 L3 R3（极少用，最小最淡），右上 − ＋；
//   · 右半屏空处 = 右摇杆（只有背包用，不画空闲提示）。
// 画面是 16:9 居中留黑边（nvn_mtl.m 的出图视口），iPhone 19.5:9 两侧各有约 H×0.09 的黑柱，摇杆提示与菱形的外沿落在黑柱里，少盖 HUD。
// 尺寸以窗口高为基准：r = H×0.058（夹在 17–32 点，DM_PAD_SCALE 再乘），iPhone 横屏 393 点高 → 面键直径 46 点（≥ 44 点可点最小值），技能键 33 点。
//
// Touch gamepad layer for Blade II: geometry (where, how big, hit testing) and drawing (CoreGraphics), plain C with no UIKit dependency,
// so the iOS shell (overlay in front_blade.m) and the Mac mock-up tool (tools/pad_mockup.m) share one copy -- what the mock-up shows is what the device draws.
// Key caps are the system's own controller glyphs (SF Symbols: a.circle, zl.button.roundedtop.horizontal, dpad, l.joystick... -- the Switch shapes),
// supplied as bitmaps by the host through the pad_glyph callback (iOS: UIImage systemImageNamed; Mac: NSImage imageWithSystemSymbolName); this file only
// places and tints them, and falls back to text when no glyph is available. Coordinates: origin top left, y down (UIKit's drawRect context; the mock-up tool flips its own bitmap context).
//
// Layout rationale (BladeII/Config/DefaultInput.ini): the game maps every controller button to an action (4 face, L/R, ZL/ZR, 4 d-pad, + -, L3 R3),
// the left stick drives the move axes (dead zone 0.25) and the right stick is only used to rotate the character in the inventory (InvenRotateMove).
// The combat HUD's four skill slots bottom right are labelled L / ZL / R / ZR, hence:
//   - bottom right: A/B/X/Y diamond (Switch order: X top, Y left, A right, B bottom; attack / dodge, largest); an outer arc of the 4 skill keys ZL L R ZR from left to top;
//   - left half: floating left stick (landing point = centre), idle = one faint joystick glyph bottom left; one d-pad glyph mid-left above the stick zone;
//   - top edge: L3 R3 top left (rarely used, smallest and faintest), - + top right;
//   - free space in the right half = right stick (inventory only, no idle hint).
// Sizes are relative to the window height: r = H*0.058 (clamped 17-32 pt, times DM_PAD_SCALE); 393 pt landscape iPhone -> 46 pt face buttons (>= 44 pt minimum), 33 pt skill keys.
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
    // 菱形：中心离右下安全角 2.4r；键距 1.45r；命中 1.1r | diamond: centre 2.4r from the bottom-right safe corner; pitch 1.45r; hit 1.1r
    CGPoint d = CGPointMake(right - 2.4 * r, bot - 2.4 * r);
    L->btn[PB_A] = pad_c(d.x + 1.45 * r, d.y, r, 1.1 * r); L->btn[PB_B] = pad_c(d.x, d.y + 1.45 * r, r, 1.1 * r);
    L->btn[PB_Y] = pad_c(d.x - 1.45 * r, d.y, r, 1.1 * r); L->btn[PB_X] = pad_c(d.x, d.y - 1.45 * r, r, 1.1 * r);
    // 技能弧：绕菱形中心半径 3.05r，190°→265°（y 向下：190° 偏左略下，265° 正上略右），顺序 ZL L R ZR；键帽半宽 0.72r，命中 0.92r
    // skill arc: radius 3.05r around the diamond centre, 190 -> 265 degrees (y down), order ZL L R ZR; cap half-width 0.72r, hit 0.92r
    static const int arc_btn[4] = {PB_ZL, PB_L, PB_R, PB_ZR}; static const double arc_deg[4] = {190, 215, 240, 265};
    for (int i = 0; i < 4; i++) { double a = arc_deg[i] * M_PI / 180; L->btn[arc_btn[i]] = pad_c(d.x + 3.05 * r * cos(a), d.y + 3.05 * r * sin(a), 0.72 * r, 0.92 * r); }
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
    pad_circle c = L->btn[i]; CGFloat box = 2 * c.r;
    if (i <= PB_Y) { pad_disc(g, c.c, c.r * 0.96, on ? 0.15 : 0.34); box = 2.04 * c.r; }          // 面键：深底 + 圆标（按下换实心） | face: dark disc + circled letter (filled when held)
    else if (i <= PB_ZR) {   // 技能键：键帽形的深色圆角底（图标 84:63）+ 键帽 | skill: key-cap shaped dark rounded backing (glyph is 84:63) + key cap
        CGFloat w = 1.5 * c.r, h = w * 63 / 84; CGPathRef rr = CGPathCreateWithRoundedRect(CGRectMake(c.c.x - w / 2, c.c.y - h / 2, w, h), h * 0.3, h * 0.3, NULL);
        CGContextSetRGBFillColor(g, 0, 0, 0, on ? 0.12 : 0.30); CGContextAddPath(g, rr); CGContextFillPath(g); CGPathRelease(rr); box = 2.0 * c.r; }
    if (!pad_glyph_draw(g, k_glyph[i][on ? 1 : 0], c.c, box, i <= PB_ZR ? (on ? 1.0 : 0.88) : (on ? 1.0 : 0.7))) {
        CGRect rr = CGRectMake(c.c.x - c.r, c.c.y - c.r, 2 * c.r, 2 * c.r);                             // 没有图标：圈 + 字 | no glyph: ring + text
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
