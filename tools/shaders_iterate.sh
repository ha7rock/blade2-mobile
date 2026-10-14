#!/bin/bash
# 一轮着色器迭代：转储目录 → 合成包 + 映射表 → 翻译链版本头 → 翻译 → c1 清单 → 重编宿主。
# 用法：tools/shaders_iterate.sh            （DM_ROOT 缺省 ~/blade2-work；转储在 $DM_ROOT/shdump，库在 $DM_ROOT/build/cache/metal_shader_cache）
# One shader iteration: dump dir → synthetic packages + map → chain header → translate → c1 list → rebuild the host.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); W=${DM_ROOT:-$HOME/blade2-work}; export DM_ROOT=$W
PY=${DM_PY:-$W/venv/bin/python3}; [ -x "$PY" ] || PY=$(command -v python3)
export DOTNET_ROOT=${DOTNET_ROOT:-$(brew --prefix dotnet)/libexec}
T=$HERE/dm/gfx/tools
"$PY" -I "$T/dmsh_from_dump.py" "$W/shdump" "$W/shpack" blade || exit 1
"$PY" -I "$T/dmsh_chain.py" --check-tools > /dev/null 2>&1 || "$PY" -I "$T/dmsh_chain.py" --update-lock > /dev/null
"$PY" -I "$T/dmsh_chain.py" --header > /dev/null; V=$("$PY" -I "$T/dmsh_chain.py"); echo "chain $V"
J=${J:-6} XLATE=$W/build/gfx/dmsh_xlate/dmsh_xlate "$PY" -I "$T/dmsh_build.py" "$W/shpack" "$W/build/cache/metal_shader_cache" 2>&1 | tail -12
"$PY" -I "$T/c1_guard.py" "$W/build/cache/metal_shader_cache/b-$V" "$HERE/dm/gfx/nvn/c1_full.tsv" 2>&1 | tail -1
make -C "$HERE/dm/host" -f Makefile.blade 2>&1 | grep -E "error" | head -5
echo "libs: $(ls "$W/build/cache/metal_shader_cache/b-$V" | grep -c '_b.metallib')  dumped: $(ls "$W/shdump" | grep -c '.ctl')"
