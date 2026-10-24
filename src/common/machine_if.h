#ifndef MACHINE_IF_H
#define MACHINE_IF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the common code (the command layer, src/common/api; the USB channel; a board's network)
// asks of the emulated machine. Each machine provides these: src/machines/beeb/beeb_machine.c
// and beeb_main.c; src/machines/master/master_machine.c and main.c. All are called on core 0,
// between the machine's own work (from frontend_frame / frontend_poll, frontend.h).

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
// the bytes of PSRAM the machine has taken (in use or set aside), for /api/status's "mem"
uint32_t machine_psram_used(void);
// the "STATS ..." line (tools/presto.py stats)
void machine_print_stats(void);
// the keyboard's LEDs: bit 0 CAPS LOCK, 1 SHIFT LOCK, 2 the cassette motor (each set when lit)
unsigned machine_leds(void);
// ---- Drive 0 (src/common/drive.c decides what goes in it)
// put a disc image in drive 0, writeable (whole sectors; dsd: two sides interleaved; NULL: no
// disc), and reset to boot it if boot. The image need last only until the call returns.
void machine_drive_load(const uint8_t *image, uint32_t size, bool dsd, bool boot);
// how many writes drive 0 has taken (a count that only goes up)
uint32_t machine_drive_writes(void);
// a sector of drive 0's disc (side 0) as it is now, if it may have been written since the last
// machine_drive_clean (false: it hasn't, out is left alone)
bool machine_drive_sector(int track, int sector, uint8_t *out);
void machine_drive_clean(void);

// a program being installed (app.c): the machine paused, and the screen given to what's going on
// and how far (of 1000), as often as that changes (a board that can't draw it keeps the machine's
// last picture); permille < 0: over without a restart, the machine carries on
void machine_install_screen(const char *what, int permille);

// the machine's framebuffer: *rows rows of the board's display format (board.h), which it doesn't
// draw in until it runs, so the board borrows it before then (frontend_start: its own screens, its
// network's buffers), giving it back black. Called once, once the machine is set up.
uint32_t *machine_framebuffer(int *rows);

// a USB "!" command only this machine has (e.g. the Model B's crtc, state, peek): false if it
// isn't one
bool machine_usb_command(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif
