// Core 0: the core's machine (machine.c includes the 65C12, the VIAs, the
// CRTC, the 1770 and the disc), compiled to run from SRAM (CMakeLists.txt).
// The core's dispatch tables go in SRAM too (.data, copied at boot) rather
// than through the flash cache.
#define CPU6502_TABLE_ATTR __attribute__((section(".data.cpu6502_tables")))
#if WITH_SOUND
// Each byte the SN76489 takes, with the machine's cycle count, to the sound on core 1
// (src/common/sound.c): the core's own hook for it (machine.c).
#include "sound.h"
#define MACHINE_SOUND_WRITE(m, byte) audio_sn_write((m)->synced, (byte))
#endif
#include "../../cores/kc-beeb/machine.c"
