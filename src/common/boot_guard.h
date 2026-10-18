#ifndef PRESTO_BOOT_H
#define PRESTO_BOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// boot guard (see boot_guard.c); does nothing unless PRESTO_BOOT_GUARD
#if PRESTO_BOOT_GUARD
void presto_boot_begin(void);       // first thing in main; reports instead if the last boot died
void presto_boot_step(uint32_t step);
void presto_boot_note(uint32_t a, uint32_t b);   // two values a panic report shows as cfsr and bfar
void presto_boot_ok(void);          // the emulator is running
#else
static inline void presto_boot_note(uint32_t a, uint32_t b) { (void) a; (void) b; }
static inline void presto_boot_begin(void) {}
static inline void presto_boot_step(uint32_t step) { (void) step; }
static inline void presto_boot_ok(void) {}
#endif

#ifdef __cplusplus
}
#endif

#endif
