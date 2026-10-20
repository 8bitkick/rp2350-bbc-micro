#!/bin/sh
# Both machines and the launcher in one .uf2 (docs/mini-os.md):
#
#   scripts/build-os.sh [build root]      (default: build-os) -> <root>/presto_os.uf2
#
# APP=<app.bin> puts an app (src/common/app_api.h: elite-agent's presto/app/build.sh makes one) in
# the app slot too.
# builds <root>/launcher, <root>/master and <root>/beeb, each machine a plain build of its own
# (OS_CMAKE_ARGS, default "-DSOUND=ON -DAMBIENT=ON", is passed to both), then joins them with
# tools/os_uf2.py. Needs the toolchain and the SDK set up (. deps/env.sh, or PICO_SDK_PATH and the
# compiler on the PATH). NINJA_JOBS limits the parallel jobs (a Raspberry Pi: 2).
# Re-running rebuilds only what changed. Flash it like any .uf2. To go back to one machine, flash
# a plain build made 'absolute' (tools/os_uf2.py --plain <machine.uf2> <out.uf2>): the boot ROM
# ignores a plain .uf2 over the partition table. The WiFi network, the machine choice and the
# save discs are kept either way.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build-os}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
ARGS=${OS_CMAKE_ARGS:--DSOUND=ON -DAMBIENT=ON}
J=${NINJA_JOBS:+-j$NINJA_JOBS}

[ -f "$OUT/launcher/build.ninja" ] || cmake -S "$ROOT/src/launcher" -B "$OUT/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$OUT/launcher" $J
for m in master beeb; do
    # shellcheck disable=SC2086
    [ -f "$OUT/$m/build.ninja" ] || cmake -S "$ROOT" -B "$OUT/$m" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMACHINE=$m $ARGS
    ninja -C "$OUT/$m" $J
done
python3 "$ROOT/tools/os_uf2.py" "$ROOT/src/launcher/partitions.json" "$OUT/launcher/presto_launcher.uf2" \
    "$OUT/master/presto_master.uf2" "$OUT/beeb/presto_beebjit.uf2" ${APP:+--app "$APP"} "$OUT/presto_os.uf2"
