#!/bin/sh
# Runs the QEMU build of the harness (harness/build.sh qemu) on mps2-an505:
#   src/machines/beeb/harness/run_qemu.sh [harness options...]
# Run it from the repository root: semihosting opens roms/... relative to the
# current directory. -icount shift=0 makes the instruction counts exact and
# runs deterministic. GDB=1 waits for gdb-multiarch on port 1234:
#   gdb-multiarch build-qemu/harness -ex 'target remote :1234'
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
ELF=${ELF:-$ROOT/build-qemu/harness}
# Rebuild the command line for semihosting: arguments with spaces or double
# quotes go in single quotes (so they can't contain single quotes themselves).
APPEND=""
for a in "$@"; do
  case "$a" in
    *" "*|*'"'*) APPEND="$APPEND '$a'" ;;
    *) APPEND="$APPEND $a" ;;
  esac
done
DEBUG=""
[ -n "$GDB" ] && DEBUG="-s -S"
exec qemu-system-arm -M mps2-an505 -display none -serial null -monitor none \
  -semihosting-config enable=on,target=native -icount shift=0 $DEBUG \
  -kernel "$ELF" -append "$APPEND"
