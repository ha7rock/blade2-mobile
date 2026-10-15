#!/bin/bash
# One scripted fight run of the Mac app + screen recording + A-B-A detection (screen and game-frame thumbnails).
# Usage: tools/aba_run.sh <tag> [rec_start_s=55] [rec_dur_s=60] [watch_s=120]
#   Extra DM_* env for the app: pass them in the environment of this script (they are inherited), e.g.
#   DM_NVN_SERIAL_FRAME=1 tools/aba_run.sh serial1
# Outputs: ~/blade2-work/run/aba/<tag>/{rec.mov, small.raw, reglum.txt, host.log, stderr.log, screen.txt, reglum_aba.txt, env.txt}
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:?tag}; RS=${2:-55}; RD=${3:-60}; WATCH=${4:-120}
W=$HOME/blade2-work; APP="$W/out/macapp/Blade II.app"; O=$W/run/aba/$TAG
CROP=${ABA_CROP:-2560:1400:640:248}
rm -rf "$O"; mkdir -p "$O"
caffeinate -dimsu -t $((WATCH + 30)) &   # display sleep stops the display link (1 Hz) and ends screencapture early
CAF=$!
rm -rf $W/run/save_play; cp -r $W/run/save $W/run/save_play
K="A@6,A@10,A@14,A@18,A@24,A@30,A@36"
for i in $(seq 0 90); do K="$K,A@$(echo "55+$i*0.7" | bc)"; done
K="$K,LSU@70+2,LSR@85+2"
env | grep '^DM_\|^MTL_' | sort > "$O/env.txt"
DM_WINDOW_TOP=1 DM_APP_ROMFS=$W/data/switch/01009cc00e224000/v131072/Update/RomFS DM_APP_SUPPORT=$W/run/appsup DM_SAVE=$W/run/save_play \
DM_WATCH=$WATCH DM_KEYS="$K" DM_NVN_REGLUM="0,0,1,1:$O/reglum.txt" DM_NVN_DRAWSET=${ABA_DRAWSET:+$O/drawset.txt} \
  "$APP/Contents/MacOS/BladeII" > "$O/stdout.log" 2> "$O/stderr.log" &
UPNS='import time;print(time.clock_gettime_ns(time.CLOCK_UPTIME_RAW))'
python3 -c "$UPNS" > "$O/t_launch.txt"
PID=$!
if [ "$RD" -gt 0 ]; then
  sleep "$RS"
  python3 -c "$UPNS" > "$O/t_rec.txt"
  screencapture -x -V "$RD" "$O/rec.mov"
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
