#ifndef PRESTO_BOOT_H
#define PRESTO_BOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// boot guard (see boot_guard.c); does nothing unless BOOT_GUARD
#if BOOT_GUARD
void boot_guard_begin(void);       // first thing in main; reports instead if the last boot died
void boot_guard_step(uint32_t step);
void boot_guard_note(uint32_t a, uint32_t b);   // two values a panic report shows as cfsr and bfar
void boot_guard_ok(void);          // the emulator is running
#else
static inline void boot_guard_note(uint32_t a, uint32_t b) { (void) a; (void) b; }
static inline void boot_guard_begin(void) {}
static inline void boot_guard_step(uint32_t step) { (void) step; }
static inline void boot_guard_ok(void) {}
#endif

// The app guard (app.c), in every build: the app's calls marked in the same RAM that survives a
// reset, so a watchdog reset that came while the app was running (a hang, or a fault left to
// the watchdog) is the app's. app_enter before each call into the app (its id), app_leave after;
// app_died, at boot, says whose it was (the id; 0: none, or not the app's).
void boot_guard_app_enter(uint32_t id);
void boot_guard_app_leave(void);
uint32_t boot_guard_app_died(void);

#ifdef __cplusplus
}
#endif

#endif
