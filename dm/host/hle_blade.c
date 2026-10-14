// Blade II: The Return of Evil（Switch 版，UE4 4.17，NintendoSDK 7.4）比暗黑 II 多用到的 SDK 服务接口，在这里接管成宿主实现。
// 清单来源：docs/main_imports_vs_d2r.tsv（main 导入的 608 个符号里 d2r 宿主没接的那些，按命名空间）。
// 原则与 d2r 相同：真机上走 IPC 的服务层一律在 SDK 接口处接管（HLE_FORCE），SDK 的 nn::os / libc / libc++ 原样运行不碰。
// 这一版先求「能启动、能出第一帧」：音频渲染器只维持协议（定时触发事件、所有请求成功、不出声），联网一律「不可用」，六轴 / 震动「没有」。
// Blade II: The Return of Evil (Switch, UE4 4.17, NintendoSDK 7.4) uses more SDK service interfaces than Diablo II; they are taken over here.
// Source of the list: docs/main_imports_vs_d2r.tsv (the symbols among main's 608 imports that the d2r host does not implement, by namespace).
// Same principle as d2r: service layers that go through IPC on a real device are taken over at the SDK interface (HLE_FORCE); the SDK's nn::os / libc / libc++ run as is.
// This first version aims at "boots and shows the first frame": the audio renderer only keeps the protocol alive (periodic event, every request succeeds, no sound),
// networking is uniformly "unavailable", six-axis / vibration "absent".
#include "hle.h"
#include "front.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <errno.h>

void hle_bind_system_event(void *ev, int autoclear);   // hle_thread.c
void hle_signal_system_event(void *ev);

// ---------- movie（subsdk0 的任天堂视频库，不装载）：d2r 已空播；UE4 的播放器还读媒体信息 ----------
// ---------- movie (Nintendo video library in subsdk0, not loaded): d2r already plays empty; UE4's player also reads media info ----------
HLE_FORCE("_ZN5movie9MediaData5ClearEv", "过场空播：空 MediaData", void, h_mv_Clear, void **self) { *self = 0; }
HLE_FORCE("_ZN5movie9MediaDataC1ERKS0_", "过场空播：空 MediaData（拷贝构造）", void, h_mv_CopyCtor, void **self, void **other) { *self = *other; }
HLE_FORCE("_ZN5movie9MediaDataaSERKS0_", "过场空播：空 MediaData（赋值）", void *, h_mv_Assign, void **self, void **other) { *self = *other; return self; }
HLE_FORCE("_ZNK5movie9MediaData10FindStringEPKcPS2_", "过场空播：媒体信息一律「没有」（返回 bool）", unsigned, h_mv_FindString, const void *self, const char *key, const char **out) { (void)self; (void)key; if (out) *out = ""; return 0; }
HLE_FORCE("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", "过场空播：媒体信息一律「没有」", unsigned, h_mv_FindRect, const void *self, const char *key, int *l, int *t, int *r, int *b) { (void)self; (void)key; if (l) *l = 0; if (t) *t = 0; if (r) *r = 0; if (b) *b = 0; return 0; }
HLE_FORCE("_ZNK5movie9MediaData9FindInt32EPKcPi", "过场空播：媒体信息一律「没有」", unsigned, h_mv_FindInt32, const void *self, const char *key, int *out) { (void)self; (void)key; if (out) *out = 0; return 0; }
HLE_FORCE("_ZNK5movie9MediaData9FindInt64EPKcPl", "过场空播：媒体信息一律「没有」", unsigned, h_mv_FindInt64, const void *self, const char *key, int64_t *out) { (void)self; (void)key; if (out) *out = 0; return 0; }

// ---------- nn::hid 六轴 / 握持方式 / 手柄支持小程序 / 震动信息 ----------
// ---------- nn::hid six-axis / joy hold type / controller support applet / vibration info ----------
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "六轴传感器：宿主没有，句柄数 0", int, h_hid_SixHandles, void *out, int n, const uint32_t *id, uint32_t styles) { (void)out; (void)n; (void)id; (void)styles; return 0; }
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六轴传感器：宿主没有", void, h_hid_SixStart, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六轴传感器：宿主没有", void, h_hid_SixStop, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "六轴传感器：宿主没有，0 条状态", int, h_hid_SixStates, void *out, int n, const void *h) { (void)out; (void)n; (void)h; return 0; }
HLE_FORCE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", "Joy-Con 握持方式：竖持（0）", int, h_hid_GetHold, void) { return 0; }
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con 握持方式：宿主不理", void, h_hid_SetHold, int t) { (void)t; }
// ControllerSupportResultInfo = {int playerCount; u32 selectedId}（SDK 公开布局）：报 1 个玩家、1P 全键手柄
// ControllerSupportResultInfo = {int playerCount; u32 selectedId} (public SDK layout): report 1 player, the 1P full-key controller
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "手柄支持小程序：宿主直接答 1P 已连", Result, h_hid_ShowSupport, int32_t *info, const void *arg) { (void)arg; if (info) { info[0] = 1; info[1] = 0; } LOG("hid::ShowControllerSupport → 1P"); return RES_OK; }
HLE_FORCE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", "震动设备信息：宿主没有震动（设备数 0，这里不应到达）", void, h_hid_VibInfo, uint32_t *info, const void *h) { (void)h; if (info) { info[0] = 0; info[1] = 0; } }

// ---------- nn::oe ----------
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "退出请求处理区：宿主无 am 服务，空操作", void, h_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "退出请求处理区：空操作", void, h_oe_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "自动休眠时间：宿主不理", void, h_oe_Inactivity, unsigned b) { (void)b; }

// ---------- nn::time 时区规则版 ----------
// ---------- nn::time with time zone rules ----------
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t wday, yday; char tzname[8]; uint8_t dst; uint8_t pad[3]; int32_t offset; } CalendarAdditionalInfo;
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", "带时区规则的日历换算：规则忽略，用宿主 localtime_r", Result, h_time_ToCalRule, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt, const void *rule) {
    (void)rule; time_t t = (time_t)*pt; struct tm tm; localtime_r(&t, &tm);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname); ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", "时区规则（0x4000 字节不透明）：宿主换算不用它，清零即可", Result, h_time_LoadRule, void *rule, const char *name) { (void)name; if (rule) memset(rule, 0, 0x4000); return RES_OK; }
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "设备时区名（36 字节）：取宿主 TZ 名", Result, h_time_LocName, char *name) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); memset(name, 0, 36); strlcpy(name, tm.tm_zone ? tm.tm_zone : "UTC", 36); return RES_OK; }
HLE_FORCE("_ZN2nn4time8FinalizeEv", "time 服务已被接管", void, h_time_Finalize, void) {}

// ---------- nn::swkbd 配置（ShowKeyboard 本体在 hle_swkbd.c） ----------
// ---------- nn::swkbd configuration (ShowKeyboard itself is in hle_swkbd.c) ----------
HLE_FORCE("_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE", "键盘配置预设：游戏已清零配置，这里不改", void, h_kbd_MakePreset, void *cfg, int preset) { (void)cfg; (void)preset; }
HLE_FORCE("_ZN2nn5swkbd16SetGuideTextUtf8EPNS0_14KeyboardConfigEPKc", "键盘提示文字：宿主输入框不显示", void, h_kbd_Guide, void *cfg, const char *s) { (void)cfg; (void)s; }
HLE_FORCE("_ZN2nn5swkbd18SetInitialTextUtf8EPNS0_15ShowKeyboardArgEPKc", "键盘初始文字：宿主输入框从空开始", void, h_kbd_Initial, void *arg, const char *s) { (void)arg; (void)s; }
HLE_FORCE("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb", "键盘工作区大小：给 SDK 公开值", uint64_t, h_kbd_WorkSize, unsigned dict) { (void)dict; return 0x8000; }
HLE_FORCE("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv", "键盘字符串缓冲大小：SDK 公开值 0x7D4", uint64_t, h_kbd_StrSize, void) { return 0x7D4; }

// ---------- nn::vi / nn::web / nn::err / nn::pctl / nn::diag ----------
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "图层缩放方式：出图由图形层自己按屏幕缩放", Result, h_vi_Scaling, void *layer, int mode) { (void)layer; (void)mode; return RES_OK; }
HLE_FORCE("_ZN2nn3web22ShowOfflineHtmlPageArgC1EPKc", "离线网页小程序参数：清零", void, h_web_ArgCtor, void *self, const char *path) { (void)path; memset(self, 0, 0x1000); }
HLE_FORCE("_ZN2nn3web26OfflineHtmlPageReturnValueC1Ev", "离线网页小程序返回值：清零", void, h_web_RetCtor, void *self) { memset(self, 0, 0x10); }
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "离线网页小程序（说明书页）：宿主不弹，报失败", Result, h_web_Show, void *ret, const void *arg) { (void)ret; (void)arg; LOG("web::ShowOfflineHtmlPage（不弹）"); return RES_FAIL; }
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "错误码显示小程序：宿主打印", void, h_err_ShowCode, uint64_t code) { LOG("err::ShowError 错误码 %04llu-%04llu", (unsigned long long)(code & 0xffffffff), (unsigned long long)(code >> 32)); }
HLE_FORCE("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", "家长控制·自由通信许可：单机，直接允许", Result, h_pctl_CheckFree, unsigned show) { (void)show; return RES_OK; }
// 可变参数：Switch 的可变参数在寄存器里、苹果在栈上，不能按 "..." 收；只打印格式串本身
// Variadic: on the Switch variadic arguments are in registers, on Apple on the stack; cannot be received as "..."; only the format string itself is printed
HLE_FORCE("_ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz", "SDK 日志（lm 服务）：宿主只打印格式串", void, h_diag_Log, const void *meta, const char *fmt) { (void)meta; if (g_flag.dbg) LOG("[nn::diag] %s", fmt ? fmt : "(null)"); }
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", "SDK 中止：宿主打印位置后退出", void, h_diag_Abort, const char *cond, const char *func, const char *file, int line) { die("nn::diag::Abort：%s（%s，%s:%d）", cond ? cond : "", func ? func : "", file ? file : "", line); }
HLE_FORCE("nndetailDiagAbortFormatImpl", "SDK 中止（带格式）：宿主打印后退出", void, h_diag_AbortFmt, const char *cond, const char *func, const char *file, int line, const void *result, const void *fmt) { (void)result; die("nn::diag::Abort：%s（%s，%s:%d）%s", cond ? cond : "", func ? func : "", file ? file : "", line, fmt ? (const char *)fmt : ""); }

// ---------- nn::audio 音频渲染器（audren）：先只维持协议 ----------
// 真机：游戏把声音数据交给渲染器（voice → final mix → device sink），每 5 ms 一次 RequestUpdate，系统事件每周期触发一次。
// 宿主这一版：打开即起一条定时线程每 5 ms 触发那只系统事件（游戏的音频线程靠它推进），所有请求成功，不混音、不出声；
// 波形缓冲永不「播完归还」。真出声（CoreAudio 混 voice）是下一步，接口形状已在这里固定。
// ---------- nn::audio audio renderer (audren): protocol only for now ----------
// Real device: the game hands sound data to the renderer (voice → final mix → device sink), one RequestUpdate every 5 ms, the system event fires once per period.
// This host version: on open, start a timer thread that signals that system event every 5 ms (the game's audio thread advances on it), every request succeeds,
// no mixing, no sound; wave buffers are never "played and returned". Real output (CoreAudio mixing the voices) is the next step; the interface shape is fixed here.
#define AUDREN_PERIOD_NS 5000000ll
static struct { void *event; pthread_t th; _Atomic int run, started; } g_ren;
static void *audren_tick(void *a) { (void)a; pthread_setname_np("blade audren tick"); while (atomic_load(&g_ren.run)) { if (atomic_load(&g_ren.started) && g_ren.event) hle_signal_system_event(g_ren.event); struct timespec ts = {0, AUDREN_PERIOD_NS}; nanosleep(&ts, NULL); } return NULL; }
// AudioRendererParameter（SDK 公开布局，0x34 字节）：+0 sampleRate +4 sampleCount +8 mixBufferCount +0xc subMixCount +0x10 voiceCount +0x14 sinkCount +0x18 effectCount +0x1c performanceFrameCount ...
// AudioRendererParameter (public SDK layout, 0x34 bytes): +0 sampleRate +4 sampleCount +8 mixBufferCount +0xc subMixCount +0x10 voiceCount +0x14 sinkCount +0x18 effectCount +0x1c performanceFrameCount ...
HLE_FORCE("_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE", "渲染器参数缺省值（SDK 公开缺省：48 kHz、240 样本、24 混音缓冲、0 子混音…）", void, h_ren_InitParam, int32_t *p) {
    memset(p, 0, 0x34); p[0] = 48000; p[1] = 240; p[2] = 24; p[3] = 0; p[4] = 24; p[5] = 1; p[6] = 0; p[7] = 0;
}
HLE_FORCE("_ZN2nn5audio29IsValidAudioRendererParameterERKNS0_22AudioRendererParameterE", "渲染器参数检查：一律合法", unsigned, h_ren_ValidParam, const void *p) { (void)p; return 1; }
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "渲染器工作区大小：宿主不用，给 1 MB", uint64_t, h_ren_WorkSize, const void *p) { (void)p; return 1 << 20; }
HLE_FORCE("_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE", "渲染器配置工作区大小：宿主不用，给 256 KB", uint64_t, h_ren_CfgWorkSize, const void *p) { (void)p; return 256 << 10; }
HLE_FORCE("_ZN2nn5audio35GetRequiredBufferSizeForI3dl2ReverbEii", "混响工作区大小：宿主不用，给 64 KB", uint64_t, h_ren_ReverbSize, int rate, int ch) { (void)rate; (void)ch; return 64 << 10; }
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "打开渲染器：绑系统事件、起 5 ms 定时触发线程", Result, h_ren_Open, void **handle, void *ev, const int32_t *p, void *work, uint64_t sz) {
    (void)work; (void)sz;
    if (g_ren.event) { LOG("audio::OpenAudioRenderer：已打开过一个，再开报失败"); return RES_FAIL; }
    g_ren.event = ev; if (ev) hle_bind_system_event(ev, 1);
    atomic_store(&g_ren.run, 1); atomic_store(&g_ren.started, 0);
    pthread_create(&g_ren.th, NULL, audren_tick, NULL);
    *handle = &g_ren;
    LOG("audio::OpenAudioRenderer %d Hz × %d 样本，voice %d sink %d effect %d（宿主：协议维持，不出声）", p[0], p[1], p[4], p[5], p[6]);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "渲染器开始：定时事件开始触发", Result, h_ren_Start, void *h) { (void)h; atomic_store(&g_ren.started, 1); return RES_OK; }
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "渲染器停止", Result, h_ren_Stop, void *h) { (void)h; atomic_store(&g_ren.started, 0); return RES_OK; }
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "渲染器关闭：停定时线程", void, h_ren_Close, void *h) { (void)h; atomic_store(&g_ren.run, 0); pthread_join(g_ren.th, NULL); g_ren.event = NULL; }
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "渲染器更新请求：一律成功", Result, h_ren_Update, void *h, const void *cfg) { (void)h; (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm", "渲染器配置初始化：宿主不读配置内容", void, h_ren_InitCfg, void *cfg, const void *p, void *work, uint64_t sz) { (void)cfg; (void)p; (void)work; (void)sz; }
HLE_FORCE("_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi", "取最终混音：一律成功（返回 bool）", unsigned, h_ren_FinalMix, void *cfg, void *mix, int n) { (void)cfg; (void)mix; (void)n; return 1; }
HLE_FORCE("_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc", "加设备输出：一律成功", Result, h_ren_AddSink, void *cfg, void *sink, void *mix, const int8_t *in, int n, const char *name) { (void)cfg; (void)sink; (void)mix; (void)in; (void)n; (void)name; return RES_OK; }
HLE_FORCE("_ZN2nn5audio14AddBufferMixerEPNS0_19AudioRendererConfigEPNS0_15BufferMixerTypeEPNS0_12FinalMixTypeE", "加缓冲混音效果：一律成功", Result, h_ren_AddMixer, void *cfg, void *fx, void *mix) { (void)cfg; (void)fx; (void)mix; return RES_OK; }
HLE_FORCE("_ZN2nn5audio14AddI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPvmPNS0_12FinalMixTypeEi", "加混响效果：一律成功", Result, h_ren_AddReverb, void *cfg, void *fx, void *buf, uint64_t sz, void *mix, int ch) { (void)cfg; (void)fx; (void)buf; (void)sz; (void)mix; (void)ch; return RES_OK; }
HLE_FORCE("_ZN2nn5audio20SetBufferMixerVolumeEPNS0_15BufferMixerTypeEif", "混音效果音量：不出声", void, h_ren_MixerVol, void *fx, int i, float v) { (void)fx; (void)i; (void)v; }
HLE_FORCE("_ZN2nn5audio25SetBufferMixerInputOutputEPNS0_15BufferMixerTypeEPKaS4_i", "混音效果输入输出：不出声", void, h_ren_MixerIO, void *fx, const int8_t *in, const int8_t *out, int n) { (void)fx; (void)in; (void)out; (void)n; }
HLE_FORCE("_ZN2nn5audio25SetI3dl2ReverbInputOutputEPNS0_15I3dl2ReverbTypeEPKaS4_i", "混响输入输出：不出声", void, h_ren_ReverbIO, void *fx, const int8_t *in, const int8_t *out, int n) { (void)fx; (void)in; (void)out; (void)n; }
HLE_FORCE("_ZN2nn5audio21SetI3dl2ReverbEnabledEPNS0_15I3dl2ReverbTypeEb", "混响开关：不出声", void, h_ren_ReverbOn, void *fx, unsigned on) { (void)fx; (void)on; }
HLE_FORCE("_ZN2nn5audio17SetFinalMixVolumeEPNS0_12FinalMixTypeEf", "最终混音音量：不出声", void, h_ren_FinalVol, void *mix, float v) { (void)mix; (void)v; }
// MemoryPoolType（0x40 字节不透明）：宿主只在前 16 字节记地址与长度
// MemoryPoolType (0x40 opaque bytes): the host records only address and size in the first 16 bytes
HLE_FORCE("_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm", "取内存池：记地址（返回 bool）", unsigned, h_ren_PoolAcq, void *cfg, uint64_t *pool, void *addr, uint64_t sz) { (void)cfg; pool[0] = (uint64_t)(uintptr_t)addr; pool[1] = sz; pool[2] = 1; return 1; }
HLE_FORCE("_ZN2nn5audio17ReleaseMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeE", "放内存池", void, h_ren_PoolRel, void *cfg, uint64_t *pool) { (void)cfg; pool[0] = pool[1] = pool[2] = 0; }
HLE_FORCE("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", "挂内存池：立即成功（返回 bool）", unsigned, h_ren_PoolAttach, uint64_t *pool) { pool[2] = 1; return 1; }
HLE_FORCE("_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", "卸内存池：立即成功（返回 bool）", unsigned, h_ren_PoolDetach, uint64_t *pool) { pool[2] = 0; return 1; }
HLE_FORCE("_ZN2nn5audio20IsMemoryPoolAttachedEPKNS0_14MemoryPoolTypeE", "内存池是否已挂（返回 bool）", unsigned, h_ren_PoolAttached, const uint64_t *pool) { return pool[2] != 0; }
HLE_FORCE("_ZN2nn5audio20GetMemoryPoolAddressEPKNS0_14MemoryPoolTypeE", "内存池地址", void *, h_ren_PoolAddr, const uint64_t *pool) { return (void *)(uintptr_t)pool[0]; }
HLE_FORCE("_ZN2nn5audio18CopyMemoryPoolDataEPKNS0_14MemoryPoolTypeEPvPKvm", "内存池数据拷贝 → memcpy", void, h_ren_PoolCopy, const void *pool, void *dst, const void *src, uint64_t n) { (void)pool; memcpy(dst, src, n); }
// VoiceType（0x100 字节不透明）：宿主只在前 8 字节记播放状态
// VoiceType (0x100 opaque bytes): the host records only the play state in the first 8 bytes
HLE_FORCE("_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvm", "取声音槽：一律成功（返回 bool）", unsigned, h_ren_VoiceAcq, void *cfg, uint32_t *voice, int rate, int ch, int fmt, int prio, const void *extra, uint64_t sz) { (void)cfg; (void)rate; (void)ch; (void)fmt; (void)prio; (void)extra; (void)sz; voice[0] = 1 /* Stopped */; voice[1] = 0; return 1; }
HLE_FORCE("_ZN2nn5audio16ReleaseVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE", "放声音槽", void, h_ren_VoiceRel, void *cfg, uint32_t *voice) { (void)cfg; voice[0] = 1; }
HLE_FORCE("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE", "声音槽目的混音：不出声", void, h_ren_VoiceDest, void *cfg, void *voice, void *mix) { (void)cfg; (void)voice; (void)mix; }
HLE_FORCE("_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE", "追加波形缓冲：收下、不播（返回 bool）", unsigned, h_ren_Append, uint32_t *voice, const void *wb) { (void)wb; voice[1]++; return 1; }
HLE_FORCE("_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE", "取已播完的波形缓冲：永远没有（不出声）", const void *, h_ren_Released, uint32_t *voice) { (void)voice; return NULL; }
HLE_FORCE("_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE", "声音槽播放状态：记下", void, h_ren_VoiceSetState, uint32_t *voice, int st) { voice[0] = (uint32_t)st; }
HLE_FORCE("_ZN2nn5audio17GetVoicePlayStateEPKNS0_9VoiceTypeE", "声音槽播放状态：照记的回", int, h_ren_VoiceGetState, const uint32_t *voice) { return (int)voice[0]; }
HLE_FORCE("_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE", "声音槽已播样本数：0（不出声）", int64_t, h_ren_VoicePlayed, const void *voice) { (void)voice; return 0; }
HLE_FORCE("_ZN2nn5audio14SetVoiceVolumeEPNS0_9VoiceTypeEf", "声音槽音量：不出声", void, h_ren_VoiceVol, void *voice, float v) { (void)voice; (void)v; }
HLE_FORCE("_ZN2nn5audio13SetVoicePitchEPNS0_9VoiceTypeEf", "声音槽音高：不出声", void, h_ren_VoicePitch, void *voice, float v) { (void)voice; (void)v; }
HLE_FORCE("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii", "声音槽混音音量：不出声", void, h_ren_VoiceMixVol, void *voice, void *mix, float v, int src, int dst) { (void)voice; (void)mix; (void)v; (void)src; (void)dst; }
// AdpcmHeaderInfo（SDK 公开布局，0x60 字节）：清零即「0 样本」；游戏按它算长度，0 长度的声音不播
// AdpcmHeaderInfo (public SDK layout, 0x60 bytes): zeroed = "0 samples"; the game computes lengths from it, and a 0-length sound is not played
HLE_FORCE("_ZN2nn5audio16ParseAdpcmHeaderEPNS0_15AdpcmHeaderInfoEPKvm", "ADPCM 头解析：不出声，报 0 样本", Result, h_ren_Adpcm, void *info, const void *data, uint64_t sz) { (void)data; (void)sz; memset(info, 0, 0x60); return RES_OK; }

// ---------- 联网（socket / ssl / nifm / ldn / friends / account）：一律不可用 ----------
// 这些是 main 还会调而 d2r 没接的；语义一律「失败 / 没有」，与 d2r 已接的那部分口径一致（hle_net.c / hle_friends.c / hle_account.c）。
// 参数不读（AAPCS64 下多传的参数无副作用），返回：Result 型给 RES_FAIL，int 型（socket 风格）给 -1，bool 给 0，计数给 0。
// ---------- Networking (socket / ssl / nifm / ldn / friends / account): uniformly unavailable ----------
// These are what main still calls and d2r did not take over; the semantics are uniformly "failure / none", consistent with the part d2r did take over (hle_net.c / hle_friends.c / hle_account.c).
// Arguments are not read (extra arguments under AAPCS64 have no side effects); returns: Result-typed give RES_FAIL, int-typed (socket style) give -1, bool give 0, counts give 0.
#define NET_RES(name, m)  HLE_FORCE(m, "联网排除：报失败", Result, name, void) { return RES_FAIL; }
#define NET_INT(name, m)  HLE_FORCE(m, "联网排除：报 -1", int, name, void) { return -1; }
#define NET_ZERO(name, m) HLE_FORCE(m, "联网排除：报 0 / 没有", uint64_t, name, void) { return 0; }
#define NET_VOID(name, m) HLE_FORCE(m, "联网排除：空操作", void, name, void) {}
NET_RES(h_sk_Init2, "_ZN2nn6socket10InitializeEPvmmi") NET_RES(h_sk_Fin, "_ZN2nn6socket8FinalizeEv")
NET_INT(h_sk_GetSockOpt, "_ZN2nn6socket10GetSockOptEiiiPvPj") NET_INT(h_sk_SetSockOpt, "_ZN2nn6socket10SetSockOptEiiiPKvj")
NET_INT(h_sk_GetAddrInfo2, "_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_") NET_VOID(h_sk_FreeAddrInfo2, "_ZN2nn6socket12FreeAddrInfoEP8addrinfo")
NET_INT(h_sk_GetPeerName, "_ZN2nn6socket11GetPeerNameEiP8sockaddrPj") NET_INT(h_sk_GetSockName, "_ZN2nn6socket11GetSockNameEiP8sockaddrPj")
NET_INT(h_sk_GetLastErrno, "_ZN2nn6socket12GetLastErrnoEv") NET_INT(h_sk_Bind, "_ZN2nn6socket4BindEiPK8sockaddrj") NET_INT(h_sk_Poll, "_ZN2nn6socket4PollEP6pollfdmi")
NET_INT(h_sk_Recv2, "_ZN2nn6socket4RecvEiPvmi") NET_INT(h_sk_Send2, "_ZN2nn6socket4SendEiPKvmi") NET_INT(h_sk_Close, "_ZN2nn6socket5CloseEi")
NET_INT(h_sk_Fcntl, "_ZN2nn6socket5FcntlEiiz") NET_INT(h_sk_Ioctl, "_ZN2nn6socket5IoctlEijPvm") NET_INT(h_sk_Accept, "_ZN2nn6socket6AcceptEiP8sockaddrPj")
NET_INT(h_sk_Listen, "_ZN2nn6socket6ListenEii") NET_INT(h_sk_Select, "_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval") NET_INT(h_sk_SendTo, "_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj")
NET_INT(h_sk_Socket, "_ZN2nn6socket6SocketEiii") NET_INT(h_sk_Connect, "_ZN2nn6socket7ConnectEiPK8sockaddrj") NET_INT(h_sk_RecvFrom2, "_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj")
NET_INT(h_sk_Shutdown, "_ZN2nn6socket8ShutdownEii")
HLE_FORCE("_ZN2nn6socket8InetNtoaE7in_addr", "纯计算：点分地址字符串（静态缓冲）", const char *, h_sk_InetNtoa, uint32_t a) { static char b[20]; snprintf(b, sizeof b, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24); return b; }
HLE_FORCE("_ZN2nn6socket8InetNtopEiPKvPcj", "纯计算：IPv4 地址 → 字符串", const char *, h_sk_InetNtop, int af, const void *src, char *dst, uint32_t n) { if (af != 2 || n < 16) return NULL; const uint8_t *p = src; snprintf(dst, n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); return dst; }
HLE_FORCE("_ZN2nn6socket8InetPtonEiPKcPv", "纯计算：字符串 → IPv4 地址", int, h_sk_InetPton, int af, const char *src, void *dst) { unsigned a, b, c, d; if (af != 2 || sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0; uint8_t *p = dst; p[0] = a; p[1] = b; p[2] = c; p[3] = d; return 1; }
HLE_FORCE("_ZN2nn6socket9InetHtonlEj", "纯计算：主机序 → 网络序", uint32_t, h_sk_Htonl, uint32_t v) { return __builtin_bswap32(v); }
HLE_FORCE("_ZN2nn6socket9InetNtohlEj", "纯计算：网络序 → 主机序", uint32_t, h_sk_Ntohl, uint32_t v) { return __builtin_bswap32(v); }
HLE_FORCE("_ZN2nn6socket9InetHtonsEt", "纯计算：主机序 → 网络序", uint32_t, h_sk_Htons, unsigned v) { return __builtin_bswap16((uint16_t)v); }
HLE_FORCE("_ZN2nn6socket9InetNtohsEt", "纯计算：网络序 → 主机序", uint32_t, h_sk_Ntohs, unsigned v) { return __builtin_bswap16((uint16_t)v); }
NET_INT(h_nsk_Bind, "nnsocketBind") NET_INT(h_nsk_Close, "nnsocketClose") NET_INT(h_nsk_Connect, "nnsocketConnect") NET_INT(h_nsk_Fcntl, "nnsocketFcntl")
NET_INT(h_nsk_GetPeerName, "nnsocketGetPeerName") NET_INT(h_nsk_GetSockName, "nnsocketGetSockName") NET_INT(h_nsk_GetSockOpt, "nnsocketGetSockOpt")
NET_INT(h_nsk_Poll, "nnsocketPoll") NET_INT(h_nsk_ResGetOpt, "nnsocketResolverGetOption") NET_INT(h_nsk_ResSetOpt, "nnsocketResolverSetOption")
NET_INT(h_nsk_SetSockOpt, "nnsocketSetSockOpt") NET_INT(h_nsk_Socket, "nnsocketSocket")
HLE_FORCE("nnsocketInetHtons", "纯计算：主机序 → 网络序", uint32_t, h_nsk_Htons, unsigned v) { return __builtin_bswap16((uint16_t)v); }
HLE_FORCE("nnsocketInetNtohs", "纯计算：网络序 → 主机序", uint32_t, h_nsk_Ntohs, unsigned v) { return __builtin_bswap16((uint16_t)v); }
NET_RES(h_ssl_ConnCreate, "nnsslConnectionCreate") NET_RES(h_ssl_ConnDestroy, "nnsslConnectionDestroy") NET_RES(h_ssl_Handshake, "nnsslConnectionDoHandshakeWithCertBuffer")
NET_RES(h_ssl_ConnId, "nnsslConnectionGetConnectionId") NET_RES(h_ssl_VerifyErr, "nnsslConnectionGetVerifyCertError") NET_RES(h_ssl_Peek, "nnsslConnectionPeek")
NET_RES(h_ssl_Pending, "nnsslConnectionPending") NET_RES(h_ssl_Read, "nnsslConnectionRead") NET_RES(h_ssl_HostName, "nnsslConnectionSetHostName")
NET_RES(h_ssl_IoMode, "nnsslConnectionSetIoMode") NET_RES(h_ssl_Option, "nnsslConnectionSetOption") NET_RES(h_ssl_CacheMode, "nnsslConnectionSetSessionCacheMode")
NET_RES(h_ssl_SockDesc, "nnsslConnectionSetSocketDescriptor") NET_RES(h_ssl_VerifyOpt, "nnsslConnectionSetVerifyOption") NET_RES(h_ssl_Write, "nnsslConnectionWrite")
NET_RES(h_ssl_CtxDestroy, "nnsslContextDestroy") NET_RES(h_ssl_CtxId, "nnsslContextGetContextId")
NET_RES(h_ssl_ImportPki, "_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE") NET_RES(h_ssl_CtxCreate, "_ZN2nn3ssl7Context6CreateENS1_10SslVersionE")
NET_VOID(h_nifm_LocalMode, "_ZN2nn4nifm19SetLocalNetworkModeEb") NET_RES(h_nifm_PrimaryIp2, "_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr")
NET_RES(h_nifm_HandleResult, "_ZN2nn4nifm26HandleNetworkRequestResultEv") NET_VOID(h_nifm_SubmitWait, "_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv")
NET_RES(h_ldn_Init, "_ZN2nn3ldn10InitializeEv") NET_RES(h_ldn_OpenStation, "_ZN2nn3ldn11OpenStationEv") NET_RES(h_ldn_CloseStation, "_ZN2nn3ldn12CloseStationEv")
NET_RES(h_ldn_CreateNet, "_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE") NET_RES(h_ldn_DestroyNet, "_ZN2nn3ldn14DestroyNetworkEv")
NET_RES(h_ldn_GetIp, "_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE") NET_ZERO(h_ldn_MakeIp, "_ZN2nn3ldn15MakeIpv4AddressEhhhh")
NET_RES(h_ldn_OpenAp, "_ZN2nn3ldn15OpenAccessPointEv") NET_RES(h_ldn_CloseAp, "_ZN2nn3ldn16CloseAccessPointEv") NET_RES(h_ldn_SetAdv, "_ZN2nn3ldn16SetAdvertiseDataEPKvm")
NET_RES(h_ldn_Scan, "_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi") NET_RES(h_ldn_Connect, "_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi")
NET_VOID(h_ldn_Fin, "_ZN2nn3ldn8FinalizeEv") NET_ZERO(h_ldn_State, "_ZN2nn3ldn8GetStateEv")
NET_VOID(h_fr_SetAlloc, "_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE") NET_VOID(h_fr_PresenceCtor, "_ZN2nn7friends12UserPresenceC1Ev") NET_VOID(h_fr_FriendCtor, "_ZN2nn7friends6FriendC1Ev")
NET_RES(h_fr_List2, "_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE") NET_RES(h_fr_AddHist2, "_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_")
NET_RES(h_fr_Blocked, "_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii") NET_RES(h_fr_OpenSess, "_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE")
NET_RES(h_fr_CloseSess, "_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE")
HLE_FORCE("_ZNK2nn7friends14FriendPresence14GetDescriptionEv", "联网排除：空串", const char *, h_fr_PresDesc, const void *s) { (void)s; return ""; }
NET_ZERO(h_fr_PresApp, "_ZNK2nn7friends14FriendPresence24GetLastPlayedApplicationEv") NET_ZERO(h_fr_PresStatus, "_ZNK2nn7friends14FriendPresence9GetStatusEv")
HLE_FORCE("_ZNK2nn7friends6Friend11GetNicknameEv", "联网排除：空串", const char *, h_fr_Nick, const void *s) { (void)s; return ""; }
HLE_FORCE("_ZNK2nn7friends6Friend11GetPresenceEv", "联网排除：指向自身（空对象）", const void *, h_fr_Pres, const void *s) { return s; }
NET_ZERO(h_fr_AccId, "_ZNK2nn7friends6Friend12GetAccountIdEv")
NET_VOID(h_acc_Cancel, "_ZN2nn7account12AsyncContext6CancelEv") NET_VOID(h_acc_Dtor, "_ZN2nn7account12AsyncContextD1Ev")
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "异步上下文：立即完成", Result, h_acc_HasDone, void *self, uint8_t *done) { (void)self; if (done) *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "本机用户列表：1 个（与 ListOpenUsers 同一个 Uid）", Result, h_acc_ListAll, int *n, uint64_t *uids, int max) { if (max >= 1 && uids) { uids[0] = 1; uids[1] = 1; } *n = max >= 1 ? 1 : 0; return RES_OK; }
HLE_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "预选用户：固定本机用户（句柄 = 1）", Result, h_acc_OpenPre, uint64_t *h) { if (h) { h[0] = 1; h[1] = 1; h[2] = 1; } return RES_OK; }
HLE_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "网络服务账号：没有", Result, h_acc_NsaAvail, uint8_t *out, const void *h) { (void)h; if (out) *out = 0; return RES_OK; }
NET_VOID(h_acc_CloseUser, "_ZN2nn7account9CloseUserERKNS0_10UserHandleE")

// ---------- nn::fs 其余：SetAllocator（SDK 内部分配器，宿主不用） ----------
HLE_FORCE("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", "fs 分配器：宿主文件系统不用 SDK 的分配器", void, h_fs_SetAlloc, void *a, void *f) { (void)a; (void)f; }
