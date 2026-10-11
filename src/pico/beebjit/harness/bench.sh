#!/bin/sh
# Instructions per emulated frame (QEMU, -icount shift=0) for a few scenes:
#   src/pico/beebjit/harness/bench.sh [extra harness options]
# At 50 frames a second, real speed on a 250 MHz M33 needs well under
# 5,000,000 instructions a frame (M33 averages more than 1 cycle each).
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
cd "$ROOT"
RUN="$HERE/run_qemu.sh"
per_frame() {
  # steady-state instructions per frame; without rendering there are no
  # frames, so the whole run's rate per 1/50 s instead (marked ~)
  awk '/per frame/ { for (i = 1; i <= NF; i++) if ($i == "avg") pf = $(i + 1) }
       /M per BBC second/ { for (i = 1; i <= NF; i++) if ($i == "M") r = $(i - 1) }
       END { if (pf != "") print pf; else if (r != "") printf "~%d\n", r * 1e6 / 50 }'
}
printf "%-16s %12s\n" scene "instr/frame"
printf "%-16s %12s\n" "MODE 7 prompt" "$($RUN -cycles 6000000 -frame-stats 100 "$@" | per_frame)"
printf "%-16s %12s\n" "MODE 2 bars" "$($RUN -cycles 14000000 -frame-stats 300 "$@" -type \
  'MODE 2\nFOR I%=0 TO 7:GCOL 0,I%:MOVE I%*160,0:MOVE I%*160+150,0:PLOT 85,I%*160,1023:PLOT 85,I%*160+150,1023:NEXT\n' | per_frame)"
if [ -f discs/elite-disc-flicker-free-sth.ssd ]; then
  printf "%-16s %12s\n" "Elite title" "$($RUN -disc discs/elite-disc-flicker-free-sth.ssd -autoboot \
    -cycles 50000000 -frame-stats 1000 "$@" | per_frame)"
fi
