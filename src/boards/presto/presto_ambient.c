/*
 * The seven WS2812 LEDs on the Presto's back (GPIO 33, GRB), lit with what's on screen.
 *
 * Their places, as Pimoroni's driver samples for them (presto.cpp), seen from the front: 0 bottom
 * right, 1 right, 2 top right, 3 top, 4 top left, 5 left, 6 bottom left. Each shows the average of
 * its cell of a 3x3 grid over the picture (the middle and the bottom middle have no LED).
 *
 * The panel's framebuffer makes this cheap: a pixel is 4 bits, B, G and R (the BBC's 8 colours,
 * see board.h), so an average is a count of each bit. Every 8th row and one pixel in 8 is
 * plenty (3600 samples). The grid is over the picture rather than the panel, or the BBC's black
 * border dulls everything: the extent of what's lit, over the last 2 s, no smaller than half the
 * panel.
 *
 * A cell's colour is that of its lit pixels, white counting a quarter as much as colours and
 * made more saturated (so colour leads, as ambient light should), as bright as the square root
 * of how much of it is lit (a red sprite on black glows red, not a dull brown), smoothed over
 * about 320 ms (flashing
 * colours and rasters don't flicker), gamma corrected, scaled to the brightness setting, and never
 * below a faint glow.
 *
 * CAPS LOCK and SHIFT LOCK can show too, red as the BBC's own: on the BBC's keyboard CAPS LOCK
 * sits just above SHIFT LOCK at the left, so they're the left LED and the bottom left one.
 *
 * Core 0 sets things up (PIO0 must be switched to GPIO 16-47 before it holds any program) and
 * asks for an update every other frame; core 1 does it when it's idle, from SRAM. The PIO sends
 * the seven words from its joined FIFO, so an update never waits.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "board.h"
#include "ws2812.pio.h"

#define LED_PIN         33
#define NUM_LEDS        7
#define ROW_STEP        8                   // sample every 8th row ...
#define ROW_WORDS       (BOARD_DISPLAY_WIDTH / 8)  // ... and the first pixel of each word
#define GLOW            3                   // the least a LED shows (of 255): a faint glow
#define BOX_UPDATES     50                  // the picture's extent: over this many (2 s)
#define TEST_US         1000000             // !leds test: each LED this long

enum { k_off, k_screen, k_test };

// each LED's cell of the 3x3 grid (row * 3 + column)
static const uint8_t k_led_cell[NUM_LEDS] = { 8, 5, 2, 1, 0, 3, 6 };

static PIO s_pio = pio0;
static int s_sm = -1;
static volatile int s_mode = k_screen;
static volatile int s_mode_before_test = k_screen;
static volatile uint32_t s_bright = 40;     // percent
static volatile bool s_locks_on = false;    // show CAPS LOCK and SHIFT LOCK (off by default)
static volatile uint8_t s_locks;            // bit 0 CAPS LOCK, 1 SHIFT LOCK lit
static volatile uint32_t s_frames;          // core 0's frame count
static uint32_t s_updated_frames;           // at core 1's last update
static volatile uint32_t s_test_start;
static volatile uint32_t s_updates;

static uint8_t s_gamma[256];
static uint8_t s_sqrt[256];
static uint32_t s_level[NUM_LEDS][3];       // smoothed, 0 .. 255 << 8
// the grid's box over the picture: words (columns of 8 pixels) and rows, end exclusive
static uint32_t s_box_w0, s_box_w1 = ROW_WORDS, s_box_y0, s_box_y1 = BOARD_DISPLAY_HEIGHT;
// what's been lit since the box last changed
static uint32_t s_seen_w0 = ~0u, s_seen_w1, s_seen_y0 = ~0u, s_seen_y1, s_box_updates;

void board_leds_init(void) {
    for (int i = 0; i < 256; i++) {
        s_gamma[i] = (uint8_t) lroundf(255.0f * powf(i / 255.0f, 2.2f));
        s_sqrt[i] = (uint8_t) lroundf(255.0f * sqrtf(i / 255.0f));
    }
    // GPIO 33 is beyond PIO0's reach until its base is 16, which can only change while it
    // holds no programs (the panel's packer, after this, uses no pins)
    if (pio_set_gpio_base(s_pio, 16) != PICO_OK) {
        printf("#ambient: PIO0 can't reach GPIO %d\n", LED_PIN);
        return;
    }
    uint offset = pio_add_program(s_pio, &ws2812_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    pio_gpio_init(s_pio, LED_PIN);
    pio_sm_set_consecutive_pindirs(s_pio, (uint) s_sm, LED_PIN, 1, true);
    pio_sm_config c = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, LED_PIN);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float) clock_get_hz(clk_sys) /
                             (800000.0f * (ws2812_T1 + ws2812_T2 + ws2812_T3)));
    pio_sm_init(s_pio, (uint) s_sm, offset, &c);
    pio_sm_set_enabled(s_pio, (uint) s_sm, true);
    for (int i = 0; i < NUM_LEDS; i++) pio_sm_put(s_pio, (uint) s_sm, 0);
}

void board_leds_frame(uint8_t locks) {
    s_locks = locks;
    s_frames++;
    __sev();
}

bool __not_in_flash_func(board_leds_due)(void) {
    return (s_sm >= 0) && ((s_frames - s_updated_frames) >= 2);
}

// the seven colours out, if the PIO has sent the last lot
static void __not_in_flash_func(send)(const uint8_t rgb[NUM_LEDS][3]) {
    if (!pio_sm_is_tx_fifo_empty(s_pio, (uint) s_sm)) return;
    for (int i = 0; i < NUM_LEDS; i++) {
        pio_sm_put(s_pio, (uint) s_sm,
                   ((uint32_t) rgb[i][1] << 24) | ((uint32_t) rgb[i][0] << 16) |
                   ((uint32_t) rgb[i][2] << 8));
    }
}

// CAPS LOCK on the left LED, SHIFT LOCK on the bottom left one, red
static void __not_in_flash_func(show_locks)(uint8_t rgb[NUM_LEDS][3]) {
    if (!s_locks_on) return;
    uint32_t v = (255 * s_bright) / 100;
    if (v < 48) v = 48;
    for (int i = 0; i < 2; i++) {
        if (s_locks & (1 << i)) {
            uint8_t *led = rgb[(i == 0) ? 5 : 6];
            led[0] = (uint8_t) v;
            led[1] = 0;
            led[2] = 0;
        }
    }
}

void __not_in_flash_func(board_leds_update)(void) {
    static uint16_t counts[9][8];           // samples of each colour, in each cell
    static int8_t word_cell[ROW_WORDS];     // each word's column of the grid, or -1
    uint8_t rgb[NUM_LEDS][3];
    s_updated_frames = s_frames;
    s_updates++;

    if (s_mode == k_off) {
        memset(rgb, 0, sizeof(rgb));
        show_locks(rgb);
        send(rgb);
        return;
    }
    if (s_mode == k_test) {
        // each LED in turn, white, the others off, then back to the screen
        uint32_t n = (time_us_32() - s_test_start) / TEST_US;
        memset(rgb, 0, sizeof(rgb));
        if (n < NUM_LEDS) {
            rgb[n][0] = rgb[n][1] = rgb[n][2] = (uint8_t) ((255 * s_bright) / 100);
        } else {
            s_mode = s_mode_before_test;
        }
        send(rgb);
        return;
    }

    memset(counts, 0, sizeof(counts));
    uint32_t bw0 = s_box_w0, bw1 = s_box_w1, by0 = s_box_y0, by1 = s_box_y1;
    for (uint32_t w = 0; w < ROW_WORDS; w++) {
        word_cell[w] = ((w < bw0) || (w >= bw1)) ? -1 : (int8_t) (((w - bw0) * 3) / (bw1 - bw0));
    }
    uint32_t seen_w0 = s_seen_w0, seen_w1 = s_seen_w1, seen_y0 = s_seen_y0, seen_y1 = s_seen_y1;
    for (uint32_t y = ROW_STEP / 2; y < BOARD_DISPLAY_HEIGHT; y += ROW_STEP) {
        const uint32_t *row = (const uint32_t *) board_display_row((int) y);
        if (row == NULL) continue;
        int cell_row = ((y < by0) || (y >= by1)) ? -1 : (int) (((y - by0) * 3) / (by1 - by0));
        bool lit = false;
        for (uint32_t w = 0; w < ROW_WORDS; w++) {
            uint32_t c = row[w] & 7;
            if (c != 0) {
                lit = true;
                if (w < seen_w0) seen_w0 = w;
                if (w >= seen_w1) seen_w1 = w + 1;
            }
            if ((cell_row >= 0) && (word_cell[w] >= 0)) {
                counts[(cell_row * 3) + word_cell[w]][c]++;
            }
        }
        if (lit) {
            if (y < seen_y0) seen_y0 = y - (ROW_STEP / 2);
            if (y >= seen_y1) seen_y1 = y + (ROW_STEP / 2);
        }
    }
    s_seen_w0 = seen_w0; s_seen_w1 = seen_w1; s_seen_y0 = seen_y0; s_seen_y1 = seen_y1;
    if (++s_box_updates >= BOX_UPDATES) {
        // the box becomes what was lit over the last 2 s (nothing lit: it stays), at least half
        // the panel each way
        if (seen_w1 > seen_w0) {
            uint32_t w0 = seen_w0, w1 = seen_w1, y0 = seen_y0, y1 = seen_y1;
            while ((w1 - w0) < (ROW_WORDS / 2)) {
                if (w0 > 0) w0--;
                if (w1 < ROW_WORDS) w1++;
            }
            while ((y1 - y0) < (BOARD_DISPLAY_HEIGHT / 2)) {
                if (y0 > 0) y0--;
                if (y1 < BOARD_DISPLAY_HEIGHT) y1++;
            }
            s_box_w0 = w0; s_box_w1 = w1; s_box_y0 = y0; s_box_y1 = y1;
        }
        s_seen_w0 = ~0u; s_seen_w1 = 0; s_seen_y0 = ~0u; s_seen_y1 = 0;
        s_box_updates = 0;
    }

    uint32_t bright = s_bright;
    for (int i = 0; i < NUM_LEDS; i++) {
        const uint16_t *n = counts[k_led_cell[i]];
        // white pixels count a quarter as much as coloured ones: the colour leads (Elite's
        // dashboard is mostly white bars, and its red would otherwise come out pink-white)
        uint32_t total = 0, red = 0, green = 0, blue = 0;
        for (uint32_t c = 1; c < 8; c++) {
            uint32_t weight = (c == 7) ? 1 : 4;
            if (c & 4) red += n[c] * weight;
            if (c & 2) green += n[c] * weight;
            if (c & 1) blue += n[c] * weight;
        }
        for (uint32_t c = 0; c < 8; c++) total += n[c];
        uint32_t lit = total - n[0];
        uint32_t target[3] = { 0, 0, 0 };
        if (lit != 0) {
            // more saturated (take off 3/4 of the least channel), as bright as the square root
            // of how much is lit
            uint32_t least = red < green ? red : green;
            if (blue < least) least = blue;
            least = (least * 3) / 4;
            red -= least; green -= least; blue -= least;
            uint32_t most = red > green ? red : green;
            if (blue > most) most = blue;
            uint32_t level = s_sqrt[(lit * 255) / total];
            target[0] = (red * level) / most;
            target[1] = (green * level) / most;
            target[2] = (blue * level) / most;
        }
        for (int ch = 0; ch < 3; ch++) {
            // an eighth of the way there each update: about 320 ms
            int32_t d = (int32_t) (target[ch] << 8) - (int32_t) s_level[i][ch];
            s_level[i][ch] = (uint32_t) ((int32_t) s_level[i][ch] + (d / 8));
            uint32_t v = (s_gamma[s_level[i][ch] >> 8] * bright) / 100;
            rgb[i][ch] = (uint8_t) ((v < GLOW) ? GLOW : v);
        }
    }
    show_locks(rgb);
    send(rgb);
}

bool board_leds_ready(void) {
    return s_sm >= 0;
}

void board_leds_get(bool *on, unsigned *bright, bool *locks) {
    *on = (s_mode == k_screen) || ((s_mode == k_test) && (s_mode_before_test == k_screen));
    *bright = (unsigned) s_bright;
    *locks = s_locks_on;
}

void board_leds_set(int on, int bright, int locks) {
    if (on >= 0) {
        if (s_mode == k_test) s_mode_before_test = on ? k_screen : k_off;
        else s_mode = on ? k_screen : k_off;
    }
    if (bright >= 0) s_bright = (bright > 100) ? 100 : (uint32_t) bright;
    if (locks >= 0) s_locks_on = (locks != 0);
}

void board_leds_command(const char *args) {
    unsigned percent;
    while (*args == ' ') args++;
    if (s_sm < 0) {
        printf("LEDS not available\n");
        return;
    }
    if (!strcmp(args, "off")) {
        board_leds_set(0, -1, -1);
    } else if (!strcmp(args, "screen")) {
        board_leds_set(1, -1, -1);
    } else if (!strcmp(args, "locks on")) {
        board_leds_set(-1, -1, 1);
    } else if (!strcmp(args, "locks off")) {
        board_leds_set(-1, -1, 0);
    } else if (!strcmp(args, "test")) {
        s_test_start = time_us_32();
        if (s_mode != k_test) s_mode_before_test = s_mode;
        s_mode = k_test;
        printf("LEDS test: each LED in turn for 1 s, 0 to 6 (expected: bottom right, right, "
               "top right, top, top left, left, bottom left)\n");
    } else if (sscanf(args, "bright %u", &percent) == 1) {
        board_leds_set(-1, (int) percent, -1);
    } else if (*args != '\0') {
        printf("LEDS usage: leds [off|screen|test|bright <percent>|locks on|locks off]\n");
        return;
    }
    printf("LEDS mode=%s bright=%lu%% locks=%s updates=%lu box=%lu..%lu,%lu..%lu\n",
           (s_mode == k_off) ? "off" : (s_mode == k_test) ? "test" : "screen",
           (unsigned long) s_bright, s_locks_on ? "on" : "off", (unsigned long) s_updates,
           (unsigned long) (s_box_w0 * 8), (unsigned long) (s_box_w1 * 8),
           (unsigned long) s_box_y0, (unsigned long) s_box_y1);
}
