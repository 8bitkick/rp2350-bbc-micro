// Core 1: the core's renderer (render.c, teletext.c) emitting straight into
// the Presto panel's framebuffer. Compiled to run from SRAM (CMakeLists.txt).
//
// The display row format (board.h: BOARD_DISPLAY_PACKED4_ROWS): 480 4-bit codes, 8 to a word from the
// low nibble (bit 0 blue, 1 green, 2 red), then an end-of-row word. One row
// per BBC scanline: the renderer's two rasters per scanline (they differ
// only in teletext) give the first. 640 picture pixels to 480: of each 4,
// pixels 0, 1 and 2.
#include <stdio.h>
#include <string.h>

#include "pico/stdio_usb.h"

#include "pico.h"
#include "hardware/sync.h"
#include "board.h"
#if BOARD_DISPLAY_FORMAT != BOARD_DISPLAY_PACKED4_ROWS || BOARD_DISPLAY_WIDTH != 480
#error "the Master's renderer draws 640-pixel lines into 480-pixel packed 4-bit rows"
#endif

#define RENDER_INDEXED 1

struct render;
static void panel_emit(struct render *r, uint32_t first, uint32_t count, int32_t vert);
static void panel_palette(const struct render *r);
static void panel_palette_entry(const struct render *r, int logical);
static void panel_frame_done(void);
#define RENDER_EMIT(r, first, count, vert) panel_emit(r, first, count, vert)
#define RENDER_PALETTE_HOOK(r) panel_palette(r)
#define RENDER_PALETTE_ENTRY_HOOK(r, logical) panel_palette_entry(r, logical)
#define RENDER_FRAME_HOOK(r) panel_frame_done()
#define RENDER_LINE2_WANTED(r) 0

#include "../../cores/kc-beeb/vstream.h"
#include "../../cores/kc-beeb/teletext.c"
#include "../../cores/kc-beeb/render.c"

#include "panel.h"
#include "../../common/app.h"

enum {
    k_rows = 256,
    k_row_words = BOARD_DISPLAY_ROW_WORDS,
    // The picture inside the renderer's canvas (its border is 4 characters).
    k_pic_x0 = RENDER_BORDER_CHARS * 16,
    k_pic_y0 = RENDER_BORDER_CHARS * 16,
};

static uint32_t s_fb[k_rows][k_row_words];
static const uint16_t *s_row_table[BOARD_DISPLAY_HEIGHT + 1];
static struct render s_render;
// A colour code (pixel.h) to the panel's 4-bit code.
static uint8_t s_code[PX_CODES];
// The frame each row was last drawn in: rows not drawn in a frame go black.
static uint32_t s_row_frame[k_rows];
static uint32_t s_frame = 1;

static inline uint8_t panel_code(uint8_t phys)
{
    // Physical colour: bit 0 red, 1 green, 2 blue. Panel: bit 0 blue, 2 red.
    return (uint8_t)(((phys & 1) << 2) | (phys & 2) | ((phys >> 2) & 1));
}

static void panel_palette(const struct render *r)
{
    for (int code = 0; code < PX_CODES; code++) {
        s_code[code] = panel_code(px_resolve((render_px)code, r->logical_to_physical, r->is_flash));
    }
}

static void panel_palette_entry(const struct render *r, int logical)
{
    s_code[logical] = panel_code(px_resolve((render_px)logical, r->logical_to_physical, r->is_flash));
    s_code[logical | 32] = panel_code(px_resolve((render_px)(logical | 32), r->logical_to_physical, r->is_flash));
}

static void panel_frame_done(void)
{
    for (int i = 0; i < k_rows; i++) {
        if (s_row_frame[i] != s_frame) {
            memset(s_fb[i], 0, (k_row_words - 1) * 4);
        }
    }
    s_frame++;
}

// Picture pixels [p0, p1) into a panel row one at a time (of each 4, 0-2).
static inline void panel_emit_pixels(uint32_t *out, const render_px *px, const uint8_t *code,
                                     int32_t p0, int32_t p1)
{
    for (int32_t p = p0; p < p1; p++) {
        if ((p & 3) == 3) {
            continue;
        }
        uint32_t d = (uint32_t)(p - (p >> 2));   // output pixel
        uint32_t w = d >> 3, sh = (d & 7) * 4;
        out[w] = (out[w] & ~(15u << sh)) | ((uint32_t)code[px[p]] << sh);
    }
}

// The canvas span [first, first + count) of raster vert into its panel row.
static void panel_emit(struct render *r, uint32_t first, uint32_t count, int32_t vert)
{
    int32_t y = (vert & ~1) - r->vert_window_start - k_pic_y0;
    if (count == 0 || y < 0 || y >= 2 * k_rows) {
        return;
    }
    uint32_t row = (uint32_t)y >> 1;
    uint32_t *out = s_fb[row];
    // The span in picture pixels.
    int32_t p0 = (int32_t)first - k_pic_x0;
    int32_t p1 = p0 + (int32_t)count;
    if (p0 < 0) p0 = 0;
    if (p1 > 640) p1 = 640;
    if (s_row_frame[row] != s_frame) {
        // The row's first span this frame: black first, unless it is the
        // whole line, which overwrites every word.
        s_row_frame[row] = s_frame;
        if (p0 != 0 || p1 != 640) {
            memset(out, 0, (k_row_words - 1) * 4);
        }
    }
    if (p0 >= p1) {
        return;
    }
    const render_px *px = &r->line[k_pic_x0];
    const uint8_t *code = s_code;
    // Whole groups of 32 (three whole output words) at once; pixel by pixel
    // only before the first group boundary and after the last.
    int32_t g0 = (p0 + 31) >> 5, g1 = p1 >> 5;
    if (g0 >= g1) {
        panel_emit_pixels(out, px, code, p0, p1);
        return;
    }
    panel_emit_pixels(out, px, code, p0, g0 * 32);
    for (int32_t g = g0; g < g1; g++) {
        const render_px *s = px + g * 32;
        uint32_t *o = out + g * 3;
#define C(i) ((uint32_t)code[s[i]])
        o[0] = C(0) | C(1) << 4 | C(2) << 8 | C(4) << 12 | C(5) << 16 | C(6) << 20 | C(8) << 24 | C(9) << 28;
        o[1] = C(10) | C(12) << 4 | C(13) << 8 | C(14) << 12 | C(16) << 16 | C(17) << 20 | C(18) << 24 | C(20) << 28;
        o[2] = C(21) | C(22) << 4 | C(24) << 8 | C(25) << 12 | C(26) << 16 | C(28) << 20 | C(29) << 24 | C(30) << 28;
#undef C
    }
    panel_emit_pixels(out, px, code, g1 * 32, p1);
}

void panel_lcd_init(void)
{
    for (int i = 0; i < k_rows; i++) {
        memset(s_fb[i], 0, sizeof s_fb[i]);
        s_fb[i][k_row_words - 1] = BOARD_DISPLAY_ROW_END;
    }
    for (int i = 0; i < BOARD_DISPLAY_HEIGHT; i++) {
        s_row_table[i] = (const uint16_t *)s_fb[(i * k_rows) / BOARD_DISPLAY_HEIGHT];
    }
    s_row_table[BOARD_DISPLAY_HEIGHT] = NULL;
    board_display_init(s_row_table);
    board_display_backlight(255);
}

void panel_init(void)
{
    render_init(&s_render, NULL);
    panel_palette(&s_render);
}

// A machine reset (core 0) at stream position s_reset_mark: the renderer
// starts again from its power-on state when it gets there, as the machine
// did (a reset puts the video ULA back without a stream event, and a
// renderer left in the old mode can stall on the new stream).
static volatile uint32_t s_reset_mark;
static volatile bool s_reset_pending;

void panel_machine_reset(struct vstream *vs)
{
    s_reset_mark = vs->head;
    __dmb();
    s_reset_pending = true;
}

void panel_consume(struct vstream *vs)
{
    struct render *r = &s_render;
    if (s_reset_pending) {
        __dmb();
        int32_t before = (int32_t)(s_reset_mark - vs->tail);
        if (before > 0) {
            render_consume_n(r, vs, before);
        }
        render_init(r, NULL);
        panel_palette(r);
        s_reset_pending = false;
    }
    // Only what is there now, so that a producer that keeps the ring full
    // (turbo) still lets the caller count core 1's busy time.
    // render_consume_n takes events in its stride, so no scan ahead for them.
    int32_t n = (int32_t)(vs->head - vs->tail);
    if (n > 0) {
        render_consume_n(r, vs, n);
    }
}

// ---- A program's loading screen: drawn into the framebuffer before core 1 starts rendering the
// machine: its picture, and what's loading and how far it's got. Bit 0 blue, 1 green, 2 red.

static void splash_px(int x, int y, int c)
{
    if (x < 0 || x >= BOARD_DISPLAY_WIDTH || y < 0 || y >= k_rows) return;
    uint32_t *w = &s_fb[y][x >> 3];
    int sh = (x & 7) * 4;
    *w = (*w & ~(0xfu << sh)) | ((uint32_t) (c & 7) << sh);
}
static void splash_rect(int x0, int y0, int w, int h, int c)
{
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) splash_px(x, y, c);
}

// text in the Master's own character set (8 x 8, in the Terminal ROM from &B900: character 32
// on), at MODE 1's size: 320 pixels across the panel's 480 (each 1.5 wide), a row a line
extern const uint8_t rom_terminal[16384];
static void splash_bbc_text(int cx, int y, const char *s, int c)
{
    int n = (int) strlen(s), x0 = cx - n * 12 / 2;
    for (int i = 0; i < n; i++) {
        int ch = (unsigned char) s[i];
        if (ch < 0x20 || ch > 0x7e) ch = '?';
        const uint8_t *g = &rom_terminal[0x3900 + (ch - 0x20) * 8];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx)) {
                    int p = i * 8 + gx;
                    for (int x = p * 3 / 2; x < (p + 1) * 3 / 2; x++) splash_px(x0 + x, y + gy, c);
                }
    }
}

// The app's loading screen picture (app_boot_image), drawn once; false if it has none
static int s_bar_x = 8, s_bar_w = BOARD_DISPLAY_WIDTH - 16;
bool panel_splash_picture(void)
{
    const struct machine_boot_image *im = app_boot_image();
    if (!im || im->w != BOARD_DISPLAY_WIDTH || im->h != k_rows) return false;
    for (int y = 0; y < k_rows; y++) {
        const uint8_t *s = &im->px[y * BOARD_DISPLAY_WIDTH / 2];
        for (int w = 0; w < k_row_words - 1; w++)      // (bit 3 isn't a colour: masked off)
            s_fb[y][w] = (s[w * 4] | s[w * 4 + 1] << 8 | s[w * 4 + 2] << 16 | (uint32_t) s[w * 4 + 3] << 24) & 0x77777777u;
        s_fb[y][k_row_words - 1] = BOARD_DISPLAY_ROW_END;
    }
    return true;
}

// what's loading and how far (of 1000): the status line and the bar, across the screen
void panel_splash_status(const char *what, int permille)
{
    enum { BLUE = 1, CYAN = 3, WHITE = 7 };        // panel nibbles (b0 blue, b1 green, b2 red)
    for (int y = 208; y < 240; y++) memset(s_fb[y], 0, (k_row_words - 1) * 4);
    char line[72];
    snprintf(line, sizeof line, "%.34s %3d%%", what, permille / 10);
    splash_bbc_text(BOARD_DISPLAY_WIDTH / 2, 212, line, WHITE);
    splash_rect(s_bar_x, 228, s_bar_w, 7, BLUE);
    splash_rect(s_bar_x + 2, 230, (s_bar_w - 4) * (permille > 1000 ? 1000 : permille) / 1000, 3, CYAN);
}
