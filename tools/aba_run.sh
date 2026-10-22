#!/bin/bash
# One scripted fight run of the Mac app + screen recording + A-B-A detection (screen and game-frame thumbnails).
# Usage: tools/aba_run.sh <tag> [rec_start_s=55] [rec_dur_s=60] [watch_s=120]
#   Extra DM_* env for the app: pass them in the environment of this script (they are inherited), e.g.
#   DM_NVN_SERIAL_FRAME=1 tools/aba_run.sh serial1
# The game window stays behind everything, small, in the bottom-right corner (DM_WINDOW_BACK=1) so a person working on the Mac is not interrupted; ABA_TOP=1 restores the old on-top window for screen recording.
# Silent by default (DM_AUDIO=0: no CoreAudio output unit at all); DM_AUDIO=1 in the environment turns sound back on.
# Outputs: ~/blade2-work/run/aba/<tag>/{rec.mov, small.raw, reglum.txt, host.log, stderr.log, screen.txt, reglum_aba.txt, env.txt}
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:?tag}; RS=${2:-55}; RD=${3:-60}; WATCH=${4:-120}
#   ABA_APP=<.app> (default ~/blade2-work/out/macapp/Blade II.app), ABA_OUT=<dir> (default ~/blade2-work/run/aba), ABA_SAVEDIR=<scratch save copy> (default ~/blade2-work/run/save_play),
#   ABA_LAG=1 adds DM_NVN_LAGLOG=<out>/lag.txt; other BladeII processes seen during the run are logged to <out>/overlap.txt (a non-empty file = timing-contaminated run)
W=$HOME/blade2-work; APP=${ABA_APP:-"$W/out/macapp/Blade II.app"}; O=${ABA_OUT:-$W/run/aba}/$TAG; SP=${ABA_SAVEDIR:-$W/run/save_play}
CROP=${ABA_CROP:-2560:1400:640:248}
# One game instance at a time (two agents share this Mac; a second instance kills / perturbs the first): mkdir lock ~/blade2-work/run/gpu.lock, stale lock (dead pid) removed
LK=$W/run/gpu.lock
while ! mkdir "$LK" 2>/dev/null; do p=$(cat "$LK/pid" 2>/dev/null); if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then rm -rf "$LK"; continue; fi; sleep 3; done
echo $$ > "$LK/pid"; echo "$TAG" > "$LK/tag"; trap 'rm -rf "$LK"' EXIT
while pgrep -x BladeII > /dev/null; do sleep 3; done   # an instance started without the lock (older script): let it finish
rm -rf "$O"; mkdir -p "$O"
caffeinate -ims -t $((WATCH + 30)) &   # 只防系统睡眠；不带 -d/-u：那两个会点亮显示器，经 HDMI-CEC 把电视切过来 | keep the system awake only; no -d/-u: those wake the display and switch the TV over HDMI-CEC
CAF=$!
rm -rf "$SP"; cp -r "${ABA_SAVESRC:-$W/run/save}" "$SP"   # ABA_SAVESRC=<存档目录>：换一份起始存档（如通关存档，开局进大厅而不是新游戏） | ABA_SAVESRC=<save dir>: start from another save (e.g. a cleared-stage save that opens in the lobby instead of a new game)
K="A@6,A@10,A@14,A@18,A@24,A@30,A@36"
for i in $(seq 0 90); do K="$K,A@$(echo "55+$i*0.7" | bc)"; done
K="$K,LSU@70+2,LSR@85+2"
[ -z "${ABA_KEYS_EXTRA:-}" ] || K="$K,$ABA_KEYS_EXTRA"
[ -z "${ABA_KEYS:-}" ] || K="$ABA_KEYS"   # 整段按键脚本替换（探索菜单用） | replace the whole key script (for exploring menus)   # 追加按键（如 PLUS@120 打开暂停菜单看装备界面） | extra keys appended (e.g. PLUS@120 to open the pause menu and look at the equipment screen)
env | grep '^DM_\|^MTL_' | sort > "$O/env.txt"
[ -n "${ABA_LAG:-}" ] && export DM_NVN_LAGLOG="$O/lag.txt"
env DM_AUDIO=${DM_AUDIO:-0} ${ABA_TOP:+DM_WINDOW_TOP=1} ${ABA_TOP:-DM_WINDOW_BACK=1} DM_APP_ROMFS=$W/data/switch/01009cc00e224000/v131072/Update/RomFS DM_APP_SUPPORT=$W/run/appsup DM_SAVE="$SP" \
DM_WATCH=$WATCH DM_KEYS="$K" DM_NVN_REGLUM="0,0,1,1:$O/reglum.txt" DM_NVN_DRAWSET=${ABA_DRAWSET:+$O/drawset.txt} \
  "$APP/Contents/MacOS/BladeII" > "$O/stdout.log" 2> "$O/stderr.log" &
UPNS='import time;print(time.clock_gettime_ns(time.CLOCK_UPTIME_RAW))'
python3 -c "$UPNS" > "$O/t_launch.txt"
PID=$!
date +%s > "$O/t_launch_wall.txt"
( while kill -0 $PID 2>/dev/null; do pgrep -x BladeII | grep -v "^$PID\$" | sed "s/^/$(date +%s) other pid /" >> "$O/overlap.txt"; sleep 2; done ) &
if [ "$RD" -gt 0 ]; then
  sleep "$RS"
  python3 -c "$UPNS" > "$O/t_rec.txt"
  screencapture -x -V "$RD" "$O/rec.mov" & SC=$!; ( sleep $((RD + 20)); kill -INT $SC 2>/dev/null ) & wait $SC   # 显示器睡着时（无 -d）screencapture 不会自己结束，到点掐掉 | with the display asleep (no -d) screencapture never ends by itself: stop it when due
fi
wait $PID
kill $CAF 2>/dev/null
cp "$HOME/Library/Logs/Blade II Mobile/host.log" "$O/host.log" 2>/dev/null
grep -a "显示节拍 至" "$O/host.log" | sed 's/；提交领先.*//' > "$O/link.txt"
grep -a "【红】" "$O/host.log" > "$O/red.txt"
if [ -f "$O/rec.mov" ]; then python3 "$HERE/tools/aba_detect.py" "$O/rec.mov" "$CROP" --raw "$O/small.raw" --toff "$(python3 -c "print(($(cat "$O/t_rec.txt")-$(cat "$O/t_launch.txt"))/1e9)")" > "$O/screen.txt"; head -1 "$O/screen.txt"; fi
python3 "$HERE/tools/aba_detect.py" --reglum "$O/reglum.txt" --t0 "$(cat "$O/t_launch.txt")" > "$O/reglum_aba.txt"; head -1 "$O/reglum_aba.txt"
echo "link: $(wc -l < "$O/link.txt") lines; last: $(tail -1 "$O/link.txt")"; echo "red lines: $(wc -l < "$O/red.txt")"
[ -s "$O/drawset.txt" ] && python3 "$HERE/tools/drawset_aba.py" "$O/drawset.txt" > "$O/drawset_aba.txt" && head -1 "$O/drawset_aba.txt"
rm -f "$O/drawset.txt.gz"; [ -s "$O/drawset.txt" ] && gzip -f "$O/drawset.txt"
