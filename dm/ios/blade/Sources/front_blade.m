// 通用宿主的 iOS 前端（front.h 的实现，对应 Mac 的 dm/host/front_mac.m）：Metal 出图视图 + 通用虚拟手柄 + 软键盘输入框 + 场景生命周期。
// 虚拟手柄：画在游戏画面之上、只画不收触点，触点由出图视图统一收；每根手指按落点归类——按钮圆内 = 该键，左半屏空处 = 左摇杆（浮动：落点为中心），
// 右半屏空处 = 右摇杆；每次变化把整组状态经 dm_input_set 交给宿主（hle_hid.c 在游戏读手柄时落地）。键位 = dm_input.h 的 DM_BTN_*，摇杆 ±32767、上为正。
// 不接触摸屏（dm_touch_set）：游戏只认手柄；DM_PAD_OVERLAY=0 不画控件。
// 实体手柄（dm/host/gamepad_generic.m，GameController）：连上第一个时覆盖层自动藏起、只剩摇杆落点区；最后一个断开再显出来。失活时手柄键一并清零。
// Generic host iOS front end (implements front.h, the counterpart of dm/host/front_mac.m on the Mac): Metal present view + generic virtual gamepad + software keyboard input box + scene lifecycle.
// Virtual gamepad: drawn over the game picture, draws only and takes no touches; touches are collected by the present view. Each finger is classified by where it lands --
// inside a button circle = that button, free space in the left half = left stick (floating: the landing point is the centre), free space in the right half = right stick;
// on every change the whole state goes to the host through dm_input_set (hle_hid.c applies it when the game polls the controller). Buttons = DM_BTN_* of dm_input.h, sticks ±32767, up positive.
// The touchscreen (dm_touch_set) is not wired: the game only accepts controllers; DM_PAD_OVERLAY=0 hides the widgets.
// Physical controllers (dm/host/gamepad_generic.m, GameController): when the first one connects the overlay hides itself and only the stick landing zones remain; it reappears when the last one disconnects. Controller buttons are zeroed on resign active as well.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "hle.h"

static CAMetalLayer *g_layer;

// ---- 虚拟手柄布局 | virtual gamepad layout ----
enum { PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_ZL, PB_ZR, PB_PLUS, PB_MINUS, PB_LS, PB_RS, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_N };
static const uint64_t k_bit[PB_N] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_PLUS, DM_BTN_MINUS, DM_BTN_LS, DM_BTN_RS, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT};
static const char *k_label[PB_N] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "−", "L3", "R3", "▲", "▼", "◀", "▶"};
typedef struct { CGPoint c; CGFloat r; } pad_circle;
typedef struct { pad_circle btn[PB_N]; CGFloat stick_r; CGSize size; } pad_layout;
// 按屏幕点数与安全区排：右下 A/B/X/Y 菱形，上沿 L ZL（左）R ZR（右）、+ −（中），L3 R3 小圆在菱形上方，左侧中部十字键（左摇杆落点区之上）；尺寸以画面高为基准
// Laid out from screen points and safe area: A/B/X/Y diamond bottom right, top edge L ZL (left) R ZR (right) + − (centre), small L3 R3 above the diamond, D-pad mid-left (above the left stick landing zone); sizes relative to the picture height
static void pad_layout_make(pad_layout *L, CGSize sz, UIEdgeInsets in) {
    CGFloat H = sz.height, W = sz.width, r = fmax(18, H * 0.065), s = r * 0.8, bot = H - in.bottom, top = in.top, left = in.left, right = W - in.right;
    L->size = sz; L->stick_r = r * 2.3;
    CGPoint d = CGPointMake(right - 2.9 * r, bot - 2.9 * r);   // 菱形中心 | diamond centre
    L->btn[PB_A] = (pad_circle){{d.x + 1.7 * r, d.y}, r}; L->btn[PB_B] = (pad_circle){{d.x, d.y + 1.7 * r}, r};
    L->btn[PB_X] = (pad_circle){{d.x - 1.7 * r, d.y}, r}; L->btn[PB_Y] = (pad_circle){{d.x, d.y - 1.7 * r}, r};
    L->btn[PB_L]  = (pad_circle){{left + 1.6 * s, top + 1.4 * s}, s}; L->btn[PB_ZL] = (pad_circle){{left + 4.4 * s, top + 1.4 * s}, s};
    L->btn[PB_R]  = (pad_circle){{right - 1.6 * s, top + 1.4 * s}, s}; L->btn[PB_ZR] = (pad_circle){{right - 4.4 * s, top + 1.4 * s}, s};
    L->btn[PB_MINUS] = (pad_circle){{W / 2 - 2.0 * s, top + 1.2 * s}, s * 0.85}; L->btn[PB_PLUS] = (pad_circle){{W / 2 + 2.0 * s, top + 1.2 * s}, s * 0.85};
    L->btn[PB_LS] = (pad_circle){{d.x - 3.4 * r, d.y - 3.2 * r}, s * 0.8}; L->btn[PB_RS] = (pad_circle){{d.x, d.y - 4.4 * r}, s * 0.8};
    CGFloat q = r * 0.75; CGPoint c = CGPointMake(left + 3.2 * r, H * 0.5 - 0.3 * r);   // 十字键中心：左侧中部，在左摇杆空闲圈之上 | D-pad centre: mid-left, above the idle left-stick ring
    L->btn[PB_UP] = (pad_circle){{c.x, c.y - 1.5 * q}, q}; L->btn[PB_DOWN] = (pad_circle){{c.x, c.y + 1.5 * q}, q};
    L->btn[PB_LEFT] = (pad_circle){{c.x - 1.5 * q, c.y}, q}; L->btn[PB_RIGHT] = (pad_circle){{c.x + 1.5 * q, c.y}, q};
}
static int pad_hit(const pad_layout *L, CGPoint p) {
    int best = -1; CGFloat bd = 1e9;
    for (int i = 0; i < PB_N; i++) { CGFloat dx = p.x - L->btn[i].c.x, dy = p.y - L->btn[i].c.y, d = sqrt(dx * dx + dy * dy); if (d < L->btn[i].r * 1.25 && d < bd) { bd = d; best = i; } }
    return best;
}

// ---- 覆盖层：只画 | overlay: draws only ----
@interface DmPadOverlay : UIView
@property (assign) pad_layout *layout;
@property (assign) uint64_t held;                 // 当前按下的键位 | buttons currently held
@property (assign) BOOL lsOn, rsOn; @property (assign) CGPoint lsC, lsP, rsC, rsP;   // 摇杆中心 / 当前点 | stick centre / current point
@end
@implementation DmPadOverlay
- (instancetype)initWithFrame:(CGRect)f { if ((self = [super initWithFrame:f])) { self.userInteractionEnabled = NO; self.opaque = NO; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw; } return self; }
- (void)drawCircle:(pad_circle)c label:(const char *)lb on:(BOOL)on ctx:(CGContextRef)g {
    CGRect r = CGRectMake(c.c.x - c.r, c.c.y - c.r, 2 * c.r, 2 * c.r);
    CGContextSetRGBFillColor(g, 1, 1, 1, on ? 0.45 : 0.14); CGContextFillEllipseInRect(g, r);
    CGContextSetRGBStrokeColor(g, 1, 1, 1, 0.55); CGContextSetLineWidth(g, 1.5); CGContextStrokeEllipseInRect(g, r);
    NSDictionary *at = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:c.r * 0.9], NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:0.85]};
    NSString *s = @(lb); CGSize ts = [s sizeWithAttributes:at]; [s drawAtPoint:CGPointMake(c.c.x - ts.width / 2, c.c.y - ts.height / 2) withAttributes:at];
}
- (void)drawStickAt:(CGPoint)c to:(CGPoint)p on:(BOOL)on ctx:(CGContextRef)g {
    CGFloat R = self.layout->stick_r;
    CGContextSetRGBStrokeColor(g, 1, 1, 1, on ? 0.6 : 0.25); CGContextSetLineWidth(g, 2); CGContextStrokeEllipseInRect(g, CGRectMake(c.x - R, c.y - R, 2 * R, 2 * R));
    CGFloat dx = p.x - c.x, dy = p.y - c.y, d = sqrt(dx * dx + dy * dy); if (d > R) { dx *= R / d; dy *= R / d; }
    CGFloat k = R * 0.42; CGContextSetRGBFillColor(g, 1, 1, 1, on ? 0.5 : 0.18); CGContextFillEllipseInRect(g, CGRectMake(c.x + dx - k, c.y + dy - k, 2 * k, 2 * k));
}
- (void)drawRect:(CGRect)rect {
    (void)rect; if (!self.layout) return; CGContextRef g = UIGraphicsGetCurrentContext();
    for (int i = 0; i < PB_N; i++) [self drawCircle:self.layout->btn[i] label:k_label[i] on:(self.held & k_bit[i]) != 0 ctx:g];
    // 左摇杆：没按时在左下画个淡圈提示落点区；按着时跟手指 | Left stick: when idle a faint ring bottom left hints the landing zone; while held it follows the finger
    CGPoint idle = CGPointMake(self.layout->stick_r * 1.6 + self.safeAreaInsets.left, self.layout->size.height - self.layout->stick_r * 1.5 - self.safeAreaInsets.bottom);
    if (self.lsOn) [self drawStickAt:self.lsC to:self.lsP on:YES ctx:g]; else [self drawStickAt:idle to:idle on:NO ctx:g];
    if (self.rsOn) [self drawStickAt:self.rsC to:self.rsP on:YES ctx:g];
}
@end

// ---- 出图视图：CAMetalLayer + 触点归类 | present view: CAMetalLayer + touch classification ----
enum { ROLE_NONE, ROLE_BTN, ROLE_LS, ROLE_RS };
typedef struct { UITouch *__unsafe_unretained t; int role, btn; CGPoint c; } slot_t;
@interface DmBladeView : UIView { pad_layout _L; slot_t _slot[16]; DmPadOverlay *_ov; BOOL _hide, _padOn; }   // _hide：DM_PAD_OVERLAY=0；_padOn：有实体手柄连着 | _hide: DM_PAD_OVERLAY=0; _padOn: a physical controller is connected
@end
@implementation DmBladeView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if (!(self = [super initWithFrame:f])) return nil;
    self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.blackColor;
    const char *e = getenv("DM_PAD_OVERLAY"); _hide = e && !atoi(e);
    pad_layout_make(&_L, f.size, UIEdgeInsetsZero);
    _ov = [[DmPadOverlay alloc] initWithFrame:self.bounds]; _ov.layout = &_L; _ov.hidden = _hide; _ov.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight; [self addSubview:_ov];
    return self;
}
- (BOOL)overlayOff { return _hide || _padOn; }
// 实体手柄连上 / 全部断开（gamepad_generic.m 在主队列回调）：藏 / 显覆盖层；藏着时触点只当摇杆，正按着的虚拟键松开 | A physical controller connected / all disconnected (gamepad_generic.m calls back on the main queue): hide / show the overlay; while hidden touches only count as sticks, held virtual buttons are released
- (void)padPresent:(BOOL)on {
    if (_padOn == on) return; _padOn = on; _ov.hidden = self.overlayOff;
    if (on) { for (int i = 0; i < 16; i++) if (_slot[i].role == ROLE_BTN) _slot[i] = (slot_t){0}; }
    [self publish]; LOG("前端：实体手柄%s，虚拟手柄覆盖层%s", on ? "已连接" : "全部断开", self.overlayOff ? "隐藏" : "显示");
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    l.drawableSize = CGSizeMake(self.bounds.size.width * l.contentsScale, self.bounds.size.height * l.contentsScale);
    pad_layout_make(&_L, self.bounds.size, self.safeAreaInsets); [_ov setNeedsDisplay];
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
static void stick_value(const pad_layout *L, CGPoint c, CGPoint p, int32_t *x, int32_t *y) {
    CGFloat dx = (p.x - c.x) / L->stick_r, dy = (p.y - c.y) / L->stick_r, d = sqrt(dx * dx + dy * dy);
    if (d > 1) { dx /= d; dy /= d; } else if (d < 0.08) dx = dy = 0;   // 小死区 | small dead zone
    *x = (int32_t)lrint(dx * DM_STICK_MAX); *y = (int32_t)lrint(-dy * DM_STICK_MAX);   // 屏幕 y 向下，Horizon 摇杆上为正 | screen y goes down, Horizon stick up is positive
}
- (void)publish {
    uint64_t bt = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0; BOOL ls = NO, rs = NO; CGPoint lc = {0}, lp = {0}, rc = {0}, rp = {0};
    for (int i = 0; i < 16; i++) { slot_t *s = &_slot[i]; if (!s->t) continue; CGPoint p = [s->t locationInView:self];
        if (s->role == ROLE_BTN) bt |= k_bit[s->btn];
        else if (s->role == ROLE_LS) { stick_value(&_L, s->c, p, &lx, &ly); ls = YES; lc = s->c; lp = p; }
        else if (s->role == ROLE_RS) { stick_value(&_L, s->c, p, &rx, &ry); rs = YES; rc = s->c; rp = p; } }
    dm_input_set(bt, lx, ly, rx, ry);
    _ov.held = bt; _ov.lsOn = ls; _ov.lsC = lc; _ov.lsP = lp; _ov.rsOn = rs; _ov.rsC = rc; _ov.rsP = rp; if (!self.overlayOff) [_ov setNeedsDisplay];
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) { int k = -1; for (int i = 0; i < 16; i++) if (!_slot[i].t) { k = i; break; } if (k < 0) continue;
        CGPoint p = [t locationInView:self]; int b = self.overlayOff ? -1 : pad_hit(&_L, p); slot_t *s = &_slot[k]; s->t = t; s->c = p;
        if (b >= 0) { s->role = ROLE_BTN; s->btn = b; } else s->role = p.x < self.bounds.size.width / 2 ? ROLE_LS : ROLE_RS; }
    [self publish];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) for (int i = 0; i < 16; i++) if (_slot[i].t == t && _slot[i].role == ROLE_BTN) {   // 手指滑到另一个键上就换键（A→B 连按的常见手法） | a finger sliding onto another button switches (the usual A→B roll)
        int b = pad_hit(&_L, [t locationInView:self]); if (b >= 0) _slot[i].btn = b; }
    [self publish];
}
- (void)drop:(NSSet<UITouch *> *)ts { for (UITouch *t in ts) for (int i = 0; i < 16; i++) if (_slot[i].t == t) _slot[i] = (slot_t){0}; [self publish]; }
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self drop:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self drop:ts]; }
- (void)clearAll { for (int i = 0; i < 16; i++) _slot[i] = (slot_t){0}; [self publish]; }
@end
static DmBladeView *g_view;

// 实体手柄（dm/host/gamepad_generic.m）：GameController 扩展手柄 → dm_pad_set；第一个连上 / 最后一个断开时回调这里藏 / 显覆盖层
// Physical controllers (dm/host/gamepad_generic.m): GameController extended gamepad → dm_pad_set; calls back here when the first connects / the last disconnects to hide / show the overlay
void dm_gamepad_start(void (*on_present)(int present)); void dm_gamepad_reset(void);
void dm_audio_session_background(int on);   // dm/host/audio_session.m：进后台 / 回前台 → 渲染器声卡单元停 / 起（与打断合成） | dm/host/audio_session.m: background / foreground → stop / start the renderer's audio unit (combined with interruptions)
static void pad_present(int on) { [g_view padPresent:on != 0]; }

UIView *front_blade_view(CGRect bounds, CGFloat scale) {
    DmBladeView *v = [[DmBladeView alloc] initWithFrame:bounds]; g_view = v;
    g_layer = (CAMetalLayer *)v.layer;
    g_layer.device = MTLCreateSystemDefaultDevice();
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.contentsScale = scale;
    g_layer.drawableSize = CGSizeMake(bounds.size.width * g_layer.contentsScale, bounds.size.height * g_layer.contentsScale);
    LOG("画面：窗口 %.0fx%.0f 点，出图 %.0fx%.0f 像素（游戏渲 1280x720，按窗口拉伸）", bounds.size.width, bounds.size.height, g_layer.drawableSize.width, g_layer.drawableSize.height);
    dm_gamepad_start(pad_present);
    return v;
}

void *front_metal_layer(void) { return (__bridge void *)g_layer; }

// 游戏输出分辨率：通用宿主两种模式都按 1280x720（Blade 只渲这个），DM_GAME_RES=宽x高 覆盖（同 front_mac.m 的 DM_GENERIC_HOST 分支）
// Game output resolution: the generic host uses 1280x720 in both modes (Blade only renders this), DM_GAME_RES=WxH overrides (same as the DM_GENERIC_HOST branch of front_mac.m)
uint64_t front_game_res(int docked) {
    (void)docked; unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return (uint64_t)h << 32 | w;
    return 720ull << 32 | 1280;
}

void front_run(void (*guest)(void)) {
    static char alt[65536];                       // 信号备用栈是每线程的：客体线程栈溢出时崩溃打印也要能跑 | The alternate signal stack is per-thread: the crash printout must still run when a guest thread overflows its stack
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_UNBLOCK, &all, NULL);   // 从 UIKit 主线程派生的线程屏蔽了 SIGUSR2 / SIGALRM（看门狗、采样） | threads spawned from the UIKit main thread block SIGUSR2 / SIGALRM (watchdog, sampling)
    guest();
}

// 软键盘输入框（front.h）：主线程弹 UIAlertController + 文本框，客体线程阻塞到确定 / 取消
// Software keyboard input box (front.h): the main thread shows a UIAlertController + text field, the guest thread blocks until OK / Cancel
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    NSString *t = title && *title ? @(title) : @"输入 | Input";
    NSString *msg = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
    NSString *init = initial ? @(initial) : @"";
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int r = 0; __block NSString *text = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:msg.length ? msg : nil preferredStyle:UIAlertControllerStyleAlert];
        a.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
        [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.text = init; f.keyboardAppearance = UIKeyboardAppearanceDark;
            if (max_chars > 0) [f addAction:[UIAction actionWithHandler:^(UIAction *act) { UITextField *tf = (UITextField *)act.sender;
                if (!tf.markedTextRange && (int)tf.text.length > max_chars) tf.text = [tf.text substringToIndex:(NSUInteger)max_chars]; }] forControlEvents:UIControlEventEditingChanged]; }];
        [a addAction:[UIAlertAction actionWithTitle:@"取消 | Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; r = 0; dispatch_semaphore_signal(done); }]];
        [a addAction:[UIAlertAction actionWithTitle:@"确定 | OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; text = a.textFields.firstObject.text ?: @""; r = 1; dispatch_semaphore_signal(done); }]];
        UIViewController *vc = g_view.window.rootViewController; if (!vc) { r = -1; dispatch_semaphore_signal(done); return; }
        [vc presentViewController:a animated:YES completion:nil];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (r == 1 && out && out_cap > 0) { strlcpy(out, text.UTF8String ?: "", (size_t)out_cap); }
    return r;
}

// 存档写失败（hle_fs.c，游戏线程调）：记日志并在主线程弹一次提示 | Save write failed (hle_fs.c, called on the game thread): log and show one alert on the main thread
void front_save_failed(int err) {
    LOG("【红】存档未保存：errno %d（%s）", err, strerror(err));
    static int shown; if (shown++) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *a = [UIAlertController alertControllerWithTitle:@"进度未保存 | Progress not saved" message:err == ENOSPC ? @"存储空间不足 | Not enough storage" : @(strerror(err)) preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [g_view.window.rootViewController presentViewController:a animated:YES completion:nil];
    });
}

// 场景生命周期（main.m 的 SceneDelegate）：0 失活 → 松开全部输入、焦点「后台中」；1 进后台 → 音频停、客体时钟冻结、GPU 挂起；
// 2 回前台 → 反向恢复；3 激活 → 焦点「前台」。与暗黑外壳 front_ios_focus_event 的宿主调用一致，只去掉了暗黑的存盘 / iCloud / 触屏层部分
// Scene lifecycle (SceneDelegate in main.m): 0 resign → release all input, focus "in background"; 1 background → audio off, guest clock frozen, GPU suspended;
// 2 foreground → restore in reverse; 3 active → focus "foreground". Same host calls as the Diablo shell's front_ios_focus_event, minus Diablo's save / iCloud / touch-layer parts
void front_blade_focus(int ev) {
    static const char *k[] = {"sceneWillResignActive", "sceneDidEnterBackground", "sceneWillEnterForeground", "sceneDidBecomeActive"};
    LOG("前端：%s", k[ev & 3]);
    switch (ev) {
        case 0: [g_view clearAll]; dm_gamepad_reset(); dm_focus_set(2); break;   // 实体手柄按着的键一并清零（松开事件在后台收不到） | controller buttons held are zeroed too (release events do not arrive in the background)
        case 1: dm_focus_set(3); dm_audio_suspend(1); dm_audio_session_background(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); break;   // 挂起返回前在途 GPU 已清空 | in-flight GPU work is drained before the suspend returns
        case 2: dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_session_background(0); dm_audio_suspend(0); dm_focus_set(2); break;
        case 3: dm_focus_set(1); break;
    }
}
