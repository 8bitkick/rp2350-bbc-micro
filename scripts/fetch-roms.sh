#!/bin/sh
# Downloads the ROMs the firmware builds in, from beebjit's ROM set, into roms/: the Model B's MOS 1.20,
# BASIC II and DFS 0.90, and (unless --beeb) the Master 128's MOS 3.20 set. Files already there are kept.
#   scripts/fetch-roms.sh [--beeb]
set -e
cd "$(dirname "$0")/.."
BASE=https://raw.githubusercontent.com/scarybeasts/beebjit/master/roms
get() {
  [ -f "$2" ] && return
  mkdir -p "$(dirname "$2")"
  echo "fetching $2"
  curl -fsSL --retry 5 --retry-delay 5 -o "$2.part" "$BASE/$1" && mv "$2.part" "$2"
}
get os12.rom roms/os/os12.rom
get basic.rom roms/general/basic2.rom
get DFS-0.9.rom roms/general/dfs09.rom
if [ "$1" != --beeb ]; then
  for r in mos dfs viewsht edit basic4 adfs view terminal; do
    get mos3.20/$r.rom roms/mos3.20/$r.rom
  done
fi
