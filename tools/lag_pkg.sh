#!/bin/bash
# Repackage ~/blade2-work/out/macapp_lag while holding the run lock (never replace the bundle under a running instance)
W=$HOME/blade2-work; LK=$W/run/gpu.lock
while ! mkdir "$LK" 2>/dev/null; do p=$(cat "$LK/pid" 2>/dev/null); if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then rm -rf "$LK"; continue; fi; sleep 3; done
echo $$ > "$LK/pid"; echo pkg > "$LK/tag"; trap 'rm -rf "$LK"' EXIT
while pgrep -x BladeII > /dev/null; do sleep 3; done
bash "$(dirname "$0")/mac_app_blade.sh" $W/build/out2 $W/out/macapp_lag > /dev/null 2>&1; ls -la "$W/out/macapp_lag/Blade II.app/Contents/MacOS/BladeII"
