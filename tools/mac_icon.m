// 把游戏原版图标（Switch 控制分区里 256×256 的 icon_*.dat，JPEG）做成 macOS 的 AppIcon.icns：
// 1024 画布、824 见方居中、圆角 22.5%（macOS 图标网格），各档尺寸由 CoreGraphics 缩放后交给 iconutil。
// 用法：clang -fobjc-arc -framework Cocoa tools/mac_icon.m -o <bin> && <bin> <icon.jpg> <AppIcon.icns>
// Turn the game's own icon (the 256x256 icon_*.dat JPEG from the Switch control partition) into a macOS AppIcon.icns:
// 1024 canvas, 824 square centred, 22.5% corner radius (the macOS icon grid); each size is scaled by CoreGraphics and handed to iconutil.
// Usage: clang -fobjc-arc -framework Cocoa tools/mac_icon.m -o <bin> && <bin> <icon.jpg> <AppIcon.icns>
#import <Cocoa/Cocoa.h>
static CGImageRef render(CGImageRef src, size_t px) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, px, px, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
    CGFloat m = px * 100.0 / 1024.0, w = px - 2 * m, r = w * 0.225;
    CGRect box = CGRectMake(m, m, w, w);
    CGPathRef p = CGPathCreateWithRoundedRect(box, r, r, NULL);
    CGContextAddPath(c, p); CGContextClip(c); CGPathRelease(p);
    CGContextDrawImage(c, box, src);
    CGImageRef out = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    return out;
}
static int write_png(CGImageRef im, NSString *path) {
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], (__bridge CFStringRef)@"public.png", 1, NULL);
    if (!d) return 0;
    CGImageDestinationAddImage(d, im, NULL);
    int ok = CGImageDestinationFinalize(d); CFRelease(d); return ok;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) { fprintf(stderr, "usage: mac_icon <icon.jpg> <AppIcon.icns>\n"); return 2; }
        CGImageSourceRef s = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[1])], NULL);
        CGImageRef src = s ? CGImageSourceCreateImageAtIndex(s, 0, NULL) : NULL;
        if (!src) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
        NSString *set = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"AppIcon-%d.iconset", getpid()]];
        [NSFileManager.defaultManager createDirectoryAtPath:set withIntermediateDirectories:YES attributes:nil error:NULL];
        int sizes[] = {16, 32, 128, 256, 512};
        for (int i = 0; i < 5; i++) {
            int n = sizes[i];
            CGImageRef a = render(src, n), b = render(src, n * 2);
            if (!write_png(a, [set stringByAppendingFormat:@"/icon_%dx%d.png", n, n]) || !write_png(b, [set stringByAppendingFormat:@"/icon_%dx%d@2x.png", n, n])) { fprintf(stderr, "png write failed\n"); return 1; }
            CGImageRelease(a); CGImageRelease(b);
        }
        NSTask *t = [NSTask new]; t.launchPath = @"/usr/bin/iconutil"; t.arguments = @[@"-c", @"icns", set, @"-o", @(argv[2])];
        [t launch]; [t waitUntilExit];
        [NSFileManager.defaultManager removeItemAtPath:set error:NULL];
        if (t.terminationStatus) { fprintf(stderr, "iconutil failed (%d)\n", t.terminationStatus); return 1; }
        return 0;
    }
}
