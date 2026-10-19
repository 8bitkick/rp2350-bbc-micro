// The Pimoroni Presto, as the common code and the machines see it (docs/porting-a-board.md):
// everything here is what another board would give differently.
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/flash.h"

#define BOARD_NAME "presto"

// ---- What it has
#define BOARD_HAS_NET 1             // WiFi (presto_net.c: the HTTP server, the web page)
#define BOARD_HAS_SD 1              // an SD card (presto_sd.c)
#define BOARD_HAS_LEDS 1            // seven WS2812 LEDs on its back (presto_ambient.c)

// ---- The display: the ST7701 480x480 panel (presto_lcd.c), packed 4-bit rows. A row is 480
// pixels, then an end of row code (bit 3, E: see st7701_parallel.pio).
#define BOARD_DISPLAY_FORMAT BOARD_DISPLAY_PACKED4_ROWS
#define BOARD_DISPLAY_WIDTH 480
#define BOARD_DISPLAY_HEIGHT 480
#define BOARD_DISPLAY_ROW_WORDS (BOARD_DISPLAY_WIDTH / 8 + 1)
#define BOARD_DISPLAY_ROW_END 0x8u

// ---- Audio out: the piezo, through a MOSFET on GPIO 43 (PWM slice 9; the backlight on 45 is 10).
// -DSOUND_PIN=40 is the Qw/ST socket's SDA instead (slice 8), for an amplifier and a speaker
// (docs/presto/speaker.md).
#define BOARD_PIEZO_PIN 43
#ifndef BOARD_AUDIO_PWM_PIN
#define BOARD_AUDIO_PWM_PIN BOARD_PIEZO_PIN
#endif

// ---- The flash layout (16 MB), the board's one table of it (src/common/mem.h's regions).
// The firmware from the bottom; at the top, down from the end: the WiFi network (a sector), a
// spare sector, the save disc (80 tracks x 10 sectors x 256 bytes), its header (a sector).
#define BOARD_FLASH_SAVE_DATA_SIZE  (80 * 10 * 256)
#define BOARD_FLASH_WIFI_OFFSET     (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define BOARD_FLASH_SPARE_OFFSET    (BOARD_FLASH_WIFI_OFFSET - FLASH_SECTOR_SIZE)
// (-DBOARD_FLASH_SAVE_SHIFT=<bytes>, a multiple of the sector: the save disc that much lower, to
// test saving without touching the real one)
#ifndef BOARD_FLASH_SAVE_SHIFT
#define BOARD_FLASH_SAVE_SHIFT 0
#endif
#define BOARD_FLASH_SAVE_DATA_OFFSET (BOARD_FLASH_SPARE_OFFSET - BOARD_FLASH_SAVE_DATA_SIZE - BOARD_FLASH_SAVE_SHIFT)
#define BOARD_FLASH_SAVE_HEAD_OFFSET (BOARD_FLASH_SAVE_DATA_OFFSET - FLASH_SECTOR_SIZE)
#define BOARD_FLASH_LAYOUT { \
    [MEM_FLASH_SAVE_HEAD] = {BOARD_FLASH_SAVE_HEAD_OFFSET, FLASH_SECTOR_SIZE}, \
    [MEM_FLASH_SAVE_DATA] = {BOARD_FLASH_SAVE_DATA_OFFSET, BOARD_FLASH_SAVE_DATA_SIZE}, \
    [MEM_FLASH_SPARE] = {BOARD_FLASH_SPARE_OFFSET, FLASH_SECTOR_SIZE}, \
    [MEM_FLASH_WIFI] = {BOARD_FLASH_WIFI_OFFSET, FLASH_SECTOR_SIZE}, \
}

#endif
