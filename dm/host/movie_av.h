// 过场视频的宿主解复用 / 解码（AVFoundation，movie_av.m）：hle_movie.c 的 Blade（UE4）分支用。
// 只用 iOS 也有的 API（AVAssetReader），不碰 AVSampleCursor 这类仅 macOS 的接口。
// Host demux / decode for cutscene videos (AVFoundation, movie_av.m): used by the Blade (UE4) branch of hle_movie.c.
// Uses only APIs that iOS also has (AVAssetReader), nothing macOS-only such as AVSampleCursor.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int kind;                         // 1 视频 / 2 音频 | 1 video / 2 audio
    char mime[32];                    // "video/avc" / "audio/mp4a-latm"（其它编码按 "video/x-host-xxxx" 报，游戏不认就不选） | other codecs are reported as "video/x-host-xxxx"; the game does not recognise them and does not pick them
    int64_t duration_us;
    int bitrate, fps;
    int width, height;                // 视频：显示尺寸（裁剪后） | video: display size (after cropping)
    int sample_rate, channels;        // 音频 | audio
    int nsamples;                     // 解码序样本表 | sample table in decode order
    int64_t *dts_us, *pts_us;
    int32_t *dur_us;
    uint8_t *sync;
} mvav_track;

typedef struct mvav_file mvav_file;
// 打开并扫一遍样本表（直通读取，不解码）；只收音视频轨，序号就是 mvav_dec_open 的 track。失败返回 NULL，原因写进 err
// Opens and scans the sample table once (pass-through read, no decoding); only audio / video tracks are kept, and their index is the track of mvav_dec_open. Returns NULL on failure with the reason in err
mvav_file *mvav_open(const char *path, char *err, size_t errn);
int mvav_ntracks(const mvav_file *f);
const mvav_track *mvav_track_at(const mvav_file *f, int i);
void mvav_close(mvav_file *f);

typedef struct mvav_dec mvav_dec;
mvav_dec *mvav_dec_open(const char *path, int track, char *err, size_t errn);
// 从 start_us 起（重新）建读取器；0 = 成功 | (re)creates the reader from start_us; 0 = success
int mvav_dec_start(mvav_dec *d, int64_t start_us, char *err, size_t errn);
// 视频：下一帧按 NV12 紧排（Y w×h，再 UV w×h/2）写进 dst；返回 1 = 有帧，0 = 流尽，-1 = 出错
// video: the next frame written to dst as tightly packed NV12 (Y w×h, then UV w×h/2); returns 1 = frame, 0 = end of stream, -1 = error
int mvav_dec_video(mvav_dec *d, uint8_t *dst, int w, int h, int64_t *pts_us);
// 音频：取 nframes 帧交错 PCM16 写进 dst，流尽后补静音；返回其中真实数据的帧数，-1 = 出错
// audio: takes nframes frames of interleaved PCM16 into dst, padding with silence after the end of stream; returns how many of them are real data, -1 = error
int mvav_dec_audio(mvav_dec *d, int16_t *dst, int nframes);
void mvav_dec_close(mvav_dec *d);
