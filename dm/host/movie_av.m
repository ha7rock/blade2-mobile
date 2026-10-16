// 过场视频的宿主解复用 / 解码：AVFoundation（AVAssetReader）。接口说明见 movie_av.h，协议见 hle_movie.c 文件头。
// 取样本表用直通输出（outputSettings = nil，只读压缩数据不解码）；解码用解压输出：视频 NV12（按码流的视频 / 全范围取 420v / 420f，
// 与硬件解码器原样输出的取值一致），音频交错 PCM16（码流原采样率 / 声道数）。
// Host demux / decode for cutscene videos: AVFoundation (AVAssetReader). See movie_av.h for the interface and the header of hle_movie.c for the protocol.
// The sample table comes from a pass-through output (outputSettings = nil, reads compressed data, no decoding); decoding uses decompressing outputs: video NV12
// (420v / 420f following the stream's video / full range, so the values match what the hardware decoder outputs as-is), audio interleaved PCM16 (the stream's own rate / channel count).
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <stdlib.h>
#include <string.h>
#include "movie_av.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // asset.tracks 同步取轨：在游戏线程上调，同步正合适 | asset.tracks synchronous track access: called on a game thread, synchronous is exactly right

struct mvav_file { int n; mvav_track t[8]; };

static NSArray<AVAssetTrack *> *av_tracks(AVAsset *a) {
    NSMutableArray *r = [NSMutableArray array];
    for (AVAssetTrack *t in a.tracks)
        if ([t.mediaType isEqualToString:AVMediaTypeVideo] || [t.mediaType isEqualToString:AVMediaTypeAudio]) [r addObject:t];
    return r;
}
static AVURLAsset *av_asset(const char *path) {
    NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    return [AVURLAsset URLAssetWithURL:u options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
}
static int64_t us_of(CMTime t) { return CMTIME_IS_NUMERIC(t) ? (int64_t)llround(CMTimeGetSeconds(t) * 1e6) : 0; }

mvav_file *mvav_open(const char *path, char *err, size_t errn) {
    @autoreleasepool {
        AVURLAsset *a = av_asset(path);
        NSArray<AVAssetTrack *> *tr = av_tracks(a);
        if (!tr.count) { snprintf(err, errn, "没有音视频轨（文件读不了或不是媒体） | no audio / video track (unreadable or not media)"); return NULL; }
        if (tr.count > 8) { snprintf(err, errn, "音视频轨 %lu 条，超过 8 | %lu audio / video tracks, more than 8", (unsigned long)tr.count, (unsigned long)tr.count); return NULL; }
        mvav_file *f = calloc(1, sizeof *f); f->n = (int)tr.count;
        for (int i = 0; i < f->n; i++) {
            AVAssetTrack *t = tr[i]; mvav_track *m = &f->t[i];
            CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
            FourCharCode cc = fd ? CMFormatDescriptionGetMediaSubType(fd) : 0;
            m->duration_us = us_of(t.timeRange.duration); m->bitrate = (int)t.estimatedDataRate;
            if ([t.mediaType isEqualToString:AVMediaTypeVideo]) {
                m->kind = 1; m->fps = (int)lrintf(t.nominalFrameRate);
                if (cc == kCMVideoCodecType_H264) strcpy(m->mime, "video/avc");
                else snprintf(m->mime, sizeof m->mime, "video/x-host-%c%c%c%c", cc >> 24, cc >> 16 & 0xff, cc >> 8 & 0xff, cc & 0xff);
                CGSize s = fd ? CMVideoFormatDescriptionGetPresentationDimensions(fd, false, true) : t.naturalSize;
                m->width = (int)s.width; m->height = (int)s.height;
            } else {
                m->kind = 2;
                if (cc == kAudioFormatMPEG4AAC) strcpy(m->mime, "audio/mp4a-latm");
                else snprintf(m->mime, sizeof m->mime, "audio/x-host-%c%c%c%c", cc >> 24, cc >> 16 & 0xff, cc >> 8 & 0xff, cc & 0xff);
                const AudioStreamBasicDescription *as = fd ? CMAudioFormatDescriptionGetStreamBasicDescription(fd) : NULL;
                if (as) { m->sample_rate = (int)as->mSampleRate; m->channels = (int)as->mChannelsPerFrame; }
            }
        }
        // 每轨一个读取器：同一读取器挂多路输出时只读一路会卡住 | one reader per track: with several outputs on one reader, draining only one of them can stall
        int cap[8] = {0};
        for (int i = 0; i < f->n; i++) {
            mvav_track *m = &f->t[i];
            NSError *e = nil;
            AVAssetReader *rd = [AVAssetReader assetReaderWithAsset:a error:&e];
            if (!rd) { snprintf(err, errn, "AVAssetReader: %s", e.localizedDescription.UTF8String); mvav_close(f); return NULL; }
            AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tr[i] outputSettings:nil];
            o.alwaysCopiesSampleData = NO;
            if (![rd canAddOutput:o]) { snprintf(err, errn, "轨 %d 加不了直通输出 | track %d: cannot add a pass-through output", i, i); mvav_close(f); return NULL; }
            [rd addOutput:o];
            if (![rd startReading]) { snprintf(err, errn, "startReading: %s", rd.error.localizedDescription.UTF8String); mvav_close(f); return NULL; }
            for (;;) {
                CMSampleBufferRef sb = [o copyNextSampleBuffer];
                if (!sb) break;
                CMItemCount ns = CMSampleBufferGetNumSamples(sb), nt = 0;
                CMSampleBufferGetSampleTimingInfoArray(sb, 0, NULL, &nt);
                CMSampleTimingInfo *ti = nt ? calloc(nt, sizeof *ti) : NULL;
                if (nt) CMSampleBufferGetSampleTimingInfoArray(sb, nt, ti, &nt);
                CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
                for (CMItemIndex k = 0; k < ns; k++) {
                    if (m->nsamples == cap[i]) {
                        cap[i] = cap[i] ? cap[i] * 2 : 1024;
                        m->dts_us = realloc(m->dts_us, cap[i] * sizeof *m->dts_us); m->pts_us = realloc(m->pts_us, cap[i] * sizeof *m->pts_us);
                        m->dur_us = realloc(m->dur_us, cap[i] * sizeof *m->dur_us); m->sync = realloc(m->sync, cap[i]);
                    }
                    // 同一缓冲里多个样本只给一条计时（音频常见）：按时长顺推 | several samples sharing one timing entry in a buffer (common for audio): advance by the duration
                    CMSampleTimingInfo t0 = ti ? ti[nt == ns ? k : 0] : (CMSampleTimingInfo){kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
                    int64_t dur = us_of(t0.duration), pts = us_of(t0.presentationTimeStamp), dts = CMTIME_IS_NUMERIC(t0.decodeTimeStamp) ? us_of(t0.decodeTimeStamp) : pts;
                    if (nt != ns && k) {
                        CMTime off = CMTimeMultiply(t0.duration, (int32_t)k);
                        pts = us_of(CMTimeAdd(t0.presentationTimeStamp, off));
                        dts = CMTIME_IS_NUMERIC(t0.decodeTimeStamp) ? us_of(CMTimeAdd(t0.decodeTimeStamp, off)) : pts;
                    }
                    int sync = 1;
                    if (att && CFArrayGetCount(att)) {
                        CFDictionaryRef d = CFArrayGetValueAtIndex(att, CFArrayGetCount(att) == ns ? k : 0);
                        CFBooleanRef ns_ = d ? CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync) : NULL;
                        if (ns_ && CFBooleanGetValue(ns_)) sync = 0;
                    }
                    int j = m->nsamples++;
                    m->dts_us[j] = dts; m->pts_us[j] = pts; m->dur_us[j] = (int32_t)dur; m->sync[j] = (uint8_t)sync;
                }
                free(ti); CFRelease(sb);
            }
            if (rd.status == AVAssetReaderStatusFailed) { snprintf(err, errn, "扫样本表失败 | sample table scan failed: %s", rd.error.localizedDescription.UTF8String); mvav_close(f); return NULL; }
            [rd cancelReading];
        }
        for (int i = 0; i < f->n; i++)
            if (!f->t[i].nsamples) { snprintf(err, errn, "轨 %d 没有样本 | track %d has no samples", i, i); mvav_close(f); return NULL; }
        return f;
    }
}
int mvav_ntracks(const mvav_file *f) { return f->n; }
const mvav_track *mvav_track_at(const mvav_file *f, int i) { return i >= 0 && i < f->n ? &f->t[i] : NULL; }
void mvav_close(mvav_file *f) {
    if (!f) return;
    for (int i = 0; i < f->n; i++) { free(f->t[i].dts_us); free(f->t[i].pts_us); free(f->t[i].dur_us); free(f->t[i].sync); }
    free(f);
}

struct mvav_dec {
    void *asset, *track, *reader, *output;            // __bridge_retained 持有 | held via __bridge_retained
    int kind, rate, channels;
    OSType pixfmt;
    int16_t *pcm; int pcm_n, pcm_pos, pcm_cap;        // 音频：读出未交付的 PCM（帧） | audio: PCM read but not yet handed out (frames)
    int ended;
};
static void rel(void **p) { if (*p) { CFRelease(*p); *p = NULL; } }

mvav_dec *mvav_dec_open(const char *path, int track, char *err, size_t errn) {
    @autoreleasepool {
        AVURLAsset *a = av_asset(path);
        NSArray<AVAssetTrack *> *tr = av_tracks(a);
        if (track < 0 || track >= (int)tr.count) { snprintf(err, errn, "轨号 %d 越界（共 %lu） | track %d out of range (%lu)", track, (unsigned long)tr.count, track, (unsigned long)tr.count); return NULL; }
        AVAssetTrack *t = tr[track];
        mvav_dec *d = calloc(1, sizeof *d);
        d->asset = (__bridge_retained void *)a; d->track = (__bridge_retained void *)t;
        CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
        if ([t.mediaType isEqualToString:AVMediaTypeVideo]) {
            d->kind = 1;
            CFTypeRef full = fd ? CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_FullRangeVideo) : NULL;
            d->pixfmt = full && CFBooleanGetValue(full) ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        } else {
            d->kind = 2;
            const AudioStreamBasicDescription *as = fd ? CMAudioFormatDescriptionGetStreamBasicDescription(fd) : NULL;
            if (!as) { snprintf(err, errn, "音频轨没有格式描述 | audio track has no format description"); mvav_dec_close(d); return NULL; }
            d->rate = (int)as->mSampleRate; d->channels = (int)as->mChannelsPerFrame;
        }
        return d;
    }
}
int mvav_dec_start(mvav_dec *d, int64_t start_us, char *err, size_t errn) {
    @autoreleasepool {
        if (d->reader) [(__bridge AVAssetReader *)d->reader cancelReading];
        rel(&d->output); rel(&d->reader);
        d->pcm_n = d->pcm_pos = 0; d->ended = 0;
        AVAsset *a = (__bridge AVAsset *)d->asset; AVAssetTrack *t = (__bridge AVAssetTrack *)d->track;
        NSError *e = nil;
        AVAssetReader *rd = [AVAssetReader assetReaderWithAsset:a error:&e];
        if (!rd) { snprintf(err, errn, "AVAssetReader: %s", e.localizedDescription.UTF8String); return -1; }
        NSDictionary *set;
        if (d->kind == 1) set = @{(id)kCVPixelBufferPixelFormatTypeKey: @(d->pixfmt)};
        else set = @{AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO,
                     AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO,
                     AVSampleRateKey: @(d->rate), AVNumberOfChannelsKey: @(d->channels)};
        AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:set];
        o.alwaysCopiesSampleData = NO;
        if (![rd canAddOutput:o]) { snprintf(err, errn, "加不了解码输出 | cannot add the decoding output"); return -1; }
        [rd addOutput:o];
        if (start_us > 0) rd.timeRange = CMTimeRangeMake(CMTimeMake(start_us, 1000000), kCMTimePositiveInfinity);
        if (![rd startReading]) { snprintf(err, errn, "startReading: %s", rd.error.localizedDescription.UTF8String); return -1; }
        d->reader = (__bridge_retained void *)rd; d->output = (__bridge_retained void *)o;
        return 0;
    }
}
static void copy_plane(uint8_t *dst, int w, int h, const uint8_t *src, size_t sstride, int sw, int sh) {
    int cw = w < sw ? w : sw, ch = h < sh ? h : sh;
    for (int y = 0; y < ch; y++) { memcpy(dst + (size_t)y * w, src + y * sstride, cw); if (cw < w) memset(dst + (size_t)y * w + cw, 0, w - cw); }
    for (int y = ch; y < h; y++) memset(dst + (size_t)y * w, 0, w);
}
int mvav_dec_video(mvav_dec *d, uint8_t *dst, int w, int h, int64_t *pts_us) {
    @autoreleasepool {
        if (!d->output || d->ended) return 0;
        AVAssetReaderTrackOutput *o = (__bridge AVAssetReaderTrackOutput *)d->output;
        for (;;) {
            CMSampleBufferRef sb = [o copyNextSampleBuffer];
            if (!sb) { d->ended = 1; return ((__bridge AVAssetReader *)d->reader).status == AVAssetReaderStatusFailed ? -1 : 0; }
            CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
            if (!ib) { CFRelease(sb); continue; }                       // 无图像的标记缓冲 | marker buffer without an image
            *pts_us = us_of(CMSampleBufferGetPresentationTimeStamp(sb));
            CVPixelBufferLockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
            int pw = (int)CVPixelBufferGetWidth(ib), ph = (int)CVPixelBufferGetHeight(ib);
            copy_plane(dst, w, h, CVPixelBufferGetBaseAddressOfPlane(ib, 0), CVPixelBufferGetBytesPerRowOfPlane(ib, 0), pw, ph);
            // UV：每行 w 字节（w/2 对 Cb Cr），h/2 行 | UV: w bytes per row (w/2 Cb Cr pairs), h/2 rows
            copy_plane(dst + (size_t)w * h, w & ~1, h / 2, CVPixelBufferGetBaseAddressOfPlane(ib, 1), CVPixelBufferGetBytesPerRowOfPlane(ib, 1),
                       (int)CVPixelBufferGetWidthOfPlane(ib, 1) * 2, (int)CVPixelBufferGetHeightOfPlane(ib, 1));
            CVPixelBufferUnlockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
            CFRelease(sb);
            return 1;
        }
    }
}
int mvav_dec_audio(mvav_dec *d, int16_t *dst, int nframes) {
    @autoreleasepool {
        int ch = d->channels, got = 0;
        while (got < nframes) {
            if (d->pcm_pos < d->pcm_n) {
                int k = d->pcm_n - d->pcm_pos; if (k > nframes - got) k = nframes - got;
                memcpy(dst + (size_t)got * ch, d->pcm + (size_t)d->pcm_pos * ch, (size_t)k * ch * 2);
                d->pcm_pos += k; got += k; continue;
            }
            if (!d->output || d->ended) break;
            CMSampleBufferRef sb = [(__bridge AVAssetReaderTrackOutput *)d->output copyNextSampleBuffer];
            if (!sb) { d->ended = 1; if (((__bridge AVAssetReader *)d->reader).status == AVAssetReaderStatusFailed) return -1; break; }
            CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
            size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
            int fr = (int)(len / (2 * ch));
            if (fr > d->pcm_cap) { d->pcm_cap = fr; d->pcm = realloc(d->pcm, (size_t)fr * ch * 2); }
            if (fr && CMBlockBufferCopyDataBytes(bb, 0, (size_t)fr * ch * 2, d->pcm) != kCMBlockBufferNoErr) { CFRelease(sb); return -1; }
            d->pcm_n = fr; d->pcm_pos = 0;
            CFRelease(sb);
        }
        if (got < nframes) memset(dst + (size_t)got * ch, 0, (size_t)(nframes - got) * ch * 2);
        return got;
    }
}
void mvav_dec_close(mvav_dec *d) {
    if (!d) return;
    @autoreleasepool {
        if (d->reader) [(__bridge AVAssetReader *)d->reader cancelReading];
        rel(&d->output); rel(&d->reader); rel(&d->track); rel(&d->asset);
    }
    free(d->pcm); free(d);
}
