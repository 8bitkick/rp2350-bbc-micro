// esp32-beeb: SAA5050 teletext. See teletext.h.

#include <string.h>

#include "teletext.h"
#include "teletext_glyphs.h"

#ifndef TELETEXT_DATA_ATTR
#define TELETEXT_DATA_ATTR
#endif

// Used for stretching 12 pixels wide into 16.
static const uint8_t k_stretch_data[] = {
    0, 255, 0, 0,  0, 85, 1, 170,  1, 170, 2, 85,  2, 255, 0, 0,
    3, 255, 0, 0,  3, 85, 4, 170,  4, 170, 5, 85,  5, 255, 0, 0,
    6, 255, 0, 0,  6, 85, 7, 170,  7, 170, 8, 85,  8, 255, 0, 0,
    9, 255, 0, 0,  9, 85, 10, 170, 10, 170, 11, 85, 11, 255, 0, 0,
};

// The three glyph sets, 96 characters of 20 rows by 16 pixels. On the host
// each pixel is a byte of coverage (0..255) for beebjit's anti-aliased look;
// on the board (RENDER_INDEXED) each row is 16 bits, which keeps the sets
// in 11 KB of internal RAM instead of 92 KB.
#ifdef RENDER_INDEXED
#define GLYPH_STRIDE 40
#else
#define GLYPH_STRIDE 320
#endif
#define GLYPH_SET_BYTES (96 * GLYPH_STRIDE)

static int s_generated;
#ifdef RENDER_INDEXED
// A glyph row byte (pixel 0 in bit 7) as two words of 0xff/0x00 pixel
// masks, so eight pixels are two selects between fg and bg.
static uint32_t s_expand[256][2];
#endif
static TELETEXT_DATA_ATTR uint8_t s_glyphs[GLYPH_SET_BYTES];
static TELETEXT_DATA_ATTR uint8_t s_gfx[GLYPH_SET_BYTES];
static TELETEXT_DATA_ATTR uint8_t s_sep_gfx[GLYPH_SET_BYTES];

// Stores a generated 16 by 20 coverage glyph into a set.
static void store_glyph(uint8_t *set, uint32_t index, const uint8_t *px)
{
    uint8_t *dst = set + index * GLYPH_STRIDE;
#ifdef RENDER_INDEXED
    for (uint32_t y = 0; y < 20; y++) {
        uint32_t bits = 0;
        for (uint32_t x = 0; x < 16; x++) {
            if (px[y * 16 + x] >= 128) {
                bits |= 0x8000u >> x;
            }
        }
        dst[y * 2] = (uint8_t)bits;
        dst[y * 2 + 1] = (uint8_t)(bits >> 8);
    }
#else
    memcpy(dst, px, 320);
#endif
}

static void draw_block(uint8_t *glyph, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            glyph[(y + i) * 6 + x + j] = 1;
        }
    }
}

static void double_up(uint8_t *dest, const uint8_t *src)
{
    for (uint32_t y = 0; y < 10; y++) {
        for (uint32_t x = 0; x < 6; x++) {
            uint8_t *p = dest + y * 24 + x * 2;
            uint8_t v = src[y * 6 + x];
            p[0] = v;
            p[1] = v;
            p[12] = v;
            p[13] = v;
        }
    }
}

static void stretch_12_to_16(uint8_t *dest, const uint8_t *src)
{
    for (uint32_t y = 0; y < 20; y++) {
        const uint8_t *s = k_stretch_data;
        for (uint32_t x = 0; x < 16; x++) {
            uint8_t v = (uint8_t)(src[s[0]] * s[1] + src[s[2]] * s[3]);
            *dest++ = v;
            s += 4;
        }
        src += 12;
    }
}

static void smooth_diagonals(uint8_t *dest, const uint8_t *src)
{
    memcpy(dest, src, 12 * 20);
    for (uint32_t y = 1; y < 19; y++) {
        for (uint32_t x = 1; x < 11; x++) {
            const uint8_t *p = src + y * 12 + x;
            int up = p[-12], down = p[12], left = p[-1], right = p[1];
            int down_left = p[11], up_right = p[-11], down_right = p[13], up_left = p[-13];
            int smooth = (down && left && !down_left) || (up && right && !up_right) ||
                         (down && right && !down_right) || (up && left && !up_left);
            if (smooth) {
                dest[y * 12 + x] = 1;
            }
        }
    }
}

void teletext_generate(void)
{
    if (s_generated) {
        return;
    }
    s_generated = 1;
#ifdef RENDER_INDEXED
    for (uint32_t b = 0; b < 256; b++) {
        uint32_t m0 = 0, m1 = 0;
        for (uint32_t i = 0; i < 4; i++) {
            if (b & (0x80u >> i)) {
                m0 |= 0xffu << (8 * i);
            }
            if (b & (0x08u >> i)) {
                m1 |= 0xffu << (8 * i);
            }
        }
        s_expand[b][0] = m0;
        s_expand[b][1] = m1;
    }
#endif
    for (uint32_t i = 0; i < 96; i++) {
        uint8_t doubled[12 * 20], pretty[12 * 20], wide[16 * 20];
        double_up(doubled, &teletext_characters[i * 60]);
        smooth_diagonals(pretty, doubled);
        stretch_12_to_16(wide, pretty);
        store_glyph(s_glyphs, i, wide);
    }
    // 0x40 - 0x5F in graphics modes use the character glyph.
    memcpy(&s_gfx[(0x40 - 0x20) * GLYPH_STRIDE], &s_glyphs[(0x40 - 0x20) * GLYPH_STRIDE], 0x20 * GLYPH_STRIDE);
    memcpy(&s_sep_gfx[(0x40 - 0x20) * GLYPH_STRIDE], &s_glyphs[(0x40 - 0x20) * GLYPH_STRIDE], 0x20 * GLYPH_STRIDE);
    // The graphics glyphs, on a 2x3 grid.
    for (uint32_t i = 0; i < 0x40; i++) {
        uint8_t gfx[6 * 10], sep[6 * 10], doubled[12 * 20], wide[16 * 20];
        memset(gfx, 0, sizeof gfx);
        memset(sep, 0, sizeof sep);
        if (i & 0x01) { draw_block(gfx, 0, 0, 3, 3); draw_block(sep, 1, 0, 2, 2); }
        if (i & 0x02) { draw_block(gfx, 3, 0, 3, 3); draw_block(sep, 4, 0, 2, 2); }
        if (i & 0x04) { draw_block(gfx, 0, 3, 3, 4); draw_block(sep, 1, 3, 2, 3); }
        if (i & 0x08) { draw_block(gfx, 3, 3, 3, 4); draw_block(sep, 4, 3, 2, 3); }
        if (i & 0x10) { draw_block(gfx, 0, 7, 3, 3); draw_block(sep, 1, 7, 2, 2); }
        if (i & 0x20) { draw_block(gfx, 3, 7, 3, 3); draw_block(sep, 4, 7, 2, 2); }
        uint32_t index = i + ((i >= 0x20) ? 0x20 : 0);
        double_up(doubled, gfx);
        stretch_12_to_16(wide, doubled);
        store_glyph(s_gfx, index, wide);
        double_up(doubled, sep);
        stretch_12_to_16(wide, doubled);
        store_glyph(s_sep_gfx, index, wide);
    }
}

static inline RENDER_FN void set_active_characters(struct teletext *t)
{
    if (t->is_graphics_active) {
        t->active_characters = t->is_separated_active ? s_sep_gfx : s_gfx;
    } else {
        t->active_characters = s_glyphs;
    }
}

#ifdef RENDER_INDEXED
#define GLYPH_ROW_BYTES 2
#else
#define GLYPH_ROW_BYTES 16
#endif

// Which glyph rows this scanline shows, for a normal and a double height
// character, two rasters each (beebjit's teletext_render arithmetic).
static inline RENDER_FN void update_row_offsets(struct teletext *t)
{
    uint32_t line = t->scanline * 2;
    if (!t->is_isv && t->crtc_ra0) {
        line++;   // rounded
    }
    t->row_off[0][0] = line * GLYPH_ROW_BYTES;
    if (t->is_isv) {
        line++;
    }
    t->row_off[0][1] = line * GLYPH_ROW_BYTES;
    line = t->scanline + (t->second_character_row_of_double ? 10 : 0);
    t->row_off[1][0] = line * GLYPH_ROW_BYTES;
    t->row_off[1][1] = line * GLYPH_ROW_BYTES;
}

static inline RENDER_FN void reset_scanline_state(struct teletext *t)
{
    t->is_graphics_active = 0;
    t->is_separated_active = 0;
    t->is_concealed = 0;
    t->double_active = 0;
    t->flash_active = 0;
    t->fg_color = t->palette[7];
    t->bg_color = t->palette[0];
    t->is_hold_graphics = 0;
    t->held_character = s_glyphs;  // space
    set_active_characters(t);
}

static inline RENDER_FN void advance_scanline(struct teletext *t)
{
    t->scanline++;
    if (t->scanline == 10) {
        t->scanline = 0;
        if (t->second_character_row_of_double) {
            t->second_character_row_of_double = 0;
        } else if (t->had_double_active_this_scanline) {
            t->second_character_row_of_double = 1;
        }
    }
    t->had_double_active_this_scanline = 0;
    update_row_offsets(t);
}

static inline RENDER_FN void new_frame_started(struct teletext *t)
{
    t->scanline = 0;
    t->second_character_row_of_double = 0;
    // The flash counter is 6 bits on the silicon.
    t->flash_count = (t->flash_count + 1) & 63;
    t->flash_visible_this_frame = (t->flash_count >= 16);
    update_row_offsets(t);
}

void teletext_init(struct teletext *t)
{
    teletext_generate();
    memset(t, 0, sizeof *t);
    for (uint32_t i = 0; i < 8; i++) {
#ifdef RENDER_INDEXED
        t->palette[i] = PX_PHYS(i);   // teletext colours are physical (see pixel.h)
#else
        // One unit per channel so PX_BLEND's multiply is per channel.
        uint32_t c = 0;
        if (i & 1) c |= 0x010000;
        if (i & 2) c |= 0x000100;
        if (i & 4) c |= 0x000001;
        t->palette[i] = c;
#endif
    }
    t->black = PX_BLACK;
    t->render_character = s_glyphs;
    t->row_glyph = t->row_glyph2 = s_glyphs;
    t->row_blank = 1;
    reset_scanline_state(t);
    new_frame_started(t);
}

static inline RENDER_FN __attribute__((always_inline)) void handle_control(struct teletext *t, uint8_t c)
{
    switch (c) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        t->is_graphics_active = 0;
        t->fg_color = t->palette[c];
        if (t->is_concealed) {
            t->is_concealed = 0;
            t->render_fg_color = t->bg_color;  // this character stays concealed
        }
        break;
    case 0x08: t->flash_active = 1; break;
    case 0x09: t->flash_active = 0; break;
    case 0x0c: t->double_active = 0; break;
    case 0x0d:
        t->double_active = 1;
        t->had_double_active_this_scanline = 1;
        break;
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        t->is_graphics_active = 1;
        t->fg_color = t->palette[c & 7];
        if (t->is_concealed) {
            t->is_concealed = 0;
            t->render_fg_color = t->bg_color;
        }
        break;
    case 0x18: t->is_concealed = 1; break;
    case 0x19: t->is_separated_active = 0; break;
    case 0x1a: t->is_separated_active = 1; break;
    case 0x1c: t->bg_color = t->palette[0]; break;
    case 0x1d: t->bg_color = t->fg_color; break;
    case 0x1e: t->is_hold_graphics = 1; break;
    case 0x1f: t->is_hold_graphics = 0; break;
    default: break;  // 0x00 and 0x10: no black select on this part
    }
    set_active_characters(t);
}

static inline RENDER_FN __attribute__((always_inline)) void do_data_byte(struct teletext *t, uint8_t data)
{
    int is_hold_graphics = t->is_hold_graphics;
    t->render_fg_color = t->fg_color;
    data &= 0x7f;
    if (data >= 0x20) {
        const uint8_t *ch = t->active_characters + GLYPH_STRIDE * (data - 0x20);
        t->render_character = ch;
        if (t->is_graphics_active) {
            if (data & 0x20) {
                t->held_character = ch;
            }
        } else {
            t->held_character = s_glyphs;
        }
    } else {
        const uint8_t *held = t->held_character;
        int was_graphics = t->is_graphics_active;
        int was_double = t->double_active;
        handle_control(t, data);
        // Hold on is set-at, hold off is set-after.
        is_hold_graphics |= t->is_hold_graphics;
        if (was_graphics && is_hold_graphics && (t->double_active == was_double)) {
            t->render_character = held;
        } else {
            t->render_character = s_glyphs;
            t->held_character = s_glyphs;
        }
    }
}

TT_API __attribute__((always_inline)) void teletext_data(struct teletext *t, uint8_t data, int is_dispen, int decode)
{
    uint32_t dp = t->data_pipe;
    uint32_t ep = t->dispen_pipe;
    uint8_t queue_data = (uint8_t)dp;
    uint32_t pipelined_dispen = ep & 1;

    t->idle_calls = (data | (uint32_t)is_dispen) ? 0 : t->idle_calls + 1;
    // IC37/IC36: bit 6 forced in the pipelined byte while DISPEN is low, so
    // off-screen bytes cannot act as control codes.
    if (!is_dispen) {
        dp |= 0x400000;
    }
    // A byte that entered with DISPEN low is forced to 0x40 and renders
    // blank (its DISPEN travels with it); decoding it only sets state the
    // next decoded byte or the line reset overwrites. Skip it: off-window
    // bytes are a third of a mode 7 line.
    if (decode && pipelined_dispen) {
        do_data_byte(t, queue_data);
    }
    t->data_pipe = (dp >> 8) | ((uint32_t)data << 16);

    // End of scanline on LOSE (DISPEN) low for at least 2 cycles.
    if ((ep & 7) == 4) {
        reset_scanline_state(t);
        advance_scanline(t);
    }
    t->dispen_pipe = ((ep >> 1) & 1) | ((uint32_t)(is_dispen != 0) << 1) | (pipelined_dispen << 2);
}

// Eight pixels of a glyph row: the left or right half of its 16.
static inline RENDER_FN void glyph_half(const uint8_t *row, int right_half,
                                        render_px *out, render_px fg, render_px bg)
{
#ifdef RENDER_INDEXED
    // Row bytes: [1] holds pixels 0-7, [0] pixels 8-15, pixel 0 in bit 7.
    // out is 4-byte aligned (line buffers, multiples of 4).
    uint32_t b = row[right_half ? 0 : 1];
    uint32_t fgx = fg * 0x01010101u;
    uint32_t bgx = bg * 0x01010101u;
    uint32_t m0 = s_expand[b][0];
    uint32_t m1 = s_expand[b][1];
    ((uint32_t *)out)[0] = (m0 & fgx) | (~m0 & bgx);
    ((uint32_t *)out)[1] = (m1 & fgx) | (~m1 & bgx);
#else
    const uint8_t *src = row + (right_half ? 8 : 0);
    for (int i = 0; i < 8; i++) {
        uint32_t v = src[i];
        out[i] = PX_BLEND(v, fg, bg);
    }
#endif
}

// Everything about the character that both ticks share, decided on the
// even tick (teletext_data runs before it and nothing changes in between).
static inline RENDER_FN void teletext_prepare_character(struct teletext *t, int two_rasters)
{
    const uint8_t *src = t->render_character;
    t->row_fg = t->render_fg_color;
    t->row_bg = t->bg_color;
    t->row_blank = !(t->dispen_pipe & 4);
    if (t->row_blank) {
        return;
    }
    if ((t->flash_active && !t->flash_visible_this_frame) ||
        (t->second_character_row_of_double && !t->double_active)) {
        src = s_glyphs;
    }
    if (t->is_concealed) {
        src = s_glyphs;
    }
    const uint32_t *off = t->row_off[t->double_active ? 1 : 0];
    uint32_t first = off[0];
    if (!two_rasters && t->is_isv && t->crtc_ra0 && !t->double_active) {
        first += GLYPH_ROW_BYTES;   // single raster: rounded even when interlaced
    }
    t->row_glyph = src + first;
    t->row_glyph2 = src + off[1];
}

// Both ticks of a character at once, two rasters each (the run loop's
// mode 7 path); the same pixels as two teletext_render calls.
TT_API void teletext_render_pair(struct teletext *t, render_px *out, render_px *next_out, int want_next)
{
    teletext_prepare_character(t, 1);
#ifdef RENDER_INDEXED
    uint32_t *o = (uint32_t *)out;
    uint32_t *o2 = (uint32_t *)next_out;
    if (t->row_blank) {
        o[0] = o[1] = o[2] = o[3] = PX_BLACK * 0x01010101u;
        if (want_next) {
            o2[0] = o2[1] = o2[2] = o2[3] = PX_BLACK * 0x01010101u;
        }
        return;
    }
    uint32_t fgx = t->row_fg * 0x01010101u;
    uint32_t bgx = t->row_bg * 0x01010101u;
    const uint8_t *g = t->row_glyph;
    const uint32_t *m;
    m = s_expand[g[1]];  o[0] = (m[0] & fgx) | (~m[0] & bgx);  o[1] = (m[1] & fgx) | (~m[1] & bgx);
    m = s_expand[g[0]];  o[2] = (m[0] & fgx) | (~m[0] & bgx);  o[3] = (m[1] & fgx) | (~m[1] & bgx);
    if (want_next) {
        const uint8_t *g2 = t->row_glyph2;
        m = s_expand[g2[1]]; o2[0] = (m[0] & fgx) | (~m[0] & bgx); o2[1] = (m[1] & fgx) | (~m[1] & bgx);
        m = s_expand[g2[0]]; o2[2] = (m[0] & fgx) | (~m[0] & bgx); o2[3] = (m[1] & fgx) | (~m[1] & bgx);
    }
#else
    if (t->row_blank) {
        for (int i = 0; i < 16; i++) {
            out[i] = t->black;
            if (want_next) next_out[i] = t->black;
        }
        return;
    }
    glyph_half(t->row_glyph, 0, out, t->row_fg, t->row_bg);
    glyph_half(t->row_glyph, 1, out + 8, t->row_fg, t->row_bg);
    if (want_next) {
        glyph_half(t->row_glyph2, 0, next_out, t->row_fg, t->row_bg);
        glyph_half(t->row_glyph2, 1, next_out + 8, t->row_fg, t->row_bg);
    }
#endif
}

// Sixteen pixels of one glyph row pair (both halves) for fg on bg.
static inline RENDER_FN __attribute__((always_inline)) void glyph_row16(const uint8_t *row, render_px *out,
                                                                       render_px fg, render_px bg)
{
#ifdef RENDER_INDEXED
    uint32_t fgx = fg * 0x01010101u;
    uint32_t x = (fg ^ bg) * 0x01010101u;   // bg = fgx ^ x: one and and one xor a word
    uint32_t bgx = fgx ^ x;
    uint32_t *o = (uint32_t *)out;
    const uint32_t *m = s_expand[row[1]];
    o[0] = bgx ^ (m[0] & x);
    o[1] = bgx ^ (m[1] & x);
    m = s_expand[row[0]];
    o[2] = bgx ^ (m[0] & x);
    o[3] = bgx ^ (m[1] & x);
#else
    glyph_half(row, 0, out, fg, bg);
    glyph_half(row, 1, out + 8, fg, bg);
#endif
}

TT_API uint32_t teletext_run(struct teletext *t, const uint16_t *ring, uint32_t mask, uint32_t tail,
                             uint32_t max, render_px *out, render_px *next_out, int want_next)
{
    // Settled DISPEN pipeline only (two words after DISPEN rises): then no
    // byte is forced, every byte decodes, the cell is not blank and the end
    // of line cannot be detected inside the run.
    t->stat_run_calls++;
    if (t->dispen_pipe != 7) {
        t->stat_run_zero++;
        return 0;
    }
    const uint32_t want = VS_PAIR | VS_DISPEN | VS_MA13;
    uint32_t dp = t->data_pipe;
    const uint8_t *rc = t->render_character;
    const uint8_t *held = t->held_character;
    const uint8_t *active = t->active_characters;
    render_px fg = t->fg_color, bg = t->bg_color;
    int graphics = t->is_graphics_active;
    // The prepare step's substitutions that stay fixed until a control code.
    int blank_glyph = (t->flash_active && !t->flash_visible_this_frame) ||
                      (t->second_character_row_of_double && !t->double_active) || t->is_concealed;
    const uint32_t *off = t->row_off[t->double_active ? 1 : 0];
    uint32_t off0 = off[0], off1 = off[1];
    render_px rfg = t->render_fg_color;
    uint32_t i = 0;
    for (; i < max; i++) {
        uint16_t w = ring[(tail + i) & mask];
        if ((w & (VS_EVENT | VS_PAIR | VS_DISPEN | VS_MA13)) != want) {
            break;
        }
        uint8_t q = (uint8_t)dp & 0x7f;   // the byte leaving the pipeline
        dp = (dp >> 8) | ((uint32_t)(w >> 8) << 16);
        if (q >= 0x20) {
            const uint8_t *ch = active + GLYPH_STRIDE * (q - 0x20);
            rc = ch;
            if (graphics) {
                if (q & 0x20) {
                    held = ch;
                }
            } else {
                held = s_glyphs;
            }
            rfg = fg;
        } else {
            // A control code: the general path, then the locals again.
            t->stat_run_controls++;
            t->data_pipe = dp;
            t->render_character = rc;
            t->held_character = held;
            t->render_fg_color = rfg;
            do_data_byte(t, q);
            rc = t->render_character;
            held = t->held_character;
            active = t->active_characters;
            fg = t->fg_color;
            bg = t->bg_color;
            rfg = t->render_fg_color;
            graphics = t->is_graphics_active;
            blank_glyph = (t->flash_active && !t->flash_visible_this_frame) ||
                          (t->second_character_row_of_double && !t->double_active) || t->is_concealed;
            off = t->row_off[t->double_active ? 1 : 0];
            off0 = off[0];
            off1 = off[1];
        }
        const uint8_t *src = blank_glyph ? s_glyphs : rc;
        render_px *o = out + 16 * i;
        glyph_row16(src + off0, o, rfg, bg);
        if (want_next) {
            glyph_row16(src + off1, next_out + 16 * i, rfg, bg);
        }
    }
    t->stat_run_words += i;
    if (i) {
        // What teletext_data and teletext_render_pair leave behind.
        t->data_pipe = dp;
        t->render_character = rc;
        t->held_character = held;
        t->render_fg_color = rfg;
        t->idle_calls = 0;
        const uint8_t *src = blank_glyph ? s_glyphs : rc;
        t->row_fg = rfg;
        t->row_bg = bg;
        t->row_blank = 0;
        t->row_glyph = src + off0;
        t->row_glyph2 = src + off1;
    }
    return i;
}

TT_API void teletext_render(struct teletext *t, int is_odd_tick, render_px *out, render_px *next_out)
{
    if (!is_odd_tick) {
        teletext_prepare_character(t, next_out != 0);
    }
    if (t->row_blank) {
#ifdef RENDER_INDEXED
        ((uint32_t *)out)[0] = ((uint32_t *)out)[1] = PX_BLACK * 0x01010101u;
        if (next_out) {
            ((uint32_t *)next_out)[0] = ((uint32_t *)next_out)[1] = PX_BLACK * 0x01010101u;
        }
#else
        for (int i = 0; i < 8; i++) {
            out[i] = t->black;
            if (next_out) next_out[i] = t->black;
        }
#endif
        return;
    }
    glyph_half(t->row_glyph, is_odd_tick, out, t->row_fg, t->row_bg);
    if (next_out) {
        glyph_half(t->row_glyph2, is_odd_tick, next_out, t->row_fg, t->row_bg);
    }
}

TT_API void teletext_ra_isv_changed(struct teletext *t, uint8_t ra, int is_isv)
{
    t->crtc_ra0 = ra & 1;
    t->is_isv = is_isv;
    update_row_offsets(t);
}

TT_API void teletext_vsync_changed(struct teletext *t, int value)
{
    (void)value;
    new_frame_started(t);
}
