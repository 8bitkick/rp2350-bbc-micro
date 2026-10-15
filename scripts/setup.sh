#!/bin/sh
# From a fresh clone to presto_master.uf2: fetches the pinned toolchain and Pico SDK into deps/,
# the BBC Master's MOS 3.20 ROMs into roms/ and the test discs into discs/, and builds the Presto
# firmware with one disc built in (it boots that disc at power on).
#
#   scripts/setup.sh [--apt] [--disc=NAME] [--no-build] [--flash]
#
#   --apt        install the host packages first (Debian, Ubuntu, Raspberry Pi OS; uses sudo)
#   --disc=NAME  the disc to build in, from discs/ (default elite-master-flicker-free-sng47.ssd):
#                pop-beeb.ssd, bs-scr-beeb.ssd, bs-twisted.ssd, EvilIn11.ssd, arcadians.ssd,
#                or your own .ssd
#   --no-build   fetch everything but don't build
#   --flash      after building, copy the firmware to a Presto in BOOTSEL mode (the RP2350 drive)
#
# Re-running is safe: anything already fetched is kept. Afterwards, ". deps/env.sh" sets up a shell
# for rebuilding: cmake -S . -B build -G Ninja -DBEEB_DISC=discs/<disc> && ninja -C build
#
# The ROMs are Acorn's, from beebjit's MOS 3.20 set; the discs are their authors' (see README).
set -e

SDK_TAG=2.2.0
ARM_GNU=14.2.rel1

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=$ROOT/deps
DO_APT=0; DO_BUILD=1; DO_FLASH=0; DISC=elite-master-flicker-free-sng47.ssd
for a in "$@"; do
  case "$a" in
    --apt) DO_APT=1 ;;
    --disc=*) DISC=${a#--disc=} ;;
    --no-build) DO_BUILD=0 ;;
    --flash) DO_FLASH=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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

# ---- ROMs and discs that aren't part of this repository
mkdir -p "$ROOT/roms/mos3.20" "$ROOT/discs"
for r in mos dfs viewsht edit basic4 adfs view terminal; do
  [ -f "$ROOT/roms/mos3.20/$r.rom" ] && continue
  say "Fetching $r.rom (MOS 3.20, from beebjit's ROM set)"
  curl -fsSL -o "$ROOT/roms/mos3.20/$r.rom" \
    "https://raw.githubusercontent.com/scarybeasts/beebjit/master/roms/mos3.20/$r.rom"
done
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

[ $DO_BUILD = 1 ] || { say "Fetched everything; build with: . deps/env.sh && cmake ... (see README)"; exit 0; }

# ---- Build
say "Building presto_master with $DISC"
cmake -S "$ROOT" -B "$ROOT/build" $GEN \
  -DPICO_SDK_PATH="$PICO_SDK_PATH" -DCMAKE_BUILD_TYPE=Release -DBEEB_DISC="$ROOT/discs/$DISC" >/dev/null
cmake --build "$ROOT/build"
UF2=$ROOT/build/presto_master.uf2
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

cat <<EOF2

To flash: hold BOOT and tap RESET on the Presto, then copy
  $UF2
to the RP2350 drive that appears (or re-run with --flash; once this firmware is on the Presto,
python3 tools/presto.py bootsel gets there without the buttons).

It boots straight into $DISC.
EOF2
