#!/bin/bash
# Trial build of the GENERIC part of the host (no dm_*.c D2R feature files): compile each source, then link and
# print the undefined symbols = the set of D2R-feature entry points the generic files still depend on.
# Usage: host_trial_build.sh <build dir>
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); B=${1:?build dir}; mkdir -p "$B"
cd "$HERE/dm/host" || exit 1
SRC="app_mac.m crash.c entry.S front_mac.m hle.c loader.c main.c memstat.m romfs_validate.c thread.c $(ls hle_*.c)"
GFX="$(ls ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m)"
LD="../third_party/libdeflate/lib/deflate_decompress.c ../third_party/libdeflate/lib/zlib_decompress.c ../third_party/libdeflate/lib/adler32.c ../third_party/libdeflate/lib/utils.c ../third_party/libdeflate/lib/arm/cpu_features.c"
EXTRA=${EXTRA_SRC:-}
fail=0
for f in $SRC $GFX $LD $EXTRA; do
  o="$B/$(basename "$f").o"
  if ! clang -O1 -g -Wall -Wno-unused-function -mmacosx-version-min=26.0 -I. -I../gfx/nvn/include -fobjc-arc -c "$f" -o "$o" 2> "$o.err"; then
    fail=$((fail+1)); echo "COMPILE FAIL $f: $(grep -m1 'error:' "$o.err")"
  fi
done
echo "compile failures: $fail"
clang -mmacosx-version-min=26.0 -o "$B/dmhost_try" "$B"/*.o -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework ImageIO -framework UniformTypeIdentifiers -framework GameController -framework Network > "$B/link.log" 2>&1
grep -E '^[[:space:]]+"_' "$B/link.log" | sed -E 's/^[[:space:]]+"_([^"]+)".*/\1/' | sort -u > "$B/undefined.txt"
echo "undefined symbols: $(wc -l < "$B/undefined.txt")"
tr '\n' ' ' < "$B/undefined.txt"; echo
[ -x "$B/dmhost_try" ] && echo "LINK OK: $B/dmhost_try"
