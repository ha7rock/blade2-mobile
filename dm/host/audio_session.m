// 通用宿主的音频会话（iOS：AVAudioSession；Mac：没有会话概念，全部空实现）。hle_blade.c 的音频渲染器在建 RemoteIO 单元前调 dm_audio_session_begin：
//   类别 Playback（不与别的 App 混音——原版 Switch 游戏独占声音；静音开关不再替我们静音，靠下面的生命周期 / 打断停声），激活会话；
//   打断（来电 / Siri / 闹钟）：开始 → 暂停渲染器的声卡单元（dm_audren_suspend 1，期间节拍线程按实时静音消耗，节奏不断）；结束且 shouldResume → 重新激活会话、恢复单元；
//   线路变化（拔耳机 / 接蓝牙）：只记日志，RemoteIO 自己跟着新线路走；媒体服务重置（mediaserverd 重启）：重新设类别与激活。
// 进后台 / 回前台（iOS 壳 front_blade_focus 调 dm_audio_session_background）与打断合成一个「该不该出声」：后台中打断结束不去启动声卡；回前台时打断还没结束也不启动。
// Audio session for the generic host (iOS: AVAudioSession; Mac: no session concept, all no-ops). The audio renderer in hle_blade.c calls dm_audio_session_begin before creating the RemoteIO unit:
//   category Playback (no mixing with other apps -- the original Switch game owns the sound; the mute switch no longer mutes for us, stopping relies on the lifecycle / interruption handling below), activate the session;
//   interruptions (call / Siri / alarm): began -> pause the renderer's audio unit (dm_audren_suspend 1, meanwhile the tick thread consumes silently in real time so pacing continues); ended with shouldResume -> re-activate the session, resume the unit;
//   route changes (headphones unplugged / Bluetooth attached): log only, RemoteIO follows the new route itself; media services reset (mediaserverd restart): set the category and activate again.
// Background / foreground (the iOS shell's front_blade_focus calls dm_audio_session_background) and interruptions combine into one "should sound play": an interruption ending while in the background does not start the device; returning to the foreground with the interruption still active does not either.
#include <TargetConditionals.h>
#include "dm.h"
void dm_audren_suspend(int on);   // hle_blade.c：渲染器声卡单元停 / 起 | hle_blade.c: stop / start the renderer's audio unit

#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
static int g_bg, g_intr;
static void apply(void) { dm_audren_suspend(g_bg || g_intr); }
static int activate(const char *why) {
    AVAudioSession *as = AVAudioSession.sharedInstance; NSError *e = nil;
    if (![as setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&e]) { LOG("【红】音频会话：设类别失败（%s）%s", why, e.localizedDescription.UTF8String); return 0; }
    if (![as setActive:YES error:&e]) { LOG("【红】音频会话：激活失败（%s）%s", why, e.localizedDescription.UTF8String); return 0; }
    LOG("音频会话：Playback 已激活（%s），线路 %s，采样率 %.0f，缓冲 %.1f ms", why, as.currentRoute.outputs.firstObject.portType.UTF8String ?: "?", as.sampleRate, as.IOBufferDuration * 1000);
    return 1;
}
void dm_audio_session_begin(void) {
    static int once; if (once) return; once = 1;
    activate("启动");
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter; NSOperationQueue *mq = NSOperationQueue.mainQueue; AVAudioSession *as = AVAudioSession.sharedInstance;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:as queue:mq usingBlock:^(NSNotification *n) {
        NSUInteger t = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        if (t == AVAudioSessionInterruptionTypeBegan) { LOG("音频会话：被打断，渲染器暂停出声"); g_intr = 1; apply(); return; }
        NSUInteger opt = [n.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
        g_intr = 0;
        if (opt & AVAudioSessionInterruptionOptionShouldResume) { NSError *e = nil; if (![as setActive:YES error:&e]) LOG("【红】音频会话：打断后重新激活失败 %s", e.localizedDescription.UTF8String); }
        apply(); LOG("音频会话：打断结束%s%s", opt & AVAudioSessionInterruptionOptionShouldResume ? "" : "（系统不建议恢复，仍尝试）", g_bg ? "，仍在后台，回前台再出声" : "，恢复出声");
    }];
    [nc addObserverForName:AVAudioSessionRouteChangeNotification object:as queue:mq usingBlock:^(NSNotification *n) {
        NSUInteger r = [n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
        LOG("音频会话：线路变化（原因 %lu）→ %s", (unsigned long)r, as.currentRoute.outputs.firstObject.portType.UTF8String ?: "?");   // 2 = 旧设备不可用（拔耳机） | 2 = old device unavailable (headphones unplugged)
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:as queue:mq usingBlock:^(NSNotification *n) { (void)n; LOG("【红】音频会话：媒体服务被重置，重新激活"); activate("媒体服务重置"); apply(); }];
}
void dm_audio_session_background(int on) { if (g_bg == on) return; g_bg = on; apply(); }
#else
void dm_audio_session_begin(void) {}
void dm_audio_session_background(int on) { (void)on; }
#endif
