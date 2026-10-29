#!/bin/sh
# All three machines and the launcher in one .uf2 (docs/dual-boot.md):
#
#   scripts/build-dual-boot.sh [build root]      (default: build-dual-boot) -> <root>/presto_dual_boot.uf2
#
# SAVE=<disc.ssd> starts the Master's save disc from that copy of its built-in Elite, with games saved on it
# (tools/dual_boot_uf2.py --save: it overwrites the Master's saves on the board).
# APP=<app.bin> puts an app (src/common/app_api.h: elite-agent's presto/app/build.sh makes one) in
# the app slot too.
# builds <root>/launcher, <root>/master, <root>/beeb and <root>/c64, each machine a plain build of its own
# (OS_CMAKE_ARGS, default "-DSOUND=ON -DAMBIENT=ON", is passed to all machines), then joins them with
# tools/dual_boot_uf2.py. Needs the toolchain and the SDK set up (. deps/env.sh, or PICO_SDK_PATH and the
# compiler on the PATH; SDK_CMAKE_ARGS, for all targets: the SDK's own, Windows' prebuilt pioasm and
# picotool, say). NINJA_JOBS limits the parallel jobs (a Raspberry Pi: 2).
# Re-running rebuilds only what changed. Flash it like any .uf2. To go back to one machine, flash
# a plain build made 'absolute' (tools/dual_boot_uf2.py --plain <machine.uf2> <out.uf2>): the boot ROM
# ignores a plain .uf2 over the partition table. The WiFi network, the machine choice and the
# save discs are kept either way.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build-dual-boot}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
ARGS=${OS_CMAKE_ARGS:--DSOUND=ON -DAMBIENT=ON}
J=${NINJA_JOBS:+-j$NINJA_JOBS}

[ -f "$OUT/launcher/build.ninja" ] || cmake -S "$ROOT/src/launcher" -B "$OUT/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release $SDK_CMAKE_ARGS
ninja -C "$OUT/launcher" $J
for m in master beeb c64; do
    # shellcheck disable=SC2086
    [ -f "$OUT/$m/build.ninja" ] || cmake -S "$ROOT" -B "$OUT/$m" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMACHINE=$m $ARGS $SDK_CMAKE_ARGS
    ninja -C "$OUT/$m" $J
done
python3 "$ROOT/tools/dual_boot_uf2.py" "$ROOT/src/launcher/partitions.json" "$OUT/launcher/presto_launcher.uf2" \
    "$OUT/master/presto_master.uf2" "$OUT/beeb/presto_beebjit.uf2" --c64 "$OUT/c64/presto_c64.uf2" ${APP:+--app "$APP"} \
    ${SAVE:+--save "$ROOT/discs/elite-master-flicker-free-sng47.ssd" "$SAVE"} "$OUT/presto_dual_boot.uf2"
