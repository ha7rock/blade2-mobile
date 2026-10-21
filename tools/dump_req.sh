#!/bin/bash
# 向正在运行的 Blade II Mac 宿主发「按需抓取」命令（nvn_mtl.m 的 req_poll）：tools/dump_req.sh [all|shot|draws|rts|texraw <序号>…] [等待秒数=8]
#   命令写进 ~/Library/Application Support/Blade II Mobile/dump.req，宿主在 10 帧内读走；然后列出 dump/ 里新出现的文件。玩家停在要看的画面时用。
# Send a capture-on-request command to the running Blade II Mac host (req_poll in nvn_mtl.m): tools/dump_req.sh [all|shot|draws|rts|texraw <serial>...] [wait seconds=8]
#   The command is written to ~/Library/Application Support/Blade II Mobile/dump.req, picked up by the host within 10 frames; the files that appeared in dump/ are then listed.
set -u
SUP="$HOME/Library/Application Support/Blade II Mobile"; REQ="$SUP/dump.req"; OUT="$SUP/dump"
CMD=${1:-all}; shift || true; WAIT=8; ARGS=""
for a in "$@"; do case "$a" in [0-9]*) if [ "$CMD" = texraw ]; then ARGS="$ARGS $a"; else WAIT=$a; fi;; esac; done
pgrep -x BladeII > /dev/null || { echo "🔴 游戏没在运行 | the game is not running"; exit 1; }
mkdir -p "$OUT"; before=$(ls -1 "$OUT" 2>/dev/null | sort)
echo "$CMD$ARGS" > "$REQ"; echo "→ $CMD$ARGS（等 ${WAIT}s）"
for i in $(seq 1 $((WAIT * 2))); do sleep 0.5; [ -f "$REQ" ] || break; done
[ -f "$REQ" ] && { echo "🔴 ${WAIT}s 内宿主没读走命令（这个包没有 req_poll？看 host.log 有没有「[req] 按需抓取」行）"; exit 1; }
sleep 2
after=$(ls -1 "$OUT" | sort); new=$(comm -13 <(echo "$before") <(echo "$after"))
[ -n "$new" ] && { echo "新文件 | new files:"; for f in $new; do ls -la "$OUT/$f" | awk '{print "  " $5 "\t" $9}'; done; } || echo "（dump/ 里还没有新文件；清单与目标转储要等下一回放帧，可再等几秒看 ls \"$OUT\"）"
tail -5 "$HOME/Library/Logs/Blade II Mobile/host.log" | grep "\[req\]" || true
