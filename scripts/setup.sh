#!/bin/sh
# From a fresh clone to presto_beebjit.uf2: fetches the pinned toolchain and Pico SDK into deps/,
# downloads flicker-free Elite, and builds the Presto firmware.
#
#   scripts/setup.sh [--apt] [--gxr] [--demos] [--audio] [--no-build] [--flash]
#
#   --apt       install the host packages first (Debian, Ubuntu, Raspberry Pi OS; uses sudo)
#   --gxr       also fetch Acorn's GXR ROM (graphics extension; some BBC Micro Bot snapshots need it)
#   --audio     enable BBC sound on the built-in piezo (omitting this disables audio)
#   --demos     also fetch Bitshifters' Wobble Colours and Bar Beeb, for the test harness
#   --no-build  fetch everything but don't build
#   --flash     after building, copy the firmware to a Presto in BOOTSEL mode (the RP2350 drive)
#
# Re-running is safe: anything already fetched is kept. Afterwards, ". deps/env.sh" sets up a shell
# for rebuilding with "ninja -C build presto_beebjit".
set -e

SDK_TAG=2.2.0
ARM_GNU=14.2.rel1

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=$ROOT/deps
DO_APT=0; DO_GXR=0; DO_DEMOS=0; DO_BUILD=1; DO_FLASH=0; DO_AUDIO=OFF
for a in "$@"; do
  case "$a" in
    --apt) DO_APT=1 ;;
    --gxr) DO_GXR=1 ;;
    --audio) DO_AUDIO=ON ;;
    --demos) DO_DEMOS=1 ;;
    --no-build) DO_BUILD=0 ;;
    --flash) DO_FLASH=1 ;;
    -h|--help) sed -n '2,/^set -e/{ /^set -e/d; p; }' "$0"; exit 0 ;;
    *) echo "unknown option $a (see --help)"; exit 1 ;;
  esac
done

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

mkdir -p "$DEPS"

# ---- Arm GNU toolchain (the one this project is built and tested with)
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) HOST=x86_64 ;;
  Linux-aarch64|Linux-arm64) HOST=aarch64 ;;
  Darwin-arm64) HOST=darwin-arm64 ;;
  Darwin-x86_64) HOST=darwin-x86_64 ;;
  *) HOST="" ;;
esac
TOOLCHAIN=$DEPS/arm-gnu-toolchain-$ARM_GNU-$HOST-arm-none-eabi
if [ -z "$HOST" ]; then
  have arm-none-eabi-gcc || { echo "no prebuilt Arm toolchain for $(uname -sm): install arm-none-eabi-gcc yourself"; exit 1; }
  TOOLCHAIN=$(dirname "$(dirname "$(command -v arm-none-eabi-gcc)")")
elif [ ! -x "$TOOLCHAIN/bin/arm-none-eabi-gcc" ]; then
  say "Fetching the Arm GNU toolchain $ARM_GNU ($HOST)"
  curl -fL --progress-bar \
    "https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_GNU/binrel/arm-gnu-toolchain-$ARM_GNU-$HOST-arm-none-eabi.tar.xz" \
    | tar -xJ -C "$DEPS"
fi

# ---- Pico SDK
if [ ! -d "$DEPS/pico-sdk/.git" ]; then
  say "Fetching the Pico SDK $SDK_TAG"
  git -c advice.detachedHead=false clone -q --depth 1 -b $SDK_TAG https://github.com/raspberrypi/pico-sdk.git "$DEPS/pico-sdk"
  git -C "$DEPS/pico-sdk" submodule update -q --init --depth 1
fi

cat > "$DEPS/env.sh" <<EOF
# . deps/env.sh: the toolchain and SDK this project builds with
export PICO_SDK_PATH=$DEPS/pico-sdk
export PICO_TOOLCHAIN_PATH=$TOOLCHAIN
export PATH=$TOOLCHAIN/bin:\$PATH
EOF
. "$DEPS/env.sh"

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

[ $DO_BUILD = 1 ] || {
  say "Fetched everything; audio selection is not saved until configuration."
  echo "Re-run scripts/setup.sh with your options and without --no-build, or:"
  echo "  . deps/env.sh && cmake -S . -B build -DPRESTO_BEEBJIT_AUDIO=$DO_AUDIO && cmake --build build --target presto_beebjit"
  exit 0
}

# ---- Build
say "Building presto_beebjit (audio: $DO_AUDIO)"
cmake -S "$ROOT" -B "$ROOT/build" $GEN \
  -DPICO_SDK_PATH="$PICO_SDK_PATH" -DCMAKE_BUILD_TYPE=Release \
  -DPRESTO_BEEBJIT_AUDIO="$DO_AUDIO" >/dev/null
cmake --build "$ROOT/build" --target presto_beebjit
UF2=$ROOT/build/src/pico/beebjit/presto_beebjit.uf2
say "Built $UF2"

if [ $DO_FLASH = 1 ]; then
  for d in /media/$USER/RP2350 /run/media/$USER/RP2350 /Volumes/RP2350; do
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

It boots straight into Elite. To use the web control panel, give it your WiFi network once over USB:
  python3 tools/presto.py wifi secrets.py     (a Pimoroni-style secrets.py with WIFI_SSID / WIFI_PASSWORD)
then open http://beeb.local/
EOF
