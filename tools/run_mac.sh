#!/bin/bash
# Run Blade II under the Mac host. Usage: run_mac.sh <rewrite output dir> [seconds] [extra env...]
#   DM_ROMFS defaults to the extracted Update/RomFS in the workspace; DM_SAVE / DM_TEMP under the workspace run/ dir.
#   Log goes to $W/run/log/host_<timestamp>.log (stderr of the host). DM_HOST_BIN=dmhost_blade_embed runs the embedded-image build (Makefile.blade embed).
#   DM_SYSRES_MB=0 (Blade's npdm SystemResourceSize) keeps UE4 on the plain heap route, so no 16 GB alias region is needed (DM_ALIAS_GB may stay at the 512 MB default).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); OUT=${1:?rewrite output dir}; SECS=${2:-90}; shift; shift 2>/dev/null
W=${DM_ROOT:-$HOME/blade2-work}; export DM_ROOT=$W
export DM_ROMFS=${DM_ROMFS:-$W/data/switch/01009cc00e224000/v131072/Update/RomFS}
export DM_SAVE=${DM_SAVE:-$W/run/save} DM_TEMP=${DM_TEMP:-$W/run/temp}
mkdir -p "$DM_SAVE" "$DM_TEMP" "$W/run/log"
[ -d "$DM_ROMFS/BladeII" ] || { echo "DM_ROMFS=$DM_ROMFS has no BladeII/ directory"; exit 1; }
LOG=$W/run/log/host_$(date +%m%d_%H%M%S).log
echo "log: $LOG"
cd "$HERE/dm/host" || exit 1
env DM_NVN_PAIRS=${DM_NVN_PAIRS:-$W/shpairs.txt} DM_NVN_TEXFACTS=${DM_NVN_TEXFACTS:-$W/texfacts.tsv} DM_SYSRES_MB=${DM_SYSRES_MB:-0} DM_WINDOW=${DM_WINDOW:-1} DM_WATCH="$SECS" DM_DBG=${DM_DBG:-1} DM_IMPORTS=${DM_IMPORTS:-0} "$@" ./${DM_HOST_BIN:-dmhost_blade} "$OUT" main sdk 2> "$LOG"
echo "exit code $?"
