// Blade II 宿主：d2r-mobile 的通用部分（装载 / SVC / 服务接管 / 图形层）里残留的 D2R 功能钩子，在这里给出中性实现。
// 原则：输入过滤一律原样放行；查询一律回答「没有 / 不在」；只在 D2R 剪口才会到达的宿主调用（svc 0x80 起）一律中止——Blade 的产物里没有这些门。
// 哪些符号：dm/tools/host_trial_build.sh 的 undefined.txt（通用部分不带任何 dm_*.c 编译后链接缺的 100 个名字）。
// Blade II host: neutral implementations of the D2R feature hooks that the generic part of d2r-mobile (loader / SVC / service takeover /
// graphics layer) still calls. Rules: input filters pass everything through; queries answer "none / not there"; host calls that only a D2R
// cut can reach (svc 0x80 and up) abort -- Blade's build output has no such gates.
// Which symbols: undefined.txt from dm/tools/host_trial_build.sh (the 100 names missing at link when the generic part is built without any dm_*.c).
#include "dm.h"
#include "front.h"
#include <stdatomic.h>

_Atomic int g_dm_frozen;

// ---- 分辨率 / 画面 | resolution / screen ----
uint64_t dm_game_res(int docked) { return front_game_res(docked); }
int dm_screen_real(int *w, int *h) { (void)w; (void)h; return 0; }
int dm_uiscreen_ingame(void) { return 1; }
void dm_uiscreen_enable(int mode) { (void)mode; }
void dm_uiscreen_res_reread(void) {}
float dm_ui_ny(float ny) { return ny; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = *y = 0; *w = *h = 0; }

// ---- 输入过滤（hle_hid.c fill）：原样放行 | input filters (hle_hid.c fill): pass through ----
int dm_padmode_input_gate(uint64_t *buttons) { (void)buttons; return 0; }
uint64_t dm_padmode_page_filter(uint64_t bt) { return bt; }
uint64_t dm_runeword_pad_filter(uint64_t bt) { return bt; }
uint64_t dm_swapskill_filter(uint64_t bt) { return bt; }
uint64_t dm_ui_alone_take(uint64_t others) { (void)others; return 0; }
uint64_t dm_ui_auto_buttons(void) { return 0; }
uint64_t dm_ui_auto_release(void) { return 0; }
void dm_cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly) { (void)buttons; (void)lx; (void)ly; }
void dm_cursor_set(int x, int y) { (void)x; (void)y; }
int dm_cursor_item(void *out) { (void)out; return 0; }
int dm_held_size(int *w, int *h) { (void)w; (void)h; return 0; }
int dm_padmode_current(void) { return 1; }                      // 1 = 手柄模式（没有触屏界面层） | 1 = controller mode (no touch UI layer)
void dm_padmode_request_kbd(int kbd) { (void)kbd; }
void dm_padmode_boot(void) {}

// ---- 每帧泵 / 动作 / 测试动作：无事可做 | per-frame pumps / actions / test actions: nothing to do ----
void dm_actions_pump(void) {} void dm_world_pump(void) {} void dm_textinput_pump(void) {} void dm_update_pump(void) {}
void dm_ui_frontend_pump(void) {} void dm_ui_mouse_pump(void) {}
void dm_belt_expand(int on) { (void)on; } void dm_belt_put(uint32_t id) { (void)id; } void dm_held_stow(void) {}
void dm_inv_sort(int page) { (void)page; } void dm_item_compare(int on) { (void)on; } int dm_item_targeting(void) { return 0; }
void dm_name_random(void) {} void dm_num_set(int n) { (void)n; } void dm_shop_buy(void) {} void dm_shop_sell_cursor(void) {}
void dm_skill_add_selected(void) {} void dm_stash_take(int n) { (void)n; } void dm_stat_add(int s, int n) { (void)s; (void)n; }
void dm_test_belt_first_potion(void) {} void dm_test_equip(int w) { (void)w; } void dm_test_hold_first_sellable(void) {}
void dm_test_hover_at(float x, float y) { (void)x; (void)y; } void dm_test_identify(int w) { (void)w; } void dm_test_sell_first(void) {}
void dm_test_stash_take(int n) { (void)n; } void dm_test_swap_held(void) {} void dm_test_use_first(int k) { (void)k; }
void dm_world_tap(float x, float y) { (void)x; (void)y; } void dm_world_watch(int t, uint32_t c) { (void)t; (void)c; }
int dm_world_watch_pos(float *x, float *y) { (void)x; (void)y; return 0; }

// ---- 界面层（触屏）：没有 | UI layer (touch): none ----
const char *dm_ui(const char *key) { return key; }
void dm_ui_drag_begin(int k, float x, float y, float t) { (void)k; (void)x; (void)y; (void)t; }
void dm_ui_drag_end(void) {} void dm_ui_drag_move(float x, float y) { (void)x; (void)y; }
unsigned dm_ui_item_query(float x, float y) { (void)x; (void)y; return 0; }
int dm_ui_item_result(unsigned seq) { (void)seq; return 3; }
void dm_ui_map_full(int on) { (void)on; } void dm_ui_ptr(int k, float x, float y, int a) { (void)k; (void)x; (void)y; (void)a; }
void dm_ui_send_queue(const char *spec) { (void)spec; } void dm_ui_tab_go(void) {} uint64_t dm_ui_taps_base(void) { return 0; }
void dm_ui_text_entered(const char *t) { (void)t; } void dm_ui_touch(int p, float x, float y) { (void)p; (void)x; (void)y; }
void dm_touch_host_live(int op, float x, float y) { (void)op; (void)x; (void)y; } void dm_touch_host_live_enable(void) {}
// dm_gamepad_start / dm_gamepad_reset：实体手柄在 gamepad_generic.m（通用实现，Mac 与 iOS 共用），不再是空实现 | physical controllers live in gamepad_generic.m (generic, shared by Mac and iOS), no longer stubbed

// ---- 状态查询 | state queries ----
int dm_ingame_now(void) { return 1; }
uint64_t dm_server_tick(void) { return UINT64_MAX; }
int dm_loadscreen_up(void) { return 0; }
void dm_hold_for_loadscreen(void) {}
int dm_unpause_for_exit(void) { return 0; }
int dm_request_save(void) { return 0; }
int dm_cloud_flush_wait(double sec) { (void)sec; return 1; }
void dm_cloud_boot(void) {}
int dm_custom_lang_explicit(void) { return -1; }
void dm_custom_init(const char *dir) { (void)dir; }
int dm_sys_lang(void) { return 0; }
void dm_update_init(int own, void (*a)(void), void (*b)(void)) { (void)own; (void)a; (void)b; }
void dm_update_offer(int b, const char *n, int u) { (void)b; (void)n; (void)u; }
void dm_update_safe_left(int u) { (void)u; }
void dm_prof_mark(const char *seg) { if (g_flag.bootprof) LOG("prof %s", seg); }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }

// ---- 诊断（diag.c 未编入）| diagnostics (diag.c not compiled in) ----
void ticks_install(void) {} void inflate_bench_maybe(void) {}   // probe_install：sampler_blade.c   // crash_sample_self：sampler_blade.c | crash_sample_self: sampler_blade.c
void uimsg_subscribe(void (*fn)(const char *, const char *, const char *)) { (void)fn; }

// ---- D2R 剪口才会到达的宿主调用：Blade 产物里没有这些门 | host calls only D2R cuts reach: Blade's build output has no such gates ----
#define NOGATE(name) void name(uint64_t *regs) { (void)regs; die("D2R host call " #name " reached: this build output must not contain such a gate"); }
NOGATE(c2s_deliver) NOGATE(c2s_deliver_sys) NOGATE(svc_drop_sound) NOGATE(svc_ground_asset) NOGATE(svc_merc_slot) NOGATE(svc_merc_slot_cl)
NOGATE(svc_rift_exit) NOGATE(svc_save_act)
void svc_join_spawn(uint64_t *regs, int corpse) { (void)regs; (void)corpse; die("D2R host call svc_join_spawn reached"); }
