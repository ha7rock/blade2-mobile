// Blade II: The Return of Evil（Switch 版，UE4 4.17，NintendoSDK 7.4）比暗黑 II 多用到的 SDK 服务接口，在这里接管成宿主实现。
// 清单来源：docs/main_imports_vs_d2r.tsv（main 导入的 608 个符号里 d2r 宿主没接的那些，按命名空间）。
// 原则与 d2r 相同：真机上走 IPC 的服务层一律在 SDK 接口处接管（HLE_FORCE），SDK 的 nn::os / libc / libc++ 原样运行不碰。
// 音频渲染器在 SDK 接口层按 voice 混音出声（见下文 audren 一节），联网一律「不可用」，六轴 / 震动「没有」。
// Blade II: The Return of Evil (Switch, UE4 4.17, NintendoSDK 7.4) uses more SDK service interfaces than Diablo II; they are taken over here.
// Source of the list: docs/main_imports_vs_d2r.tsv (the symbols among main's 608 imports that the d2r host does not implement, by namespace).
// Same principle as d2r: service layers that go through IPC on a real device are taken over at the SDK interface (HLE_FORCE); the SDK's nn::os / libc / libc++ run as is.
// The audio renderer mixes per voice at the SDK interface (see the audren section below), networking is uniformly "unavailable", six-axis / vibration "absent".
#include "hle.h"
#include "front.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <errno.h>

void hle_bind_system_event(void *ev, int autoclear);   // hle_thread.c
void hle_signal_system_event(void *ev);
int hle_signal_system_event_try(void *ev);   // 容错版：句柄没了返回 0 | tolerant: returns 0 when the handle is gone

// ---------- movie（subsdk0 的任天堂视频库，不装载）：d2r 已空播；UE4 的播放器还读媒体信息 ----------
// ---------- movie (Nintendo video library in subsdk0, not loaded): d2r already plays empty; UE4's player also reads media info ----------
HLE_FORCE("_ZN5movie9MediaData5ClearEv", "过场空播：空 MediaData", void, h_mv_Clear, void **self) { *self = 0; }
HLE_FORCE("_ZN5movie9MediaDataC1ERKS0_", "过场空播：空 MediaData（拷贝构造）", void, h_mv_CopyCtor, void **self, void **other) { *self = *other; }
HLE_FORCE("_ZN5movie9MediaDataaSERKS0_", "过场空播：空 MediaData（赋值）", void *, h_mv_Assign, void **self, void **other) { *self = *other; return self; }
// FindString / FindInt32 / FindInt64 已在 hle_movie.c 里 | FindString / FindInt32 / FindInt64 are already in hle_movie.c
HLE_FORCE("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", "过场空播：媒体信息一律「没有」", unsigned, h_mv_FindRect, const void *self, const char *key, int *l, int *t, int *r, int *b) { (void)self; (void)key; if (l) *l = 0; if (t) *t = 0; if (r) *r = 0; if (b) *b = 0; return 0; }

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
// Switch 的可变参数就在 x2..x7 里（标准 AAPCS64），这里按格式串自己消费前 6 个整数 / 字符串参数（够看启动日志的数字）
// The Switch's variadic arguments are simply in x2..x7 (standard AAPCS64); the first 6 integer / string arguments are consumed here by the format string (enough to read the numbers in the boot log)
static void diag_fmt(char *out, size_t cap, const char *fmt, const uint64_t *a, int na) {
    size_t o = 0; int ai = 0;
    for (const char *p = fmt; *p && o + 32 < cap; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        const char *q = p + 1; while (*q && strchr("0123456789.-+ #lhzj", *q)) q++;
        if (!*q) break;
        uint64_t v = ai < na ? a[ai] : 0; if (*q != '%') ai++;
        switch (*q) {
        case 'd': case 'i': o += snprintf(out + o, cap - o, "%lld", (long long)(strchr(p, 'l') && strchr(p, 'l') < q ? (int64_t)v : (int32_t)v)); break;
        case 'u': o += snprintf(out + o, cap - o, "%llu", (unsigned long long)(strchr(p, 'l') && strchr(p, 'l') < q ? v : (uint32_t)v)); break;
        case 'x': case 'X': case 'p': o += snprintf(out + o, cap - o, "%#llx", (unsigned long long)v); break;
        case 's': o += snprintf(out + o, cap - o, "%s", v ? (const char *)(uintptr_t)v : "(null)"); break;
        case 'c': out[o++] = (char)v; break;
        case '%': out[o++] = '%'; break;
        default: o += snprintf(out + o, cap - o, "%%%c", *q); break;
        }
        p = q;
    }
    out[o] = 0; while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) out[--o] = 0;
}
HLE_FORCE("_ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz", "SDK 日志（lm 服务）：宿主按格式串打印（前 6 个参数）", void, h_diag_Log, const void *meta, const char *fmt, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)meta; if (!g_flag.dbg) return;
    uint64_t a[6] = {a0, a1, a2, a3, a4, a5}; char buf[1024]; diag_fmt(buf, sizeof buf, fmt ? fmt : "(null)", a, 6);
    if (buf[0]) LOG("[nn::diag] %s", buf);
}
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", "SDK 中止：宿主打印位置与回溯后退出", void, h_diag_Abort, const char *cond, const char *func, const char *file, int line) {
    LOG("nn::diag::Abort：%s（%s，%s:%d）——回溯：", cond ? cond : "", func ? func : "", file ? file : "", line);
    dm_backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0));
    die("nn::diag::Abort");
}
HLE_FORCE("nndetailDiagAbortFormatImpl", "SDK 中止（带格式）：宿主打印后退出", void, h_diag_AbortFmt, const char *cond, const char *func, const char *file, int line, const void *result, const void *fmt) { (void)result; die("nn::diag::Abort：%s（%s，%s:%d）%s", cond ? cond : "", func ? func : "", file ? file : "", line, fmt ? (const char *)fmt : ""); }

// ---------- nn::audio 音频渲染器（audren）：宿主在 SDK 接口层按 voice 混音出声 ----------
// 真机：游戏把声音交给渲染器（voice → final mix → device sink），每 5 ms 一次 RequestUpdate，系统事件每周期触发一次。
// 宿主：不解析 RequestUpdate 的参数块，而是在 SDK 接口处记下每只 voice 的格式 / 波形缓冲队列 / 音量 / 音高 / 播放状态；
// 混音端（真出声 = CoreAudio 回调；静音接收端 = 5 ms 节拍线程）逐 voice 解码（PcmInt16 / DSP-ADPCM）、线性插值重采样到渲染器采样率、
// 乘 voice 音量 × 混音矩阵 × 最终混音音量，叠成立体声 int16。播完的波形缓冲经 GetReleasedWaveBuffer 归还，已播样本数如实上报，
// UE4 的音频线程才会持续喂数据。效果：BufferMixer 的路由并入「混音缓冲 → 左右声道」增益表；I3dl2Reverb 忽略（只出干声）。
// 系统事件：游戏拿到后 Detach 读端再 Destroy 临时对象（写端句柄被关），所以 hle_thread.c 按内核事件对象触发而不按句柄；节拍线程用容错触发。
// 开关：DM_AUDIO=0 静音接收端（Mac 缺省，按实时消耗）/ DM_AUDIO=1 真出声，与 hle_audio.c 同口径；
//       DM_AUDIO_OUT_DUMP=<文件> 把混音结果（16 位立体声、渲染器采样率）续写出去，用来核对「有没有声」。
// ---------- nn::audio audio renderer (audren): the host mixes per voice at the SDK interface ----------
// Real device: the game hands sound data to the renderer (voice → final mix → device sink), one RequestUpdate every 5 ms, the system event fires once per period.
// Host: the RequestUpdate parameter block is not parsed; instead every voice's format / wave buffer queue / volume / pitch / play state is recorded at the SDK interface;
// the mixing side (real output = CoreAudio callback; silent sink = the 5 ms tick thread) decodes each voice (PcmInt16 / DSP-ADPCM), resamples with linear interpolation to the renderer rate,
// multiplies voice volume × mix matrix × final mix volume, and sums into stereo int16. Consumed wave buffers are returned through GetReleasedWaveBuffer and played sample counts are honest,
// which is what keeps UE4's audio thread feeding. Effects: the BufferMixer routing is folded into the "mix buffer → L/R" gain table; I3dl2Reverb is ignored (dry signal only).
// System event: the game detaches the read end and destroys the temporary object (closing the write-end handle), so hle_thread.c signals the kernel event object rather than the handle; the tick thread uses the tolerant signal.
// Switches: DM_AUDIO=0 silent sink (Mac default, consumes in real time) / DM_AUDIO=1 real output, same convention as hle_audio.c;
//           DM_AUDIO_OUT_DUMP=<file> appends the mixed result (16-bit stereo, renderer rate), used to check "is there any sound".
#define AUDREN_PERIOD_NS 5000000ll
enum { REN_PLAY = 0, REN_STOP = 1, REN_PAUSE = 2 };           // nn::audio::VoiceType::PlayState
enum { REN_SF_PCM16 = 2, REN_SF_ADPCM = 6 };                   // nn::audio::SampleFormat
enum { REN_VMAX = 64, REN_WBMAX = 8, REN_BUFMAX = 32, REN_SINKMAX = 6, REN_FXMAX = 8, REN_SRCRING = 64 };
// WaveBuffer（SDK 公开布局，0x30 字节） | WaveBuffer (public SDK layout, 0x30 bytes)
typedef struct { const void *buffer; uint64_t size; int32_t start, end; uint8_t loop, eos; uint8_t pad[6]; const void *ctx; uint64_t ctxsz; } wavebuf_t;
typedef struct {
    int used; const void *guest;              // 客体 VoiceType* | Guest VoiceType*
    int rate, ch, fmt, state;
    int16_t coef[16]; int has_coef;           // ADPCM 系数（AcquireVoiceSlot 的参数块） | ADPCM coefficients (AcquireVoiceSlot's parameter block)
    float vol, pitch;
    float mixvol[2][REN_BUFMAX];              // [voice 声道][混音缓冲] | [voice channel][mix buffer]
    struct { const void *guest; wavebuf_t wb; } q[REN_WBMAX]; uint32_t qh, qt;   // 待播 | Pending
    const void *rel[REN_WBMAX]; uint32_t rh, rt;                                  // 已播完 | Released
    int has_cur; wavebuf_t cur; const void *cur_guest; int64_t pos, end;        // 正在播的缓冲（客体指针留着归还）与帧位置 | Buffer being played (guest pointer kept for the return) and its frame position
    int64_t played;
    int16_t h1, h2;                           // ADPCM 历史 | ADPCM history
    int16_t ring[REN_SRCRING][2]; int rn;     // 已解码源帧（重采样用） | Decoded source frames (for the resampler)
    double frac;
    int nappend, nrel, nstart, verbose;       // verbose：头几只取到的 voice 把音量 / 状态 / 追加写进日志（DM_DBG） | verbose: the first few acquired voices log volume / state / appends (DM_DBG)
} voice_t;
typedef struct { const void *guest; int n; int8_t in[REN_BUFMAX], out[REN_BUFMAX]; float vol[REN_BUFMAX]; } bufmixer_t;
static struct {
    void *event; pthread_t th; _Atomic int run, started;
    pthread_mutex_t m;
    int rate, outrate, period, nbuf;          // 渲染器采样率 / 混音输出采样率（CoreAudio 不收渲染器采样率时 48 kHz）/ 每周期样本数 / 混音缓冲数 | Renderer rate / mix output rate (48 kHz when CoreAudio refuses the renderer rate) / samples per period / mix buffer count
    float finalvol;
    int sink_n; int8_t sink_in[REN_SINKMAX];
    bufmixer_t fx[REN_FXMAX]; int nfx;
    float bufgain[REN_BUFMAX][2]; int route_dirty;
    voice_t v[REN_VMAX];
    AudioUnit au; int real;
    FILE *dump;
    uint64_t frames, ticks, reverb_warned; int peak; int bad_event;
    uint64_t last_stat_ns;
} g_ren;

static int real_audio_ren(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : 0; }
// 客体 VoiceType / MemoryPoolType 都只是一个指针大小的句柄（SDK 公开布局：{ void *_pInfo }，游戏把它们按 8 字节一个排成数组），宿主只写这 8 字节：指向宿主对象
// Guest VoiceType / MemoryPoolType are each a pointer-sized handle (public SDK layout: { void *_pInfo }, the game lays them out in arrays 8 bytes apart); the host writes only these 8 bytes: a pointer to the host object
static voice_t *vget(const void *g) {
    if (!g) return NULL; voice_t *v = *(voice_t *const *)g; uintptr_t off = (uintptr_t)v - (uintptr_t)g_ren.v;
    if (!v || off >= sizeof g_ren.v || off % sizeof(voice_t) || !v->used) return NULL;
    return v;
}
typedef struct { void *addr; uint64_t size; int attached; } mempool_t;
// 混音缓冲 → 左右声道增益表：设备输出的输入缓冲直接对应声道（2 声道 L/R；6 声道 FL FR FC LFE BL BR 折成立体声），
// BufferMixer 效果把 in[i] × vol[i] 并入 out[i]，迭代两遍以容纳一层级联。
// Mix buffer → L/R gain table: the device sink's input buffers map straight to channels (2 ch L/R; 6 ch FL FR FC LFE BL BR folded to stereo),
// a BufferMixer effect adds in[i] × vol[i] into out[i]; iterated twice to allow one level of chaining.
static void ren_route(void) {
    static const float ch6[6][2] = {{1, 0}, {0, 1}, {0.707f, 0.707f}, {0, 0}, {1, 0}, {0, 1}};
    float g0[REN_BUFMAX][2] = {{0}}, g1[REN_BUFMAX][2], g2[REN_BUFMAX][2];
    for (int k = 0; k < g_ren.sink_n; k++) { int b = g_ren.sink_in[k]; if (b < 0 || b >= REN_BUFMAX) continue;
        if (g_ren.sink_n == 6) { g0[b][0] += ch6[k][0]; g0[b][1] += ch6[k][1]; }
        else if (g_ren.sink_n == 1) { g0[b][0] += 1; g0[b][1] += 1; }
        else { g0[b][k & 1] += 1; } }
    memcpy(g1, g0, sizeof g1);
    for (int pass = 0; pass < 2; pass++) {
        memcpy(g2, g0, sizeof g2);
        for (int f = 0; f < g_ren.nfx; f++) for (int i = 0; i < g_ren.fx[f].n; i++) {
            int in = g_ren.fx[f].in[i], out = g_ren.fx[f].out[i]; if (in < 0 || in >= REN_BUFMAX || out < 0 || out >= REN_BUFMAX) continue;
            g2[in][0] += g_ren.fx[f].vol[i] * g1[out][0]; g2[in][1] += g_ren.fx[f].vol[i] * g1[out][1]; }
        memcpy(g1, g2, sizeof g1);
    }
    memcpy(g_ren.bufgain, g1, sizeof g_ren.bufgain); g_ren.route_dirty = 0;
}
static void vrelease(voice_t *v, const void *guest) {
    if (v->rt - v->rh >= REN_WBMAX) { LOG("【红】音频 voice %d 已播完队列溢出（游戏不取 GetReleasedWaveBuffer？）", (int)(v - g_ren.v)); return; }
    v->rel[v->rt++ & (REN_WBMAX - 1)] = guest; v->nrel++;
}
static int64_t wb_frames(const voice_t *v, const wavebuf_t *wb) {   // 缓冲总帧数 | Total frames in the buffer
    return v->fmt == REN_SF_ADPCM ? (int64_t)(wb->size / 8) * 14 : (int64_t)(wb->size / (2u * (unsigned)v->ch));
}
static int16_t adpcm_sample(voice_t *v, int64_t i) {   // 顺序解码第 i 帧（历史在 v->h1/h2） | Decode frame i in sequence (history in v->h1/h2)
    const uint8_t *d = v->cur.buffer; uint64_t off = (uint64_t)(i / 14) * 8; if (off + 8 > v->cur.size) return 0;
    int ps = d[off], k = (int)(i % 14); int nib = d[off + 1 + k / 2]; nib = k & 1 ? nib & 15 : nib >> 4; if (nib >= 8) nib -= 16;
    int scale = 1 << (ps & 15), ci = (ps >> 4) & 7;
    int64_t s = ((((int64_t)nib * scale) << 11) + 1024 + (int64_t)v->coef[ci * 2] * v->h1 + (int64_t)v->coef[ci * 2 + 1] * v->h2) >> 11;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    v->h2 = v->h1; v->h1 = (int16_t)s; return (int16_t)s;
}
static void vstart_pos(voice_t *v, int64_t start) {   // 从 start 帧开始播当前缓冲（ADPCM：按上下文设历史，帧内偏移先解码丢弃） | Start the current buffer at frame start (ADPCM: history from the context, decode-and-discard the intra-frame offset)
    v->pos = start;
    if (v->fmt == REN_SF_ADPCM) {
        v->h1 = v->h2 = 0;
        if (v->cur.ctx && v->cur.ctxsz >= 6) { const int16_t *c = v->cur.ctx; v->h1 = c[1]; v->h2 = c[2]; }
        for (int64_t i = (start / 14) * 14; i < start; i++) adpcm_sample(v, i);
    }
}
static void vnext_buffer(voice_t *v) {   // 从待播队列取下一块 | Take the next block from the pending queue
    v->has_cur = 0;
    while (v->qh != v->qt) {
        v->cur = v->q[v->qh & (REN_WBMAX - 1)].wb; const void *guest = v->q[v->qh & (REN_WBMAX - 1)].guest; v->qh++;
        int64_t total = wb_frames(v, &v->cur);
        if (!v->cur.buffer || total <= 0 || v->cur.start < 0 || v->cur.start >= total) { vrelease(v, guest); continue; }   // 空块：直接归还 | Empty block: return it right away
        v->end = (v->cur.end > v->cur.start && v->cur.end <= total) ? v->cur.end : total;
        v->has_cur = 1; v->cur_guest = guest;
        vstart_pos(v, v->cur.start);
        if (v->verbose && v->nstart++ < 2) { const uint8_t *d = v->cur.buffer; int16_t h1 = v->h1, h2 = v->h2; int16_t smp[6]; int64_t p0 = v->pos;
            for (int i = 0; i < 6; i++) smp[i] = v->fmt == REN_SF_ADPCM ? adpcm_sample(v, p0 + i) : ((const int16_t *)d)[(p0 + i) * v->ch];
            if (v->fmt == REN_SF_ADPCM) { v->h1 = h1; v->h2 = h2; }   // 只是看一眼，历史复原 | Just a peek, restore the history
            LOG("audren 槽 %d 开播：帧 %lld..%lld，头字节 %02x %02x %02x %02x，系数 %d %d %d %d，上下文 %d/%d/%d，首样本 %d %d %d %d %d %d", (int)(v - g_ren.v), (long long)p0, (long long)v->end, d[0], d[1], d[2], d[3], v->coef[0], v->coef[1], v->coef[2], v->coef[3], v->cur.ctx ? ((const int16_t *)v->cur.ctx)[0] : 0, h1, h2, smp[0], smp[1], smp[2], smp[3], smp[4], smp[5]); }
        return;
    }
}
// 顺序产出 n 帧源样本（voice 声道数，交错），消耗波形缓冲队列；没数据给 0
// Produce n source frames in sequence (voice channel count, interleaved), consuming the wave buffer queue; zeros when starved
static void vsource(voice_t *v, int16_t (*dst)[2], int n) {
    for (int k = 0; k < n; k++) {
        for (;;) {
            if (!v->has_cur) { vnext_buffer(v); if (!v->has_cur) { dst[k][0] = dst[k][1] = 0; goto next; } }
            if (v->pos < v->end) break;
            if (v->cur.loop && v->end > v->cur.start) { vstart_pos(v, v->cur.start); break; }
            vrelease(v, v->cur_guest); v->has_cur = 0;
        }
        if (v->fmt == REN_SF_ADPCM) { dst[k][0] = dst[k][1] = adpcm_sample(v, v->pos); }
        else { const int16_t *p = (const int16_t *)v->cur.buffer + v->pos * v->ch; dst[k][0] = p[0]; dst[k][1] = v->ch > 1 ? p[1] : p[0]; }
        v->pos++; v->played++;
    next:;
    }
}
// 一只 voice 混进 frames 帧立体声累加器：重采样步长 = 源采样率 × 音高 / 渲染器采样率，线性插值；增益 = 音量 × 混音矩阵 × 最终混音音量
// Mix one voice into the stereo accumulator for frames: resample step = source rate × pitch / renderer rate, linear interpolation; gain = volume × mix matrix × final mix volume
static void vmix(voice_t *v, float (*acc)[2], int frames) {
    double step = (double)v->rate * (v->pitch > 0 ? v->pitch : 1.0) / g_ren.outrate; if (step < 1.0 / 64) step = 1.0 / 64; if (step > 16) step = 16;
    float g[2][2] = {{0, 0}, {0, 0}};
    for (int c = 0; c < v->ch && c < 2; c++) for (int b = 0; b < g_ren.nbuf && b < REN_BUFMAX; b++) { float m = v->mixvol[c][b]; if (m == 0) continue;
        g[c][0] += m * g_ren.bufgain[b][0]; g[c][1] += m * g_ren.bufgain[b][1]; }
    float fv = v->vol * g_ren.finalvol; for (int c = 0; c < 2; c++) { g[c][0] *= fv; g[c][1] *= fv; }
    int src_ch = v->ch > 1 ? 2 : 1;
    for (int i = 0; i < frames; i++) {
        if (v->rn < 2) { int need = REN_SRCRING - v->rn; vsource(v, v->ring + v->rn, need); v->rn += need; }
        float t = (float)v->frac, s0 = v->ring[0][0] + (v->ring[1][0] - v->ring[0][0]) * t, s1 = v->ring[0][1] + (v->ring[1][1] - v->ring[0][1]) * t;
        if (src_ch == 1) { acc[i][0] += s0 * g[0][0]; acc[i][1] += s0 * g[0][1]; }
        else { acc[i][0] += s0 * g[0][0] + s1 * g[1][0]; acc[i][1] += s0 * g[0][1] + s1 * g[1][1]; }
        v->frac += step; int adv = (int)v->frac; v->frac -= adv;
        while (adv) { if (v->rn == 0) { vsource(v, v->ring, 1); v->rn = 1; }
                      int d = adv < v->rn ? adv : v->rn; memmove(v->ring, v->ring + d, (size_t)(v->rn - d) * sizeof v->ring[0]); v->rn -= d; adv -= d; }
    }
}
// 混出 frames 帧立体声 int16（持锁调用） | Mix frames of stereo int16 (called with the lock held)
static void ren_mix(int16_t *out, int frames) {
    static float acc[4096][2];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, (size_t)frames * sizeof acc[0]);
    if (g_ren.route_dirty) ren_route();
    if (atomic_load(&g_ren.started))
        for (int i = 0; i < REN_VMAX; i++) { voice_t *v = &g_ren.v[i]; if (v->used && v->state == REN_PLAY) vmix(v, acc, frames); }
    int peak = 0;
    static float gain = -1; if (gain < 0) { const char *e = getenv("DM_AUDIO_GAIN"); gain = e ? (float)atof(e) : 1.0f; if (gain <= 0) gain = 1.0f; }   // DM_AUDIO_GAIN：总输出增益（缺省 1.0；实测最吵的 10 秒削波约 1%，想保守可设 0.7） | DM_AUDIO_GAIN: master output gain (default 1.0; the loudest 10 s clip about 1%, set 0.7 to be conservative)
    for (int i = 0; i < frames; i++) for (int c = 0; c < 2; c++) { float s = acc[i][c] * gain; int k = (int)(s > 32767 ? 32767 : s < -32768 ? -32768 : s); out[i * 2 + c] = (int16_t)k; if (k < 0) k = -k; if (k > peak) peak = k; }
    if (peak > g_ren.peak) g_ren.peak = peak;
    g_ren.frames += (uint64_t)frames;
    if (g_ren.dump) fwrite(out, 4, (size_t)frames, g_ren.dump);
    uint64_t now = now_ns();   // 每 10 s 一行统计：在播 voice 数、累计帧、峰值 | One stats line every 10 s: playing voices, cumulative frames, peak
    if (now - g_ren.last_stat_ns > 10000000000ull) { g_ren.last_stat_ns = now; int playing = 0, used = 0, rel = 0;
        char det[512]; int dn = 0; det[0] = 0;
        for (int i = 0; i < REN_VMAX; i++) { voice_t *v = &g_ren.v[i]; if (!v->used) continue; used++; if (v->state == REN_PLAY && v->has_cur) playing++; rel += v->nrel;
            if (dn < (int)sizeof det - 48) dn += snprintf(det + dn, sizeof det - (size_t)dn, " [%d:%s%s %lld/%d]", i, v->state == REN_PLAY ? "播" : v->state == REN_STOP ? "停" : "暂", v->has_cur ? "" : "空", (long long)v->played, v->qt - v->qh); }
        LOG("audren 统计：voice 占用 %d 在播 %d，累计混音 %llu 帧（%.1f s），已归还波形缓冲 %d，峰值 %d/32767；槽[状态 已播/排队]%s", used, playing, (unsigned long long)g_ren.frames, (double)g_ren.frames / g_ren.outrate, rel, g_ren.peak, det); g_ren.peak = 0; }
}
static OSStatus ren_au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)ctx; (void)f; (void)ts; (void)bus;
    int16_t *out = bl->mBuffers[0].mData; UInt32 want = bl->mBuffers[0].mDataByteSize / 4; if (frames > want) frames = want;
    pthread_mutex_lock(&g_ren.m);
    for (UInt32 done = 0; done < frames;) { int n = (int)(frames - done); if (n > 4096) n = 4096; ren_mix(out + done * 2, n); done += (UInt32)n; }
    pthread_mutex_unlock(&g_ren.m);
    return noErr;
}
static int ren_au_open(int rate) {   // 立体声 16 位、渲染器采样率喂给 DefaultOutput（它自己转到设备采样率） | Stereo 16-bit at the renderer rate into DefaultOutput (it converts to the device rate itself)
#if TARGET_OS_IPHONE
    // iOS 没有 DefaultOutput：输出单元是 RemoteIO（音频会话由 d2r 的 hle_audio.c 在 iOS 上统一激活） | iOS has no DefaultOutput: the output unit is RemoteIO (the audio session is activated on iOS by d2r's hle_audio.c)
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output, .componentSubType = kAudioUnitSubType_RemoteIO, .componentManufacturer = kAudioUnitManufacturer_Apple};
#else
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output, .componentSubType = kAudioUnitSubType_DefaultOutput, .componentManufacturer = kAudioUnitManufacturer_Apple};
#endif
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &g_ren.au)) die("audren：AudioUnit 建不出来");
    AudioStreamBasicDescription f = {.mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {ren_au_render, NULL};
    if (AudioUnitSetProperty(g_ren.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(g_ren.au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(g_ren.au)) { AudioComponentInstanceDispose(g_ren.au); g_ren.au = NULL; return 0; }
    return 1;
}
// 5 ms 节拍线程：按绝对时刻触发系统事件（容错版）；静音接收端模式下顺便按实时混掉一个周期（归还缓冲、推进样本数）
// 5 ms tick thread: signals the system event on an absolute schedule (tolerant variant); in silent-sink mode it also mixes one period in real time (returns buffers, advances sample counts)
static void *audren_tick(void *a) {
    (void)a; pthread_setname_np("blade audren tick");
    static int16_t scratch[4096 * 2]; uint64_t t = now_ns();
    while (atomic_load(&g_ren.run)) {
        t += AUDREN_PERIOD_NS; uint64_t now = now_ns(); if (t > now) { struct timespec ts = {0, (long)(t - now)}; nanosleep(&ts, NULL); } else if (now - t > 200000000ull) t = now;
        if (atomic_load(&g_ren.started) && g_ren.event) {
            if (!hle_signal_system_event_try(g_ren.event) && !g_ren.bad_event) { g_ren.bad_event = 1; LOG("【红】音频渲染器事件未绑定，触发不了"); }
            if (!g_ren.au) { pthread_mutex_lock(&g_ren.m); ren_mix(scratch, g_ren.period); pthread_mutex_unlock(&g_ren.m); }
        }
        g_ren.ticks++;
    }
    return NULL;
}
// AudioRendererParameter（SDK 公开布局，0x34 字节）：+0 sampleRate +4 sampleCount +8 mixBufferCount +0xc subMixCount +0x10 voiceCount +0x14 sinkCount +0x18 effectCount +0x1c performanceFrameCount ...
// AudioRendererParameter (public SDK layout, 0x34 bytes): +0 sampleRate +4 sampleCount +8 mixBufferCount +0xc subMixCount +0x10 voiceCount +0x14 sinkCount +0x18 effectCount +0x1c performanceFrameCount ...
HLE_FORCE("_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE", "渲染器参数缺省值（SDK 公开缺省：48 kHz、240 样本、24 混音缓冲、0 子混音…）", void, h_ren_InitParam, int32_t *p) {
    memset(p, 0, 0x34); p[0] = 48000; p[1] = 240; p[2] = 24; p[3] = 0; p[4] = 24; p[5] = 1; p[6] = 0; p[7] = 0;
}
HLE_FORCE("_ZN2nn5audio29IsValidAudioRendererParameterERKNS0_22AudioRendererParameterE", "渲染器参数检查：一律合法", unsigned, h_ren_ValidParam, const void *p) { (void)p; return 1; }
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "渲染器工作区大小：宿主不用，给 1 MB", uint64_t, h_ren_WorkSize, const void *p) { (void)p; return 1 << 20; }
HLE_FORCE("_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE", "渲染器配置工作区大小：宿主不用，给 256 KB", uint64_t, h_ren_CfgWorkSize, const void *p) { (void)p; return 256 << 10; }
HLE_FORCE("_ZN2nn5audio35GetRequiredBufferSizeForI3dl2ReverbEii", "混响工作区大小：宿主不用，给 64 KB", uint64_t, h_ren_ReverbSize, int rate, int ch) { (void)rate; (void)ch; return 64 << 10; }
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "打开渲染器：绑系统事件、起 5 ms 节拍线程、开 CoreAudio（DM_AUDIO=1）", Result, h_ren_Open, void **handle, void *ev, const int32_t *p, void *work, uint64_t sz) {
    (void)work; (void)sz;
    if (g_ren.event) { LOG("audio::OpenAudioRenderer：已打开过一个，再开报失败"); return RES_FAIL; }
    static int once; if (!once) { once = 1; pthread_mutex_init(&g_ren.m, NULL); }
    pthread_mutex_lock(&g_ren.m);
    g_ren.rate = p[0] > 0 ? p[0] : 48000; g_ren.period = p[1] > 0 && p[1] <= 4096 ? p[1] : 240; g_ren.nbuf = p[2] > 0 && p[2] <= REN_BUFMAX ? p[2] : REN_BUFMAX;
    g_ren.finalvol = 1; g_ren.sink_n = 0; g_ren.nfx = 0; g_ren.route_dirty = 1; memset(g_ren.v, 0, sizeof g_ren.v);
    g_ren.frames = 0; g_ren.peak = 0; g_ren.bad_event = 0; g_ren.last_stat_ns = now_ns();
    if (p[2] > REN_BUFMAX) LOG("【红】audren：混音缓冲 %d 超过宿主上限 %d，多出的不出声", p[2], REN_BUFMAX);
    { const char *e = getenv("DM_AUDIO_OUT_DUMP"); if (e && !g_ren.dump && !(g_ren.dump = fopen(e, "wb"))) LOG("【红】DM_AUDIO_OUT_DUMP 打不开 %s", e); }
    g_ren.real = real_audio_ren();
    g_ren.outrate = g_ren.rate;
    if (g_ren.real && !g_ren.au) { if (!ren_au_open(g_ren.rate)) { LOG("【红】audren：AudioUnit 不收 %d Hz，改喂 48 kHz", g_ren.rate); if (!ren_au_open(48000)) die("audren：AudioUnit 设置失败"); g_ren.outrate = 48000; } }
    pthread_mutex_unlock(&g_ren.m);
    g_ren.event = ev; if (ev) hle_bind_system_event(ev, 1);
    atomic_store(&g_ren.run, 1); atomic_store(&g_ren.started, 0);
    pthread_create(&g_ren.th, NULL, audren_tick, NULL);
    *handle = &g_ren;
    LOG("audio::OpenAudioRenderer %d Hz × %d 样本，混音缓冲 %d voice %d sink %d effect %d（宿主：%s%s）", p[0], p[1], p[2], p[4], p[5], p[6], g_ren.au ? "CoreAudio 真出声" : "静音接收端", g_ren.dump ? "，混音结果转储中" : "");
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "渲染器开始：节拍事件开始触发、CoreAudio 开播", Result, h_ren_Start, void *h) {
    (void)h; if (atomic_exchange(&g_ren.started, 1) == 0 && g_ren.au && AudioOutputUnitStart(g_ren.au)) die("audren：AudioUnit 启动失败");
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "渲染器停止", Result, h_ren_Stop, void *h) {
    (void)h; if (atomic_exchange(&g_ren.started, 0) == 1 && g_ren.au) AudioOutputUnitStop(g_ren.au);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "渲染器关闭：停节拍线程与 CoreAudio", void, h_ren_Close, void *h) {
    (void)h; h_ren_Stop(h); atomic_store(&g_ren.run, 0); pthread_join(g_ren.th, NULL); g_ren.event = NULL;
    if (g_ren.au) { AudioUnitUninitialize(g_ren.au); AudioComponentInstanceDispose(g_ren.au); g_ren.au = NULL; }
    if (g_ren.dump) { fclose(g_ren.dump); g_ren.dump = NULL; }
}
// 参数块不读：voice / 效果 / 音量都已在接口处记下 | The parameter block is not read: voices / effects / volumes were all recorded at the interface
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "渲染器更新请求：一律成功", Result, h_ren_Update, void *h, const void *cfg) { (void)h; (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm", "渲染器配置初始化：宿主不读配置内容", void, h_ren_InitCfg, void *cfg, const void *p, void *work, uint64_t sz) { (void)cfg; (void)p; (void)work; (void)sz; }
HLE_FORCE("_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi", "取最终混音：一律成功（返回 bool）", unsigned, h_ren_FinalMix, void *cfg, void *mix, int n) { (void)cfg; (void)mix; (void)n; return 1; }
HLE_FORCE("_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc", "加设备输出：记输入缓冲 → 声道映射", Result, h_ren_AddSink, void *cfg, void *sink, void *mix, const int8_t *in, int n, const char *name) {
    (void)cfg; (void)sink; (void)mix; pthread_mutex_lock(&g_ren.m);
    g_ren.sink_n = n > REN_SINKMAX ? REN_SINKMAX : n < 0 ? 0 : n; for (int i = 0; i < g_ren.sink_n; i++) g_ren.sink_in[i] = in[i]; g_ren.route_dirty = 1;
    pthread_mutex_unlock(&g_ren.m);
    LOG("audio::AddDeviceSink %s %d 声道，输入缓冲 %d %d %d %d %d %d", name ? name : "", n, n > 0 ? in[0] : -1, n > 1 ? in[1] : -1, n > 2 ? in[2] : -1, n > 3 ? in[3] : -1, n > 4 ? in[4] : -1, n > 5 ? in[5] : -1);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio14AddBufferMixerEPNS0_19AudioRendererConfigEPNS0_15BufferMixerTypeEPNS0_12FinalMixTypeE", "加缓冲混音效果：登记（路由并入增益表）", Result, h_ren_AddMixer, void *cfg, void *fx, void *mix) {
    (void)cfg; (void)mix; pthread_mutex_lock(&g_ren.m);
    if (g_ren.nfx < REN_FXMAX) { bufmixer_t *m = &g_ren.fx[g_ren.nfx++]; memset(m, 0, sizeof *m); m->guest = fx; for (int i = 0; i < REN_BUFMAX; i++) m->vol[i] = 1; }
    else LOG("【红】audren：BufferMixer 超过 %d 个，忽略", REN_FXMAX);
    pthread_mutex_unlock(&g_ren.m); return RES_OK;
}
static bufmixer_t *fxget(const void *fx) { for (int i = 0; i < g_ren.nfx; i++) if (g_ren.fx[i].guest == fx) return &g_ren.fx[i]; return NULL; }
HLE_FORCE("_ZN2nn5audio14AddI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPvmPNS0_12FinalMixTypeEi", "加混响效果：一律成功（宿主不做混响，只出干声）", Result, h_ren_AddReverb, void *cfg, void *fx, void *buf, uint64_t sz, void *mix, int ch) { (void)cfg; (void)fx; (void)buf; (void)sz; (void)mix; (void)ch; return RES_OK; }
HLE_FORCE("_ZN2nn5audio20SetBufferMixerVolumeEPNS0_15BufferMixerTypeEif", "混音效果音量：记进路由", void, h_ren_MixerVol, void *fx, int i, float v) {
    pthread_mutex_lock(&g_ren.m); bufmixer_t *m = fxget(fx); if (m && i >= 0 && i < REN_BUFMAX) { m->vol[i] = v; g_ren.route_dirty = 1; } pthread_mutex_unlock(&g_ren.m);
    static int logged; if (m && g_flag.dbg && logged < 8) { logged++; LOG("audio::SetBufferMixerVolume #%d 路 %d = %.3f", (int)(m - g_ren.fx), i, v); }
}
HLE_FORCE("_ZN2nn5audio25SetBufferMixerInputOutputEPNS0_15BufferMixerTypeEPKaS4_i", "混音效果输入输出：记进路由", void, h_ren_MixerIO, void *fx, const int8_t *in, const int8_t *out, int n) {
    pthread_mutex_lock(&g_ren.m); bufmixer_t *m = fxget(fx);
    if (m) { m->n = n > REN_BUFMAX ? REN_BUFMAX : n < 0 ? 0 : n; for (int i = 0; i < m->n; i++) { m->in[i] = in[i]; m->out[i] = out[i]; } g_ren.route_dirty = 1; }
    pthread_mutex_unlock(&g_ren.m);
    if (m && g_flag.dbg) LOG("audio::SetBufferMixerInputOutput #%d：%d 路，%d→%d %d→%d %d→%d …", (int)(m - g_ren.fx), n, n > 0 ? in[0] : -1, n > 0 ? out[0] : -1, n > 1 ? in[1] : -1, n > 1 ? out[1] : -1, n > 2 ? in[2] : -1, n > 2 ? out[2] : -1);
}
HLE_FORCE("_ZN2nn5audio25SetI3dl2ReverbInputOutputEPNS0_15I3dl2ReverbTypeEPKaS4_i", "混响输入输出：宿主不做混响", void, h_ren_ReverbIO, void *fx, const int8_t *in, const int8_t *out, int n) { (void)fx; (void)in; (void)out; (void)n; }
HLE_FORCE("_ZN2nn5audio21SetI3dl2ReverbEnabledEPNS0_15I3dl2ReverbTypeEb", "混响开关：宿主不做混响（首次开启报一次）", void, h_ren_ReverbOn, void *fx, unsigned on) { (void)fx; if ((on & 1) && !g_ren.reverb_warned) { g_ren.reverb_warned = 1; LOG("audren：游戏开了 I3dl2 混响，宿主只出干声"); } }
HLE_FORCE("_ZN2nn5audio17SetFinalMixVolumeEPNS0_12FinalMixTypeEf", "最终混音音量", void, h_ren_FinalVol, void *mix, float v) {
    (void)mix; pthread_mutex_lock(&g_ren.m); int chg = g_ren.finalvol != v; g_ren.finalvol = v; pthread_mutex_unlock(&g_ren.m);
    if (chg && g_flag.dbg) LOG("audio::SetFinalMixVolume %.3f", v);
}
// MemoryPoolType：宿主对象记地址、长度、挂载状态；数据直接从客体内存读，无需真的挂载
// MemoryPoolType: the host object records address, size and attach state; data is read straight from guest memory, no real attach needed
static mempool_t *pget(const void *g) { return g ? *(mempool_t *const *)g : NULL; }
HLE_FORCE("_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm", "取内存池：记地址（返回 bool）", unsigned, h_ren_PoolAcq, void *cfg, mempool_t **pool, void *addr, uint64_t sz) { (void)cfg; mempool_t *p = calloc(1, sizeof *p); p->addr = addr; p->size = sz; p->attached = 0; *pool = p; return 1; }
HLE_FORCE("_ZN2nn5audio17ReleaseMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeE", "放内存池", void, h_ren_PoolRel, void *cfg, mempool_t **pool) { (void)cfg; free(*pool); *pool = NULL; }
HLE_FORCE("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", "挂内存池：立即成功（返回 bool）", unsigned, h_ren_PoolAttach, void *pool) { mempool_t *p = pget(pool); if (p) p->attached = 1; return p != NULL; }
HLE_FORCE("_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", "卸内存池：立即成功（返回 bool）", unsigned, h_ren_PoolDetach, void *pool) { mempool_t *p = pget(pool); if (p) p->attached = 0; return p != NULL; }
HLE_FORCE("_ZN2nn5audio20IsMemoryPoolAttachedEPKNS0_14MemoryPoolTypeE", "内存池是否已挂（返回 bool）", unsigned, h_ren_PoolAttached, const void *pool) { mempool_t *p = pget(pool); return p && p->attached; }
HLE_FORCE("_ZN2nn5audio20GetMemoryPoolAddressEPKNS0_14MemoryPoolTypeE", "内存池地址", void *, h_ren_PoolAddr, const void *pool) { mempool_t *p = pget(pool); return p ? p->addr : NULL; }
HLE_FORCE("_ZN2nn5audio18CopyMemoryPoolDataEPKNS0_14MemoryPoolTypeEPvPKvm", "内存池数据拷贝 → memcpy", void, h_ren_PoolCopy, const void *pool, void *dst, const void *src, uint64_t n) { (void)pool; memcpy(dst, src, n); }
// VoiceType：8 字节句柄指向宿主 voice_t（见 vget） | VoiceType: an 8-byte handle pointing at the host voice_t (see vget)
HLE_FORCE("_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvm", "取声音槽：记格式 / 采样率 / 声道 / ADPCM 系数（返回 bool）", unsigned, h_ren_VoiceAcq, void *cfg, voice_t **voice, int rate, int ch, int fmt, int prio, const void *extra, uint64_t sz) {
    (void)cfg; pthread_mutex_lock(&g_ren.m);
    int slot = -1; for (int i = 0; i < REN_VMAX; i++) if (!g_ren.v[i].used) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_ren.m); LOG("【红】audren：voice 槽用完（%d）", REN_VMAX); return 0; }
    voice_t *v = &g_ren.v[slot]; memset(v, 0, sizeof *v);
    v->used = 1; v->guest = voice; v->rate = rate > 0 ? rate : g_ren.rate; v->ch = ch == 2 ? 2 : 1; v->fmt = fmt; v->state = REN_STOP; v->vol = 1; v->pitch = 1;
    if (fmt == REN_SF_ADPCM && extra && sz >= 32) { memcpy(v->coef, extra, 32); v->has_coef = 1; }
    *voice = v;
    pthread_mutex_unlock(&g_ren.m);
    static int nacq; if (nacq < 12) { nacq++; v->verbose = g_flag.dbg; LOG("audio::AcquireVoiceSlot 槽 %d：%d Hz × %d 声道，格式 %s，优先级 %d，参数块 %llu 字节", slot, rate, ch, fmt == REN_SF_PCM16 ? "PcmInt16" : fmt == REN_SF_ADPCM ? "Adpcm" : "?", prio, (unsigned long long)sz); }
    if (fmt != REN_SF_PCM16 && fmt != REN_SF_ADPCM) { static int warned; if (!warned) { warned = 1; LOG("【红】audren：采样格式 %d 宿主不支持（只做 PcmInt16 / Adpcm），该 voice 不出声", fmt); } }
    if (fmt == REN_SF_ADPCM && !v->has_coef) { static int warned; if (!warned) { warned = 1; LOG("【红】audren：ADPCM voice 没带系数（参数块 %llu 字节），解码会失真", (unsigned long long)sz); } }
    return 1;
}
HLE_FORCE("_ZN2nn5audio16ReleaseVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE", "放声音槽", void, h_ren_VoiceRel, void *cfg, void **voice) {
    (void)cfg; pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); if (v) v->used = 0; *voice = NULL; pthread_mutex_unlock(&g_ren.m);
}
HLE_FORCE("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE", "声音槽目的混音：只有最终混音，不用记", void, h_ren_VoiceDest, void *cfg, void *voice, void *mix) { (void)cfg; (void)voice; (void)mix; }
HLE_FORCE("_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE", "追加波形缓冲：入待播队列（返回 bool，满了 false）", unsigned, h_ren_Append, void *voice, const wavebuf_t *wb) {
    pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); int ok = 0;
    if (v && wb && v->qt - v->qh < REN_WBMAX) { v->q[v->qt & (REN_WBMAX - 1)].guest = wb; v->q[v->qt & (REN_WBMAX - 1)].wb = *wb; v->qt++; ok = 1; v->nappend++; }
    int slot = v ? (int)(v - g_ren.v) : -1, nap = v ? v->nappend : 0, verbose = v && v->verbose;
    pthread_mutex_unlock(&g_ren.m);
    if (verbose && nap <= 2 && wb) LOG("audio::AppendWaveBuffer 槽 %d #%d：%p %llu 字节，样本 %d..%d，循环 %d 末尾 %d，上下文 %llu 字节", slot, nap, wb->buffer, (unsigned long long)wb->size, wb->start, wb->end, wb->loop, wb->eos, (unsigned long long)wb->ctxsz);
    return (unsigned)ok;
}
HLE_FORCE("_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE", "取已播完的波形缓冲：按播放进度归还", const void *, h_ren_Released, void *voice) {
    pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); const void *r = NULL;
    if (v && v->rh != v->rt) r = v->rel[v->rh++ & (REN_WBMAX - 1)];
    pthread_mutex_unlock(&g_ren.m); return r;
}
HLE_FORCE("_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE", "声音槽播放状态：Play / Stop / Pause（Stop 归还全部缓冲、清进度）", void, h_ren_VoiceSetState, void *voice, int st) {
    pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice);
    if (v) {
        if (st == REN_STOP && v->state != REN_STOP) {   // 停：正在播的与排队的全部归还，样本计数清零 | Stop: return the playing and the queued buffers, reset the sample count
            if (v->has_cur) { vrelease(v, v->cur_guest); v->has_cur = 0; }
            while (v->qh != v->qt) { vrelease(v, v->q[v->qh & (REN_WBMAX - 1)].guest); v->qh++; }
            v->played = 0; v->rn = 0; v->frac = 0;
        }
        v->state = st;
    }
    int slot = v ? (int)(v - g_ren.v) : -1, verbose = v && v->verbose;
    pthread_mutex_unlock(&g_ren.m);
    if (verbose) LOG("audio::SetVoicePlayState 槽 %d：%s", slot, st == REN_PLAY ? "Play" : st == REN_STOP ? "Stop" : st == REN_PAUSE ? "Pause" : "?");
}
HLE_FORCE("_ZN2nn5audio17GetVoicePlayStateEPKNS0_9VoiceTypeE", "声音槽播放状态：照记的回", int, h_ren_VoiceGetState, const void *voice) { pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); int st = v ? v->state : REN_STOP; pthread_mutex_unlock(&g_ren.m); return st; }
HLE_FORCE("_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE", "声音槽已播样本数：混音端如实累计", int64_t, h_ren_VoicePlayed, const void *voice) {
    pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); int64_t n = v ? v->played : 0; pthread_mutex_unlock(&g_ren.m); return n;
}
HLE_FORCE("_ZN2nn5audio14SetVoiceVolumeEPNS0_9VoiceTypeEf", "声音槽音量", void, h_ren_VoiceVol, void *voice, float x) {
    pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); int slot = v ? (int)(v - g_ren.v) : -1, verbose = v && v->verbose && v->nappend <= 2; if (v) v->vol = x; pthread_mutex_unlock(&g_ren.m);
    if (verbose) LOG("audio::SetVoiceVolume 槽 %d：%.3f", slot, x);
}
HLE_FORCE("_ZN2nn5audio13SetVoicePitchEPNS0_9VoiceTypeEf", "声音槽音高（重采样步长倍率）", void, h_ren_VoicePitch, void *voice, float x) { pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); if (v) v->pitch = x; pthread_mutex_unlock(&g_ren.m); }
HLE_FORCE("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii", "声音槽混音音量：voice 声道 → 混音缓冲", void, h_ren_VoiceMixVol, void *voice, void *mix, float x, int src, int dst) {
    (void)mix; pthread_mutex_lock(&g_ren.m); voice_t *v = vget(voice); int slot = v ? (int)(v - g_ren.v) : -1, verbose = v && v->verbose && v->nappend <= 2; if (v && src >= 0 && src < 2 && dst >= 0 && dst < REN_BUFMAX) v->mixvol[src][dst] = x; pthread_mutex_unlock(&g_ren.m);
    if (verbose && x != 0) LOG("audio::SetVoiceMixVolume 槽 %d：声道 %d → 缓冲 %d = %.3f", slot, src, dst, x);
}
// ParseAdpcmHeader 不接管：它是 SDK 里的纯计算（解析 DSP-ADPCM 大端文件头，无 IPC），让 SDK 自己跑；游戏按它给的系数 / 样本数 / 循环点建 voice 与波形缓冲
// ParseAdpcmHeader is not taken over: it is pure computation inside the SDK (parses the big-endian DSP-ADPCM file header, no IPC), so the SDK runs it itself; the game builds voices and wave buffers from the coefficients / sample count / loop points it reports

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
NET_INT(h_sk_Recv2, "_ZN2nn6socket4RecvEiPvmi") NET_INT(h_sk_Send2, "_ZN2nn6socket4SendEiPKvmi") NET_INT(h_sk_Fcntl, "_ZN2nn6socket5FcntlEiiz") NET_INT(h_sk_Ioctl, "_ZN2nn6socket5IoctlEijPvm") NET_INT(h_sk_Accept, "_ZN2nn6socket6AcceptEiP8sockaddrPj")
NET_INT(h_sk_Select, "_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval") NET_INT(h_sk_SendTo, "_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj")
NET_INT(h_sk_Socket, "_ZN2nn6socket6SocketEiii") NET_INT(h_sk_Connect, "_ZN2nn6socket7ConnectEiPK8sockaddrj") NET_INT(h_sk_RecvFrom2, "_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj")
NET_INT(h_sk_Shutdown, "_ZN2nn6socket8ShutdownEii")
HLE_FORCE("_ZN2nn6socket8InetNtoaE7in_addr", "纯计算：点分地址字符串（静态缓冲）", const char *, h_sk_InetNtoa, uint32_t a) { static char b[20]; snprintf(b, sizeof b, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24); return b; }
HLE_FORCE("_ZN2nn6socket8InetNtopEiPKvPcj", "纯计算：IPv4 地址 → 字符串", const char *, h_sk_InetNtop, int af, const void *src, char *dst, uint32_t n) { if (af != 2 || n < 16) return NULL; const uint8_t *p = src; snprintf(dst, n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); return dst; }
HLE_FORCE("_ZN2nn6socket8InetPtonEiPKcPv", "纯计算：字符串 → IPv4 地址", int, h_sk_InetPton, int af, const char *src, void *dst) { unsigned a, b, c, d; if (af != 2 || sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0; uint8_t *p = dst; p[0] = a; p[1] = b; p[2] = c; p[3] = d; return 1; }
HLE_FORCE("_ZN2nn6socket9InetHtonlEj", "纯计算：主机序 → 网络序", uint32_t, h_sk_Htonl, uint32_t v) { return __builtin_bswap32(v); }
HLE_FORCE("_ZN2nn6socket9InetNtohlEj", "纯计算：网络序 → 主机序", uint32_t, h_sk_Ntohl, uint32_t v) { return __builtin_bswap32(v); }
HLE_FORCE("_ZN2nn6socket9InetHtonsEt", "纯计算：主机序 → 网络序", uint32_t, h_sk_Htons, unsigned v) { return __builtin_bswap16((uint16_t)v); }
HLE_FORCE("_ZN2nn6socket9InetNtohsEt", "纯计算：网络序 → 主机序", uint32_t, h_sk_Ntohs, unsigned v) { return __builtin_bswap16((uint16_t)v); }
HLE_FORCE("nnsocketInetHtons", "纯计算：主机序 → 网络序", uint32_t, h_nsk_Htons, unsigned v) { return __builtin_bswap16((uint16_t)v); }
HLE_FORCE("nnsocketInetNtohs", "纯计算：网络序 → 主机序", uint32_t, h_nsk_Ntohs, unsigned v) { return __builtin_bswap16((uint16_t)v); }
NET_RES(h_ssl_ImportPki, "_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE") NET_VOID(h_nifm_LocalMode, "_ZN2nn4nifm19SetLocalNetworkModeEb") NET_RES(h_nifm_PrimaryIp2, "_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr")
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
