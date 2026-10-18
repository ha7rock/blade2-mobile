// 通用宿主的 iOS 前端（front.h 的实现，对应 Mac 的 dm/host/front_mac.m）：Metal 出图视图 + 通用虚拟手柄 + 软键盘输入框 + 场景生命周期。
// 虚拟手柄：画在游戏画面之上、只画不收触点，触点由出图视图统一收；每根手指按落点归类——按钮圆内 = 该键，左半屏空处 = 左摇杆（浮动：落点为中心），
// 右半屏空处 = 右摇杆；每次变化把整组状态经 dm_input_set 交给宿主（hle_hid.c 在游戏读手柄时落地）。键位 = dm_input.h 的 DM_BTN_*，摇杆 ±32767、上为正。
// 图层的几何与绘制在 pad_layout.h（布局依据与尺寸口径见该文件头），Mac 上 tools/pad_mockup.m 用同一份代码出效果图；空闲 2.5 s 淡出、按键有震感。
// 不接触摸屏（dm_touch_set）：游戏只认手柄；DM_PAD_OVERLAY=0 不画控件。
// 实体手柄（dm/host/gamepad_generic.m，GameController）：连上第一个时覆盖层自动藏起、只剩摇杆落点区；最后一个断开再显出来。失活时手柄键一并清零。
// Generic host iOS front end (implements front.h, the counterpart of dm/host/front_mac.m on the Mac): Metal present view + generic virtual gamepad + software keyboard input box + scene lifecycle.
// Virtual gamepad: drawn over the game picture, draws only and takes no touches; touches are collected by the present view. Each finger is classified by where it lands --
// inside a button circle = that button, free space in the left half = left stick (floating: the landing point is the centre), free space in the right half = right stick;
// on every change the whole state goes to the host through dm_input_set (hle_hid.c applies it when the game polls the controller). Buttons = DM_BTN_* of dm_input.h, sticks ±32767, up positive.
// Geometry and drawing of the layer live in pad_layout.h (layout rationale and sizing in its header); tools/pad_mockup.m renders mock-ups on the Mac from the same code; fades after 2.5 s idle, haptic tap on buttons.
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

// ---- 虚拟手柄：几何与绘制在 pad_layout.h（与 tools/pad_mockup.m 共用） | virtual gamepad: geometry and drawing live in pad_layout.h (shared with tools/pad_mockup.m) ----
// 环境变量：DM_PAD_OVERLAY=0 不画；DM_PAD_SCALE=0.8–1.4 整体缩放；DM_PAD_ALPHA=空闲透明度（缺省 0.45，松手 2.5 s 后淡下去，再碰立即恢复）；DM_PAD_HAPTIC=0 关按键震感
// Environment: DM_PAD_OVERLAY=0 hides it; DM_PAD_SCALE=0.8-1.4 scales everything; DM_PAD_ALPHA = idle opacity (default 0.45, fades 2.5 s after the last touch, restored on the next touch); DM_PAD_HAPTIC=0 disables button haptics
#include "pad_layout.h"
static double env_num(const char *k, double dflt) { const char *e = getenv(k); return e && *e ? atof(e) : dflt; }

// ---- 覆盖层：只画 | overlay: draws only ----
@interface DmPadOverlay : UIView
@property (assign) pad_layout *layout;
@property (assign) pad_state state;
@end
@implementation DmPadOverlay
- (instancetype)initWithFrame:(CGRect)f { if ((self = [super initWithFrame:f])) { self.userInteractionEnabled = NO; self.opaque = NO; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw; } return self; }
- (void)drawRect:(CGRect)rect { (void)rect; if (!self.layout) return; pad_state s = self.state; pad_draw(UIGraphicsGetCurrentContext(), self.layout, &s); }
@end

// ---- 出图视图：CAMetalLayer + 触点归类 | present view: CAMetalLayer + touch classification ----
enum { ROLE_NONE, ROLE_BTN, ROLE_LS, ROLE_RS };
typedef struct { UITouch *__unsafe_unretained t; int role, btn; CGPoint c; } slot_t;
@interface DmBladeView : UIView { pad_layout _L; slot_t _slot[16]; DmPadOverlay *_ov; BOOL _hide, _padOn; CGFloat _scale, _idleAlpha; CFAbsoluteTime _lastTouch; BOOL _dim; NSTimer *_fade; UIImpactFeedbackGenerator *_tap; }   // _hide：DM_PAD_OVERLAY=0；_padOn：有实体手柄连着 | _hide: DM_PAD_OVERLAY=0; _padOn: a physical controller is connected
@end
@implementation DmBladeView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if (!(self = [super initWithFrame:f])) return nil;
    self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.blackColor;
    const char *e = getenv("DM_PAD_OVERLAY"); _hide = e && !atoi(e);
    _scale = fmin(1.4, fmax(0.8, env_num("DM_PAD_SCALE", 1.0))); _idleAlpha = fmin(1.0, fmax(0.1, env_num("DM_PAD_ALPHA", 0.45)));
    if (env_num("DM_PAD_HAPTIC", 1) > 0) { _tap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight]; [_tap prepare]; }
    pad_layout_make(&_L, f.size, 0, 0, 0, 0, _scale);
    _ov = [[DmPadOverlay alloc] initWithFrame:self.bounds]; _ov.layout = &_L; _ov.hidden = _hide; _ov.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight; [self addSubview:_ov];
    _lastTouch = CFAbsoluteTimeGetCurrent();
    _fade = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self selector:@selector(fadeTick) userInfo:nil repeats:YES];
    return self;
}
- (BOOL)overlayOff { return _hide || _padOn; }
// 空闲淡出：松手 2.5 s 后降到 _idleAlpha；任何触点立即回到 1（动画 0.25 s），HUD 不被长期压住 | idle fade: 2.5 s after the last touch drop to _idleAlpha; any touch brings it back to 1 (0.25 s animation) so the HUD is not covered permanently
- (void)fadeTick {
    if (self.overlayOff) return;
    BOOL dim = CFAbsoluteTimeGetCurrent() - _lastTouch > 2.5; if (dim == _dim) return; _dim = dim;
    [UIView animateWithDuration:dim ? 0.6 : 0.15 animations:^{ self->_ov.alpha = dim ? self->_idleAlpha : 1.0; }];
}
- (void)wake { _lastTouch = CFAbsoluteTimeGetCurrent(); if (_dim) { _dim = NO; [_fade fire]; _dim = NO; _ov.alpha = 1.0; } }
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
    UIEdgeInsets in = self.safeAreaInsets; pad_layout_make(&_L, self.bounds.size, in.top, in.left, in.bottom, in.right, _scale); [_ov setNeedsDisplay];
    static CGSize last; if (!CGSizeEqualToSize(last, self.bounds.size)) { last = self.bounds.size;
        LOG("虚拟手柄：窗口 %.0fx%.0f 点，安全区 上%.0f 左%.0f 下%.0f 右%.0f，画面 %.0f,%.0f %.0fx%.0f，基准半径 %.1f 点（缩放 %.2f）", last.width, last.height, in.top, in.left, in.bottom, in.right, _L.pic.origin.x, _L.pic.origin.y, _L.pic.size.width, _L.pic.size.height, _L.r, _scale); }
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (void)publish {
    uint64_t bt = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0; pad_state st = {0};
    for (int i = 0; i < 16; i++) { slot_t *s = &_slot[i]; if (!s->t) continue; CGPoint p = [s->t locationInView:self];
        if (s->role == ROLE_BTN) bt |= k_bit[s->btn];
        else if (s->role == ROLE_LS) { pad_stick_value(&_L, s->c, p, &lx, &ly); st.ls_on = 1; st.ls_c = s->c; st.ls_p = p; }
        else if (s->role == ROLE_RS) { pad_stick_value(&_L, s->c, p, &rx, &ry); st.rs_on = 1; st.rs_c = s->c; st.rs_p = p; } }
    dm_input_set(bt, lx, ly, rx, ry);
    st.held = bt; _ov.state = st; if (!self.overlayOff) [_ov setNeedsDisplay];
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e; [self wake];
    for (UITouch *t in ts) { int k = -1; for (int i = 0; i < 16; i++) if (!_slot[i].t) { k = i; break; } if (k < 0) continue;
        CGPoint p = [t locationInView:self]; int b = self.overlayOff ? -1 : pad_hit(&_L, p); slot_t *s = &_slot[k]; s->t = t; s->c = p;
        if (b >= 0) { s->role = ROLE_BTN; s->btn = b; [_tap impactOccurred]; } else s->role = p.x < _L.split_x ? ROLE_LS : ROLE_RS; }
    [self publish];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e; [self wake];
    for (UITouch *t in ts) for (int i = 0; i < 16; i++) if (_slot[i].t == t && _slot[i].role == ROLE_BTN) {   // 手指滑到另一个键上就换键（A→B 连按的常见手法） | a finger sliding onto another button switches (the usual A→B roll)
        int b = pad_hit(&_L, [t locationInView:self]); if (b >= 0 && b != _slot[i].btn) { _slot[i].btn = b; [_tap impactOccurred]; } }
    [self publish];
}
- (void)drop:(NSSet<UITouch *> *)ts { [self wake]; for (UITouch *t in ts) for (int i = 0; i < 16; i++) if (_slot[i].t == t) _slot[i] = (slot_t){0}; [self publish]; }
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
    LOG("画面：窗口 %.0fx%.0f 点，出图 %.0fx%.0f 像素（游戏渲 1280x720，出图时 16:9 居中留黑边）", bounds.size.width, bounds.size.height, g_layer.drawableSize.width, g_layer.drawableSize.height);
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
