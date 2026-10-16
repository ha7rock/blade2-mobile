#!/bin/bash
# 把 Blade II 的 Mac 宿主包成能双击起的 macOS App：tools/mac_app_blade.sh <改写产物目录(out2)> [输出目录]
#   产出 <输出目录>/Blade II.app（缺省 ~/blade2-work/out/macapp）。包里带：宿主程序、改写后的游戏程序（Resources/product，约 150 MB）、
#   着色器映射表 / c1 表、编好的着色器库（Resources/msl/b-<链版本>，只带 .metallib 与 .json）。游戏资源（RomFS）不进包：
#   第一次启动弹文件夹选择框，选解出来的 Update/RomFS（里面有 BladeII/Content/Paks），记进偏好；存档在 ~/Library/Application Support/Blade II Mobile/save，
#   日志在 ~/Library/Logs/Blade II Mobile/host.log。本地临时签名，本机直接能跑。
# Wrap the Blade II Mac host into a double-clickable macOS App: tools/mac_app_blade.sh <rewrite output dir (out2)> [output dir]
#   Produces <output dir>/Blade II.app (default ~/blade2-work/out/macapp). The package holds: the host, the rewritten game program (Resources/product, ~150 MB),
#   the shader map / c1 table, the compiled shader library (Resources/msl/b-<chain>, .metallib + .json only). The game resources (RomFS) are not in the package:
#   the first launch asks for the folder (the extracted Update/RomFS containing BladeII/Content/Paks) and remembers it; saves go to ~/Library/Application Support/Blade II Mobile/save,
#   the log to ~/Library/Logs/Blade II Mobile/host.log. Ad-hoc signed, runs on this machine.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd); DM=$HERE/dm; ROOT=${DM_ROOT:-$HOME/blade2-work}
P=${1:?改写产物目录 | rewrite output dir}; OUT=${2:-$ROOT/out/macapp}
[ -f "$P/main.patched.bin" ] || { echo "🔴 ${P} 里没有 main.patched.bin"; exit 2; }
NAME="Blade II"; BID=${DM_MAC_BUNDLE_ID:-org.blade2mobile.mac}
make -C "$DM/host" -f Makefile.blade > /dev/null
MIN=$(sed -n 's/^MACOS_MIN ?= *//p' "$DM/host/Makefile.blade")
APP="$OUT/$NAME.app"; rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$DM/host/dmhost_blade" "$APP/Contents/MacOS/BladeII"
RES="$APP/Contents/Resources"
mkdir -p "$RES/product"; for f in main sdk; do cp "$P/$f.patched.bin" "$P/$f.fixups.tsv" "$P/$f.gates.tsv" "$P/$f.params.tsv" "$RES/product/"; cp "$P/$f.cuts.tsv" "$RES/product/" 2>/dev/null || true; done
cp "$DM/gfx/nvn/shader_map.tsv" "$RES/shader_map.tsv"
[ ! -f "$DM/gfx/nvn/c1_full.tsv" ] || cp "$DM/gfx/nvn/c1_full.tsv" "$RES/c1_full.tsv"
V=$(sed -n 's/.*DMSH_CHAIN "\(.*\)".*/\1/p' "$DM/gfx/nvn/dmsh_chain.h")
MSL=${DM_MSL_DIR:-$ROOT/build/cache/metal_shader_cache}/b-$V
[ -d "$MSL" ] || { echo "🔴 没有着色器库 ${MSL}（先跑 tools/shaders_iterate.sh）"; exit 2; }
mkdir -p "$RES/msl/b-$V"; rsync -a --include='*.metallib' --include='*.json' --exclude='*' "$MSL/" "$RES/msl/b-$V/"
VER=${DM_MAC_VERSION:-0.1}; BUILD=${DM_MAC_BUILD:-$(date +%m%d%H%M)}
echo "版本 ${VER} 构建 ${BUILD} 提交 $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo 未知) 着色器链 b-$V" > "$RES/build.txt"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>$NAME</string>
<key>CFBundleDisplayName</key><string>$NAME</string>
<key>CFBundleIdentifier</key><string>$BID</string>
<key>CFBundleExecutable</key><string>BladeII</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VER</string>
<key>CFBundleVersion</key><string>$BUILD</string>
<key>LSMinimumSystemVersion</key><string>$MIN</string>
<key>NSHighResolutionCapable</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
<key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
PL
codesign --force --sign - --deep "$APP" > /dev/null 2>&1 || { echo "🔴 签名失败"; exit 3; }
echo "✅ ${APP}（$(du -sh "$APP" | cut -f1)）：双击启动，第一次选游戏资源文件夹 Update/RomFS"
