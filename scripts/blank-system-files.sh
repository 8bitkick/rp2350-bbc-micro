#!/bin/sh
# Zero-filled stand-ins in roms/ at the sizes the build expects, so the firmware compiles without the
# real files (CI checks the build only; the firmware won't boot). Files already there are kept.
set -e
cd "$(dirname "$0")/.."
blank() { [ -f "roms/$1" ] && return; mkdir -p "$(dirname "roms/$1")"; head -c "$2" /dev/zero > "roms/$1"; }
blank os/os12.rom 16384
blank general/basic2.rom 16384
blank general/dfs09.rom 8192
for r in mos dfs viewsht edit basic4 adfs view terminal; do blank "mos3.20/$r.rom" 16384; done
