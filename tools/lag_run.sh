#!/bin/bash
# One isolated A-B-A + lag run: waits until no other BladeII instance runs, then tools/aba_run.sh with ABA_LAG=1 into ~/blade2-work/run/aba/black/<tag>
# Usage: [ENV...] tools/lag_run.sh <tag>   (app: ABA_APP, default ~/blade2-work/out/macapp_lag/Blade II.app)
HERE=$(cd "$(dirname "$0")/.." && pwd); W=$HOME/blade2-work
export ABA_APP=${ABA_APP:-"$W/out/macapp_lag/Blade II.app"} ABA_OUT=$W/run/aba/black ABA_SAVEDIR=$W/run/save_play_lag ABA_LAG=1 DM_PRESENT_LINK=0 ABA_DRAWSET=${ABA_DRAWSET-1}
"$HERE/tools/aba_run.sh" "$1" 55 0 ${ABA_WATCH:-160} > /dev/null 2>&1
python3 "$HERE/tools/lag_stat.py" "$ABA_OUT/$1" | tee -a "$ABA_OUT/summary.txt"
