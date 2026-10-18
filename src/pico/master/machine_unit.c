// Core 0: the core's machine (machine.c includes the 65C12, the VIAs, the
// CRTC, the 1770 and the disc), compiled to run from SRAM (CMakeLists.txt).
// The core's dispatch tables go in SRAM too (.data, copied at boot) rather
// than through the flash cache.
#define CPU6502_TABLE_ATTR __attribute__((section(".data.cpu6502_tables")))
#include "../../master/machine.c"
