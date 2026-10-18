#ifndef MACHINE_IF_H
#define MACHINE_IF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the common code (the command layer, src/common/api; the USB channel; a board's network)
// asks of the emulated machine. Each machine provides these: src/machines/beeb/presto_machine.c
// and presto_beebjit.c; src/machines/master/presto_machine.c and main.c. All are called on core 0,
// between the machine's own work (from frontend_frame / frontend_poll, frontend.h).
// Disc changes are in presto_sd.h (presto_disc_insert, presto_disc_name, presto_sd_eject_disc).

// "beeb" (BBC Model B) or "master" (BBC Master 128): /api/status's "machine"
const char *machine_name(void);

// a key by its place in the BBC's keyboard matrix (row << 4 | column)
void machine_key(int code, bool down);
// a key from the USB channel: an SDL scancode (tools/presto.py key)
void machine_key_sdl(uint32_t scancode, bool down);
// BREAK
void machine_break(void);
// hold SHIFT for the next second, so that a reset boots the disc (as SHIFT+BREAK)
void machine_boot_shift(void);
// run as fast as possible (every frame still drawn)
bool machine_turbo_get(void);
void machine_turbo_set(bool on);
// the speed and load figures on the panel: false from machine_overlay_has if the build has none
bool machine_overlay_has(void);
bool machine_overlay_get(void);
void machine_overlay_set(bool on);
// each core's load and the emulation speed, in percent
void machine_perf_get(unsigned *core0, unsigned *core1, unsigned *speed);
// the "STATS ..." line (tools/presto.py stats)
void machine_print_stats(void);
// the keyboard's LEDs: bit 0 CAPS LOCK, 1 SHIFT LOCK, 2 the cassette motor (each set when lit)
unsigned machine_leds(void);
// a USB "!" command only this machine has (e.g. the Model B's crtc, state, peek): false if it
// isn't one
bool machine_usb_command(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif
