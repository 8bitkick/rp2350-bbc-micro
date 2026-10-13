#!/bin/sh
# Builds the beebjit test harness (harness_main.c) without the board:
#   src/pico/beebjit/harness/build.sh a32    [outdir]  # Cortex-M33 code as a 32-bit Linux
#                                                      # process (Arm64 host with AArch32, e.g. Pi 5)
#   src/pico/beebjit/harness/build.sh qemu   [outdir]  # bare metal for QEMU mps2-an505 (Cortex-M33)
#   src/pico/beebjit/harness/build.sh native [outdir]  # the host's own compiler, for profiling
# a32 and qemu use the same compiler and CPU flags as the RP2350 firmware.
# PACKED4=0 renders ARGB pixels instead of the Presto's packed 4-bit codes.
# WIRE=1 sends video's render calls through the render wire (beebjit_wire.h),
# drained on the same core, and reports the drawing's share.
# Output: build-<target>/harness.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
TARGET=${1:-a32}
OUT=${2:-$ROOT/build-$TARGET}
M33FLAGS="-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp"

case "$TARGET" in
  a32)
    CC=${CC:-arm-none-eabi-gcc}; ARCHFLAGS=$M33FLAGS; HOSTSRC="a32/a32_linux.c"; ASMFLAGS="-DBEEBJIT_ASM_THUMB2=1 -DBEEBJIT_JIT_COMPACT=1 -DBEEBJIT_DISC_COMPACT=1 -DBEEBJIT_MALLOC_BIG=beebjit_embedded_malloc_big"
    # Linux on the Pi 5 has 16K pages: segments must be aligned to them.
    LDFLAGS="-nostartfiles -static -Wl,-z,max-page-size=0x10000 -Wl,-Ttext-segment=0x10000"
    ;;
  qemu)
    CC=${CC:-arm-none-eabi-gcc}; ARCHFLAGS=$M33FLAGS; HOSTSRC="qemu/qemu_an505.c"
    ASMFLAGS="-DBEEBJIT_ASM_THUMB2=1 -DBEEBJIT_JIT_COMPACT=1 -DBEEBJIT_DISC_COMPACT=1 -DBEEBJIT_MALLOC_BIG=beebjit_embedded_malloc_big -DBEEBJIT_THUMB2_JIT_ADDR=0x80000000"
    LDFLAGS="-nostartfiles --specs=rdimon.specs -T$HERE/../qemu/an505.ld -Wl,--no-warn-rwx-segments"
    ;;
  native)
    CC=${CC:-gcc}; ARCHFLAGS=""; HOSTSRC="harness/native_host.c"; LDFLAGS=""
    ;;
  *)
    echo "usage: $0 a32|qemu|native [outdir]"; exit 1
    ;;
esac
mkdir -p "$OUT/obj"

CFLAGS="$ARCHFLAGS \
  -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  -Wno-address-of-packed-member -Wno-format -Wno-char-subscripts -Wno-pointer-to-int-cast \
  -Wno-unused-function -ffunction-sections -fdata-sections \
  -DBEEBJIT_NO_MMU=1 -DBEEBJIT_MALLOC=beebjit_embedded_malloc \
  -DBEEBJIT_ALLOC_CALLER=beebjit_embedded_alloc_caller \
  -DBEEBJIT_REALLOC=beebjit_embedded_realloc -DBEEBJIT_FREE=beebjit_embedded_free \
  -DBEEBJIT_HEADLESS=1 ${ASMFLAGS} -DBEEBJIT_RENDER_PACKED4=${PACKED4:-1} -DBEEBJIT_INTERP_COUNTDOWN32=1 -DBEEBJIT_VIDEO_FAST_RUNS=1 -DBEEBJIT_RENDER_WIRE=${WIRE:-0} ${EXTRA_CFLAGS} \
  -I$ROOT/src/beebjit -I$HERE/.."

SOURCES=$(grep -v '^#' "$HERE/../beebjit_sources.txt")
if [ "$TARGET" = native ]; then
  # Use the null asm backend on any host (upstream would pick x64/arm64).
  SOURCES=$(echo "$SOURCES" | sed -e 's#^asm/asm_common.c#asm/null/asm_common_null.c#' \
    -e 's#^asm/asm_jit.c#asm/null/asm_jit_null.c#' -e 's#^asm/asm_inturbo.c#asm/null/asm_inturbo_null.c#')
fi
OBJS=""
for f in $SOURCES; do
  o="$OUT/obj/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
  $CC $CFLAGS -c "$ROOT/src/beebjit/$f" -o "$o" &
  OBJS="$OBJS $o"
done
for f in os_embedded.c beebjit_embedded.c beebjit_panel.c beebjit_wire.c harness/harness_main.c $HOSTSRC; do
  o="$OUT/obj/$(basename "$f" .c).o"
  $CC $CFLAGS -c "$HERE/../$f" -o "$o" &
  OBJS="$OBJS $o"
done
wait

$CC $CFLAGS $LDFLAGS -Wl,--gc-sections -o "$OUT/harness" $OBJS -lm
if [ "$TARGET" = native ]; then size "$OUT/harness"; else arm-none-eabi-size "$OUT/harness"; fi
