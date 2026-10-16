// 过场 / 背景视频（movie:: = subsdk0 任天堂 movie 库）。两条实现，按宿主选：
//   通用宿主（DM_GENERIC_HOST，Blade II / UE4 4.17 的 SwitchMediaPlayer）：真播放——AVFoundation 解复用 + 解码（movie_av.m），按游戏的协议
//   交 NV12 帧和 PCM16，下文是 Blade 的协议；
//   暗黑 II（d2r 宿主）：「空播」（不解码，按容器时长发流尾），协议写在文件后半那一段的开头。
// Blade 的协议（main 原映像偏移，读码所得；播放器对象 P 由游戏 new 出，字段偏移相对 P）：
//   打开 0x34113d0：0x340ee40 按扩展名定容器类型（mp4 / mkv / webm / ogg → P+0x18）→ new 16 字节 → Extractor(类型) → 槽2 SetDataSource(UTF-8 路径
//     "rom:/BladeII/Content/Movies/<名>.mp4") → 槽4 GetTrackCount；状态 0 且轨数 > 0 才算开成。→ 0x340f670 逐轨：槽5 GetTrackConfiguration(i, MediaData*)，
//     FindString "mime" 前缀 "video/" / "audio/"；"video/avc" ⇒ 解码器类型 1（"video/x-vnd.on2.vp8" ⇒ 2/3），"audio/mp4a-latm" ⇒ 4（"audio/vorbis" ⇒ 5）；
//     每轨把 MediaData 拷构造进轨表（视频表 P+0x400 步长 0x58，音频表 P+0x410 步长 0x40；+0 MediaData、+8 类型、+0x10 轨号），再读
//     FindInt64 "durationUs,"（键里带逗号：原版就查不到，保持查不到）、FindInt32 "bit-rate"、视频 "frame-rate" "width" "height" "stride"、FindRect "crop"，
//     音频 "sample-rate" "channel-count"。没有视频轨 ⇒ 关掉。取第 0 条视频轨 / 第 0 条音频轨（P+0x514 / P+0x510）。
//   准备 0x34114b0 → 0x340f260：槽4 GetTrackCount；视频 Decoder(类型, 输出格式 4 = NV12, 模式 1, GetThreadPriority())（new 16 字节）→ 槽3 RegisterClientEvents(P+0x50
//     = 4 个 EventType*：输入可用 / 输出可用 / 格式变化 / 出错；登记时这 4 个指针还是空的，0x3411050 才填 ⇒ 库只能存结构指针、发事件时再读) → 槽2 Configure(轨表项的 MediaData) → Ext 槽6 SelectTrack(轨号) → 锁下 槽14 GetOutputBufferSize(&大小, &对齐)
//     → 槽4 GetOutputFormat(MediaData*) 读 "width" "height" "stride" FindRect "crop"；音频同样（输出格式 1 = PCM16，事件 P+0x28，读 "sample-rate" "channel-count"）。
//     → 0x3411050 起两条客体线程（解码事件线程 0x3410ac0、喂样本线程 0x3410f60）→ 槽5 Start（音频、视频）。
//   输出格式枚举（subsdk0 DecoderImpl::ConvertDecoderOutputFormat / GetOutputBufferSize）：1 = PCM16 交错（缓冲 = 声道数 << 12 字节），2 = ARGB（w·h·4），
//     4 = NV12 紧排（Y w×h 再 UV w×h/2，缓冲 = w·h + (w/2)(h/2)·2）。槽14 的第二个输出是对齐（游戏拿去做分配对齐，0 时用 0x20）。
//   事件线程 0x3410ac0（WaitAny）：输入可用 ⇒ 循环 槽9 AcquireInputBufferIndex(&idx, &剩余) 直到非 0，idx 进空闲输入表（音频 P+0x420 / 视频 P+0x430），每个都 Signal 喂样本事件；
//     输出可用 ⇒ 0x34108f0 循环 槽12 AcquireOutputBufferIndex(&idx, &pts, &标志, &剩余) 直到非 0：标志 4 ⇒ 立即 槽15 Release、置 P+0x520（流尾）；否则
//     {pts, 标志, idx} 进输出表（音频 P+0x460 / 视频 P+0x470）。视频 pts 在入表时加循环偏移：pts == 0 视为回到开头，偏移 += 上一帧 pts + 帧间隔（全局 0x7c9dac0 / +8，
//     进程内从不清零）。格式变化 ⇒ 重读 槽14 + 槽4。
//   喂样本 0x34100a0：Ext 槽9 GetSampleTrackIndex(u64*)；0x2000 ⇒ 循环（P+0x523）则 槽8 SeekTo(0) 再问，否则给两个解码器的全部空闲输入发流尾；
//     轨号对上 ⇒ 取该解码器一个空闲输入 → 0x340ff50：槽10 GetInputBuffer(idx, Buffer*) → Ext 槽10 Read(Buffer*)（0x2000 或长度 0 ⇒ 循环则 SeekTo(0) 重读，
//     否则标志 4）→ Ext 槽12 GetSampleTime(&pts) → Offset / Size → 槽11 SendInputBufferForDecode(idx, 偏移, 长度, pts, 标志) → Ext 槽11 Advance（0x2000 ⇒ 全发流尾）。
//     没有空闲输入就返回，等下一次输入可用。⇒ 游戏只搬运样本字节，不看内容；交错顺序由 Extractor 决定。
//   取帧（每帧，两个用户：UE4 播放器 0x3a2e0d0、启动影片播放器 0x3b5cc00）0x3411a80(目标 µs = 自开播起的系统时间)：锁下扫视频输出表，取 pts < 目标的最后一帧，
//     更早的直接 槽15 Release；→ 槽13 GetOutputBuffer(idx, Buffer{游戏暂存区, 容量, 范围 0..容量}) → 槽15 Release。暂存区按 NV12 拆两半 memcpy 进 G8（w×h）与
//     R8G8（w/2×h/2）两张纹理，着色器转 RGB ⇒ 输出必须是紧排 NV12、宽高 = GetOutputFormat 的 width / height。
//   取音频 0x3411cb0：弹音频输出表头 → 槽13 GetOutputBuffer → Size() 即字节数 → 槽15 Release。UE4 播放器交给音频汇（声道 / 采样率来自 GetOutputFormat），
//     启动影片播放器直接 nn::audio AppendWaveBuffer（样本数 = 字节 / (声道·2)）⇒ PCM16 交错。
//   播完：0x340ee00 = P+0x520 且两张输出表都空。跳过 / 关闭 0x3411770：停两条线程 → 槽6 Stop（音频、视频）→ 清表 → 槽1 删除解码器、删除 Extractor（游戏的 operator delete）。
//   循环（标题背景）：只在 Extractor 层：取轨号 / Read 遇流尾 ⇒ SeekTo(0) 接着读，解码器不见流尾；pts 回 0 由上面的偏移接上。
// 本剪口（通用宿主）：
//   Extractor = 样本游标：SetDataSource 时 movie_av 扫出各轨样本表（直通读，不解码）；已选轨按解码时间交错（同刻视频先），和 NuMediaExtractor 一样 Advance 走过最后一个
//     样本仍回 0，流尾由下一次 GetSampleTrackIndex / Read 报。Read 不给压缩数据，给 32 字节令牌 {轨, 样本号, 定位代数, 帧数, 定位时刻}（游戏只搬运）。
//   Decoder = 令牌 → 宿主解码：一条客体工作线程（nn::os::CreateThread 建，SignalEvent 都在它上面发）按令牌顺序向 AVAssetReader 取下一帧 / 下一段 PCM。
//     背压同硬件：输出槽（视频 8、音频 16）被游戏占着时不解码、不还输入（视频 8、音频 16 个输入），喂样本线程自然停下。令牌不连续（定位代数变 / 样本号跳）
//     ⇒ 读取器从该样本时刻重建。视频 pts：解码器的第一帧 1 µs 起（不是 0：0 会让上面那个从不清零的全局把上一段影片的时长加成偏移；开播前的 SeekTo(0) 也不改），之后循环回开头的第一帧正好 0。
//     音频 pts = 样本 pts，每个 AAC 包出样本时长对应的帧数（解码器修掉的起头补零让末尾少两包，补静音）。流尾：所有令牌解完后交一个标志 4 的空项，此后收到的输入不再归还。
//   MediaData 是 8 字节句柄 → 宿主字典（拷构造 / 赋值深拷），轨配置里另带私有键 dm-path / dm-track 给 Configure 找文件。没实现的虚槽一律报红陷阱。
// Cutscene / background videos (movie:: = the Nintendo movie library in subsdk0). Two implementations, chosen per host:
//   generic host (DM_GENERIC_HOST, Blade II / the SwitchMediaPlayer of UE4 4.17): real playback -- AVFoundation demux + decode (movie_av.m), handing NV12 frames and PCM16
//   over the game's protocol; the Blade protocol is described below;
//   Diablo II (d2r host): "empty playback" (no decoding, end of stream after the container duration), its protocol is at the start of the second half of this file.
// Blade's protocol (offsets in the original main image, from reading the code; the player object P is new'ed by the game, field offsets are relative to P):
//   Open 0x34113d0: 0x340ee40 derives the container type from the extension (mp4 / mkv / webm / ogg -> P+0x18) -> new 16 bytes -> Extractor(type) -> slot 2
//     SetDataSource(UTF-8 path "rom:/BladeII/Content/Movies/<name>.mp4") -> slot 4 GetTrackCount; open succeeds only with status 0 and > 0 tracks. -> 0x340f670 per track:
//     slot 5 GetTrackConfiguration(i, MediaData*), FindString "mime" prefix "video/" / "audio/"; "video/avc" => decoder type 1 ("video/x-vnd.on2.vp8" => 2/3),
//     "audio/mp4a-latm" => 4 ("audio/vorbis" => 5); each track's MediaData is copy-constructed into a track table (video P+0x400 stride 0x58, audio P+0x410 stride 0x40;
//     +0 MediaData, +8 type, +0x10 track index), then FindInt64 "durationUs," (the key has a comma: the original never finds it, kept that way), FindInt32 "bit-rate",
//     video "frame-rate" "width" "height" "stride", FindRect "crop", audio "sample-rate" "channel-count". No video track => close. Video track 0 / audio track 0 are
//     taken (P+0x514 / P+0x510).
//   Prepare 0x34114b0 -> 0x340f260: slot 4 GetTrackCount; video Decoder(type, output format 4 = NV12, mode 1, GetThreadPriority()) (new 16 bytes) -> slot 3
//     RegisterClientEvents(P+0x50 = 4 EventType*: input available / output available / format changed / error; the 4 pointers are still empty at registration and only filled
//     by 0x3411050 => the library must keep the struct pointer and read it when signalling) -> slot 2 Configure(MediaData of the track table entry)
//     -> Ext slot 6 SelectTrack(track index) -> under a mutex slot 14 GetOutputBufferSize(&size, &alignment) -> slot 4 GetOutputFormat(MediaData*) reading "width"
//     "height" "stride" FindRect "crop"; audio likewise (output format 1 = PCM16, events P+0x28, reads "sample-rate" "channel-count").
//     -> 0x3411050 starts two guest threads (decoder event thread 0x3410ac0, sample feeder thread 0x3410f60) -> slot 5 Start (audio, video).
//   Output format enum (subsdk0 DecoderImpl::ConvertDecoderOutputFormat / GetOutputBufferSize): 1 = interleaved PCM16 (buffer = channels << 12 bytes), 2 = ARGB (w*h*4),
//     4 = tightly packed NV12 (Y w x h then UV w x h/2, buffer = w*h + (w/2)(h/2)*2). The second output of slot 14 is an alignment (the game allocates with it, 0x20 when 0).
//   Event thread 0x3410ac0 (WaitAny): input available => loop slot 9 AcquireInputBufferIndex(&idx, &remaining) until non-zero, idx goes into the free input list
//     (audio P+0x420 / video P+0x430), each one signals the feeder event; output available => 0x34108f0 loops slot 12 AcquireOutputBufferIndex(&idx, &pts, &flags,
//     &remaining) until non-zero: flag 4 => slot 15 Release at once and set P+0x520 (end of stream); otherwise {pts, flags, idx} goes into the output list (audio P+0x460 /
//     video P+0x470). Video pts get a loop offset when enqueued: pts == 0 counts as back at the start, offset += previous pts + frame interval (global 0x7c9dac0 / +8,
//     never cleared in the process). Format changed => re-read slot 14 + slot 4.
//   Feeder 0x34100a0: Ext slot 9 GetSampleTrackIndex(u64*); 0x2000 => if looping (P+0x523) slot 8 SeekTo(0) and ask again, otherwise send end of stream into every free input
//     of both decoders; matching track => take a free input of that decoder -> 0x340ff50: slot 10 GetInputBuffer(idx, Buffer*) -> Ext slot 10 Read(Buffer*) (0x2000 or
//     length 0 => SeekTo(0) and read again when looping, otherwise flag 4) -> Ext slot 12 GetSampleTime(&pts) -> Offset / Size -> slot 11 SendInputBufferForDecode(idx,
//     offset, length, pts, flags) -> Ext slot 11 Advance (0x2000 => end of stream to all). No free input => return and wait for the next input available.
//     => the game only carries sample bytes around and never looks at them; the interleaving order is the Extractor's.
//   Frame fetch (every frame, two users: the UE4 player 0x3a2e0d0 and the startup movie player 0x3b5cc00) 0x3411a80(target us = system time since playback started):
//     under a mutex scans the video output list, takes the last frame with pts < target and releases older ones with slot 15; -> slot 13 GetOutputBuffer(idx,
//     Buffer{game staging area, capacity, range 0..capacity}) -> slot 15 Release. The staging area is split as NV12 and memcpy'd into a G8 (w x h) and an R8G8
//     (w/2 x h/2) texture, a shader converts to RGB => the output must be tightly packed NV12 with width / height = GetOutputFormat's width / height.
//   Audio fetch 0x3411cb0: pops the head of the audio output list -> slot 13 GetOutputBuffer -> Size() is the byte count -> slot 15 Release. The UE4 player hands it to
//     an audio sink (channels / rate from GetOutputFormat), the startup movie player does nn::audio AppendWaveBuffer directly (samples = bytes / (channels*2)) => interleaved PCM16.
//   Finished: 0x340ee00 = P+0x520 and both output lists empty. Skip / close 0x3411770: stops both threads -> slot 6 Stop (audio, video) -> clears the lists -> slot 1
//     deletes the decoders and the Extractor (the game's operator delete).
//   Looping (title background): only at the Extractor level: track index / Read hits end of stream => SeekTo(0) and keep reading, the decoder never sees end of
//     stream; pts going back to 0 is stitched by the offset above.
// This cut (generic host):
//   Extractor = sample cursor: at SetDataSource movie_av scans every track's sample table (pass-through read, no decoding); selected tracks are interleaved by decode
//     time (video first on a tie); like NuMediaExtractor, Advance past the last sample still returns 0 and end of stream is reported by the next GetSampleTrackIndex / Read.
//     Read hands no compressed data but a 32-byte token {track, sample index, seek generation, frame count, seek time} (the game only carries it).
//   Decoder = token -> host decoding: one guest worker thread (made with nn::os::CreateThread, all SignalEvent calls are made on it) takes the next frame / next
//     PCM run from AVAssetReader in token order. Back pressure as on hardware: while the game holds the output slots (video 8, audio 16) nothing is decoded and no
//     input is returned (video 8, audio 16 inputs), so the feeder stops by itself. Non-contiguous tokens (seek generation changed / sample index jumped) => the reader
//     is rebuilt from that sample's time. Video pts: the decoder's first frame is 1 us (not 0: 0 would make the never-cleared global above add the previous
//     movie's duration as an offset; a SeekTo(0) before playback does not change that), the first frame after a later loop back is exactly 0. Audio pts = the sample's pts, each AAC packet yields the frames of its
//     duration (the decoder trims the leading priming, so the last two packets come up short and are padded with silence). End of stream: after all tokens are
//     decoded, one empty item with flag 4 is handed over, and inputs received after that are not returned any more.
//   MediaData is an 8-byte handle -> host dictionary (copy construction / assignment deep-copy); the track configuration carries private keys dm-path / dm-track so
//   Configure can find the file. Any virtual slot not implemented is a failure trap.
#include "hle.h"
#include <time.h>

#define ST_OK        0
#define ST_ERROR     0x1000      // movie::Status 一般错误（NnExtractor 没底层对象时回的值） | movie::Status generic error (what NnExtractor returns without an underlying object)
#define ST_EOS       0x2000      // movie::Status 流尾（Blade 0x34100cc / D2R 0x8995b8 比较的值） | movie::Status end of stream (the value compared at Blade 0x34100cc / D2R 0x8995b8)
#define ST_TRY_AGAIN 0x1008      // 暂无缓冲（D2R 游戏 0x899564 自己用的值）：Acquire* 没有可给的索引时回它 | no buffer for now (the value D2R's game itself uses at 0x899564): Acquire* returns it when it has no index to give
#define FLAG_EOS     4
#define NN_OS_DEFAULT_THREAD_PRIORITY 16
typedef struct { void *data; uint64_t cap; int32_t off, size; } mvbuf;   // movie::Buffer 的宿主布局（游戏栈上留了 ≥0x40 字节） | host layout of movie::Buffer (the game leaves ≥0x40 bytes on its stack)
#define TRAP(cls, slot, what) static void mv_trap_##cls##_##slot(void) { die("movie 剪口：%s 槽%d %s 被调用（不在协议内）", #cls, slot, what); }

#if DM_GENERIC_HOST
#include "movie_av.h"
#define MVLOG(fmt, ...) do { if (g_flag.movie) LOG("movie [%.3f] " fmt, (now_ns() - g_t0_ns) / 1e9, ##__VA_ARGS__); } while (0)

// ---------- MediaData：8 字节句柄 → 宿主字典 ----------
// ---------- MediaData: 8-byte handle -> host dictionary ----------
enum { MD_I32 = 1, MD_I64, MD_STR, MD_RECT };
typedef struct { char key[24]; int type; int64_t i; int32_t r[4]; char s[1024]; } md_ent;
typedef struct { int n; md_ent e[16]; } md_t;
static const md_ent *md_find(void *const *self, const char *k, int type) {
    const md_t *m = *self; if (!m) return NULL;
    for (int i = 0; i < m->n; i++) if (m->e[i].type == type && !strcmp(m->e[i].key, k)) return &m->e[i];
    return NULL;
}
static md_ent *md_put(void **self, const char *k, int type) {
    md_t *m = *self; if (!m) *self = m = calloc(1, sizeof *m);
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].key, k)) { memset(&m->e[i], 0, sizeof m->e[i]); strlcpy(m->e[i].key, k, sizeof m->e[i].key); m->e[i].type = type; return &m->e[i]; }
    if (m->n == (int)(sizeof m->e / sizeof m->e[0])) die("movie 剪口：MediaData 键满（%s）", k);
    md_ent *e = &m->e[m->n++]; strlcpy(e->key, k, sizeof e->key); e->type = type; return e;
}
static void md_i32(void **self, const char *k, int32_t v) { md_put(self, k, MD_I32)->i = v; }
static void md_i64(void **self, const char *k, int64_t v) { md_put(self, k, MD_I64)->i = v; }
static void md_str(void **self, const char *k, const char *v) { strlcpy(md_put(self, k, MD_STR)->s, v, sizeof ((md_ent *)0)->s); }
static void md_rect(void **self, const char *k, int l, int t, int r, int b) { md_ent *e = md_put(self, k, MD_RECT); e->r[0] = l; e->r[1] = t; e->r[2] = r; e->r[3] = b; }
static void md_clear(void **self) { free(*self); *self = NULL; }
static void md_copy(void **self, void *const *src) { md_t *s = *src; *self = NULL; if (s) { md_t *m = malloc(sizeof *m); memcpy(m, s, sizeof *m); *self = m; } }
HLE_FORCE("_ZN5movie9MediaDataC1Ev", "过场视频：MediaData = 宿主字典句柄", void, mv_MediaData, void **self) { *self = NULL; }
HLE_FORCE("_ZN5movie9MediaDataD1Ev", "过场视频：MediaData 析构", void, mv_MediaDataD, void **self) { md_clear(self); }
HLE_FORCE("_ZN5movie9MediaData5ClearEv", "过场视频：MediaData 清空", void, mv_MediaDataClear, void **self) { md_clear(self); }
HLE_FORCE("_ZN5movie9MediaDataC1ERKS0_", "过场视频：MediaData 拷构造（轨表）", void, mv_MediaDataCopy, void **self, void *const *src) { md_copy(self, src); }
HLE_FORCE("_ZN5movie9MediaDataaSERKS0_", "过场视频：MediaData 赋值（选轨）", void **, mv_MediaDataAssign, void **self, void *const *src) {
    if (self != src) { void *old = *self; md_copy(self, src); free(old); }
    return self;
}
HLE_FORCE("_ZNK5movie9MediaData9FindInt32EPKcPi", "过场视频：MediaData 查 int32", int, mv_FindInt32, void *const *self, const char *k, int32_t *v) {
    const md_ent *e = md_find(self, k, MD_I32); if (e) *v = (int32_t)e->i; return e != NULL;
}
HLE_FORCE("_ZNK5movie9MediaData9FindInt64EPKcPl", "过场视频：MediaData 查 int64", int, mv_FindInt64, void *const *self, const char *k, int64_t *v) {
    const md_ent *e = md_find(self, k, MD_I64); if (e) *v = e->i; return e != NULL;
}
HLE_FORCE("_ZNK5movie9MediaData10FindStringEPKcPS2_", "过场视频：MediaData 查字符串", int, mv_FindString, void *const *self, const char *k, const char **v) {
    const md_ent *e = md_find(self, k, MD_STR); if (e) *v = e->s; return e != NULL;
}
HLE_FORCE("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", "过场视频：MediaData 查矩形", int, mv_FindRect, void *const *self, const char *k, int32_t *l, int32_t *t, int32_t *r, int32_t *b) {
    const md_ent *e = md_find(self, k, MD_RECT); if (e) { *l = e->r[0]; *t = e->r[1]; *r = e->r[2]; *b = e->r[3]; } return e != NULL;
}

// ---------- 公共：对象删除（游戏 new 的 16 字节，删除析构走游戏自己的 operator delete）、客体工作线程 ----------
// ---------- Common: object deletion (16 bytes new'ed by the game, the deleting destructor uses the game's own operator delete), guest worker thread ----------
static void guest_delete(void *p) {
    static void (*del)(void *);
    if (!del) for (int i = 0; i < g_nmod && !del; i++) if (!strcmp(g_mod[i].name, "main")) del = module_sym(&g_mod[i], "_ZdlPv");
    if (!del) die("movie 剪口：main 里找不到 operator delete（_ZdlPv）");
    del(p);
}
HLE_FORCE("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", "过场视频：库对象由游戏 new / delete，库内分配器不用",
          void, mv_SetAllocator, void *alloc, void *fr, void *realloc_, void *user) { (void)alloc; (void)fr; (void)realloc_; (void)user; }
HLE_FORCE("_ZN5movie17GetThreadPriorityEv", "过场视频：只作 Decoder 构造参数", int, mv_GetThreadPriority, void) { return NN_OS_DEFAULT_THREAD_PRIORITY; }

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER, g_idle = PTHREAD_COND_INITIALIZER;
static void (*g_signal_event)(void *);
static void worker_main(void *arg);
static void worker_ensure(void) {                          // 在客体线程上调（Decoder::Start 里） | called on a guest thread (inside Decoder::Start)
    static int started; pthread_mutex_lock(&g_lock); int go = !started; started = 1; pthread_mutex_unlock(&g_lock);
    if (!go) return;
    static _Alignas(16) uint8_t thread_obj[0x400];
    uint32_t (*create)(void *, void (*)(void *), void *, void *, size_t, int) = guest_sym("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    void (*start)(void *) = guest_sym("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    if (!create || !start || !g_signal_event) die("movie 剪口：缺 nn::os 线程 / 事件符号");
    size_t ss = 0x100000; void *stack = aligned_alloc(0x1000, ss);  // AVFoundation 在这条线程上跑，栈给足 1 MB | AVFoundation runs on this thread, give it a full 1 MB stack
    uint32_t r = create(thread_obj, worker_main, NULL, stack, ss, NN_OS_DEFAULT_THREAD_PRIORITY);
    if (r) die("movie 剪口：建解码工作线程失败 %#x", r);
    start(thread_obj);
}

// ---------- Extractor：样本游标 ----------
// ---------- Extractor: sample cursor ----------
typedef struct { char magic[4]; int32_t track, index, gen, frames, pad; int64_t seek_us; } mvtok;   // 32 字节令牌 | 32-byte token
_Static_assert(sizeof(mvtok) == 32, "mvtok");
typedef struct {
    char host[1024];
    mvav_file *f;
    int sel[8], pos[8];
    int gen; int64_t seek_us;
    int64_t last_pts;
} mvx_t;
static mvx_t *X(void *self) { mvx_t *x = ((mvx_t **)self)[1]; if (!x) die("movie 剪口：Extractor 没有宿主状态"); return x; }
static int ext_next(const mvx_t *x) {                      // 下一个样本所在轨，-1 = 流尾 | track of the next sample, -1 = end of stream
    int best = -1; int64_t bd = 0;
    for (int t = 0; x->f && t < mvav_ntracks(x->f); t++) {
        const mvav_track *m = mvav_track_at(x->f, t);
        if (!x->sel[t] || x->pos[t] >= m->nsamples) continue;
        int64_t d = m->dts_us[x->pos[t]];
        if (best < 0 || d < bd || (d == bd && m->kind == 1 && mvav_track_at(x->f, best)->kind != 1)) { best = t; bd = d; }
    }
    return best;
}
static void ext_delete(void *self) {
    mvx_t *x = ((mvx_t **)self)[1];
    if (x) { mvav_close(x->f); free(x); }
    guest_delete(self);
}
static int ext_SetDataSourcePath(void *self, const char *path) {
    mvx_t *x = X(self);
    mvav_close(x->f); memset(x, 0, sizeof *x);
    if (!path || !dm_fs_host_path(path, x->host, sizeof x->host)) { LOG("movie：%s 在 RomFS 里找不到 → 答打不开（游戏跳过这段视频）", path ? path : "(null)"); return ST_ERROR; }
    char err[256]; int64_t t0 = now_ns();
    if (!(x->f = mvav_open(x->host, err, sizeof err))) { LOG("movie：%s 打不开（%s）→ 答打不开（游戏跳过这段视频）", path, err); return ST_ERROR; }
    char desc[256] = ""; size_t k = 0;
    for (int t = 0; t < mvav_ntracks(x->f); t++) {
        const mvav_track *m = mvav_track_at(x->f, t);
        k += snprintf(desc + k, sizeof desc - k, m->kind == 1 ? " [%d %s %dx%d %dfps %d 样本]" : " [%d %s %d Hz %d 声道 %d 样本]", t, m->mime,
                      m->kind == 1 ? m->width : m->sample_rate, m->kind == 1 ? m->height : m->channels, m->kind == 1 ? m->fps : m->nsamples, m->nsamples);
        if (k >= sizeof desc) break;
    }
    LOG("movie：打开 %s（%.0f ms）%s", path, (now_ns() - t0) / 1e6, desc);
    return ST_OK;
}
static int ext_GetTrackCount(void *self, int *n) { mvx_t *x = X(self); *n = x->f ? mvav_ntracks(x->f) : 0; return ST_OK; }
static int ext_GetTrackConfiguration(void *self, int i, void **md) {
    mvx_t *x = X(self); const mvav_track *m = x->f ? mvav_track_at(x->f, i) : NULL;
    if (!m) die("movie 剪口：GetTrackConfiguration 轨号 %d 越界", i);
    md_clear(md);
    md_str(md, "mime", m->mime); md_i64(md, "durationUs", m->duration_us); md_i32(md, "bit-rate", m->bitrate);
    if (m->kind == 1) {
        md_i32(md, "frame-rate", m->fps); md_i32(md, "width", m->width); md_i32(md, "height", m->height); md_i32(md, "stride", m->width);
        md_rect(md, "crop", 0, 0, m->width - 1, m->height - 1);
    } else { md_i32(md, "sample-rate", m->sample_rate); md_i32(md, "channel-count", m->channels); }
    md_str(md, "dm-path", x->host); md_i32(md, "dm-track", i);
    return ST_OK;
}
static int ext_SelectTrack(void *self, int i) {
    mvx_t *x = X(self); if (!x->f || i < 0 || i >= mvav_ntracks(x->f)) die("movie 剪口：SelectTrack 轨号 %d 越界", i);
    x->sel[i] = 1; MVLOG("SelectTrack %d", i); return ST_OK;
}
static int ext_SeekTo(void *self, int64_t us, int mode) {
    (void)mode; mvx_t *x = X(self);
    for (int t = 0; x->f && t < mvav_ntracks(x->f); t++) {
        const mvav_track *m = mvav_track_at(x->f, t); int p = 0;
        for (int k = 0; k < m->nsamples && m->pts_us[k] <= us; k++) if (m->sync[k]) p = k;   // 不晚于目标的最后一个同步样本 | last sync sample not later than the target
        x->pos[t] = us > 0 ? p : 0;
    }
    x->gen++; x->seek_us = us;
    MVLOG("SeekTo %lld µs（第 %d 代）", (long long)us, x->gen);
    return ST_OK;
}
static int ext_GetSampleTrackIndex(void *self, uint64_t *t) { int n = ext_next(X(self)); if (n < 0) return ST_EOS; *t = (uint64_t)n; return ST_OK; }
static int ext_Read(void *self, mvbuf *b) {
    mvx_t *x = X(self); int t = ext_next(x);
    b->off = b->size = 0;
    if (t < 0) return ST_EOS;
    if (b->cap < sizeof(mvtok)) die("movie 剪口：Read 的缓冲只有 %llu 字节，放不下令牌", (unsigned long long)b->cap);
    const mvav_track *m = mvav_track_at(x->f, t); int k = x->pos[t];
    mvtok tok = {{'D', 'M', 'M', 'V'}, t, k, x->gen, m->kind == 2 ? (int32_t)((m->dur_us[k] * (int64_t)m->sample_rate + 500000) / 1000000) : 1, 0, x->seek_us};
    memcpy(b->data, &tok, sizeof tok); b->size = sizeof tok;
    x->last_pts = m->pts_us[k];
    return ST_OK;
}
static int ext_Advance(void *self) { mvx_t *x = X(self); int t = ext_next(x); if (t < 0) return ST_EOS; x->pos[t]++; return ST_OK; }
static int ext_GetSampleTime(void *self, int64_t *us) {
    mvx_t *x = X(self); int t = ext_next(x);
    *us = t < 0 ? x->last_pts : mvav_track_at(x->f, t)->pts_us[x->pos[t]];
    return t < 0 ? ST_EOS : ST_OK;
}
TRAP(ext, 0, "~Extractor（非删除）") TRAP(ext, 3, "SetDataSource(void const*, int)") TRAP(ext, 7, "UnselectTrack")
TRAP(ext, 13, "GetSampleMetaData") TRAP(ext, 14, "GetBufferedRange") TRAP(ext, 15, "SetDataSource(reader, path)")
static void *const g_ext_vt[16] = {
    mv_trap_ext_0, ext_delete, ext_SetDataSourcePath, mv_trap_ext_3, ext_GetTrackCount, ext_GetTrackConfiguration, ext_SelectTrack, mv_trap_ext_7,
    ext_SeekTo, ext_GetSampleTrackIndex, ext_Read, ext_Advance, ext_GetSampleTime, mv_trap_ext_13, mv_trap_ext_14, mv_trap_ext_15,
};
HLE_FORCE("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", "过场视频：Extractor 装宿主虚表（AVFoundation 样本游标）",
          void, mv_Extractor, void **self, int type) {
    MVLOG("Extractor(容器类型 %d)", type);
    self[0] = (void *)g_ext_vt; self[1] = calloc(1, sizeof(mvx_t));
}

// ---------- Decoder：令牌 → 宿主解码 ----------
// ---------- Decoder: token -> host decoding ----------
#define MV_MAXBUF 16
enum { OUT_FREE, OUT_BUSY, OUT_READY, OUT_HELD };
typedef struct { int idx, valid; mvtok tok; int64_t pts; uint32_t flags; } mvpend;
typedef struct mvd {
    int kind;                         // 1 视频（NV12）/ 2 音频（PCM16） | 1 video (NV12) / 2 audio (PCM16)
    char path[1024]; int track;
    int w, h, rate, ch;
    void **ev;                        // 游戏的 DecoderEvents 结构（4 个 EventType*）：只存指针，发时再读——游戏先登记、后填（0x3411050） | the game's DecoderEvents struct (4 EventType*): only the pointer is kept and read at signal time -- the game registers first and fills it later (0x3411050)
    int configured, started, stopped, busy, eos_in;
    int nin, nout;
    int in_free[MV_MAXBUF], n_in_free;
    mvpend pend[MV_MAXBUF]; int npend;
    _Alignas(16) uint8_t inbuf[MV_MAXBUF][sizeof(mvtok)];
    struct { int st, size; uint32_t flags; int64_t pts; uint8_t *data; } out[MV_MAXBUF];
    int ready[MV_MAXBUF], nready;
    size_t out_cap;
    mvav_dec *av; int gen, next_idx, need_base; int64_t base, last_pts;
    int warned_foreign, decoded_any;
    unsigned n_dec, n_acq, n_get, n_rel;   // 计数（DM_MOVIE 日志） | counters (DM_MOVIE log)
    struct mvd *link;
} mvd_t;
static mvd_t *g_decs;                 // 已 Start 过、未删除的解码器 | decoders that have been started and not deleted
static mvd_t *D(void *self) { mvd_t *d = ((mvd_t **)self)[1]; if (!d) die("movie 剪口：Decoder 没有宿主状态"); return d; }
static void dec_reset_bufs(mvd_t *d) {                    // 持锁 | lock held
    d->n_in_free = d->nin; for (int i = 0; i < d->nin; i++) d->in_free[i] = i;
    d->npend = 0; d->nready = 0; d->eos_in = 0;
    for (int i = 0; i < d->nout; i++) d->out[i].st = OUT_FREE;
}
static void signal_ev(mvd_t *d, int k) { if (!d->ev || !d->ev[k]) die("movie 剪口：%s 解码器的事件 %d 未登记", d->kind == 1 ? "视频" : "音频", k); g_signal_event(d->ev[k]); }
static void wait_idle(mvd_t *d) { while (d->busy) pthread_cond_wait(&g_idle, &g_lock); }

// 工作线程解一个令牌：不持锁；返回输出字节数，-1 = 这个令牌不出东西 | the worker decodes one token without the lock; returns output bytes, -1 = this token yields nothing
static int decode_one(mvd_t *d, const mvpend *p, int s, int64_t *pts) {
    char err[256];
    if (p->tok.gen != d->gen || p->tok.index != d->next_idx) {
        int64_t at = p->tok.index == 0 ? 0 : p->pts;
        MVLOG("%s 解码器从 %lld µs 重建读取器（样本 %d，第 %d 代，期望样本 %d 第 %d 代）", d->kind == 1 ? "视频" : "音频", (long long)at, p->tok.index, p->tok.gen, d->next_idx, d->gen);
        if (mvav_dec_start(d->av, at, err, sizeof err)) die("movie 剪口：%s 建读取器失败：%s", d->path, err);
        if (p->tok.gen != d->gen) { d->need_base = 1; d->gen = p->tok.gen; }
    }
    d->next_idx = p->tok.index + 1;
    if (d->kind == 1) {
        int64_t t; int r = mvav_dec_video(d->av, d->out[s].data, d->w, d->h, &t);
        if (r < 0) die("movie 剪口：%s 视频解码出错", d->path);
        if (!r) return -1;
        if (d->need_base) {   // 本解码器第一帧 ⇒ 1 µs；之后定位回开头（循环）⇒ 首帧 0；定位到中间 ⇒ 原时刻 | this decoder's first frame => 1 us; later seek back to the start (loop) => first frame 0; seek into the middle => as is
            d->base = !d->decoded_any ? t - 1 : p->tok.seek_us == 0 ? t : 0; d->need_base = 0;
        }
        d->decoded_any = 1;
        *pts = t - d->base;
        return d->w * d->h + (d->w / 2) * (d->h / 2) * 2;
    }
    int n = p->tok.frames, maxn = (int)(d->out_cap / (d->ch * 2));
    if (n < 1) n = 1;
    if (n > maxn) n = maxn;
    if (mvav_dec_audio(d->av, (int16_t *)d->out[s].data, n) < 0) die("movie 剪口：%s 音频解码出错", d->path);
    *pts = p->pts;
    return n * d->ch * 2;
}
static int free_slot(const mvd_t *d) { for (int i = 0; i < d->nout; i++) if (d->out[i].st == OUT_FREE) return i; return -1; }
static void worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        mvd_t *d; int s = -1;
        for (d = g_decs; d; d = d->link) if (d->started && !d->stopped && !d->busy && d->npend && (s = free_slot(d)) >= 0) break;
        if (!d) { pthread_cond_wait(&g_work, &g_lock); continue; }
        mvpend p = d->pend[0]; memmove(d->pend, d->pend + 1, --d->npend * sizeof *d->pend);
        if (p.flags & FLAG_EOS) {                           // 流尾项：输入不再归还（编解码器停在流尾态，直到 Flush / 重新 Start） | end-of-stream item: the input is not returned (the codec stays in end-of-stream state until Flush / a new Start)
            d->out[s] = (typeof(d->out[s])){OUT_READY, 0, FLAG_EOS, d->last_pts, d->out[s].data};
            d->ready[d->nready++] = s;
            MVLOG("%s 解码器交流尾项", d->kind == 1 ? "视频" : "音频");
            signal_ev(d, 1); continue;
        }
        if (!p.valid || p.tok.track != d->track) {          // 别的轨的样本（循环时 SeekTo 后的 Read 可能交错到别的轨）：硬件解码器同样不出东西 | another track's sample (after SeekTo in a loop, Read may interleave into the other track): a hardware decoder yields nothing either
            if (!d->warned_foreign++) LOG("movie：%s 解码器收到不属于它的样本（轨 %d 样本 %d），丢弃", d->kind == 1 ? "视频" : "音频", p.tok.track, p.tok.index);
            d->in_free[d->n_in_free++] = p.idx; signal_ev(d, 0); continue;
        }
        d->busy = 1; d->out[s].st = OUT_BUSY;
        pthread_mutex_unlock(&g_lock);
        int64_t pts = 0; int size = decode_one(d, &p, s, &pts);
        pthread_mutex_lock(&g_lock);
        d->busy = 0; pthread_cond_broadcast(&g_idle);
        if (d->stopped) { d->out[s].st = OUT_FREE; continue; }   // Stop / 删除中：不再发事件 | being stopped / deleted: no more events
        if (size >= 0) {
            d->out[s].st = OUT_READY; d->out[s].size = size; d->out[s].flags = 0; d->out[s].pts = pts; d->last_pts = pts;
            d->ready[d->nready++] = s; signal_ev(d, 1);
            if (d->n_dec++ % 120 == 0)
                MVLOG("%s 解码器：已解 %u（样本 %d，pts %lld µs），游戏取走 %u / 取数据 %u / 归还 %u", d->kind == 1 ? "视频" : "音频", d->n_dec, p.tok.index, (long long)pts, d->n_acq, d->n_get, d->n_rel);
        } else d->out[s].st = OUT_FREE;
        d->in_free[d->n_in_free++] = p.idx; signal_ev(d, 0);
    }
}

static void dec_delete(void *self) {
    mvd_t *d = ((mvd_t **)self)[1];
    if (d) {
        pthread_mutex_lock(&g_lock);
        d->stopped = 1; wait_idle(d);
        for (mvd_t **pp = &g_decs; *pp; pp = &(*pp)->link) if (*pp == d) { *pp = d->link; break; }
        pthread_mutex_unlock(&g_lock);
        mvav_dec_close(d->av);
        for (int i = 0; i < d->nout; i++) free(d->out[i].data);
        free(d);
    }
    guest_delete(self);
}
static int dec_Configure(void *self, void **md) {
    mvd_t *d = D(self);
    const md_ent *p = md_find(md, "dm-path", MD_STR), *t = md_find(md, "dm-track", MD_I32);
    if (!p || !t) die("movie 剪口：Configure 的 MediaData 不是本剪口的轨配置（缺 dm-path / dm-track）");
    strlcpy(d->path, p->s, sizeof d->path); d->track = (int)t->i;
    const md_ent *a = md_find(md, d->kind == 1 ? "width" : "sample-rate", MD_I32), *b = md_find(md, d->kind == 1 ? "height" : "channel-count", MD_I32);
    if (!a || !b || a->i <= 0 || b->i <= 0) die("movie 剪口：Configure 缺%s", d->kind == 1 ? "宽高" : "采样率 / 声道数");
    if (d->kind == 1) { d->w = (int)a->i; d->h = (int)b->i; d->out_cap = (size_t)d->w * d->h + (size_t)(d->w / 2) * (d->h / 2) * 2; }
    else { d->rate = (int)a->i; d->ch = (int)b->i; d->out_cap = (size_t)d->ch << 12; }
    char err[256];
    if (!(d->av = mvav_dec_open(d->path, d->track, err, sizeof err))) die("movie 剪口：%s 轨 %d 开解码器失败：%s", d->path, d->track, err);
    for (int i = 0; i < d->nout; i++) d->out[i].data = malloc(d->out_cap);
    d->gen = -1; d->next_idx = -1; d->configured = 1;
    pthread_mutex_lock(&g_lock); dec_reset_bufs(d); pthread_mutex_unlock(&g_lock);
    MVLOG("%s 解码器 Configure：轨 %d %s", d->kind == 1 ? "视频" : "音频", d->track, d->path);
    return ST_OK;
}
static int dec_RegisterClientEvents(void *self, void **ev) { mvd_t *d = D(self); d->ev = ev; return ST_OK; }
static int dec_GetOutputFormat(void *self, void **md) {
    mvd_t *d = D(self);
    md_clear(md);
    if (d->kind == 1) {
        md_str(md, "mime", "video/raw"); md_i32(md, "width", d->w); md_i32(md, "height", d->h); md_i32(md, "stride", d->w);
        md_i32(md, "slice-height", d->h); md_rect(md, "crop", 0, 0, d->w - 1, d->h - 1);
    } else { md_str(md, "mime", "audio/raw"); md_i32(md, "sample-rate", d->rate); md_i32(md, "channel-count", d->ch); }
    return ST_OK;
}
static int dec_Start(void *self) {
    mvd_t *d = D(self);
    if (!d->configured) die("movie 剪口：Decoder 未 Configure 就 Start");
    if (!g_signal_event && !(g_signal_event = guest_sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE"))) die("movie 剪口：缺 nn::os::SignalEvent");
    worker_ensure();
    pthread_mutex_lock(&g_lock);
    if (d->stopped) { wait_idle(d); dec_reset_bufs(d); d->next_idx = -1; }
    int listed = 0; for (mvd_t *q = g_decs; q; q = q->link) listed |= q == d;
    if (!listed) { d->link = g_decs; g_decs = d; }
    d->started = 1; d->stopped = 0;
    signal_ev(d, 0);                                        // 全部输入空着：在游戏线程上直接发「输入可用」 | all inputs are free: signal "input available" right here on the game thread
    pthread_mutex_unlock(&g_lock);
    MVLOG("%s 解码器 Start", d->kind == 1 ? "视频" : "音频");
    return ST_OK;
}
static int dec_Stop(void *self) {
    mvd_t *d = D(self);
    pthread_mutex_lock(&g_lock); d->stopped = 1; wait_idle(d); pthread_mutex_unlock(&g_lock);
    MVLOG("%s 解码器 Stop", d->kind == 1 ? "视频" : "音频");
    return ST_OK;
}
static int dec_Flush(void *self) {
    mvd_t *d = D(self);
    pthread_mutex_lock(&g_lock); wait_idle(d); dec_reset_bufs(d); d->next_idx = -1;
    if (d->started && !d->stopped) signal_ev(d, 0);
    pthread_mutex_unlock(&g_lock);
    MVLOG("%s 解码器 Flush", d->kind == 1 ? "视频" : "音频");
    return ST_OK;
}
static int dec_AcquireInputBufferIndex(void *self, int *idx, int *remaining) {
    mvd_t *d = D(self); int r = ST_TRY_AGAIN;
    pthread_mutex_lock(&g_lock);
    if (d->started && !d->stopped && d->n_in_free) {
        *idx = d->in_free[0]; memmove(d->in_free, d->in_free + 1, --d->n_in_free * sizeof *d->in_free);
        *remaining = d->n_in_free; r = ST_OK;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}
static int dec_GetInputBuffer(void *self, int idx, mvbuf *b) {
    mvd_t *d = D(self);
    if (idx < 0 || idx >= d->nin) die("movie 剪口：GetInputBuffer 索引 %d 越界", idx);
    b->data = d->inbuf[idx]; b->cap = sizeof d->inbuf[idx]; b->off = b->size = 0;
    return ST_OK;
}
static int dec_SendInputBufferForDecode(void *self, int idx, int off, int size, int64_t pts, uint32_t flags) {
    mvd_t *d = D(self);
    if (idx < 0 || idx >= d->nin) die("movie 剪口：SendInputBufferForDecode 索引 %d 越界", idx);
    mvpend p = {idx, 0, {{0}}, pts, flags};
    if (off >= 0 && size >= (int)sizeof(mvtok) && off + size <= (int)sizeof d->inbuf[idx]) {
        memcpy(&p.tok, d->inbuf[idx] + off, sizeof p.tok); p.valid = !memcmp(p.tok.magic, "DMMV", 4);
    }
    pthread_mutex_lock(&g_lock);
    if (flags & FLAG_EOS) {
        if (d->eos_in) { pthread_mutex_unlock(&g_lock); return ST_OK; }   // 已在流尾态：吞掉 | already at end of stream: swallowed
        d->eos_in = 1; MVLOG("%s 解码器收到流尾", d->kind == 1 ? "视频" : "音频");
    }
    if (d->npend == MV_MAXBUF) die("movie 剪口：待解队列满（输入索引被重复提交？）");
    d->pend[d->npend++] = p;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
    return ST_OK;
}
static int dec_AcquireOutputBufferIndex(void *self, int *idx, int64_t *pts, uint32_t *flags, int *remaining) {
    mvd_t *d = D(self); int r = ST_TRY_AGAIN;
    pthread_mutex_lock(&g_lock);
    if (d->nready) {
        int s = d->ready[0]; memmove(d->ready, d->ready + 1, --d->nready * sizeof *d->ready);
        d->out[s].st = OUT_HELD; *idx = s; *pts = d->out[s].pts; *flags = d->out[s].flags; *remaining = d->nready; r = ST_OK; d->n_acq++;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}
static int dec_GetOutputBuffer(void *self, int idx, mvbuf *b) {
    mvd_t *d = D(self);
    if (idx < 0 || idx >= d->nout || d->out[idx].st != OUT_HELD) die("movie 剪口：GetOutputBuffer 索引 %d 不是游戏取走的输出", idx);
    int n = d->out[idx].size;                               // 游戏持有期间槽内容不变，拷贝不用锁 | the slot does not change while the game holds it, copying needs no lock
    if ((uint64_t)n > b->cap) die("movie 剪口：GetOutputBuffer 游戏缓冲 %llu 字节 < 帧 %d 字节", (unsigned long long)b->cap, n);
    memcpy(b->data, d->out[idx].data, n); b->off = 0; b->size = n; d->n_get++;
    return ST_OK;
}
static int dec_GetOutputBufferSize(void *self, int *size, int *align) {
    mvd_t *d = D(self);
    *size = (int)d->out_cap; *align = 1;                    // 原版第二个输出是解码器的状态值 1（游戏拿去当对齐） | in the original the second output is the decoder's state value 1 (the game uses it as an alignment)
    return ST_OK;
}
static int dec_ReleaseOutputBufferIndex(void *self, int idx) {
    mvd_t *d = D(self);
    pthread_mutex_lock(&g_lock);
    if (idx < 0 || idx >= d->nout || d->out[idx].st != OUT_HELD) die("movie 剪口：ReleaseOutputBufferIndex 索引 %d 不是游戏取走的输出", idx);
    d->out[idx].st = OUT_FREE; d->n_rel++; pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
    return ST_OK;
}
TRAP(dec, 0, "~Decoder（非删除）") TRAP(dec, 7, "Reset") TRAP(dec, 16, "GetLastError")
TRAP(dec, 17, "GetOutputBuffer(idx, Buffer*, MediaData*)") TRAP(dec, 18, "GetOutputBufferFormat")
static void *const g_dec_vt[19] = {
    mv_trap_dec_0, dec_delete, dec_Configure, dec_RegisterClientEvents, dec_GetOutputFormat, dec_Start, dec_Stop, mv_trap_dec_7,
    dec_Flush, dec_AcquireInputBufferIndex, dec_GetInputBuffer, dec_SendInputBufferForDecode, dec_AcquireOutputBufferIndex,
    dec_GetOutputBuffer, dec_GetOutputBufferSize, dec_ReleaseOutputBufferIndex, mv_trap_dec_16, mv_trap_dec_17, mv_trap_dec_18,
};
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "过场视频：Decoder 装宿主虚表（令牌 → AVFoundation 解码）",
          void, mv_Decoder, void **self, int type, int fmt, int mode, int prio) {
    (void)prio;
    int kind = type == 1 && fmt == 4 ? 1 : type == 4 && fmt == 1 ? 2 : 0;   // H.264 → NV12 / AAC → PCM16（Blade 只有这两种） | H.264 -> NV12 / AAC -> PCM16 (all Blade uses)
    if (!kind) die("movie 剪口：Decoder(类型 %d, 输出格式 %d, 模式 %d) 不支持（只接 H.264→NV12 / AAC→PCM16）", type, fmt, mode);
    mvd_t *d = calloc(1, sizeof *d); d->kind = kind;
    d->nin = d->nout = kind == 1 ? 8 : 16;
    self[0] = (void *)g_dec_vt; self[1] = d;
    MVLOG("Decoder(类型 %d, 输出格式 %d, 模式 %d)", type, fmt, mode);
}

#else   // ---------------- 暗黑 II：空播 | Diablo II: empty playback ----------------
// 过场 / 背景视频剪口（movie:: = subsdk0 任天堂 movie 库；视频内容在复刻排除范围内，不解码、不出画面）。
// 语义：每段视频按容器里的真实时长「空播」，时序与原版一致——一次性视频到时长走游戏自己的「播完」路径（按键跳过照常生效），
// 循环视频（选角背景 logoloop）由游戏自己 SeekTo 回开头、按原时长空转。排除的是内容，状态机不剪。
// 游戏侧协议（main 3.3.94012 原映像偏移，读码所得）：
//   播放器 P = [0x2e29b88]。Setup 0x8996d0：Ext 槽15 SetDataSource(读取器, 路径) → 槽4 GetTrackCount → Decoder 构造
//     → Dec 槽3 RegisterClientEvents(&P+0x1b8 = {输入事件, 输出事件, 种类3, 种类4}) → 槽2 Configure → 启动解码线程 → 槽5 Start；
//     Open 返回 (Start 状态 == 0)。
//   解码线程 0x899270：WaitAny；种类 1（输入可用）：Dec 槽9 AcquireInputBufferIndex → 槽10 GetInputBuffer → Ext 槽10 Read：
//     0x2000 = 流尾 ⇒ 循环（P+0x12）则 Ext 槽8 SeekTo(0)，否则置 P+0x488、标志 4；→ Ext 槽12 GetSampleTime
//     → Dec 槽11 SendInputBufferForDecode(idx, off, size, pts, 标志) → Ext 槽11 Advance。
//     种类 2（输出可用）：Dec 槽12 AcquireOutputBufferIndex(&idx,&pts,&标志,&剩余) 状态 0/0x2000 ⇒ 入帧队列。
//   每帧 0xbbfcf4 → 0x898230：帧队列空时返回 !P[+0x489]；取帧 0x89a4b0：Dec 槽18(idx, MediaData*) 非 0 且标志 4 且非循环
//     ⇒ P+0x489 = 1 ⇒ 游戏走正常播完 0xbbfb60。丢帧 0x899bc8 只在队列 ≥2 时（单个流尾项不会被丢）。
// 本剪口的时间线：Start 记 t0、挂定时 → 时长内不发任何事件（0 帧，游戏视为在播）→ 到时长由客体计时线程发「输入可用」
//   → Read 报 0x2000 →（循环）SeekTo 重挂定时 /（一次性）Send(标志 4) 时在解码线程上发「输出可用」→ Acquire 交出标志 4 项。
// 时钟：宿主单调时钟（不按帧）。未用到的虚槽一律报红陷阱（被调到即中止并报名），不靠桩返回 0 侥幸通过。
// Cutscene / background video cut (movie:: = the Nintendo movie library in subsdk0; video content is out of the reimplementation scope, not decoded, no picture output).
// Semantics: each video is "played empty" for the real duration in the container, with timing matching the original game -- a one-shot video takes the game's own "finished playing" path at its duration (skipping by key works as usual),
// a looping video (character select background logoloop) is SeekTo'd back to the start by the game itself and idles for the original duration. What is excluded is the content, the state machine is not cut.
// Game-side protocol (offsets in the original main 3.3.94012 image, obtained by reading the code):
//   Player P = [0x2e29b88]. Setup 0x8996d0: Ext slot 15 SetDataSource(reader, path) → slot 4 GetTrackCount → Decoder construction
//     → Dec slot 3 RegisterClientEvents(&P+0x1b8 = {input event, output event, kind 3, kind 4}) → slot 2 Configure → start the decode thread → slot 5 Start;
//     Open returns (Start status == 0).
//   Decode thread 0x899270: WaitAny; kind 1 (input available): Dec slot 9 AcquireInputBufferIndex → slot 10 GetInputBuffer → Ext slot 10 Read:
//     0x2000 = end of stream ⇒ if looping (P+0x12) then Ext slot 8 SeekTo(0), otherwise set P+0x488 and flag 4; → Ext slot 12 GetSampleTime
//     → Dec slot 11 SendInputBufferForDecode(idx, off, size, pts, flags) → Ext slot 11 Advance.
//     Kind 2 (output available): Dec slot 12 AcquireOutputBufferIndex(&idx,&pts,&flags,&remaining) status 0/0x2000 ⇒ enqueue into the frame queue.
//   Every frame 0xbbfcf4 → 0x898230: returns !P[+0x489] when the frame queue is empty; frame fetch 0x89a4b0: Dec slot 18(idx, MediaData*) non-zero and flag 4 and not looping
//     ⇒ P+0x489 = 1 ⇒ the game takes the normal finished-playing path 0xbbfb60. Frame drop 0x899bc8 only when the queue is ≥2 (a single end-of-stream item is not dropped).
// This cut's timeline: Start records t0 and arms the timer → no event is sent within the duration (0 frames, the game considers it playing) → at the duration the guest timer thread sends "input available"
//   → Read reports 0x2000 → (looping) SeekTo re-arms the timer / (one-shot) at Send(flag 4) "output available" is sent on the decode thread → Acquire hands over the flag 4 item.
// Clock: host monotonic clock (not per frame). Unused virtual slots are all failure traps (abort and report the name when called), not stubs returning 0 that pass by luck.
#define MV_SKIP_NS   100000000ll   // 不带视频数据时每段「播放」的时长：0.1 秒 | duration of each "playback" when there is no video data: 0.1 s
static int64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ll + t.tv_nsec; }
#define MVLOG(fmt, ...) do { if (g_flag.movie) LOG("movie 空播 [%.3f] " fmt, (now_ns() - g_t0_ns) / 1e9, ##__VA_ARGS__); } while (0)

// ---------- WebM 时长：EBML Segment/Info 的 Duration × TimecodeScale ----------
// rd(ctx, 偏移, 缓冲, 长度) 返回读到字节数。读不到 / 没有 Duration 返回 -1（调用方报红，不给缺省值）。
// ---------- WebM duration: Duration × TimecodeScale of EBML Segment/Info ----------
// rd(ctx, offset, buffer, length) returns the number of bytes read. Returns -1 if it cannot be read / there is no Duration (the caller reports a failure, no default value is given).
typedef int64_t (*mv_read_fn)(void *ctx, int64_t off, void *buf, size_t n);
static int ebml_vint(const uint8_t *p, size_t n, int keep_marker, uint64_t *v) {   // 返回长度，0 = 非法 | returns the length, 0 = invalid
    if (!n || !p[0]) return 0;
    int len = __builtin_clz((unsigned)p[0]) - 23;          // 首字节前导 0 个数 + 1 | number of leading 0s in the first byte + 1
    if (len < 1 || len > 8 || (size_t)len > n) return 0;
    uint64_t x = keep_marker ? p[0] : (p[0] & (0xFF >> len));
    for (int i = 1; i < len; i++) x = (x << 8) | p[i];
    *v = x; return len;
}
int64_t mv_webm_duration_ns(mv_read_fn rd, void *ctx) {
    uint8_t b[64]; uint64_t id, sz; int a, c;
    int64_t off = 0, seg_end = -1;
    for (int depth = 0; depth < 2;) {                       // depth0：顶层找 Segment；depth1：Segment 内找 Info | depth0: find Segment at top level; depth1: find Info inside Segment
        int64_t got = rd(ctx, off, b, sizeof b);
        if (got < 2 || !(a = ebml_vint(b, got, 1, &id)) || !(c = ebml_vint(b + a, got - a, 0, &sz))) return -1;
        int unknown = sz == (1ull << (7 * c)) - 1;          // 全 1 = 未知长度 | all 1s = unknown length
        int64_t body = off + a + c;
        if (depth == 0 && id == 0x18538067) { seg_end = unknown ? INT64_MAX : body + (int64_t)sz; off = body; depth = 1; continue; }
        if (depth == 1 && id == 0x1549A966) {               // Info
            if (unknown || sz > 4096) return -1;
            uint8_t info[4096]; if (rd(ctx, body, info, sz) != (int64_t)sz) return -1;
            uint64_t scale = 1000000; double dur = -1;      // TimecodeScale 缺省 1ms（Matroska 规范缺省值） | TimecodeScale defaults to 1ms (Matroska specification default)
            for (size_t p = 0; p < sz;) {
                uint64_t eid, esz; int ea = ebml_vint(info + p, sz - p, 1, &eid); if (!ea) return -1;
                int ec = ebml_vint(info + p + ea, sz - p - ea, 0, &esz); if (!ec || p + ea + ec + esz > sz) return -1;
                const uint8_t *e = info + p + ea + ec;
                if (eid == 0x2AD7B1) { scale = 0; for (uint64_t i = 0; i < esz; i++) scale = (scale << 8) | e[i]; }
                else if (eid == 0x4489 && (esz == 4 || esz == 8)) {
                    uint64_t r = 0; for (uint64_t i = 0; i < esz; i++) r = (r << 8) | e[i];
                    if (esz == 4) { uint32_t r32 = (uint32_t)r; float f; memcpy(&f, &r32, 4); dur = f; } else memcpy(&dur, &r, 8);
                }
                p += ea + ec + esz;
            }
            return dur > 0 && scale ? (int64_t)(dur * (double)scale + 0.5) : -1;
        }
        if (depth == 1 && id == 0x1F43B675) return -1;      // 到 Cluster 还没见到 Info | reached Cluster without having seen Info
        if (unknown) return -1;
        off = body + (int64_t)sz;
        if (depth == 1 && off >= seg_end) return -1;
    }
    return -1;
}

// ---------- 播放状态（同一时刻游戏只有一个播放器 P；按对象登记，交接靠「最近一次取源」）----------
// ---------- Playback state (the game has only one player P at a time; registered per object, handover relies on "the most recent source fetch") ----------
typedef struct {
    int64_t dur_ns, deadline;         // deadline：下一次发「输入可用」的单调时刻（0 = 未挂） | deadline: monotonic time of the next "input available" (0 = not armed)
    int64_t fired;                    // 上一次到点的截止时刻：循环按媒体时间续接（新截止 = 上次截止 + 时长），不累积唤醒延迟 | deadline of the previous firing: loops continue in media time (new deadline = previous deadline + duration), wake-up delay does not accumulate
    void *ev_in, *ev_out;             // 游戏的 nn::os::EventType*（DecoderEvents[0] / [1]） | the game's nn::os::EventType* (DecoderEvents[0] / [1])
    int eos_out;                      // 已收到标志 4 输入，待交出流尾输出项 | flag 4 input received, end-of-stream output item still to be handed over
    int64_t last_pts_us;
} mv_t;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static mv_t *g_pending;               // 最近一次 SetDataSource 解出的状态，等 Decoder 构造时认领 | state produced by the most recent SetDataSource, waiting to be claimed when the Decoder is constructed
static mv_t *g_armed;                 // 计时线程盯着的那一个 | the one the timer thread is watching
static void (*g_signal_event)(void *);

// 客体计时线程：经 SDK nn::os::CreateThread 建（入口是本函数），这样 SignalEvent 是在正规客体线程上调的
// Guest timer thread: created through the SDK's nn::os::CreateThread (entry is this function), so that SignalEvent is called on a proper guest thread
static void timer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        mv_t *m = g_armed;
        if (!m || !m->deadline) { pthread_cond_wait(&g_cv, &g_lock); continue; }
        int64_t now = mono_ns();
        if (now < m->deadline) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            int64_t t = ts.tv_sec * 1000000000ll + ts.tv_nsec + (m->deadline - now);
            ts.tv_sec = t / 1000000000; ts.tv_nsec = t % 1000000000;
            pthread_cond_timedwait(&g_cv, &g_lock, &ts); continue;
        }
        g_armed = NULL; m->fired = m->deadline;           // 一次定时只发一次；持锁发，和 Stop / 删除串行 | one timer fires only once; fired with the lock held, serialized with Stop / delete
        MVLOG("到时长，发输入可用");
        g_signal_event(m->ev_in);
    }
}
static void timer_ensure(void) {                          // 在客体线程上调（Decoder::Start 里） | called on a guest thread (inside Decoder::Start)
    static int started; if (started) return; started = 1;
    static _Alignas(16) uint8_t thread_obj[0x400];
    uint32_t (*create)(void *, void (*)(void *), void *, void *, size_t, int) =
        guest_sym("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    void (*start)(void *) = guest_sym("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    g_signal_event = guest_sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!create || !start || !g_signal_event) die("movie 剪口：缺 nn::os 线程 / 事件符号");
    size_t ss = 0x10000; void *stack = aligned_alloc(0x1000, ss);
    uint32_t r = create(thread_obj, timer_main, NULL, stack, ss, NN_OS_DEFAULT_THREAD_PRIORITY);
    if (r) die("movie 剪口：建计时线程失败 %#x", r);
    start(thread_obj);
}
static void arm(mv_t *m, int64_t at) {                    // 持锁调用 | called with the lock held
    m->deadline = at; g_armed = m; pthread_cond_signal(&g_cv);
}

// SetAllocator 登记的客体分配器：库对象由游戏用同一分配器申请后原地构造，删除析构须用登记的 free 还回去
// Guest allocator registered by SetAllocator: library objects are allocated by the game with the same allocator and constructed in place, the deleting destructor must return them with the registered free
static void (*g_free)(void *, void *); static void *g_user;
HLE_FORCE("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", "过场空播：登记客体分配器（删除析构用其 free）",
          void, mv_SetAllocator, void *alloc, void (*fr)(void *, void *), void *realloc_, void *user) {
    (void)alloc; (void)realloc_; g_free = fr; g_user = user;
}

// ---- 虚表：对象首字 = 虚表地址点，次字 = 本剪口的 mv_t*（游戏为库对象分配的空间远大于 16 字节）----
// ---- Vtable: first word of the object = vtable address point, second word = this cut's mv_t* (the space the game allocates for library objects is far larger than 16 bytes) ----
#undef TRAP
#define TRAP(cls, slot, what) static void mv_trap_##cls##_##slot(void) { die("movie 剪口：%s 槽%d %s 被调用（不在空播协议内）", #cls, slot, what); }
static mv_t *M(void *self) { return ((mv_t **)self)[1]; }
static void mv_dtor(void *self) { (void)self; }
static void mv_delete(void *self) {
    pthread_mutex_lock(&g_lock);
    mv_t *m = M(self);
    if (m && g_armed == m) g_armed = NULL;
    if (m && g_pending == m) g_pending = NULL;
    pthread_mutex_unlock(&g_lock);
    if (!g_free) die("movie 剪口：未登记分配器");
    g_free(self, g_user);
}

// ---- 游戏的 ClientStreamReader（main 虚表 0x20b0e90）：槽2 bool Open(路径, void**h)、槽3 ReadAt(h,off,buf,n)、槽4 Close(h) ----
// ---- The game's ClientStreamReader (main vtable 0x20b0e90): slot 2 bool Open(path, void**h), slot 3 ReadAt(h,off,buf,n), slot 4 Close(h) ----
typedef struct { void **reader; void *h; } rdctx;
static int64_t rd_reader(void *c_, int64_t off, void *buf, size_t n) {
    rdctx *c = c_; void **vt = *(void ***)c->reader;
    return ((int64_t (*)(void *, void *, int64_t, void *, size_t))vt[3])(c->reader, c->h, off, buf, n);
}

// Extractor（16 槽）
// Extractor (16 slots)
static int ext_SetDataSourceReader(void *self, void **reader, const char *path) {
    // 过场 / 背景视频是产品排除项（过场动画不接，游戏自然跳过）：资源包里不带视频数据，这里不打开文件、不读时长，
    // 一律按 MV_SKIP_NS 即「播完」——一次性视频走游戏自己的播完路径，循环视频（选角背景 logoloop）按这个周期空转。
    // DM_MOVIE_REAL=1：按容器里的真实时长空播（旧行为；对真值时序用，需要带视频数据的资源包）。
    // Cutscene / background videos are out of scope for this product (cutscenes are not hooked up, the game skips them naturally): the resource pack carries no video data, here no file is opened and no duration is read,
    // always MV_SKIP_NS and then "finished playing" -- one-shot videos take the game's own finished-playing path, looping videos (character select background logoloop) idle at this period.
    // DM_MOVIE_REAL=1: no-op playback by the real duration in the container (old behavior; for timing against ground truth, needs a resource pack with video data).
    (void)reader; int64_t d = MV_SKIP_NS;
    static int real = -1; if (real < 0) { const char *e = getenv("DM_MOVIE_REAL"); real = e && !strcmp(e, "1"); }
    if (real) {
        void **vt = *(void ***)reader; rdctx c = {reader, NULL};
        if (!(((uint32_t (*)(void *, const char *, void **))vt[2])(reader, path, &c.h) & 1)) die("movie 剪口：读取器打不开 %s", path);
        d = mv_webm_duration_ns(rd_reader, &c);
        ((void (*)(void *, void *))vt[4])(reader, c.h);
        if (d <= 0) die("movie 剪口：%s 读不出 WebM 时长（Segment/Info/Duration）", path);
    }
    mv_t *m = calloc(1, sizeof *m); m->dur_ns = d;
    pthread_mutex_lock(&g_lock); ((mv_t **)self)[1] = m; g_pending = m; pthread_mutex_unlock(&g_lock);
    MVLOG("%s 时长 %.3f 秒", path, d / 1e9);
    return ST_OK;
}
static int ext_GetTrackCount(void *self, int *n) { (void)self; *n = 0; return ST_OK; }   // 0 轨 ⇒ Setup 跳过选轨 | 0 tracks ⇒ Setup skips track selection
static int ext_SeekTo(void *self, int64_t us, int mode) {   // 循环视频回开头：重新计一轮时长 | looping video back to the start: count one more round of the duration
    (void)us; (void)mode; mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); arm(m, (m->fired ? m->fired : mono_ns()) + m->dur_ns); m->last_pts_us = 0; pthread_mutex_unlock(&g_lock);
    MVLOG("循环回开头");
    return ST_OK;
}
static int ext_Read(void *self, void *buf) {
    (void)buf; mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); int64_t dl = m->deadline; pthread_mutex_unlock(&g_lock);
    if (dl && mono_ns() < dl) die("movie 剪口：未到时长就被要求读样本（协议外）");
    m->last_pts_us = m->dur_ns / 1000;
    return ST_EOS;
}
static int ext_Advance(void *self) { (void)self; return ST_OK; }
static int ext_GetSampleTime(void *self, int64_t *us) { *us = M(self)->last_pts_us; return ST_OK; }
// UE4（Blade II）的播放器走槽 2 SetDataSource(路径)：没有读取器可读时长，一律按 MV_SKIP_NS 空播
// UE4's (Blade II) player uses slot 2 SetDataSource(path): no reader to read the duration from, always play empty for MV_SKIP_NS
static int ext_SetDataSourcePath(void *self, const char *path) {
    // 先按「打不开」答（非 0 状态）：UE4 的播放器会记一条日志然后跳过这段视频，不走它后面依赖解码器对象的路径
    // （按 ST_OK 走时它读了 0 轨后仍去碰解码器成员 → main+0x3a29f94 空指针写）。要真空播再把 Extractor / Decoder 的 UE4 调法补全
    // Answer "cannot open" first (non-zero status): UE4's player logs a line and skips the movie instead of taking the later path that depends on the decoder object
    // (with ST_OK it read 0 tracks and still touched decoder members → null pointer write at main+0x3a29f94). To really play empty, complete UE4's use of Extractor / Decoder
    (void)self; LOG("movie：UE4 按路径打开 %s → 答打不开（跳过视频）", path ? path : "(null)");
    return 1;
}
TRAP(ext, 3, "SetDataSource(void const*, int)") TRAP(ext, 5, "GetTrackConfiguration")
TRAP(ext, 6, "SelectTrack") TRAP(ext, 7, "UnselectTrack") TRAP(ext, 9, "GetTrackIndexForAvailableData")
TRAP(ext, 13, "GetSampleMetaData") TRAP(ext, 14, "GetBufferedRange")
static void *const g_ext_vt[16] = {
    mv_dtor, mv_delete, ext_SetDataSourcePath, mv_trap_ext_3, ext_GetTrackCount, mv_trap_ext_5, mv_trap_ext_6, mv_trap_ext_7,
    ext_SeekTo, mv_trap_ext_9, ext_Read, ext_Advance, ext_GetSampleTime, mv_trap_ext_13, mv_trap_ext_14,
    ext_SetDataSourceReader,
};

// Decoder（19 槽）
// Decoder (19 slots)
static int dec_Configure(void *self, void *md) { (void)self; (void)md; return ST_OK; }
static int dec_RegisterClientEvents(void *self, void **ev) { mv_t *m = M(self); m->ev_in = ev[0]; m->ev_out = ev[1]; return ST_OK; }
static int dec_Start(void *self) {
    timer_ensure();
    MVLOG("Start");
    mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); arm(m, mono_ns() + m->dur_ns); pthread_mutex_unlock(&g_lock);
    return ST_OK;
}
static int dec_Stop(void *self) {                          // 按键跳过 / 收尾：立即撤销定时 | skipped by key / wrap-up: cancel the timer immediately
    mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); m->deadline = 0; if (g_armed == m) g_armed = NULL; pthread_mutex_unlock(&g_lock);
    return ST_OK;
}
static int dec_AcquireInputBufferIndex(void *self, int *idx, int *remaining) {
    mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); int due = m->deadline && mono_ns() >= m->deadline; pthread_mutex_unlock(&g_lock);
    *idx = due ? 0 : -1; *remaining = 0;
    MVLOG("取输入缓冲 %s", due ? "到时" : "未到");
    return ST_OK;
}
static uint8_t g_inbuf[16];
static int dec_GetInputBuffer(void *self, int idx, mvbuf *b) {
    (void)self; (void)idx; b->data = g_inbuf; b->cap = sizeof g_inbuf; b->off = b->size = 0; return ST_OK;
}
static int dec_SendInputBufferForDecode(void *self, int idx, int off, int size, int64_t pts, int flags) {
    (void)idx; (void)off; (void)size; (void)pts; mv_t *m = M(self);
    if (flags & FLAG_EOS) {                                 // 一次性视频播到头：在解码线程上直接发「输出可用」 | a one-shot video played to the end: send "output available" directly on the decode thread
        pthread_mutex_lock(&g_lock); m->eos_out = 1; m->deadline = 0; pthread_mutex_unlock(&g_lock);
        MVLOG("播完");
        g_signal_event(m->ev_out);
    }
    return ST_OK;
}
static int dec_AcquireOutputBufferIndex(void *self, int *idx, int64_t *pts, int *flags, int *remaining) {
    mv_t *m = M(self); *remaining = 0;
    pthread_mutex_lock(&g_lock); int eos = m->eos_out; m->eos_out = 0; pthread_mutex_unlock(&g_lock);
    if (!eos) return ST_TRY_AGAIN;                          // 无输出 ⇒ 游戏不入队 | no output ⇒ the game does not enqueue
    *idx = 0; *pts = m->dur_ns / 1000; *flags = FLAG_EOS;
    return ST_OK;
}
static int dec_ReleaseOutputBufferIndex(void *self, int idx) { (void)self; (void)idx; return ST_OK; }
static int dec_GetOutputBufferFormat(void *self, int idx, void *md) { (void)self; (void)idx; (void)md; return ST_EOS; }   // 非 0 ⇒ 取帧按流尾项处理 | non-zero ⇒ frame fetch treats it as the end-of-stream item
TRAP(dec, 4, "GetOutputFormat") TRAP(dec, 7, "Reset") TRAP(dec, 8, "Flush")
TRAP(dec, 13, "GetOutputBuffer") TRAP(dec, 14, "GetOutputBufferSize")
TRAP(dec, 16, "GetLastError") TRAP(dec, 17, "GetOutputBuffer(MediaData)")
static void *const g_dec_vt[19] = {
    mv_dtor, mv_delete, dec_Configure, dec_RegisterClientEvents, mv_trap_dec_4, dec_Start, dec_Stop, mv_trap_dec_7,
    mv_trap_dec_8, dec_AcquireInputBufferIndex, dec_GetInputBuffer, dec_SendInputBufferForDecode, dec_AcquireOutputBufferIndex,
    mv_trap_dec_13, mv_trap_dec_14, dec_ReleaseOutputBufferIndex, mv_trap_dec_16, mv_trap_dec_17, dec_GetOutputBufferFormat,
};

HLE_FORCE("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", "过场空播：Extractor 装空播虚表（取源时读 WebM 时长）",
          void, mv_Extractor, void **self, int type) { (void)type; self[0] = (void *)g_ext_vt; self[1] = NULL; }
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "过场空播：Decoder 装空播虚表（按时长发流尾）",
          void, mv_Decoder, void **self, int type, int fmt, int mode, int prio) {
    (void)type; (void)fmt; (void)mode; (void)prio;
    pthread_mutex_lock(&g_lock); mv_t *m = g_pending; g_pending = NULL; pthread_mutex_unlock(&g_lock);
    if (!m) die("movie 剪口：Decoder 构造前没有取过源");
    self[0] = (void *)g_dec_vt; self[1] = m;
}
HLE_FORCE("_ZN5movie17GetThreadPriorityEv", "过场空播：只作 Decoder 构造参数", int, mv_GetThreadPriority, void) { return NN_OS_DEFAULT_THREAD_PRIORITY; }

// MediaData：Setup / 取帧在栈上构造析构（栈槽 8 字节）；Find* 只在选轨与真帧路径里（空播不可达）
// MediaData: constructed and destructed on the stack by Setup / frame fetch (stack slot 8 bytes); Find* is only on the track selection and real frame paths (unreachable in no-op playback)
HLE_FORCE("_ZN5movie9MediaDataC1Ev", "过场空播：空 MediaData", void, mv_MediaData, void **self) { *self = 0; }
HLE_FORCE("_ZN5movie9MediaDataD1Ev", "过场空播：空 MediaData", void, mv_MediaDataD, void **self) { (void)self; }
#define TRAPFN(mangled, cname, what) \
    HLE_FORCE(mangled, "过场空播：协议外（" what "）", void, cname, void) { die("movie 剪口：%s 被调用（不在空播协议内）", what); }
TRAPFN("_ZNK5movie9MediaData9FindInt32EPKcPi", mv_FindInt32, "MediaData::FindInt32")
TRAPFN("_ZNK5movie9MediaData9FindInt64EPKcPl", mv_FindInt64, "MediaData::FindInt64")
TRAPFN("_ZNK5movie9MediaData10FindStringEPKcPS2_", mv_FindString, "MediaData::FindString")
#endif

// Buffer：解码线程与取帧在游戏栈上构造（每处 ≥0x40 字节），宿主布局 mvbuf
// Buffer: constructed on the game stack by the decode thread and frame fetch (≥0x40 bytes at each site), host layout mvbuf
HLE_FORCE("_ZN5movie6BufferC1Ev", "过场空播：空 Buffer", void, mv_Buffer, mvbuf *b) { memset(b, 0, sizeof *b); }
HLE_FORCE("_ZN5movie6BufferD1Ev", "过场空播：Buffer 无资源", void, mv_BufferD, mvbuf *b) { (void)b; }
HLE_FORCE("_ZNK5movie6Buffer4DataEv", "过场空播：Buffer 数据指针", void *, mv_BufData, const mvbuf *b) { return b->data; }
HLE_FORCE("_ZNK5movie6Buffer6OffsetEv", "过场空播：Buffer 偏移", int, mv_BufOffset, const mvbuf *b) { return b->off; }
HLE_FORCE("_ZNK5movie6Buffer4SizeEv", "过场空播：Buffer 长度", int, mv_BufSize, const mvbuf *b) { return b->size; }
HLE_FORCE("_ZN5movie6Buffer8SetRangeEii", "过场空播：Buffer 设范围", void, mv_BufSetRange, mvbuf *b, int off, int size) { b->off = off; b->size = size; }
HLE_FORCE("_ZN5movie6Buffer18SetDataAndCapacityEPvm", "过场空播：Buffer 设数据", void, mv_BufSetData, mvbuf *b, void *p, uint64_t cap) { b->data = p; b->cap = cap; b->off = b->size = 0; }
