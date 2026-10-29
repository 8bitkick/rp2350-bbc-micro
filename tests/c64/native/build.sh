set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HOST="$(cd "$(dirname "$0")" && pwd)"
mkdir -p /tmp/c64host
CORE="$ROOT/deps/frank-c64/src"
MACHINE="$ROOT/src/machines/c64"
cc -fsanitize=address,undefined -c "$MACHINE/disk_file.c" -o /tmp/c64host/disk.o
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -DFRODO_RP2350 -DPSRAM_MAX_FREQ_MHZ=125 -I"$HOST" -I"$MACHINE" -I"$CORE" -I"$CORE/rp2350" "$HOST/main.cpp" "$HOST/arch.cpp" /tmp/c64host/disk.o "$MACHINE/display.cpp" "$CORE"/rp2350/{C64_rp2350,Prefs_rp2350,ROM_data,SID_rp2350,Tape_stub}.cpp "$CORE"/{CPUC64,VIC,CIA,IEC,Cartridge,REU,1541d64,1541gcr,CPU1541,VIA}.cpp -o /tmp/c64host/test
ASAN_OPTIONS=detect_leaks=0 /tmp/c64host/test "$ROOT/discs/elite-commodore-64-gma86-pal.d64"
