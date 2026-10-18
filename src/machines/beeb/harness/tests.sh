#!/bin/sh
# Regression checks for the beebjit port, run in a harness build:
#   src/machines/beeb/harness/tests.sh [a32|qemu|native]   (default a32; run from anywhere)
# Compares the panel framebuffer (as the Presto would show it) against golden
# checksums, so rendering changes must give identical pixels, and checks text
# output: MOS boot, BASIC over RS423, NMOS decimal ADC/SBC edge cases.
# GOLDEN=update rewrites harness/golden.txt (a wire build's entries end "-wire").
# JIT=1 runs the JIT (-jit). JIT mode turns off beebjit's video memory sync
# (see beebjit_embedded.c), so it's checked against the interpreter's results
# without it too: NOSYNC=1 runs the interpreter that way, and
# GOLDEN=update NOSYNC=1 records those ("-nosync" entries).
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
TARGET=${1:-a32}
OUT=$(mktemp -d)
GOLD="$HERE/golden.txt"
cd "$ROOT"
case "$TARGET" in
  qemu) RUN="$HERE/run_qemu.sh" ;;
  *) RUN="$ROOT/build-$TARGET/harness" ;;
esac
FAIL=0
MODEARGS=""
SUFFIX=""
if [ "$JIT" = 1 ]; then
  MODEARGS="-jit"; SUFFIX="-nosync"
elif [ "$NOSYNC" = 1 ]; then
  MODEARGS="-opt video:border-chars=0,video:no-deinterlace-bitmap,video:no-memory-sync"; SUFFIX="-nosync"
fi
# A wire build runs a rendered frame in slices (video.c, BEEBJIT_VIDEO_SLICE_LINES), so a frame
# caught mid-update can differ from the other builds': its goldens are "-wire" entries.
case "$TARGET$ELF" in *wire*) SUFFIX="$SUFFIX-wire" ;; esac
run() { $RUN "$@" $MODEARGS; }

check_text() { # name expected-substring file
  if grep -qF -- "$2" "$3"; then echo "ok    $1"; else echo "FAIL  $1 (no '$2')"; FAIL=1; fi
}
check_frames() { # name output-file: the hash over every frame
  sum=$(grep -a "frame hash" "$2" | sed 's/.*: //')
  set -- "$1$SUFFIX" "$2"
  if [ "$GOLDEN" = update ]; then
    grep -v "^$1-frames " "$GOLD" 2>/dev/null > "$GOLD.new"; echo "$1-frames $sum" >> "$GOLD.new"; mv "$GOLD.new" "$GOLD"
    echo "set   $1 frames $sum"
  elif grep -q "^$1-frames $sum\$" "$GOLD"; then echo "ok    $1 every frame"
  else echo "FAIL  $1 frames ($sum)"; FAIL=1; fi
}
check_image() { # name ppm
  sum=$(md5sum "$2" | cut -d' ' -f1)
  set -- "$1$SUFFIX" "$2"
  if [ "$GOLDEN" = update ]; then
    grep -v "^$1 " "$GOLD" 2>/dev/null > "$GOLD.new"; echo "$1 $sum" >> "$GOLD.new"; mv "$GOLD.new" "$GOLD"
    echo "set   $1 $sum"
  elif grep -q "^$1 $sum\$" "$GOLD"; then echo "ok    $1 image"
  else echo "FAIL  $1 image ($sum)"; FAIL=1; fi
}

# 1. Boot to BASIC: MODE 7 screen text and panel image.
run -cycles 6000000 -mode7 -frame-crc -ppm "$OUT/boot.ppm" > "$OUT/boot.txt" 2>&1
check_text boot "|BBC Computer 32K" "$OUT/boot.txt"
check_text boot-basic "|BASIC" "$OUT/boot.txt"
check_image boot "$OUT/boot.ppm"
check_frames boot "$OUT/boot.txt"

# 2. BASIC over RS423, MODE 2 graphics.
run -cycles 12000000 -frame-crc -ppm "$OUT/mode2.ppm" -type \
  'PRINT 6*7\nMODE 2\nFOR I%=0 TO 7:GCOL 0,I%:MOVE I%*160,0:MOVE I%*160+150,0:PLOT 85,I%*160,900:PLOT 85,I%*160+150,900:NEXT\nCOLOUR 3:PRINT "HELLO"\n' \
  > "$OUT/mode2.txt" 2>&1
check_text rs423 "42" "$OUT/mode2.txt"
check_image mode2 "$OUT/mode2.ppm"
check_frames mode2 "$OUT/mode2.txt"

# 2b. Teletext: colours, double height, graphics, flashing, held graphics.
run -cycles 12000000 -frame-crc -ppm "$OUT/tt.ppm" -type \
  'MODE 7\nPRINT CHR$129;"RED ";CHR$130;"GREEN ";CHR$141;"DOUBLE":PRINT CHR$141;"DOUBLE"\nPRINT CHR$151;"#5j?";CHR$154;"#5j?";CHR$158;CHR$146;"x";CHR$136;"FLASH";CHR$157;CHR$132;"BG"\n' \
  > "$OUT/tt.txt" 2>&1
check_image teletext "$OUT/tt.ppm"
check_frames teletext "$OUT/tt.txt"

# 2c. Teletext under load: a scrolling screen of control codes (colour, double
#     height, graphics, flash, background), through the character-run paths.
run -cycles 14000000 -frame-crc -ppm "$OUT/tt2.ppm" -type \
  '10 MODE 7\n20 P.CHR$129;"abc";CHR$141;"DH";CHR$151;"#j";CHR$136;"F";CHR$157;CHR$132;"x";\n30 GOTO 20\nRUN\n' \
  > "$OUT/tt2.txt" 2>&1
check_image teletext-scroll "$OUT/tt2.ppm"
check_frames teletext-scroll "$OUT/tt2.txt"

# 2d. Mosaics beside text, single and double height, scrolling by one line a
#     round so double-height pairs fall on both row parities: contiguous and
#     separated graphics, held graphics, graphics colours, back to text.
run -cycles 24000000 -frame-crc -ppm "$OUT/tt3.ppm" -type \
  '10 MODE 7\n20 G$=CHR$255+CHR$53+CHR$106+CHR$127+CHR$32+CHR$63+CHR$124\n25 FOR I%=1 TO 5\n30 PRINT CHR$145;G$;CHR$135;"TEXT";CHR$146;G$\n40 PRINT CHR$154;CHR$145;G$;CHR$153;CHR$146;G$;CHR$135;"ALPHA"\n50 PRINT CHR$147;CHR$158;G$;CHR$131;CHR$148;G$;CHR$159;CHR$149;G$\n60 PRINT CHR$141;CHR$145;G$;CHR$135;"DBL";CHR$154;CHR$146;G$\n70 PRINT CHR$141;CHR$145;G$;CHR$135;"DBL";CHR$154;CHR$146;G$\n80 PRINT CHR$141;CHR$158;CHR$150;G$;CHR$131;CHR$151;G$;CHR$159;"END"\n90 PRINT CHR$141;CHR$158;CHR$150;G$;CHR$131;CHR$151;G$;CHR$159;"END"\n95 PRINT "ODD ";I%\n96 NEXT\n100 GOTO 100\nRUN\n' \
  > "$OUT/tt3.txt" 2>&1
check_image teletext-mosaics "$OUT/tt3.ppm"
check_frames teletext-mosaics "$OUT/tt3.txt"

# 3. NMOS decimal mode, invalid BCD included (Exile's loader relies on it):
#    &92+&6E+C -> &67 C=1; &90+&90 -> &80 C=1; &00-&01 -> &99 C=0.
run -cycles 16000000 -type \
  '10P%=&900\n20[OPT 0\n30SED:SEC:LDA #&92:ADC #&6E:STA &70:PHP:PLA:STA &71\n40CLC:LDA #&90:ADC #&90:STA &72:PHP:PLA:STA &73\n50SEC:LDA #&00:SBC #&01:STA &74:PHP:PLA:STA &75\n60CLD:RTS:]\n70CALL &900:PRINT ~?&70;" ";?&71 AND 1;" ";~?&72;" ";?&73 AND 1;" ";~?&74;" ";?&75 AND 1\nRUN\n' \
  > "$OUT/bcd.txt" 2>&1
check_text bcd "67 1 80 1 99 0" "$OUT/bcd.txt"

# 4. Flicker-free Elite from disc, if it's there (scripts/fetch-elite.sh).
if [ -f discs/elite-disc-flicker-free-sth.ssd ]; then
  run -disc discs/elite-disc-flicker-free-sth.ssd -autoboot -cycles 40000000 \
    -frame-crc -ppm "$OUT/elite.ppm" > "$OUT/elite.txt" 2>&1
  check_image elite "$OUT/elite.ppm"
  check_frames elite "$OUT/elite.txt"
fi

# 5. Register-heavy raster demos (Bitshifters'), if they're there: every
#    hardware register access is compiled into JIT code (encoded callbacks).
find_disc() {
  for d in "discs/$1"; do
    [ -f "$d" ] && { echo "$d"; return; }
  done
}
D=$(find_disc bs-wobble-colours.ssd)
if [ -n "$D" ]; then
  run -disc "$D" -autoboot -cycles 30000000 -fake-time -frame-crc \
    -ppm "$OUT/wobble.ppm" > "$OUT/wobble.txt" 2>&1
  check_image wobble "$OUT/wobble.ppm"
  check_frames wobble "$OUT/wobble.txt"
fi
D=$(find_disc bs-bar-beeb.ssd)
if [ -n "$D" ]; then
  run -disc "$D" -type '*RUN BARBEEB\n' -cycles 30000000 -fake-time -frame-crc \
    -ppm "$OUT/barbeeb.ppm" > "$OUT/barbeeb.txt" 2>&1
  check_image barbeeb "$OUT/barbeeb.ppm"
  check_frames barbeeb "$OUT/barbeeb.txt"
fi

rm -rf "$OUT"
[ $FAIL = 0 ] && echo "all passed" || echo "FAILURES"
exit $FAIL
