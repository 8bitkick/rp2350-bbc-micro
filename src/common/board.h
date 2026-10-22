#ifndef BOARD_H
#define BOARD_H

// The board interface: what src/common and src/machines ask of an RP2350 board, and all they
// know of it. A board (src/boards/<board>/) implements these functions and gives the values in
// its board_config.h; docs/porting-a-board.md says how. Nothing here is speculative: it's what the
// Presto needs, named for what it is rather than for the Presto.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Start-up: the board's pins in a safe state (the Presto: its piezo driven off), first thing
void board_init(void);

// ---- The display
// Its framebuffer format, which the machines' renderers draw in (a machine refuses to build for
// a format it doesn't support):
//   BOARD_DISPLAY_PACKED4_ROWS: a table of rows, BOARD_DISPLAY_HEIGHT of them, each
//   BOARD_DISPLAY_ROW_WORDS 16-bit words: BOARD_DISPLAY_WIDTH 4-bit pixels, 8 to a 32-bit word
//   from its low end (bit 0 blue, 1 green, 2 red: the BBC's 8 colours), then BOARD_DISPLAY_ROW_END;
//   scanned out continuously by the board (DMA), so a row's pointer can change at any time.
#define BOARD_DISPLAY_PACKED4_ROWS 1
// board_config.h: BOARD_DISPLAY_FORMAT, BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT,
// BOARD_DISPLAY_ROW_WORDS, BOARD_DISPLAY_ROW_END

// rows[i] is the source for display row i; the DMA reads the table directly, so it must stay
// valid, live in RAM, and have a NULL at rows[BOARD_DISPLAY_HEIGHT]. Entries may be changed at
// any time (pointer writes are atomic).
void board_display_init(const uint16_t *const *rows);
void board_display_set_rows(const uint16_t *const *rows);
void board_display_backlight(uint8_t brightness);
// a row's pixels as byte swapped RGB565 (screenshots); buf holds BOARD_DISPLAY_WIDTH
const uint16_t *board_display_row_rgb565(int row, uint16_t *buf);
// a row as the display shows it, in the board's format
const uint16_t *board_display_row(int row);
// the display's refresh, for a machine that locks its frames to it
uint32_t board_display_frames(void);
uint32_t board_display_frame_time_us(void);     // time_us_32() when this frame's display area began
uint32_t board_display_frame_period_us(void);   // the refresh period
uint32_t board_display_frame_lines(void);       // lines a frame, blanking included
int32_t board_display_scan_row(uint32_t now_us);  // the row being scanned then, from this frame's
                                                  // display area (< 0: before it; >= height: after)
void board_display_debug(void);                 // "!lcd": the scanout's state
// the display's frame interrupt, since boot: the most it ran late (us), the frames whose rows were
// out of step (the picture rolled), the frames it missed, the frames it ran too late in to check
void board_display_counters(uint32_t *late_max_us, uint32_t *rolled, uint32_t *missed, uint32_t *late);

// ---- Big memory (the Presto: 8 MB of PSRAM, through the XIP cache at 0x11000000). The
// machines lay it out themselves: the Model B's JIT code goes at its bottom.
size_t board_psram_init(void);      // set it up: its size in bytes (0: none, or not working)
size_t board_psram_size(void);
uint8_t *board_psram(void);         // its start, or NULL

// ---- Audio out (-DSOUND): board_config.h's BOARD_AUDIO_PWM_PIN, a PWM output (src/common/sound.c
// drives it as a 10-bit DAC at 244 kHz, fed by DMA)

// ---- Storage: an SD card (BOARD_HAS_SD), where disc images are kept
// print the .ssd / .dsd files on the card ("DISC <name> <bytes>" lines, then "DISCS <count>")
void board_sd_list_discs(void);
// load a disc from the card into drive 0 (drive.h), and reset to boot it
bool board_sd_load_disc(const char *name);
// call fn for each .ssd / .dsd on the card; the count, or -1 with no card
int board_sd_each_disc(void (*fn)(const char *name, uint32_t size, void *ctx), void *ctx);
// the same a disc at a time, across calls (a reply made as it goes out): begin (NULL: no card), then next
// until it says there are no more, then end. Each listing its own; one ends early if the card is mounted
// again meanwhile (another card operation)
void *board_sd_discs_begin(void);
bool board_sd_discs_next(void *it, char *name, uint32_t name_size, uint32_t *size);
void board_sd_discs_end(void *it);
// write an image to the card's root (a plain .ssd/.dsd name)
bool board_sd_save(const char *name, const uint8_t *data, uint32_t size);
// "!put <name> <size>": receive a disc image over USB and write it to the card
void board_sd_put(const char *args);

// ---- LEDs lit with what's on screen (BOARD_HAS_LEDS, -DAMBIENT; the Presto: seven on its back)
// on core 0 at boot, before the display (the Presto's needs PIO0 before anything loads a program)
void board_leds_init(void);
// each emulated frame, on core 0: the keyboard's lock LEDs (bit 0 CAPS LOCK, 1 SHIFT LOCK lit)
void board_leds_frame(uint8_t locks);
// on core 1, when it's idle: true if an update is due (then call board_leds_update)
bool board_leds_due(void);
void board_leds_update(void);
bool board_leds_ready(void);        // whether they started
// the settings: the screen's colours on, the brightness (percent), CAPS LOCK / SHIFT LOCK shown
void board_leds_get(bool *on, unsigned *bright, bool *locks);
void board_leds_set(int on, int bright, int locks);      // -1 leaves one as it is
// "!leds [off|screen|test|bright <percent>|locks on|locks off]": the state ("LEDS ..."), or change it
void board_leds_command(const char *args);

// ---- A network (BOARD_HAS_NET): net_if.h
// ---- The flash layout: board_config.h's BOARD_FLASH_LAYOUT, through mem.h

#ifdef __cplusplus
}
#endif

#endif
