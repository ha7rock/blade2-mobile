#!/bin/bash
# One summary line per run dir: tools/aba_summary.sh <tag>... ; appends to ~/blade2-work/run/aba/summary.tsv
A=$HOME/blade2-work/run/aba; T=$(cd "$(dirname "$0")" && pwd)
for d in "$@"; do
  D=$A/$d; [ -d "$D" ] || continue
  if [ ! -f "$D/drawset_fight.txt" ] && { [ -f "$D/drawset.txt" ] || [ -f "$D/drawset.txt.gz" ]; }; then
    if [ -f "$D/drawset.txt" ]; then python3 "$T/drawset_aba.py" "$D/drawset.txt" --from 3000 > "$D/drawset_fight.txt"; else gzip -dc "$D/drawset.txt.gz" > /tmp/ds_$$.txt; python3 "$T/drawset_aba.py" /tmp/ds_$$.txt --from 3000 > "$D/drawset_fight.txt"; rm -f /tmp/ds_$$.txt; fi
  fi
  rg=$(head -1 "$D/reglum_aba.txt" 2>/dev/null | sed 's/\[reglum\] //')
  sc=$(head -1 "$D/screen.txt" 2>/dev/null | sed 's/\[screen\] //')
  ds=$(head -1 "$D/drawset_fight.txt" 2>/dev/null | sed 's/\[drawset\] //')
  vs=$(sed -n 3p "$D/drawset_fight.txt" 2>/dev/null | grep -o '68f84649cc703743:[0-9]*\|499e06f881530aaa:[0-9]*' | sed 's/68f84649cc703743/shadow/;s/499e06f881530aaa/prepass/' | tr '\n' ' ')
  env=$(tr '\n' ' ' < "$D/env.txt" 2>/dev/null)
  p50=$(grep -a '计时 至帧' "$D/host.log" 2>/dev/null | tail -1 | grep -o 'p50 [0-9.]*' | head -1)
  red=$(wc -l < "$D/red.txt" 2>/dev/null | tr -d ' ')
  printf '%s\t%s\tgame[%s]\tscreen[%s]\tdrawset_fight[%s %s]\tframe %s\tred %s\n' "$d" "$env" "$rg" "$sc" "$ds" "$vs" "$p50" "$red" | tee -a $A/summary.tsv
done
