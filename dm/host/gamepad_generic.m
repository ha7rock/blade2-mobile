// 通用宿主的实体手柄（Mac 与 iOS 共用；GameController.framework 的扩展手柄 → 宿主 dm_pad_set，与键盘 / 虚拟手柄在 hle_hid.c 取手柄处合并）。
// 不是暗黑的 dm/ios/Sources/dm_gamepad.m：没有触屏 / 手柄模式切换、没有回城接管、没有按键帽字母换排，只做「系统手柄 → Switch 键位」。
// 面键按位置排（GCExtendedGamepad.h：buttonA 下、buttonB 右、buttonX 左、buttonY 上；Switch 的位置是 右 A、下 B、上 X、左 Y）：
//   下（buttonA）→ B，右（buttonB）→ A，左（buttonX）→ Y，上（buttonY）→ X。游戏画的是 Switch 键帽提示，这样提示条上画在哪个位置就按哪个位置。
//   DM_PAD_FACE=letter 改成按字母排（下 A → A …），给宁愿看字母不看位置的 Xbox 玩家。
// 肩键 L / R，扳机 ZL / ZR（pressed，系统缺省阈值），摇杆按下 L3 / R3，菜单键（buttonMenu）→ ＋，选项键（buttonOptions）→ −，十字键 → 十字键。
// 摇杆：径向死区 12%（DM_PAD_DEADZONE=0.xx 覆盖）后重新拉满到 ±32767，上为正（GameController 与 Horizon 一致）。
// 多个手柄：各存各的，交给游戏的是合起来的——键取或、每根摇杆取推得最远的；断开一个只拿掉它那一份。
// 热插拔：GCControllerDidConnect / DidDisconnect 通知（主队列）；启动时先接上已连着的并把数目写进日志。
// on_present 回调（dm_gamepad_start 的参数）：第一个手柄连上调 1、最后一个断开调 0，启动时按当前数目调一次——iOS 壳用它藏 / 显虚拟手柄覆盖层，Mac 传 NULL。
// dm_gamepad_reset（前端失焦 / 进后台时调）：所有手柄的键和摇杆清零——回调只在值变化时来，按着键切走、在后台松开，回来不会补「松开」。
// Physical controllers for the generic host (shared by Mac and iOS; the extended gamepad of GameController.framework -> host dm_pad_set, merged with the
// keyboard / virtual gamepad where hle_hid.c reads the controller).
// Not Diablo's dm/ios/Sources/dm_gamepad.m: no touch / controller mode switching, no town-portal takeover, no per-glyph letter layouts; only "system gamepad -> Switch buttons".
// Face buttons are mapped by position (GCExtendedGamepad.h: buttonA bottom, buttonB right, buttonX left, buttonY top; Switch positions are right A, bottom B, top X, left Y):
//   bottom (buttonA) -> B, right (buttonB) -> A, left (buttonX) -> Y, top (buttonY) -> X. The game draws Switch button glyphs, so the position drawn on the prompt is the position to press.
//   DM_PAD_FACE=letter switches to mapping by letter (bottom A -> A ...), for Xbox players who prefer to read the letter rather than the position.
// Shoulders L / R, triggers ZL / ZR (pressed, system default threshold), stick clicks L3 / R3, menu button (buttonMenu) -> +, options button (buttonOptions) -> -, D-pad -> D-pad.
// Sticks: 12% radial dead zone (DM_PAD_DEADZONE=0.xx overrides) then rescaled to +/-32767, up positive (GameController and Horizon agree).
// Multiple controllers: each keeps its own state and the game gets the combination -- buttons ORed, per stick the one pushed furthest wins; disconnecting one removes only its share.
// Hot plug: GCControllerDidConnect / DidDisconnect notifications (main queue); at start the already connected ones are attached and the count is logged.
// on_present callback (argument of dm_gamepad_start): called with 1 when the first controller connects, 0 when the last disconnects, and once at start with the current count --
// the iOS shell uses it to hide / show the virtual gamepad overlay, the Mac passes NULL.
// dm_gamepad_reset (called by the front end on focus loss / entering background): zero the buttons and sticks of all controllers -- callbacks only arrive on value changes,
// so a button held while switching away and released in the background gets no "release" on return.
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <math.h>
#include <string.h>
#include "dm.h"
#include "dm_input.h"

enum { NSLOT = 8 };
static struct { const void *who; uint64_t b; int32_t ax[4]; } g_s[NSLOT];
static int g_count;                            // 已接上的扩展手柄数 | number of attached extended gamepads
static void (*g_on_present)(int present);
static int g_face_letter = -1; static float g_dead = -1;

static int slot_of(const void *who, int make) {
    int fr = -1;
    for (int i = 0; i < NSLOT; i++) { if (g_s[i].who == who) return i; if (fr < 0 && !g_s[i].who) fr = i; }
    if (make && fr >= 0) { memset(&g_s[fr], 0, sizeof g_s[fr]); g_s[fr].who = who; return fr; }
    return -1;
}
// 把各手柄合起来交给宿主 | Merge all controllers and hand the result to the host
static void apply(void) {
    uint64_t b = 0; int32_t ax[4] = {0}; double best[2] = {0};
    for (int i = 0; i < NSLOT; i++) {
        if (!g_s[i].who) continue; b |= g_s[i].b;
        for (int k = 0; k < 2; k++) { double m = hypot(g_s[i].ax[2 * k], g_s[i].ax[2 * k + 1]); if (m > best[k]) { best[k] = m; ax[2 * k] = g_s[i].ax[2 * k]; ax[2 * k + 1] = g_s[i].ax[2 * k + 1]; } }
    }
    dm_pad_set(b, ax[0], ax[1], ax[2], ax[3]);
}
// 摇杆：径向死区后重新拉满 | Stick: radial dead zone, then rescale to full range
static void stick(float x, float y, int32_t *ox, int32_t *oy) {
    if (g_dead < 0) { const char *e = getenv("DM_PAD_DEADZONE"); g_dead = e ? (float)atof(e) : 0.12f; if (g_dead < 0 || g_dead > 0.9f) g_dead = 0.12f; }
    float m = hypotf(x, y);
    if (m <= g_dead) { *ox = *oy = 0; return; }
    float k = (fminf(m, 1.0f) - g_dead) / (1.0f - g_dead) / m;   // 死区边沿 = 0，边缘 = 1 | dead-zone edge = 0, rim = 1
    *ox = (int32_t)lroundf(x * k * DM_STICK_MAX); *oy = (int32_t)lroundf(y * k * DM_STICK_MAX);
}
static void publish(GCController *c, GCExtendedGamepad *g) {
    if (g_face_letter < 0) { const char *e = getenv("DM_PAD_FACE"); g_face_letter = e && !strcmp(e, "letter"); }
    uint64_t b = 0;
    int bot = g.buttonA.pressed, rgt = g.buttonB.pressed, lft = g.buttonX.pressed, top = g.buttonY.pressed;
    if (g_face_letter) { if (bot) b |= DM_BTN_A; if (rgt) b |= DM_BTN_B; if (lft) b |= DM_BTN_X; if (top) b |= DM_BTN_Y; }
    else { if (rgt) b |= DM_BTN_A; if (bot) b |= DM_BTN_B; if (top) b |= DM_BTN_X; if (lft) b |= DM_BTN_Y; }   // Switch 位置 | Switch positions
    if (g.leftShoulder.pressed) b |= DM_BTN_L; if (g.rightShoulder.pressed) b |= DM_BTN_R;
    if (g.leftTrigger.pressed) b |= DM_BTN_ZL; if (g.rightTrigger.pressed) b |= DM_BTN_ZR;
    if (g.leftThumbstickButton.pressed) b |= DM_BTN_LS; if (g.rightThumbstickButton.pressed) b |= DM_BTN_RS;
    if (g.buttonMenu.pressed) b |= DM_BTN_PLUS; if (g.buttonOptions.pressed) b |= DM_BTN_MINUS;
    if (g.dpad.left.pressed) b |= DM_BTN_LEFT; if (g.dpad.up.pressed) b |= DM_BTN_UP; if (g.dpad.right.pressed) b |= DM_BTN_RIGHT; if (g.dpad.down.pressed) b |= DM_BTN_DOWN;
    int i = slot_of((__bridge const void *)c, 1); if (i < 0) return;
    g_s[i].b = b;
    stick(g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value, &g_s[i].ax[0], &g_s[i].ax[1]);
    stick(g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value, &g_s[i].ax[2], &g_s[i].ax[3]);
    apply();
}
static void present_changed(void) { static int was = -1; int now = g_count > 0; if (now == was) return; was = now; if (g_on_present) g_on_present(now); }
static void attach(GCController *c) {
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) { LOG("手柄：%s 不是扩展手柄，不接", c.vendorName.UTF8String ?: "?"); return; }
    if (slot_of((__bridge const void *)c, 0) >= 0) return;   // 已接过（启动枚举与连接通知可能都到） | already attached (start enumeration and the connect notification can both arrive)
    if (slot_of((__bridge const void *)c, 1) < 0) { LOG("手柄：槽满（%d），%s 不接", NSLOT, c.vendorName.UTF8String ?: "?"); return; }
    g_count++;
    LOG("手柄：连上 %s（类别 %s），现 %d 个；面键按%s排：下→B 右→A 左→Y 上→X%s", c.vendorName.UTF8String ?: "?", c.productCategory.UTF8String ?: "?", g_count,
        getenv("DM_PAD_FACE") && !strcmp(getenv("DM_PAD_FACE"), "letter") ? "字母" : "位置", getenv("DM_PAD_FACE") && !strcmp(getenv("DM_PAD_FACE"), "letter") ? "（DM_PAD_FACE=letter：改为下→A 右→B 左→X 上→Y）" : "");
    __weak GCController *wc = c;
    g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) { (void)el; GCController *sc = wc; if (sc) publish(sc, gp); };
    present_changed();
}
static void detach(GCController *c) {
    int i = slot_of((__bridge const void *)c, 0); if (i < 0) return;
    memset(&g_s[i], 0, sizeof g_s[i]); g_count--; apply();
    LOG("手柄：断开 %s，余 %d 个", c.vendorName.UTF8String ?: "?", g_count);
    present_changed();
}

int dm_gamepad_count(void) { return g_count; }
void dm_gamepad_reset(void) {
    int any = 0;
    for (int i = 0; i < NSLOT; i++) if (g_s[i].who) { any |= g_s[i].b != 0 || g_s[i].ax[0] || g_s[i].ax[1] || g_s[i].ax[2] || g_s[i].ax[3]; g_s[i].b = 0; memset(g_s[i].ax, 0, sizeof g_s[i].ax); }
    if (any) { LOG("手柄：失焦，键和摇杆清零"); apply(); }
}
void dm_gamepad_start(void (*on_present)(int present)) {
    static int started; if (started) return; started = 1;
    g_on_present = on_present;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { attach(n.object); }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { detach(n.object); }];
    NSArray<GCController *> *cs = GCController.controllers;
    LOG("手柄：GameController 启动，系统当前列出 %lu 个（DM_PAD_FACE=%s，死区 %s）", (unsigned long)cs.count, getenv("DM_PAD_FACE") ?: "position", getenv("DM_PAD_DEADZONE") ?: "0.12");
    for (GCController *c in cs) attach(c);   // 接上一个就会回调 on_present(1) | attaching one calls back on_present(1)
    if (!g_count) present_changed();         // 一个都没有：按当前数目回调一次 0 | none: call back once with 0 for the current count
}
