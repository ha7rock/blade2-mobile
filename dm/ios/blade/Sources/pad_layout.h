// Blade II 触摸虚拟手柄的图层：几何（放哪、多大、命中）与绘制（CoreGraphics + CoreText），纯 C，不依赖 UIKit，
// 所以 iOS 壳（front_blade.m 的覆盖层）和 Mac 上的效果图工具（tools/pad_mockup.m）用的是同一份代码——效果图里看到的就是真机上画的。
// 坐标系：原点左上、y 向下（UIKit drawRect 的上下文即如此；效果图工具自己翻转位图上下文）。
//
// 布局依据（BladeII/Config/DefaultInput.ini）：游戏把全部手柄键都映射成动作（面键 4、肩键 L/R、扳机 ZL/ZR、十字键 4、＋ −、L3 R3），
// 左摇杆走移动轴（死区 0.25），右摇杆只在背包里旋转角色（InvenRotateMove）。战斗 HUD 右下角 4 个技能格的键提示是 L / ZL / R / ZR，所以：
//   · 右下：A/B/X/Y 菱形（主攻击 / 闪避区，最大）；外圈 4 个技能键按 ZL L R ZR 从左到上排成弧，都在右手拇指转动半径内；
//   · 左半屏：浮动左摇杆（落点为中心），空闲时在左下画一个淡圈提示；十字键小簇放左侧中部、摇杆圈之上；
//   · 顶边：左上 L3 R3（极少用，最小最淡），右上 − ＋；
//   · 右半屏空处 = 右摇杆（只有背包用，不画空闲圈）。
// 画面是 16:9 居中留黑边（nvn_mtl.m 的出图视口），iPhone 19.5:9 两侧各有约 H×0.09 的黑柱，摇杆空闲圈与菱形的外沿正好落在黑柱里，少盖 HUD。
// 尺寸以窗口高为基准：r = H×0.075（夹在 20–40 点，DM_PAD_SCALE 再乘），iPhone 横屏 393 点高 → 直径 59 点，符合 44 点的可点最小尺寸。
//
// Touch gamepad layer for Blade II: geometry (where, how big, hit testing) and drawing (CoreGraphics + CoreText), plain C with no UIKit dependency,
// so the iOS shell (overlay in front_blade.m) and the Mac mock-up tool (tools/pad_mockup.m) share one copy -- what the mock-up shows is what the device draws.
// Coordinates: origin top left, y down (what UIKit's drawRect context uses; the mock-up tool flips its bitmap context itself).
//
// Layout rationale (BladeII/Config/DefaultInput.ini): the game maps every controller button to an action (4 face, L/R, ZL/ZR, 4 d-pad, + -, L3 R3),
// the left stick drives the move axes (dead zone 0.25) and the right stick is only used to rotate the character in the inventory (InvenRotateMove).
// The combat HUD's four skill slots bottom right are labelled L / ZL / R / ZR, hence:
//   - bottom right: A/B/X/Y diamond (attack / dodge, largest); an outer arc of the 4 skill keys ZL L R ZR from left to top, all within the right thumb's swing;
//   - left half: floating left stick (landing point = centre), a faint idle ring bottom left as a hint; small d-pad cluster mid-left above the ring;
//   - top edge: L3 R3 top left (rarely used, smallest and faintest), - + top right;
//   - free space in the right half = right stick (inventory only, no idle ring).
// The picture is 16:9 centred with black bars (present viewport in nvn_mtl.m); a 19.5:9 iPhone has ~H*0.09 pillars each side, and the idle stick ring
// and the diamond's outer edge land in those pillars, covering less HUD.
// Sizes are relative to the window height: r = H*0.075 (clamped 20-40 pt, times DM_PAD_SCALE); 393 pt landscape iPhone -> 59 pt diameter, above the 44 pt minimum.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <stdint.h>
#include "dm_input.h"

enum { PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_ZL, PB_ZR, PB_PLUS, PB_MINUS, PB_LS, PB_RS, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_N };
static const uint64_t k_bit[PB_N] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_PLUS, DM_BTN_MINUS, DM_BTN_LS, DM_BTN_RS, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT};
static const char *k_label[PB_N] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "−", "L3", "R3", "▲", "▼", "◀", "▶"};
typedef struct { CGPoint c; CGFloat r, hit; } pad_circle;          // hit：命中半径（比画的大） | hit: hit-test radius (larger than the drawn one)
typedef struct {
    pad_circle btn[PB_N];
    CGFloat r, stick_r;             // 基准半径 / 摇杆满偏半径 | base radius / stick full-deflection radius
    CGPoint ls_idle;                // 左摇杆空闲提示圈的中心 | centre of the idle left-stick hint ring
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
    CGFloat H = sz.height, W = sz.width, r = fmin(40, fmax(20, H * 0.075)) * (scale > 0 ? scale : 1);
    CGFloat bot = H - in_bottom, top = in_top, left = in_left, right = W - in_right;
    L->size = sz; L->pic = pad_picture_rect(sz); L->r = r; L->stick_r = 2.0 * r; L->split_x = W * 0.45;
    // 菱形：中心离右下安全角 2.8r；键距 1.55r；命中 1.2r（相邻键命中圈不重叠：1.55 > 1.2 + 0.35） | diamond: centre 2.8r from the bottom-right safe corner; pitch 1.55r; hit 1.2r
    CGPoint d = CGPointMake(right - 2.8 * r, bot - 2.8 * r);
    L->btn[PB_A] = pad_c(d.x + 1.55 * r, d.y, r, 1.2 * r); L->btn[PB_B] = pad_c(d.x, d.y + 1.55 * r, r, 1.2 * r);
    L->btn[PB_X] = pad_c(d.x - 1.55 * r, d.y, r, 1.2 * r); L->btn[PB_Y] = pad_c(d.x, d.y - 1.55 * r, r, 1.2 * r);
    // 技能弧：绕菱形中心半径 3.3r，185°→266°（y 向下：185° 偏左略下，266° 正上略右），顺序 ZL L R ZR；半径 0.68r，弧上相邻间隔 0.18r
    // skill arc: radius 3.3r around the diamond centre, 185 -> 266 degrees (y down: 185 = left slightly below, 266 = straight up slightly right), order ZL L R ZR
    static const int arc_btn[4] = {PB_ZL, PB_L, PB_R, PB_ZR}; static const double arc_deg[4] = {185, 212, 239, 266};
    for (int i = 0; i < 4; i++) { double a = arc_deg[i] * M_PI / 180; L->btn[arc_btn[i]] = pad_c(d.x + 3.3 * r * cos(a), d.y + 3.3 * r * sin(a), 0.68 * r, 0.95 * r); }
    // 顶边：右上 − ＋，左上 L3 R3 | top edge: - + top right, L3 R3 top left
    L->btn[PB_PLUS] = pad_c(right - 0.9 * r, top + 0.85 * r, 0.52 * r, 0.8 * r); L->btn[PB_MINUS] = pad_c(right - 2.3 * r, top + 0.85 * r, 0.52 * r, 0.8 * r);
    L->btn[PB_LS] = pad_c(left + 0.9 * r, top + 0.85 * r, 0.48 * r, 0.75 * r); L->btn[PB_RS] = pad_c(left + 2.2 * r, top + 0.85 * r, 0.48 * r, 0.75 * r);
    // 左摇杆空闲圈：左下安全角内 2.6r；十字键簇在圈上方 | idle left-stick ring 2.6r inside the bottom-left safe corner; d-pad cluster above it
    L->ls_idle = CGPointMake(left + 2.6 * r, bot - 2.6 * r);
    CGFloat q = 0.58 * r; CGPoint c = CGPointMake(left + 2.2 * r, L->ls_idle.y - L->stick_r - 2.2 * r);
    if (c.y - 1.45 * q - q < top + 1.8 * r) c.y = top + 1.8 * r + 1.45 * q + q;   // 矮屏：别压到 L3 R3 | short screens: stay clear of L3 R3
    L->btn[PB_UP] = pad_c(c.x, c.y - 1.45 * q, q, 1.1 * q); L->btn[PB_DOWN] = pad_c(c.x, c.y + 1.45 * q, q, 1.1 * q);
    L->btn[PB_LEFT] = pad_c(c.x - 1.45 * q, c.y, q, 1.1 * q); L->btn[PB_RIGHT] = pad_c(c.x + 1.45 * q, c.y, q, 1.1 * q);
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
// 键：深色半透明底 + 白边 + 标签；按下时底变白。技能键 / 顶边小键字号按半径缩 | button: dark translucent fill + white ring + label; fill turns white while held
static inline void pad_draw_button(CGContextRef g, pad_circle c, const char *lb, int on) {
    CGRect r = CGRectMake(c.c.x - c.r, c.c.y - c.r, 2 * c.r, 2 * c.r);
    if (on) CGContextSetRGBFillColor(g, 1, 1, 1, 0.55); else CGContextSetRGBFillColor(g, 0, 0, 0, 0.28);
    CGContextFillEllipseInRect(g, r);
    CGContextSetRGBStrokeColor(g, 1, 1, 1, on ? 0.9 : 0.6); CGContextSetLineWidth(g, 1.5); CGContextStrokeEllipseInRect(g, r);
    int two = lb[0] && lb[1] && (unsigned char)lb[0] < 0x80;   // 两个 ASCII 字符（ZL/ZR/L3/R3）字号略小 | two ASCII characters get a slightly smaller font
    pad_text(g, lb, c.c, c.r * (two ? 0.78 : 1.0), on ? 1.0 : 0.85);
}
// 空闲提示圈画成满偏半径的 0.7（只是提示落点区，少盖左下的连击数 / 血条），按着时按满偏画 | the idle hint ring is drawn at 0.7 of the full-deflection radius (just marks the landing zone, covers less of the combo counter / HP bar bottom left); while held it is drawn at full size
static inline void pad_draw_stick(CGContextRef g, const pad_layout *L, CGPoint c, CGPoint p, int on) {
    CGFloat R = on ? L->stick_r : L->stick_r * 0.7;
    CGContextSetRGBFillColor(g, 0, 0, 0, on ? 0.18 : 0.10); CGContextFillEllipseInRect(g, CGRectMake(c.x - R, c.y - R, 2 * R, 2 * R));
    CGContextSetRGBStrokeColor(g, 1, 1, 1, on ? 0.65 : 0.3); CGContextSetLineWidth(g, 2); CGContextStrokeEllipseInRect(g, CGRectMake(c.x - R, c.y - R, 2 * R, 2 * R));
    CGFloat dx = p.x - c.x, dy = p.y - c.y, d = sqrt(dx * dx + dy * dy); if (d > R) { dx *= R / d; dy *= R / d; }
    CGFloat k = R * 0.42; CGContextSetRGBFillColor(g, 1, 1, 1, on ? 0.6 : 0.22); CGContextFillEllipseInRect(g, CGRectMake(c.x + dx - k, c.y + dy - k, 2 * k, 2 * k));
    CGContextSetRGBStrokeColor(g, 1, 1, 1, on ? 0.9 : 0.4); CGContextSetLineWidth(g, 1.5); CGContextStrokeEllipseInRect(g, CGRectMake(c.x + dx - k, c.y + dy - k, 2 * k, 2 * k));
}
static inline void pad_draw(CGContextRef g, const pad_layout *L, const pad_state *S) {
    for (int i = 0; i < PB_N; i++) pad_draw_button(g, L->btn[i], k_label[i], (S->held & k_bit[i]) != 0);
    if (S->ls_on) pad_draw_stick(g, L, S->ls_c, S->ls_p, 1); else pad_draw_stick(g, L, L->ls_idle, L->ls_idle, 0);
    if (S->rs_on) pad_draw_stick(g, L, S->rs_c, S->rs_p, 1);
}
