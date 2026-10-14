#!/bin/bash
# Blade II: The Return of Evil (Switch, title 01009cc00e224000, update v131072 = 2.0.0, NintendoSDK 7.4):
# take the updated program files (ExeFS: main / sdk / subsdk0 / rtld / main.npdm) and the assets (RomFS: UE4 paks + movies)
# out of YOUR OWN game dump and put them into the workspace. Nothing is copied anywhere else or uploaded.
# Usage: extract_game.sh <game dump .xci or .nsp> <prod.keys>
# Output: $DM_ROOT/data/switch/01009cc00e224000/v131072/Update/{ExeFS,RomFS}   (DM_ROOT defaults to ~/blade2-work)
# Needs: hactool (https://github.com/SciresM/hactool, `make`), on PATH or HACTOOL=<path>. About 10 GB free disk.
# Differences from d2r-mobile's dm/tools/extract_game.sh (same structure, adapted):
#   · title id / version / SDK of this game; the update program NCA (BKTR) is layered on the base NCA of the same title
#   · prod.keys dumps from some tools carry a few malformed lines (34-hex-digit mariko_master_kek_source_*): hactool aborts on
#     them, so a sanitized copy is written next to the output (only well-formed lines are kept; nothing is added)
#   · version check = the NSO build ids (bytes 0x40..0x50 of each NSO), not sha256 of the whole file
set -u
[ $# -eq 2 ] || { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
PKG=$1; KEYS=$2; DM_ROOT=${DM_ROOT:-$HOME/blade2-work}; TITLE=01009cc00e224000; OUT=$DM_ROOT/data/switch/$TITLE/v131072/Update
die() { echo "✗ $*" >&2; exit 1; }
HT=${HACTOOL:-$(command -v hactool || true)}; [ -n "$HT" ] && [ -x "$HT" ] || die "hactool not found: build it and put it on PATH, or set HACTOOL=<path>"
[ -f "$PKG" ] || die "no such game dump: $PKG"; [ -f "$KEYS" ] || die "no such key file: $KEYS"
[ ! -e "$OUT" ] || die "$OUT already exists; move it away first to redo"
T=$DM_ROOT/build/cache/extract.$$; mkdir -p "$T/nca" || die "cannot create $T"; trap 'rm -rf "$T"' EXIT
say() { echo "[$(date +%T)] $*"; }
# sanitized key copy: keep only lines whose value is 32 / 64 / 512 / 1056 hex digits
awk -F'=' '{v=$2; gsub(/[ \t\r]/,"",v); if (length(v)==32||length(v)==64||length(v)==512||length(v)==1056) print}' "$KEYS" > "$T/prod.keys"
ht() { "$HT" -k "$T/prod.keys" --disablekeywarns --suppresskeys "$@"; }

say "unpacking the game dump"
case "$PKG" in
  *.xci|*.XCI) ht -t xci --securedir="$T/nca" "$PKG" > "$T/unpack.log" 2>&1 || die "cannot unpack (wrong keys or damaged file), log $T/unpack.log: $(tail -1 "$T/unpack.log")";;
  *.nsp|*.NSP) ht -t pfs0 --outdir="$T/nca" "$PKG" > "$T/unpack.log" 2>&1 || die "cannot unpack (wrong keys or damaged file): $(tail -1 "$T/unpack.log")";;
  *) die "only .xci or .nsp";;
esac
BASE=; UPD=
for f in "$T"/nca/*.nca; do
  case "$f" in *.cnmt.nca) continue;; esac
  info=$(ht -t nca "$f" 2>&1)
  echo "$info" | grep -q "Content Type: *Program" || continue
  echo "$info" | grep -q "^Title ID: *$TITLE" || continue
  if echo "$info" | grep -q "BKTR"; then UPD=$f; else BASE=$f; fi
done
[ -n "$BASE" ] || die "base game program content not found in the dump"
[ -n "$UPD" ] || die "no update in the dump: this project targets update v131072 (SDK 7.4); the base alone (SDK 6.5.2) is not supported"

say "extracting program files and assets (about 4.5 GB)"
mkdir -p "$OUT.part" || die "cannot create $OUT.part"
ht -t nca --basenca="$BASE" --exefsdir="$OUT.part/ExeFS" --romfsdir="$OUT.part/RomFS" "$UPD" > "$T/extract.log" 2>&1 || { rm -rf "$OUT.part"; die "cannot extract the update: $(tail -1 "$T/extract.log")"; }

ok=1
while read -r want name; do
  got=$(xxd -s 0x40 -l 16 -p "$OUT.part/ExeFS/$name" 2> /dev/null)
  if [ "$got" = "$want" ]; then echo "  ✓ $name build id $got"; else echo "  ✗ $name build id ${got:-missing} ≠ $want"; ok=0; fi
done <<IDS
0379ac316b16fbc1a00d2f0dc139ffa9 main
6bfe1085ed54bdf0dcc5d0ea0262b7c3 sdk
1bfa289d84f5608331f88deedf2005c1 subsdk0
IDS
[ -f "$OUT.part/RomFS/BladeII/Content/Paks/BladeII-Switch.pak" ] || { echo "  ✗ RomFS has no BladeII/Content/Paks/BladeII-Switch.pak"; ok=0; }
[ "$ok" = 1 ] || { rm -rf "$OUT.part"; die "wrong version: the dump's update is not v131072"; }
mv "$OUT.part" "$OUT" || die "rename to $OUT failed"
say "done: $OUT"
