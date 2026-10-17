#!/bin/bash
# Blade II iOS 壳：生成 Gen/ 与工程、编译；装机；启动。用法：
#   DM_OUT=<改写产物目录> ./run.sh build     生成 + 编译。本机配置（../local.env，见 ../local.env.example）里有 DM_TEAM_ID 就签名出 Release 包；
#                                            没有就只做编译检查（CODE_SIGNING_ALLOWED=NO，Debug，不能装机）
#   ./run.sh install                          把上一次 build 的 App 装到 DM_DEVICE（要 DM_TEAM_ID 签过）
#   ./run.sh launch                           在 DM_DEVICE 上启动；日志在 App 的 Documents/boot.txt（「文件」App 里可见）
# 输入：DM_OUT（main / sdk 的 .patched.bin、fixups / gates / cuts 表）、iOS 版着色器库 $DM_ROOT/build/ios/dmsh_b/b-<V>
#      （出法：IOSOUT=$DM_ROOT/build/ios/dmsh_b python3 dm/gfx/tools/dmsh_build.py $DM_ROOT/shpack $DM_ROOT/build/cache/metal_shader_cache；V 见 dm/gfx/nvn/dmsh_chain.h）
# 产物与日志在 $DM_ROOT/run/ios/（缺省 ~/blade2-work）。
# Blade II iOS shell: generate Gen/ and the project, compile; install; launch. Usage:
#   DM_OUT=<rewrite output dir> ./run.sh build   generate + compile. With DM_TEAM_ID in the local config (../local.env, see ../local.env.example) a signed Release build is made;
#                                                without it only a compile check (CODE_SIGNING_ALLOWED=NO, Debug, cannot be installed)
#   ./run.sh install                              install the last build onto DM_DEVICE (needs a DM_TEAM_ID-signed build)
#   ./run.sh launch                               launch on DM_DEVICE; the log is in the App's Documents/boot.txt (visible in the Files app)
# Inputs: DM_OUT (main / sdk .patched.bin, fixups / gates / cuts tables), the iOS shader library $DM_ROOT/build/ios/dmsh_b/b-<V>
#      (built by: IOSOUT=$DM_ROOT/build/ios/dmsh_b python3 dm/gfx/tools/dmsh_build.py $DM_ROOT/shpack $DM_ROOT/build/cache/metal_shader_cache; V in dm/gfx/nvn/dmsh_chain.h)
# Outputs and logs go to $DM_ROOT/run/ios/ (default ~/blade2-work).
set -u
cd "$(dirname "$0")"
source ../tools/local_env.sh
W=${DM_ROOT:-$HOME/blade2-work}; R=$W/run/ios; mkdir -p "$R"
DD=${DM_IOS_DD:-$R/dd_blade}
B=${DM_BUNDLE_ID:-org.example.blade2}; T=${DM_TEAM_ID:-}
CFG=${DM_CONFIG:-$([ -n "$T" ] && echo Release || echo Debug)}
APP=$DD/Build/Products/$CFG-iphoneos/BladeII.app
MODS="main sdk"
t(){ echo "[$(date +%T)] $*"; }
case "${1:-build}" in
build)
  OUT=${DM_OUT:?设 DM_OUT=<改写产物目录>（main / sdk 的 .patched.bin 所在目录）| set DM_OUT=<rewrite output dir>}
  for m in $MODS; do [ -f "$OUT/$m.patched.bin" ] && [ -f "$OUT/$m.fixups.tsv" ] || { t "🔴 $OUT 里没有 $m.patched.bin / $m.fixups.tsv"; exit 1; }; done
  rm -rf Gen; mkdir -p Gen/dm Gen/msl
  # 宿主源文件 / 头路径 / -D / 框架：取自 Makefile.blade 实际展开的编译命令（-DDM_GENERIC_HOST=1 -DDM_FORCE_LOOSE=1 -DDM_ARGV0 -DDM_PROGRAM_ID 都从这里来）
  # Host sources / header paths / -D / frameworks: taken from the actually expanded Makefile.blade compile command (-DDM_GENERIC_HOST=1 -DDM_FORCE_LOOSE=1 -DDM_ARGV0 -DDM_PROGRAM_ID all come from there)
  python3 ../tools/gen_build.py ../../host Gen Makefile.blade dmhost_blade > Gen/dm.xcconfig || { t "🔴 gen_build 失败"; exit 1; }
  # 映像内嵌：main 的补丁区在映像前面（PATCH_OFF < 0），gen_embed.py / loader.c 的 iOS 分支都认 | Image embedding: main's patch area lies before the image (PATCH_OFF < 0), understood by gen_embed.py and the loader's iOS branch
  F=$(python3 ../tools/gen_embed.py "$OUT" Gen/embed.S $MODS) || { t "🔴 gen_embed 失败"; exit 1; }
  echo "OTHER_LDFLAGS = \$(inherited) \$(DM_FRAMEWORKS) $F" >> Gen/dm.xcconfig
  for m in $MODS; do cp "$OUT/$m.fixups.tsv" "$OUT/$m.gates.tsv" Gen/dm/; done
  [ -f "$OUT/main.cuts.tsv" ] && cp "$OUT/main.cuts.tsv" Gen/dm/
  cp ../../gfx/nvn/shader_map.tsv ../../gfx/nvn/c1_full.tsv Gen/dm/
  [ -f ../../gfx/nvn/pso_manifest.tsv ] && cp ../../gfx/nvn/pso_manifest.tsv Gen/dm/
  # iOS 版着色器库：只带当前翻译链版本 | iOS shader library: only the current chain version
  V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' ../../gfx/nvn/dmsh_chain.h); MSL=${DM_MSL_IOS:-$W/build/ios/dmsh_b}
  [ -n "$V" ] && [ -d "$MSL/b-$V" ] && ls "$MSL/b-$V" | grep -q "_b.metallib$" || { t "🔴 缺 iOS 版着色器库 $MSL/b-${V}（见文件头的出包命令）"; exit 1; }
  cp -cR "$MSL/b-$V" Gen/msl/ 2>/dev/null || cp -R "$MSL/b-$V" Gen/msl/
  t "Gen/：$(ls Gen/src | wc -l | tr -d ' ') 个宿主源文件，着色器库 b-$V $(ls Gen/msl/b-$V | grep -c '_b.metallib$') 个"
  DM_BUNDLE_ID=$B DM_TEAM_ID=$T xcodegen generate -q --spec project.yml > /dev/null || { t "🔴 xcodegen 失败"; exit 1; }
  [ "${DM_GEN_ONLY:-}" = 1 ] && { t "生成完成（DM_GEN_ONLY）"; exit 0; }
  if [ -n "$T" ]; then
    xcodebuild -project BladeIOS.xcodeproj -scheme BladeII -configuration "$CFG" -destination 'generic/platform=iOS' -derivedDataPath "$DD" -allowProvisioningUpdates build > "$R/build_blade.log" 2>&1 \
      || { t "🔴 构建失败（$R/build_blade.log）"; grep -E "error:" "$R/build_blade.log" | head -5; exit 1; }
    codesign -d --entitlements :- "$APP" 2>/dev/null | plutil -p - | head -8
  else
    t "没有 DM_TEAM_ID：只做编译检查（不签名，不能装机）| no DM_TEAM_ID: compile check only (unsigned, not installable)"
    xcodebuild -project BladeIOS.xcodeproj -scheme BladeII -configuration "$CFG" -destination 'generic/platform=iOS' -derivedDataPath "$DD" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build > "$R/build_blade.log" 2>&1 \
      || { t "🔴 构建失败（$R/build_blade.log）"; grep -E "error:" "$R/build_blade.log" | head -5; exit 1; }
  fi
  t "构建完成：${APP}（$(du -sh "$APP" 2>/dev/null | cut -f1)）"; exit 0;;
install)
  local_env_need DM_DEVICE || exit 1
  xcrun devicectl device install app --device "$DM_DEVICE" "$APP" > "$R/install_blade.log" 2>&1 || { t "🔴 装机失败"; tail -5 "$R/install_blade.log"; exit 1; }
  t "装机完成"; exit 0;;
launch)
  local_env_need DM_DEVICE || exit 1
  xcrun devicectl device process launch --device "$DM_DEVICE" --terminate-existing "$B" || exit 1
  t "已启动；日志：App 的 Documents/boot.txt"; exit 0;;
*) echo "usage: $0 build | install | launch"; exit 1;;
esac
