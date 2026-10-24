// The Presto's own screens (presto_screen.h): drawn on core 0 into rows of the panel's own format,
// which a row table of their own puts on the panel in place of the machine's.
//
// An overlay is a short list of boxes, text and QR codes. When it changes, every panel row is made
// from it again, and a row the same as the one above it (or as the background) is shown from that
// one: a screen of text and a QR code is a hundred or so different rows, not 480, so it fits in
// ~25 KB. Before the machine starts, those rows (and the network's buffers, presto_alloc) are in the
// machine's framebuffer, lent to us: it isn't drawn in until then. The panel never scans that
// memory while it's ours: our table (from the heap, 2 KB, for as long as a screen is up) shows a
// blank row wherever there's nothing to draw.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "board.h"
#include "app.h"
#include "machine_hook.h"
#include "presto_screen.h"
#include "lib/qrcodegen.h"
#include "lib/font8x8_basic.h"

#define W BOARD_DISPLAY_WIDTH
#define H BOARD_DISPLAY_HEIGHT
#define ROW_WORDS BOARD_DISPLAY_ROW_WORDS
#define ROW_BYTES (ROW_WORDS * 4)

static uint32_t frames(void) {
    return board_display_frames();
}

// ---- Our row table: on the panel while a screen is up (and, before the machine starts, from the
// first screen until the framebuffer goes back)

static const uint16_t **s_table;        // NULL: the machine's is up
static uint32_t *s_blank;               // a black row
static const uint16_t *const *s_machine_table;

static bool own_table(void) {
    if (s_table) return true;
    s_table = malloc((H + 1) * sizeof *s_table);
    s_blank = malloc(ROW_BYTES);
    if (!s_table || !s_blank) {
        free(s_table);
        free(s_blank);
        s_table = NULL;
        return false;
    }
    memset(s_blank, 0, ROW_BYTES);
    s_blank[ROW_WORDS - 1] = BOARD_DISPLAY_ROW_END;
    for (int y = 0; y < H; y++) s_table[y] = (const uint16_t *) s_blank;
    s_table[H] = NULL;
    if (!s_machine_table) s_machine_table = presto_lcd_rows();
    board_display_set_rows(s_table);    // (from the next frame)
    return true;
}

// ---- Memory: the lent framebuffer in 2 KB blocks, a run of them for anything bigger

#define BLOCK 2048
#define MAX_BLOCKS 32
static uint32_t *s_fb;          // the machine's framebuffer, lent (NULL: given back, or never lent)
static int s_fb_rows;
static uint8_t *s_arena;
static int s_nblocks;
static uint32_t s_used;         // a bit a block
static uint8_t s_run[MAX_BLOCKS];
static uint32_t s_arena_from;   // usable from this frame (the panel has left it by then)
static bool s_arena_clean;      // cleared since the panel last showed it
static bool s_draining;         // to be given back: nothing more goes in it
static bool s_restoring;        // given back: the machine's table up again from the next frame
static uint32_t s_restore_frame;

void screen_lend(uint32_t *rows, int n) {
    s_fb = rows;
    s_fb_rows = n;
    s_arena = (uint8_t *) rows;
    s_nblocks = (int) ((size_t) n * ROW_BYTES / BLOCK);
    if (s_nblocks > MAX_BLOCKS) s_nblocks = MAX_BLOCKS;
    s_machine_table = presto_lcd_rows();
}

static bool arena_usable(void) {
    if (!s_fb || !s_table || s_draining || s_restoring || (int32_t) (frames() - s_arena_from) < 0) return false;
    if (!s_arena_clean) {
        // (the loading screen's picture, say)
        memset(s_arena, 0, (size_t) s_nblocks * BLOCK);
        s_arena_clean = true;
    }
    return true;
}

// our table up, with the framebuffer out of the panel's sight before anything's put in it
static bool take_panel(void) {
    if (s_restoring) {
        s_restoring = false;
        board_display_set_rows(s_table);
        s_arena_from = frames() + 2;
        s_arena_clean = false;
    }
    if (!s_table) {
        if (!own_table()) return false;
        s_arena_from = frames() + 2;
        s_arena_clean = false;
    }
    // (before the machine starts: two frames, so the rows are drawn in the framebuffer, not the heap)
    if (s_fb) {
        uint32_t t0 = time_us_32();
        while ((int32_t) (frames() - s_arena_from) < 0 && time_us_32() - t0 < 100000) tight_loop_contents();
    }
    return true;
}

void *presto_alloc(size_t n) {
    if (n && n <= (size_t) MAX_BLOCKS * BLOCK && arena_usable()) {
        int need = (int) ((n + BLOCK - 1) / BLOCK);
        for (int i = 0; i + need <= s_nblocks; i++) {
            uint32_t mask = (need == 32 ? ~0u : ((1u << need) - 1)) << i;
            if (!(s_used & mask)) {
                s_used |= mask;
                s_run[i] = (uint8_t) need;
                return s_arena + (size_t) i * BLOCK;
            }
        }
    }
    return malloc(n);
}

void *presto_calloc(size_t n) {
    void *p = presto_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void presto_free(void *p) {
    uint8_t *b = p;
    if (s_fb && b >= s_arena && b < s_arena + (size_t) s_nblocks * BLOCK) {
        int i = (int) ((b - s_arena) / BLOCK), n = s_run[i];
        memset(b, 0, (size_t) n * BLOCK);    // (it may have held a password)
        s_used &= ~((n == 32 ? ~0u : ((1u << n) - 1)) << i);
        s_run[i] = 0;
        return;
    }
    free(p);
}

// ---- The overlay

#define ITEMS 28
#define TEXT_POOL 560
#define QR_VERSION 5    // up to 37x37 modules: enough for a WiFi network's name and password
#define QR_BYTES qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION)
#define CHUNK_ROWS (BLOCK / ROW_BYTES)
#define MAX_CHUNKS ((H + CHUNK_ROWS - 1) / CHUNK_ROWS)

enum { BOX, TEXT, QR };

typedef struct {
    uint8_t kind, scale, colour;
    int16_t x, y, w, h;
    uint16_t at;        // TEXT: where its text is in the pool
} item_t;

typedef struct {
    uint8_t background;
    bool dirty, secret;
    uint8_t n;
    item_t items[ITEMS];
    uint16_t pool_used;
    char pool[TEXT_POOL];
    uint8_t qr[QR_BYTES];
    uint32_t *chunks[MAX_CHUNKS];     // the rows drawn, CHUNK_ROWS to a chunk
} overlay_t;

static overlay_t *ov;
static overlay_t *s_closed;     // closed, to be freed once the panel's done with it
static uint32_t s_free_at;

static void free_overlay(overlay_t *o) {
    for (int i = 0; i < MAX_CHUNKS; i++) if (o->chunks[i]) presto_free(o->chunks[i]);
    memset(o, 0, sizeof *o);    // (it may have shown a password)
    presto_free(o);
}

bool overlay_open(int background) {
    screen_loading(NULL, -1);
    if (!ov) {
        if (s_closed) {
            ov = s_closed;      // not freed yet: use it again
            s_closed = NULL;
        } else {
            if (!take_panel()) return false;
            ov = presto_calloc(sizeof *ov);
            if (!ov) return false;
        }
    }
    ov->n = 0;
    ov->pool_used = 0;
    ov->secret = false;
    ov->background = (uint8_t) (background & 7);
    ov->dirty = true;
    return true;
}

bool overlay_is_open(void) {
    return ov != NULL;
}

void overlay_private(void) {
    if (ov) ov->secret = true;
}

bool screen_private(void) {
    return (ov && ov->secret) || (s_closed && s_closed->secret);
}

void overlay_close(void) {
    if (!ov) return;
    if (s_closed) free_overlay(s_closed);
    if (s_fb) {
        for (int y = 0; y < H; y++) s_table[y] = (const uint16_t *) s_blank;
    } else {
        board_display_set_rows(s_machine_table);
    }
    s_closed = ov;
    ov = NULL;
    s_free_at = frames() + 2;
}

static item_t *add(int kind) {
    if (!ov || ov->n >= ITEMS) return NULL;
    item_t *it = &ov->items[ov->n++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t) kind;
    ov->dirty = true;
    return it;
}

void overlay_box(int x, int y, int w, int h, int colour) {
    item_t *it = add(BOX);
    if (!it) return;
    it->x = (int16_t) x;
    it->y = (int16_t) y;
    it->w = (int16_t) w;
    it->h = (int16_t) h;
    it->colour = (uint8_t) (colour & 7);
}

void overlay_text(int centre_x, int y, int scale, int colour, const char *text) {
    size_t len = strlen(text);
    if (!ov || ov->pool_used + len + 1 > TEXT_POOL) return;
    item_t *it = add(TEXT);
    if (!it) return;
    memcpy(ov->pool + ov->pool_used, text, len + 1);
    it->at = ov->pool_used;
    ov->pool_used = (uint16_t) (ov->pool_used + len + 1);
    it->scale = (uint8_t) scale;
    it->w = (int16_t) (len * 8 * (size_t) scale);
    it->h = (int16_t) (8 * scale);
    it->x = (int16_t) (centre_x - it->w / 2);
    it->y = (int16_t) y;
    it->colour = (uint8_t) (colour & 7);
}

int overlay_qr(int centre_x, int y, int max_px, const char *text) {
    uint8_t temp[QR_BYTES];
    if (!ov) return 0;
    bool ok = qrcodegen_encodeText(text, temp, ov->qr, qrcodegen_Ecc_MEDIUM, 1, QR_VERSION, qrcodegen_Mask_AUTO, true);
    memset(temp, 0, sizeof temp);
    if (!ok) return 0;
    // as large as fits, with a quiet zone of 3 modules around it
    int size = qrcodegen_getSize(ov->qr);
    int scale = max_px / (size + 6);
    if (scale < 1) scale = 1;
    int px = size * scale, quiet = 3 * scale;
    overlay_box(centre_x - px / 2 - quiet, y, px + 2 * quiet, px + 2 * quiet, SCREEN_WHITE);
    item_t *it = add(QR);
    if (!it) return 0;
    it->scale = (uint8_t) scale;
    it->x = (int16_t) (centre_x - px / 2);
    it->y = (int16_t) (y + quiet);
    it->w = it->h = (int16_t) px;
    it->colour = SCREEN_BLACK;
    return px + 2 * quiet;
}

// pixels [x0, x1) of a row in a colour (c: its code in every nibble)
static void span(uint32_t *row, int x0, int x1, uint32_t c) {
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    if (x0 >= x1) return;
    int w0 = x0 >> 3, w1 = (x1 - 1) >> 3;
    uint32_t m0 = ~0u << ((x0 & 7) * 4), m1 = ~0u >> ((7 - ((x1 - 1) & 7)) * 4);
    if (w0 == w1) {
        uint32_t m = m0 & m1;
        row[w0] = (row[w0] & ~m) | (c & m);
        return;
    }
    row[w0] = (row[w0] & ~m0) | (c & m0);
    for (int w = w0 + 1; w < w1; w++) row[w] = c;
    row[w1] = (row[w1] & ~m1) | (c & m1);
}

static void compose(const overlay_t *o, uint32_t *row, int y) {
    uint32_t bg = o->background * 0x11111111u;
    for (int w = 0; w < ROW_WORDS - 1; w++) row[w] = bg;
    row[ROW_WORDS - 1] = BOARD_DISPLAY_ROW_END;
    for (int i = 0; i < o->n; i++) {
        const item_t *it = &o->items[i];
        if (y < it->y || y >= it->y + it->h) continue;
        int s = it->scale ? it->scale : 1;
        uint32_t c = it->colour * 0x11111111u;
        if (it->kind == BOX) {
            span(row, it->x, it->x + it->w, c);
        } else if (it->kind == TEXT) {
            int gy = (y - it->y) / s;
            int x = it->x;
            for (const char *p = o->pool + it->at; *p && x < W; p++, x += 8 * s) {
                uint8_t bits = (uint8_t) font8x8_basic[*p & 0x7f][gy & 7];
                for (int k = 0; bits; k++, bits >>= 1)
                    if (bits & 1) span(row, x + k * s, x + (k + 1) * s, c);
            }
        } else if (it->kind == QR) {
            int my = (y - it->y) / s, size = qrcodegen_getSize(o->qr);
            for (int mx = 0; mx < size; mx++)
                if (qrcodegen_getModule(o->qr, mx, my)) span(row, it->x + mx * s, it->x + (mx + 1) * s, c);
        }
    }
}

// every row made again; each new row in the next free slot of the chunks
static void render(overlay_t *o) {
    int k = 0;
    const uint32_t *prev = NULL, *bg = NULL;
    for (int y = 0; y < H; y++) {
        int ci = k / CHUNK_ROWS;
        if (!o->chunks[ci] && !(o->chunks[ci] = presto_alloc(CHUNK_ROWS * ROW_BYTES))) {
            s_table[y] = (const uint16_t *) s_blank;     // (out of memory: the rest blank)
            continue;
        }
        uint32_t *row = o->chunks[ci] + (k % CHUNK_ROWS) * ROW_WORDS;
        compose(o, row, y);
        if (prev && !memcmp(prev, row, ROW_BYTES)) {
            s_table[y] = (const uint16_t *) prev;
        } else if (bg && !memcmp(bg, row, ROW_BYTES)) {
            s_table[y] = (const uint16_t *) (prev = bg);
        } else {
            if (!bg) bg = row;      // (the first row is the background)
            s_table[y] = (const uint16_t *) (prev = row);
            k++;
        }
    }
}

void overlay_frame_done(void) {
    if (s_closed && (int32_t) (frames() - s_free_at) >= 0) {
        free_overlay(s_closed);
        s_closed = NULL;
        if (!s_fb && !ov) {
            free(s_table);
            free(s_blank);
            s_table = NULL;
            s_blank = NULL;
        }
    }
    if (!ov || !ov->dirty || !take_panel()) return;
    ov->dirty = false;
    render(ov);
    if (presto_lcd_rows() != (const uint16_t *const *) s_table) board_display_set_rows(s_table);
}

bool screen_give_back(void) {
    if (!s_fb) return true;
    s_draining = true;
    if (ov || s_closed || s_used) return false;
    if (s_table) {
        if (!s_restoring) {
            // the framebuffer as the machine left it to us: black, each row's end code
            for (int r = 0; r < s_fb_rows; r++) {
                memset(s_fb + r * ROW_WORDS, 0, ROW_BYTES - 4);
                s_fb[r * ROW_WORDS + ROW_WORDS - 1] = BOARD_DISPLAY_ROW_END;
            }
            board_display_set_rows(s_machine_table);
            s_restoring = true;
            s_restore_frame = frames() + 2;
            return false;
        }
        if ((int32_t) (frames() - s_restore_frame) < 0) return false;
        free(s_table);
        free(s_blank);
        s_table = NULL;
        s_blank = NULL;
        s_restoring = false;
    } else {
        // (only the loading screen was ever up, on the machine's own table)
        for (int r = 0; r < s_fb_rows; r++) memset(s_fb + r * ROW_WORDS, 0, ROW_BYTES - 4);
    }
    s_fb = NULL;
    s_arena = NULL;
    s_nblocks = 0;
    return true;
}

// ---- The loading screen: in the framebuffer, as the machine's table shows it (BOARD_DISPLAY_HEIGHT
// rows from s_fb_rows), so the app's picture shows as it does on the Master's own loading screen

static bool s_loading, s_picture;
static char s_loading_text[40];
static int s_loading_permille = -2;

static void fb_px(int x, int r, int c) {
    if (x < 0 || x >= W || r < 0 || r >= s_fb_rows) return;
    uint32_t *w = &s_fb[r * ROW_WORDS + (x >> 3)];
    int sh = (x & 7) * 4;
    *w = (*w & ~(0xfu << sh)) | ((uint32_t) (c & 7) << sh);
}

// text sx pixels to each of the glyph's across and sy rows down, centred on cx
static void fb_text(int cx, int r, const char *s, int c, int sx2, int sy) {
    int n = (int) strlen(s), x0 = cx - n * 8 * sx2 / 4;
    for (int i = 0; i < n; i++) {
        const unsigned char *g = font8x8_basic[s[i] & 0x7f];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (1 << gx)) {
                    int p = i * 8 + gx;
                    for (int x = p * sx2 / 2; x < (p + 1) * sx2 / 2; x++)
                        for (int k = 0; k < sy; k++) fb_px(x0 + x, r + gy * sy + k, c);
                }
    }
}

static void fb_rect(int x0, int r0, int w, int h, int c) {
    for (int r = r0; r < r0 + h; r++)
        for (int x = x0; x < x0 + w; x++) fb_px(x, r, c);
}

static void loading_picture(void) {
    const struct machine_boot_image *im = app_boot_image();
    s_picture = im && im->w == W && im->h == s_fb_rows;
    for (int r = 0; r < s_fb_rows; r++) {
        uint32_t *row = s_fb + r * ROW_WORDS;
        if (s_picture) {
            const uint8_t *p = &im->px[r * W / 2];
            for (int w = 0; w < ROW_WORDS - 1; w++)     // (bit 3 isn't a colour: masked off)
                row[w] = (p[w * 4] | p[w * 4 + 1] << 8 | p[w * 4 + 2] << 16 | (uint32_t) p[w * 4 + 3] << 24) & 0x77777777u;
        } else {
            memset(row, 0, ROW_BYTES - 4);
        }
        row[ROW_WORDS - 1] = BOARD_DISPLAY_ROW_END;
    }
}

void screen_loading(const char *what, int permille) {
    if (permille < 0 || !what) {
        s_loading = false;
        return;
    }
    // (only before any other screen: the framebuffer's then the arena's)
    if (!s_fb || s_table) return;
    if (!s_loading) {
        s_loading = true;
        loading_picture();
        s_loading_permille = -2;
    }
    if (permille > 1000) permille = 1000;
    if (permille == s_loading_permille && !strcmp(what, s_loading_text)) return;
    s_loading_permille = permille;
    snprintf(s_loading_text, sizeof s_loading_text, "%s", what);
    // the line and the bar where the Master's own loading screen has them
    for (int r = 208; r < 240 && r < s_fb_rows; r++) memset(s_fb + r * ROW_WORDS, 0, ROW_BYTES - 4);
    char line[56];
    snprintf(line, sizeof line, "%.34s %3d%%", what, permille / 10);
    fb_text(W / 2, 212, line, SCREEN_WHITE, 3, 1);
    int bar_x = 8, bar_w = W - 16;
    fb_rect(bar_x, 228, bar_w, 7, SCREEN_BLUE);
    fb_rect(bar_x + 2, 230, (bar_w - 4) * permille / 1000, 3, SCREEN_CYAN);
}

void screen_loading_note(const char *line1, const char *line2) {
    if (!s_fb || s_table) return;
    if (!s_loading) {
        s_loading = true;
        loading_picture();
    }
    s_loading_permille = -2;    // (the next screen_loading draws its line again)
    for (int r = 192; r < 240 && r < s_fb_rows; r++) memset(s_fb + r * ROW_WORDS, 0, ROW_BYTES - 4);
    // (8 pixels a character: room for a network's name of 32 and more)
    fb_text(W / 2, 204, line1, SCREEN_YELLOW, 2, 1);
    fb_text(W / 2, 220, line2, SCREEN_WHITE, 2, 1);
}
