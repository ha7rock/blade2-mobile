// iOS 虚拟手柄图层的效果图：在 Mac 上用 dm/ios/blade/Sources/pad_layout.h 的同一份几何与绘制代码，把图层画在一张游戏截图上，
// 按给定的 iPhone 点尺寸、安全区和像素比出 PNG——不用真机就能看布局、改尺寸；真机上 front_blade.m 画的就是这份。
//   clang -fobjc-arc -framework AppKit -framework CoreText -I dm/host -I dm/ios/blade/Sources tools/pad_mockup.m -o /tmp/pad_mockup
//   /tmp/pad_mockup <截图.jpg> <出.png> <宽 点> <高 点> <上 左 下 右 安全区> [scale=3] [pressed=A,ZL|-] [stick=dx,dy|-] [alpha=1]
//   例：iPhone 15 Pro 横屏 852x393，安全区 0 59 21 59；iPhone SE 667x375，安全区 0 0 0 0
// Mock-up of the iOS touch-gamepad layer: on the Mac, draw the layer over a game screenshot with the very same geometry and drawing code
// (dm/ios/blade/Sources/pad_layout.h) at a given iPhone point size, safe area and pixel ratio, and write a PNG -- layout and sizes can be judged
// and tuned without a device; front_blade.m draws exactly this on the phone.
#import <AppKit/AppKit.h>
#include <string.h>
#include <stdlib.h>
#include "pad_layout.h"

// 键帽图标：NSImage 的系统符号渲成 3x 位图并缓存（pad_layout.h 只用其 alpha） | key-cap glyphs: NSImage system symbols rendered into 3x bitmaps and cached (pad_layout.h uses only their alpha)
static CGImageRef mac_glyph(const char *name) {
    static NSMutableDictionary *cache; if (!cache) cache = [NSMutableDictionary new];
    NSString *k = @(name); id v = cache[k]; if (v) return v == NSNull.null ? NULL : (__bridge CGImageRef)v;
    NSImage *im = [[NSImage imageWithSystemSymbolName:k accessibilityDescription:nil] imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:64 weight:NSFontWeightMedium]];
    CGImageRef cg = NULL;
    if (im) { NSSize sz = im.size; CGFloat q = 3;
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)(sz.width * q) pixelsHigh:(NSInteger)(sz.height * q) bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [im drawInRect:NSMakeRect(0, 0, sz.width * q, sz.height * q) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
        [NSGraphicsContext restoreGraphicsState]; cg = CGImageRetain(rep.CGImage);
    } else fprintf(stderr, "没有图标 %s\n", name);
    cache[k] = cg ? (__bridge id)cg : NSNull.null; return cg;
}
static int label_index(const char *s) { for (int i = 0; i < PB_N; i++) if (!strcmp(s, k_label[i])) return i; return -1; }

int main(int argc, char **argv) {
    if (argc < 9) { fprintf(stderr, "用法 | usage: pad_mockup <shot.jpg> <out.png> <W> <H> <top> <left> <bottom> <right> [scale] [pressed=A,ZL|-] [stick=dx,dy|-] [alpha]\n"); return 2; }
    @autoreleasepool {
        NSImage *img = [[NSImage alloc] initWithContentsOfFile:@(argv[1])]; if (!img) { fprintf(stderr, "读不了 %s\n", argv[1]); return 1; }
        CGImageRef src = [img CGImageForProposedRect:NULL context:nil hints:nil];
        CGFloat W = atof(argv[3]), H = atof(argv[4]), it = atof(argv[5]), il = atof(argv[6]), ib = atof(argv[7]), ir = atof(argv[8]);
        CGFloat scale = argc > 9 ? atof(argv[9]) : 3; const char *pressed = argc > 10 ? argv[10] : "-", *stick = argc > 11 ? argv[11] : "-"; CGFloat alpha = argc > 12 ? atof(argv[12]) : 1;
        size_t pw = (size_t)(W * scale), ph = (size_t)(H * scale);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef g = CGBitmapContextCreate(NULL, pw, ph, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGContextSetRGBFillColor(g, 0, 0, 0, 1); CGContextFillRect(g, CGRectMake(0, 0, pw, ph));
        // 翻成 y 向下的点坐标系（与 UIKit drawRect 一致） | flip into a y-down point coordinate system (same as UIKit's drawRect)
        CGContextTranslateCTM(g, 0, ph); CGContextScaleCTM(g, scale, -scale);
        pad_glyph = mac_glyph; pad_layout L; pad_layout_make(&L, CGSizeMake(W, H), it, il, ib, ir, 1.0);
        // 画面：16:9 居中（图片在翻转的上下文里要再翻一次） | picture: 16:9 centred (the image must be flipped back inside the flipped context)
        CGContextSaveGState(g); CGContextTranslateCTM(g, 0, L.pic.origin.y * 2 + L.pic.size.height); CGContextScaleCTM(g, 1, -1);
        CGContextSetInterpolationQuality(g, kCGInterpolationHigh); CGContextDrawImage(g, L.pic, src); CGContextRestoreGState(g);
        // 安全区示意：细虚线 | safe-area guide: thin dashed line
        CGContextSetRGBStrokeColor(g, 1, 0.8, 0.2, 0.5); CGContextSetLineWidth(g, 0.7); CGFloat dash[2] = {4, 4}; CGContextSetLineDash(g, 0, dash, 2);
        CGContextStrokeRect(g, CGRectMake(il, it, W - il - ir, H - it - ib)); CGContextSetLineDash(g, 0, NULL, 0);
        pad_state S = {0};
        if (strcmp(pressed, "-")) { char buf[128]; strlcpy(buf, pressed, sizeof buf); for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) { int i = label_index(t); if (i >= 0) S.held |= k_bit[i]; else fprintf(stderr, "没有这个键 %s\n", t); } }
        if (strcmp(stick, "-")) { double dx = 0, dy = 0; sscanf(stick, "%lf,%lf", &dx, &dy); S.ls_on = 1; S.ls_c = CGPointMake(L.ls_idle.x + 0.6 * L.r, L.ls_idle.y - 0.4 * L.r); S.ls_p = CGPointMake(S.ls_c.x + dx * L.stick_r, S.ls_c.y - dy * L.stick_r); }
        CGContextSetAlpha(g, alpha); CGContextBeginTransparencyLayer(g, NULL); pad_draw(g, &L, &S); CGContextEndTransparencyLayer(g);
        CGImageRef out = CGBitmapContextCreateImage(g);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:out]; NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:@(argv[2]) atomically:YES]) { fprintf(stderr, "写不了 %s\n", argv[2]); return 1; }
        printf("%s：%zux%zu 像素，基准半径 %.1f 点，画面 %.0f,%.0f %.0fx%.0f，摇杆满偏 %.0f 点，分界 x=%.0f\n", argv[2], pw, ph, L.r, L.pic.origin.x, L.pic.origin.y, L.pic.size.width, L.pic.size.height, L.stick_r, L.split_x);
        for (int i = 0; i < PB_N; i++) printf("  %-3s 中心 %6.1f,%6.1f 半径 %4.1f 命中 %4.1f\n", k_label[i], L.btn[i].c.x, L.btn[i].c.y, L.btn[i].r, L.btn[i].hit);
        CGImageRelease(out); CGContextRelease(g); CGColorSpaceRelease(cs);
    }
    return 0;
}
