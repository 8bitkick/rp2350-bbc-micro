#!/bin/sh
# From a fresh clone to the Presto firmware: fetches the pinned toolchain and Pico SDK into deps/,
# flicker-free Elite (and with --roms the machines' ROMs), and builds the dual-boot firmware, both machines and
# the launcher in one .uf2 (build-dual-boot/presto_dual_boot.uf2, docs/dual-boot.md), the machine
# chosen on the web panel.
#
#   scripts/setup.sh [--apt] [--roms] [--gxr] [--demos] [--beeb | --master [--disc=NAME]] [--no-build] [--flash]
#
#   --apt        install the host packages first (Debian, Ubuntu, Raspberry Pi OS; uses sudo)
#   --roms       also fetch the ROMs into roms/ (scripts/fetch-roms.sh, from beebjit's ROM set); without
#                it, the ROMs must already be there
#   --gxr        also fetch Acorn's GXR ROM (graphics extension; some BBC Micro Bot snapshots need it)
#   --demos      also fetch Bitshifters' Wobble Colours and Bar Beeb, for the test harness
#   --beeb       the BBC Model B alone (beebjit): builds build/presto_beebjit.uf2
#   --master     the BBC Master 128 alone (Kieran Connell's emulator): builds build-master/presto_master.uf2
#   --disc=NAME  (--master) the disc to build in, from discs/ (default
#                elite-master-flicker-free-sng47.ssd): pop-beeb.ssd, bs-scr-beeb.ssd, bs-twisted.ssd,
#                EvilIn11.ssd, arcadians.ssd, or your own .ssd
#   --no-build   fetch everything but don't build
#   --flash      after building, copy the firmware to a Presto in BOOTSEL mode (the RP2350 drive)
#
# Re-running is safe: anything already fetched is kept. Afterwards, ". deps/env.sh" sets up a shell
# for rebuilding with "scripts/build-dual-boot.sh" (--beeb: "ninja -C build presto_beebjit", --master:
# "ninja -C build-master").
set -e

SDK_TAG=2.2.0
ARM_GNU=14.2.rel1
SDK_TOOLS_REL=v2.2.0-4   # Windows only: raspberrypi/pico-sdk-tools release with pioasm and picotool

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Git Bash on Windows: C:/... paths, which both the shell and the native Windows tools understand
PWD_OPT=""
case "$(uname -s)" in MINGW*|MSYS*) PWD_OPT=-W ;; esac
ROOT=$(cd "$ROOT" && pwd $PWD_OPT)
# DEPS=<dir> scripts/setup.sh keeps the toolchain and SDK elsewhere (outside a synced folder, say);
# deps/env.sh records it, so later runs use it too.
[ -n "$DEPS" ] || DEPS=$(sed -n 's/^export PRESTO_DEPS=//p' "$ROOT/deps/env.sh" 2>/dev/null || true)
DEPS=${DEPS:-$ROOT/deps}
mkdir -p "$DEPS" "$ROOT/deps"
DEPS=$(cd "$DEPS" && pwd $PWD_OPT)
DO_APT=0; DO_ROMS=0; DO_GXR=0; DO_DEMOS=0; DO_BUILD=1; DO_FLASH=0; MACH=both; DISC=; DISC_DEFAULT=elite-master-flicker-free-sng47.ssd
for a in "$@"; do
  case "$a" in
    --apt) DO_APT=1 ;;
    --roms) DO_ROMS=1 ;;
    --gxr) DO_GXR=1 ;;
    --demos) DO_DEMOS=1 ;;
    --beeb) MACH=beeb ;;
    --master) MACH=master ;;
    --disc=*) DISC=${a#--disc=} ;;
    --no-build) DO_BUILD=0 ;;
    --flash) DO_FLASH=1 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "unknown option $a (see --help)"; exit 1 ;;
  esac
done
[ -z "$DISC" ] || [ $MACH = master ] || { echo "--disc needs --master (the dual-boot Master boots Elite)"; exit 1; }
DISC=${DISC:-$DISC_DEFAULT}

say() { printf '\n== %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

if [ $DO_APT = 1 ]; then
  say "Installing host packages"
  sudo apt-get update
  sudo apt-get install -y git cmake ninja-build python3 python3-serial build-essential curl xz-utils
fi

say "Checking host tools"
missing=""
for t in git cmake python3 curl tar; do have $t || missing="$missing $t"; done
if [ -n "$missing" ]; then
  echo "missing:$missing"
  echo "  Debian / Ubuntu / Raspberry Pi OS: scripts/setup.sh --apt"
  echo "  macOS: brew install cmake ninja python  (and Xcode's command line tools)"
  exit 1
fi
GEN="-G Ninja"; have ninja || GEN=""
echo "ok (cmake $(cmake --version | head -1 | awk '{print $3}'), $([ -n "$GEN" ] && echo ninja || echo make))"

# ---- Arm GNU toolchain (the one this project is built and tested with)
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) HOST=x86_64 ;;
  Linux-aarch64|Linux-arm64) HOST=aarch64 ;;
  Darwin-arm64) HOST=darwin-arm64 ;;
  Darwin-x86_64) HOST=darwin-x86_64 ;;
  MINGW*-x86_64|MSYS*-x86_64) HOST=mingw-w64-x86_64 ;;
  *) HOST="" ;;
esac
TOOLCHAIN=$DEPS/arm-gnu-toolchain-$ARM_GNU-$HOST-arm-none-eabi
ARM_URL=https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_GNU/binrel/arm-gnu-toolchain-$ARM_GNU-$HOST-arm-none-eabi
if [ -z "$HOST" ]; then
  have arm-none-eabi-gcc || { echo "no prebuilt Arm toolchain for $(uname -sm): install arm-none-eabi-gcc yourself"; exit 1; }
  TOOLCHAIN=$(dirname "$(dirname "$(command -v arm-none-eabi-gcc)")")
elif [ ! -x "$TOOLCHAIN/bin/arm-none-eabi-gcc" ] && [ ! -x "$TOOLCHAIN/bin/arm-none-eabi-gcc.exe" ]; then
  say "Fetching the Arm GNU toolchain $ARM_GNU ($HOST)"
  if [ "$HOST" = mingw-w64-x86_64 ]; then
    # a .zip with no top-level directory
    curl -fL --progress-bar -o "$DEPS/arm-gnu-toolchain.zip" "$ARM_URL.zip"
    unzip -q -o "$DEPS/arm-gnu-toolchain.zip" -d "$TOOLCHAIN" && rm "$DEPS/arm-gnu-toolchain.zip"
  else
    curl -fL --progress-bar "$ARM_URL.tar.xz" | tar -xJ -C "$DEPS"
  fi
fi

# ---- Windows: prebuilt pioasm and picotool (elsewhere the SDK builds them with the host compiler)
SDK_TOOLS=""
if [ "$HOST" = mingw-w64-x86_64 ]; then
  for tz in pioasm:pico-sdk-tools-$SDK_TAG-x64-win picotool:picotool-$SDK_TAG-a4-x64-win; do
    t=${tz%%:*}; z=${tz#*:}
    if [ ! -f "$DEPS/$t/${t}Config.cmake" ]; then
      say "Fetching $z"
      curl -fL --progress-bar -o "$DEPS/$z.zip" \
        "https://github.com/raspberrypi/pico-sdk-tools/releases/download/$SDK_TOOLS_REL/$z.zip"
      unzip -q -o "$DEPS/$z.zip" "$t/*" -d "$DEPS" && rm "$DEPS/$z.zip"
    fi
    SDK_TOOLS="$SDK_TOOLS -D${t}_DIR=$DEPS/$t"
  done
fi

# ---- Pico SDK
if [ ! -d "$DEPS/pico-sdk/.git" ]; then
  say "Fetching the Pico SDK $SDK_TAG"
  git -c advice.detachedHead=false clone -q --depth 1 -b $SDK_TAG https://github.com/raspberrypi/pico-sdk.git "$DEPS/pico-sdk"
  git -C "$DEPS/pico-sdk" submodule update -q --init --depth 1
fi

cat > "$ROOT/deps/env.sh" <<EOF
# . deps/env.sh: the toolchain and SDK this project builds with
export PRESTO_DEPS=$DEPS
export PICO_SDK_PATH=$DEPS/pico-sdk
export PICO_TOOLCHAIN_PATH=$TOOLCHAIN
export PATH=$(cd "$TOOLCHAIN/bin" && pwd):\$PATH
EOF
. "$ROOT/deps/env.sh"

# ---- Discs and ROMs that aren't part of this repository
if [ ! -f "$ROOT/discs/elite-disc-flicker-free-sth.ssd" ]; then
  say "Fetching flicker-free Elite (Mark Moxon)"
  mkdir -p "$ROOT/discs"
  "$ROOT/scripts/fetch-elite.sh"
fi
if [ $DO_GXR = 1 ] && [ ! -f "$ROOT/roms/general/gxr.rom" ]; then
  say "Fetching the GXR ROM (from beebjit's ROM set)"
  curl -fsSL -o "$ROOT/roms/general/gxr.rom" \
    https://raw.githubusercontent.com/scarybeasts/beebjit/master/roms/gxr.rom
fi
if [ $DO_DEMOS = 1 ]; then
  for d in bs-wobble-colours.ssd bs-bar-beeb.ssd; do
    [ -f "$ROOT/discs/$d" ] && continue
    say "Fetching $d (Bitshifters)"
    curl -fsSL -o "$ROOT/discs/$d" "https://bitshifters.github.io/content/$d"
  done
fi

# ---- ROMs
if [ $DO_ROMS = 1 ]; then
  say "Fetching the ROMs (beebjit's ROM set)"
  "$ROOT/scripts/fetch-roms.sh" $([ $MACH = beeb ] && echo --beeb)
fi
NEED="os/os12.rom general/basic2.rom general/dfs09.rom"
[ $MACH = beeb ] || for r in mos dfs viewsht edit basic4 adfs view terminal; do NEED="$NEED mos3.20/$r.rom"; done
ROMS_MISSING=""
for r in $NEED; do [ -f "$ROOT/roms/$r" ] || ROMS_MISSING="$ROMS_MISSING roms/$r"; done
if [ -n "$ROMS_MISSING" ]; then
  echo "The build needs these ROMs, not found:"
  for r in $ROMS_MISSING; do echo "  $r"; done
  echo "Put them there, or run scripts/setup.sh --roms to download them (see README)."
  [ $DO_BUILD = 1 ] && exit 1
fi

# ---- The Master 128's test discs
if [ $MACH != beeb ]; then
  mkdir -p "$ROOT/discs"
  for u in \
    https://elite.bbcelite.com/versions/flicker_free_elite/elite-master-flicker-free-sng47.ssd \
    https://bitshifters.github.io/content/pop-beeb.ssd \
    https://bitshifters.github.io/content/bs-scr-beeb.ssd \
    https://bitshifters.github.io/content/bs-twisted.ssd \
    https://bitshifters.github.io/content/EvilIn11.ssd; do
    d=$(basename "$u")
    [ -f "$ROOT/discs/$d" ] && continue
    say "Fetching $d"
    curl -fsSL -o "$ROOT/discs/$d" "$u"
  done
  if [ ! -f "$ROOT/discs/arcadians.ssd" ]; then
    say "Fetching arcadians.ssd"
    curl -fsSL -A "Mozilla/5.0" -o "$ROOT/discs/arcadians.ssd" \
      https://www.bbcmicro.co.uk/gameimg/discs/20/Disc002-Arcadians.ssd
  fi
  [ -f "$ROOT/discs/$DISC" ] || { echo "no discs/$DISC"; exit 1; }
fi

[ $DO_BUILD = 1 ] || { say "Fetched everything; build with: . deps/env.sh && cmake ... (see README)"; exit 0; }

# ---- Build
if [ $MACH = both ]; then
  have ninja || { echo "the dual-boot build needs Ninja (or build one machine: --beeb, --master)"; exit 1; }
  say "Building the dual-boot firmware (both machines and the launcher)"
  SDK_CMAKE_ARGS="$SDK_TOOLS" "$ROOT/scripts/build-dual-boot.sh"
  UF2=$ROOT/build-dual-boot/presto_dual_boot.uf2
  BOOTS="Elite on the BBC Master 128 or the Model B (the choice is on the web panel)"
elif [ $MACH = master ]; then
  say "Building presto_master with $DISC"
  cmake -S "$ROOT" -B "$ROOT/build-master" $GEN -DMACHINE=master \
    -DPICO_SDK_PATH="$PICO_SDK_PATH" -DCMAKE_BUILD_TYPE=Release -DBEEB_DISC="$ROOT/discs/$DISC" $SDK_TOOLS >/dev/null
  cmake --build "$ROOT/build-master" --target presto_master
  UF2=$ROOT/build-master/presto_master.uf2
  BOOTS="$DISC on the BBC Master 128"
else
  say "Building presto_beebjit"
  cmake -S "$ROOT" -B "$ROOT/build" $GEN \
    -DPICO_SDK_PATH="$PICO_SDK_PATH" -DCMAKE_BUILD_TYPE=Release $SDK_TOOLS >/dev/null
  cmake --build "$ROOT/build" --target presto_beebjit
  UF2=$ROOT/build/presto_beebjit.uf2
  BOOTS="Elite"
fi
say "Built $UF2"

if [ $DO_FLASH = 1 ]; then
  WINDRIVE=""
  if [ "$HOST" = mingw-w64-x86_64 ]; then
    WINDRIVE=$(powershell.exe -NoProfile -Command \
      "Get-Volume | ? FileSystemLabel -eq 'RP2350' | % DriveLetter" | tr -d '\r' | head -1)
    [ -n "$WINDRIVE" ] && WINDRIVE=/$(echo "$WINDRIVE" | tr A-Z a-z)
  fi
  for d in /media/$USER/RP2350 /run/media/$USER/RP2350 /Volumes/RP2350 $WINDRIVE; do
    if [ -d "$d" ]; then
      cp "$UF2" "$d/" && sync && say "Flashed via $d" && exit 0
    fi
  done
  echo "No RP2350 drive found: hold BOOT and tap RESET on the Presto, then run this again with --flash."
  exit 1
fi

cat <<EOF

To flash: hold BOOT and tap RESET on the Presto, then copy
  $UF2
to the RP2350 drive that appears (or re-run with --flash).

It boots straight into $BOOTS. To use the web control panel, give it your WiFi network once over USB:
  python3 tools/presto.py wifi secrets.py     (a Pimoroni-style secrets.py with WIFI_SSID / WIFI_PASSWORD)
then open http://beeb.local/
EOF
