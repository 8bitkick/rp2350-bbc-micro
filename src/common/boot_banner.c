// A bounded power-on banner. The upload staging area is idle until the HTTP
// listener is enabled after this function returns. No permanent framebuffer RAM.
#include "boot_banner.h"
#include "board.h"
#include "drive.h"
#include "machine_if.h"
#include "net_if.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if NIMBUS_TEST
// Optional companion PR: these hooks do not write pairing flash during startup.
extern void presto_bluetooth_boot_poll(void);
extern const char *presto_bluetooth_boot_status(void);
extern const char *presto_keyboard_boot_status(void);
#endif
#include "boot_logo.h"
#ifndef BOOT_BUILD_ID
#define BOOT_BUILD_ID "development"
#endif

// Original compact 5x7 uppercase bitmap alphabet, one row per byte.
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .:-/";
static const uint8_t glyphs[][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14},{0,0,0,0,0,0,0},{0,0,0,0,0,12,12},
 {0,12,12,0,12,12,0},{0,0,0,31,0,0,0},{1,2,2,4,8,8,16}
};
#define ROW_BYTES (BOARD_DISPLAY_ROW_WORDS * 4)
#define BANNER_ROWS 240
_Static_assert(2 * BANNER_ROWS * ROW_BYTES <= DRIVE_IMAGE_MAX, "banner exceeds upload staging");

static void pixel(uint32_t *fb, int x, int y, unsigned colour) {
    if (x < 0 || x >= 480 || y < 0 || y >= BANNER_ROWS) return;
    uint32_t *p = fb + y * BOARD_DISPLAY_ROW_WORDS + x / 8;
    unsigned shift = (x % 8) * 4;
    *p = (*p & ~(15u << shift)) | ((colour & 7u) << shift);

}
static void text(uint32_t *fb, int x, int y, const char *s) {
    for (; *s && x + 10 <= 464; s++, x += 12) {
        const char *g = strchr(alphabet, toupper((unsigned char)*s));
        if (!g) continue;
        for (int yy = 0; yy < 7; yy++) for (int xx = 0; xx < 5; xx++)
            if (glyphs[g-alphabet][yy] & (16 >> xx)) {
                pixel(fb, x+xx*2, y+yy, 7); pixel(fb, x+xx*2+1, y+yy, 7);
            }
    }
}
static void read_status(char *value, size_t size, char *line, size_t line_size) {
#if BOARD_HAS_NET
    net_boot_status(value, size);
#else
    snprintf(value, size, "UNAVAILABLE");
#endif
#if NIMBUS_TEST
    #if BT_KEYBOARD_CLASSIC
    snprintf(line, line_size, "JOYSTICK: %s\nKEYBOARD: %s", presto_bluetooth_boot_status(), presto_keyboard_boot_status());
#else
    snprintf(line, line_size, "JOYSTICK: %s\nKEYBOARD: DISABLED IN BUILD", presto_bluetooth_boot_status());
#endif
#else
    snprintf(line, line_size, "JOYSTICK: DISABLED IN BUILD\nKEYBOARD: DISABLED IN BUILD");
#endif
}
static void status(uint32_t *fb, const char *value, const char *line) {
    // Only rewrite status rows; the logo and build text stay stable.
    for (int y = 110; y < 180; y++) memset(fb + y * BOARD_DISPLAY_ROW_WORDS, 0, ROW_BYTES - 4);
    text(fb, 20, 114, "DHCP IP ADDRESS");
    text(fb, 20, 127, value);
    const char *second = strchr(line, '\n');
    char first[64];
    snprintf(first, sizeof first, "%.*s", second ? (int)(second-line) : (int)strlen(line), line);
    text(fb, 20, 155, first);
    if(second)text(fb, 20, 168, second+1);
}
static void retire_display_table(void) {
    uint32_t frame = board_display_frames();
    while ((uint32_t)(board_display_frames() - frame) < 2) {
        watchdog_update();
        sleep_ms(1);
    }
}
void boot_banner_show(void) {
    // Web machine switches use a watchdog restart: leave their network and
    // controller startup path untouched, and skip recovery restarts too.
    if (watchdog_caused_reboot()) return;
    // Recovery paths without an initialized display/staging area safely skip it.
    uint32_t *fb = (uint32_t *)drive0_staging();
    const uint16_t *const *previous = board_display_get_rows();
    if (!fb || !previous) return;
    const uint16_t **rows = malloc(2 * (BOARD_DISPLAY_HEIGHT + 1) * sizeof *rows);
    if (!rows) return; // Failure must never prevent the emulator from booting.
    const unsigned words = BANNER_ROWS * BOARD_DISPLAY_ROW_WORDS;
    uint32_t *buffers[2] = {fb, fb + words};
    const uint16_t **tables[2] = {rows, rows + BOARD_DISPLAY_HEIGHT + 1};
    memset(fb, 0, 2 * BANNER_ROWS * ROW_BYTES);
    for (int y = 0; y < BANNER_ROWS; y++) {
        fb[(y + 1) * BOARD_DISPLAY_ROW_WORDS - 1] = BOARD_DISPLAY_ROW_END;
        rows[y*2] = rows[y*2+1] = (const uint16_t *)(fb + y * BOARD_DISPLAY_ROW_WORDS);
    }
    rows[480] = NULL;
    for (int y = 0; y < 480; y++) tables[1][y] = (const uint16_t *)(buffers[1] + (y/2) * BOARD_DISPLAY_ROW_WORDS);
    tables[1][480] = NULL;
    for (int y=0; y<48; y++) for (int x=0; x<48; x++) {
        pixel(fb, 16+x*2, 8+y, boot_logo[y*48+x]);
        pixel(fb, 17+x*2, 8+y, boot_logo[y*48+x]);
    }
    text(fb, 132, 20, "RP2350");
    text(fb, 132, 34, "BBC MICRO");
    text(fb, 20, 72, "BUILD " BOOT_BUILD_ID);
    text(fb, 20, 86, __DATE__ " " __TIME__);
    text(fb, 20, 196, "WEB: BEEB.LOCAL");
    text(fb, 20, 216, "STARTING EMULATOR...");
    char value[48], line[128], last_value[48], last_line[128];
    read_status(value, sizeof value, line, sizeof line);
    status(fb, value, line);
    strcpy(last_value, value); strcpy(last_line, line);
    // Never expose an old emulator scanline/cursor while changing row tables.
    board_display_backlight(0);
    board_display_set_rows(rows);
    retire_display_table();
    board_display_backlight(255);
    unsigned active = 0;
    const uint64_t started = time_us_64();
    const uint64_t minimum_until = started + 3000000;
    const uint64_t network_timeout = started + 30000000;
    uint64_t address_until = 0;
    bool wait_for_address = false;
#if BOARD_HAS_NET
    wait_for_address = net_boot_configured();
#endif
    do {
        watchdog_update();
#if BOARD_HAS_NET
        net_boot_poll();
#endif
#if NIMBUS_TEST
        presto_bluetooth_boot_poll();
#endif
        read_status(value, sizeof value, line, sizeof line);
        if (strcmp(value, last_value) || strcmp(line, last_line)) {
            unsigned next = active ^ 1u;
            memcpy(buffers[next], buffers[active], BANNER_ROWS * ROW_BYTES);
            status(buffers[next], value, line);
            board_display_set_rows(tables[next]);
            // The previous buffer must leave DMA before it can be drawn into.
            retire_display_table();
            active = next;
            strcpy(last_value, value); strcpy(last_line, line);
        }
#if BOARD_HAS_NET
        // Start the reading interval only after the IP text has reached scanout.
        if (wait_for_address && !address_until && net_boot_address_ready())
            address_until = time_us_64() + 2000000;
#endif
        uint64_t now = time_us_64();
        bool network_done = !wait_for_address ||
            (address_until ? now >= address_until : now >= network_timeout);
        if (now >= minimum_until && network_done) break;
        sleep_ms(20);
    } while (true);
    board_display_backlight(0);
    board_display_set_rows(previous);
    retire_display_table();
    board_display_backlight(255);
    free(rows);
}

