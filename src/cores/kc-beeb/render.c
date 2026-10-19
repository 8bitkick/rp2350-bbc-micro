// esp32-beeb: renderer. See render.h.
//
// Pixels are produced into one raster line buffer indexed by beam position
// relative to the horizontal window, and the rendered span is handed to
// RENDER_EMIT at the next hsync or vsync with the vertical beam position of
// that raster. The default emit copies the span into the ARGB canvas on the
// two rasters beebjit draws (deinterlaced); the board defines RENDER_EMIT to
// scale the line into its panel strips instead. Everything else is beebjit's
// beam model so the canvas stays pixel identical.

#include <string.h>

#include "render.h"

#ifdef RENDER_EXACT_TELETEXT
// Experiment: beebjit's always-run, always-decode teletext pipeline.
#define TT_RUN(ma13, idle) 1
#define TT_DECODE(m7) 1
#else
#define TT_RUN(ma13, idle) ((ma13) || (idle) < 2)
#define TT_DECODE(m7) (m7)
#endif

#define BG PX_BLACK

// Whether the emitter will use the second raster (line2) of the raster
// about to be rendered; the canvas always does, the panel only for the
// scanlines it doubles, and the run loops skip the work otherwise.
#ifndef RENDER_LINE2_WANTED
#define RENDER_LINE2_WANTED(r) 1
#endif

// A cycle counter for the profile fields, if the platform has one.
#ifndef RENDER_CYCLES
#define RENDER_CYCLES() 0u
#endif

// Called at each flyback, after the last raster has been emitted.
#ifndef RENDER_FRAME_HOOK
#define RENDER_FRAME_HOOK(r) ((void)0)
#endif
// Called at each hsync, after the scanline's last span has been emitted
// and before anything of the next is rendered.
#ifndef RENDER_LINE_HOOK
#define RENDER_LINE_HOOK(r) ((void)0)
#endif

#ifndef RENDER_EMIT
#define RENDER_EMIT(r, first, count, vert) render_emit_canvas(r, first, count, vert)
#define RENDER_EMIT_CANVAS 1
#endif

// Called when the palette or the flash state changes, after the span
// rendered under the old one has been emitted (RENDER_INDEXED: the
// emitter resolves colour codes and keeps its own lookup).
#ifndef RENDER_PALETTE_HOOK
#define RENDER_PALETTE_HOOK(r) ((void)0)
#endif
// One logical colour was written (the common case: a demo rewrites the
// palette every scanline).
#ifndef RENDER_PALETTE_ENTRY_HOOK
#define RENDER_PALETTE_ENTRY_HOOK(r, logical) RENDER_PALETTE_HOOK(r)
#endif

#ifdef RENDER_EMIT_CANVAS
// Copies line[first..first+count) to canvas rows vert&~1 and the next.
// With colour codes, the canvas gets the resolved 3-bit physical colour.
static void render_emit_canvas(struct render *r, uint32_t first, uint32_t count, int32_t vert)
{
    if (!r->buffer || count == 0) {
        return;
    }
    int32_t v = vert & ~1;
    if (v >= r->vert_window_end || v < r->vert_window_start) {
        return;
    }
    render_px *row = r->buffer + (v - r->vert_window_start) * RENDER_WIDTH;
#ifdef RENDER_INDEXED
    for (uint32_t i = first; i < first + count; i++) {
        row[i] = px_resolve(r->line[i], r->logical_to_physical, r->is_flash);
        row[RENDER_WIDTH + i] = px_resolve(r->line2[i], r->logical_to_physical, r->is_flash);
    }
#else
    memcpy(row + first, r->line + first, count * sizeof(render_px));
    memcpy(row + RENDER_WIDTH + first, r->line2 + first, count * sizeof(render_px));
#endif
}
#endif

// Emits what this raster rendered so far; the next pixels start a new span.
static inline RENDER_FN void end_span(struct render *r)
{
    if (r->span_end > r->span_start) {
        RENDER_EMIT(r, r->span_start, r->span_end - r->span_start, r->vert_beam_pos);
        r->span_start = r->span_end;
    }
}

// Emits what this raster rendered so far and starts a fresh span.
static inline RENDER_FN void flush_line(struct render *r)
{
    if (r->span_end > r->span_start) {
        RENDER_EMIT(r, r->span_start, r->span_end - r->span_start, r->vert_beam_pos);
    }
    r->span_start = r->span_end = 0;
}

static inline RENDER_FN void reset_render_pos(struct render *r)
{
    r->render_pos = RENDER_LINE_END;
    int32_t vert = r->vert_beam_pos & ~1;
    if (vert >= r->vert_window_end || vert < r->vert_window_start) {
        return;
    }
    if (r->horiz_beam_pos >= r->horiz_window_end || r->horiz_beam_pos < r->horiz_window_start) {
        return;
    }
    r->render_pos = r->horiz_beam_pos - r->horiz_window_start;
    if (r->span_end == r->span_start) {
        r->span_start = r->span_end = r->render_pos;
    }
    r->line2_wanted = RENDER_LINE2_WANTED(r);
}

void render_init(struct render *r, render_px *buffer)
{
    memset(r, 0, sizeof *r);
    r->buffer = buffer;
    teletext_init(&r->teletext);
    // beebjit: horizontal and vertical offsets of the standard picture.
    r->horiz_window_start = (15 - RENDER_BORDER_CHARS) * 16;
    r->horiz_window_end = RENDER_WIDTH + r->horiz_window_start;
    r->vert_window_start = (4 - RENDER_BORDER_CHARS) * 16;
    r->vert_window_end = RENDER_HEIGHT + r->vert_window_start;
    r->mode = RENDER_MODE0;
    r->is_clock_2mhz = 1;
    r->chars_per_line = 3;
    r->needs_mode_recalc = 1;
    r->cursor_segment_index = -1;
    r->framing_changed = 1;  // beebjit: set at power on
    for (int i = 0; i < 8; i++) {
        r->black[i] = BG;
    }
    for (uint32_t i = 0; i < RENDER_WIDTH; i++) {
        r->line[i] = r->line2[i] = BG;
    }
    if (buffer) {
        for (uint32_t i = 0; i < RENDER_WIDTH * RENDER_HEIGHT; i++) {
            buffer[i] = BG;
        }
    }
    render_reset(r);
    r->horiz_beam_pos = 0;
    r->vert_beam_pos = 0;
    reset_render_pos(r);
}

void render_reset(struct render *r)
{
    for (int i = 0; i < 16; i++) {
        r->logical_to_physical[i] = 0;
    }
    r->tables_built = 0;
#ifdef RENDER_INDEXED
    r->slot_mode[0][0] = r->slot_mode[0][1] = r->slot_mode[1][0] = r->slot_mode[1][1] = -1;
    r->cur_1mhz = r->table_1mhz_b;   // (so the first build goes to slot 0)
    r->cur_2mhz = r->table_2mhz_b;
#endif
    for (int i = 0; i < 8; i++) {
        r->palette[i] = PX_RGB(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        r->palette[i + 8] = PX_BLACK;  // never read: see display_color
    }
    // video_ula_power_on_reset: teletext, 1 MHz.
    r->is_clock_2mhz = 0;
    r->chars_per_line = 0;
    r->is_teletext = 1;
    r->needs_mode_recalc = 1;
    RENDER_PALETTE_HOOK(r);
}

static render_px display_color(struct render *r, uint8_t bits)
{
    uint8_t physical = r->logical_to_physical[bits] ^ 7;
    // Bit 3 is the flash bit: the colour alternates with its complement.
    if (physical & 8) {
        physical &= 7;
        if (r->is_flash) {
            physical ^= 7;
        }
    }
    return r->palette[physical];
}

// The 16 logical colours as pixels: with the flash bit resolved on the
// host, as codes the emitter resolves on the board.
static RENDER_FN void physical_palette(struct render *r, render_px phys[16])
{
    for (int i = 0; i < 16; i++) {
#ifdef RENDER_INDEXED
        phys[i] = PX_LOGICAL(i);
        (void)r;
#else
        phys[i] = display_color(r, (uint8_t)i);
#endif
    }
}

// The logical colour of each of the up to 8 pixels in a byte, in beebjit's
// shift-register order (the ULA shifts the byte left with 1s shifted in).
static inline RENDER_FN uint8_t byte_pixel_index(uint8_t sr)
{
    return (uint8_t)(((sr & 0x02) >> 1) | ((sr & 0x08) >> 2) | ((sr & 0x20) >> 3) | ((sr & 0x80) >> 4));
}

// One table entry: the byte's pixels at num_pixels per byte spread over
// `width` output pixels. Returns the logical colours it used.
static RENDER_FN uint16_t fill_entry(render_px *out, uint32_t width, uint32_t i, uint32_t num_pixels,
                                     const render_px phys[16])
{
    uint32_t stride = width / num_pixels;
    uint8_t sr = (uint8_t)i;
    render_px pixel = 0;
    uint16_t used = 0;
    for (uint32_t j = 0; j < width; j++) {
        if (j % stride == 0) {
            uint8_t idx = byte_pixel_index(sr);
            pixel = phys[idx];
            used |= (uint16_t)(1u << idx);
            sr = (uint8_t)((sr << 1) | 1);
        }
        out[j] = pixel;
    }
    return used;
}

static RENDER_FN void generate_1mhz(struct render *r, render_px (*table)[16], uint32_t num_pixels)
{
    render_px phys[16];
    physical_palette(r, phys);
    for (uint32_t i = 0; i < 256; i++) {
        r->byte_logicals[1][i] = fill_entry(table[i], 16, i, num_pixels, phys);
    }
}

static RENDER_FN void generate_2mhz(struct render *r, render_px (*table)[8], uint32_t num_pixels)
{
    render_px phys[16];
    physical_palette(r, phys);
    for (uint32_t i = 0; i < 256; i++) {
        r->byte_logicals[0][i] = fill_entry(table[i], 8, i, num_pixels, phys);
    }
}

// Palette writes are applied to the tables lazily, before the next pixel
// is rendered: only the entries whose pixels use a changed logical colour
// are rebuilt, and several writes on one scanline cost one pass. (Host
// only: on the board the tables hold logical codes and never change for
// the palette.)
static RENDER_FN void palette_fixup(struct render *r)
{
    uint16_t bit = r->palette_dirty;
    r->palette_dirty = 0;
    render_px phys[16];
    physical_palette(r, phys);
    if (r->tables_built & 1) {
        int mode = r->table_mode[0];
        uint32_t n = mode == RENDER_MODE0 ? 8 : mode == RENDER_MODE1 ? 4 : mode == RENDER_MODE2 ? 2 : 1;
        for (uint32_t i = 0; i < 256; i++) {
            if (r->byte_logicals[0][i] & bit) {
                fill_entry(r->table_2mhz[i], 8, i, n, phys);
            }
        }
    }
    if (r->tables_built & 2) {
        int mode = r->table_mode[1];
        uint32_t n = mode == RENDER_MODE4_80 ? 16 : mode == RENDER_MODE4 ? 8 : mode == RENDER_MODE5 ? 4 : 2;
        for (uint32_t i = 0; i < 256; i++) {
            if (r->byte_logicals[1][i] & bit) {
                fill_entry(r->table_1mhz[i], 16, i, n, phys);
            }
        }
    }
}

static RENDER_FN void prepare_tables(struct render *r)
{
    int mode = r->mode;
    uint32_t c0 = RENDER_CYCLES();
    if (mode == RENDER_MODE7) {
        return;
    }
#ifdef RENDER_INDEXED
    {
        int kind = !(mode == RENDER_MODE0 || mode == RENDER_MODE1 || mode == RENDER_MODE2 || mode == RENDER_MODE2_10);
        int *sm = r->slot_mode[kind];
        int slot = sm[0] == mode ? 0 : sm[1] == mode ? 1 : -1;
        if (slot < 0) {
            // Replace the slot not in use now.
            const void *cur = kind ? (const void *)r->cur_1mhz : (const void *)r->cur_2mhz;
            slot = (cur == (kind ? (const void *)r->table_1mhz : (const void *)r->table_2mhz)) ? 1 : 0;
            if (kind) {
                uint32_t n = mode == RENDER_MODE4_80 ? 16 : mode == RENDER_MODE4 ? 8 : mode == RENDER_MODE5 ? 4 : 2;
                generate_1mhz(r, slot ? r->table_1mhz_b : r->table_1mhz, n);
            } else {
                static const uint32_t px[4] = {8, 4, 2, 1};
                uint32_t n = px[mode == RENDER_MODE0 ? 0 : mode == RENDER_MODE1 ? 1 : mode == RENDER_MODE2 ? 2 : 3];
                generate_2mhz(r, slot ? r->table_2mhz_b : r->table_2mhz, n);
            }
            sm[slot] = mode;
            r->stat_tables++;
            r->stat_clk_tables += RENDER_CYCLES() - c0;
        }
        if (kind) {
            r->cur_1mhz = slot ? r->table_1mhz_b : r->table_1mhz;
        } else {
            r->cur_2mhz = slot ? r->table_2mhz_b : r->table_2mhz;
        }
        return;
    }
#endif
    if (r->palette_dirty) {
        palette_fixup(r);
        r->stat_tables++;
        r->stat_clk_tables += RENDER_CYCLES() - c0;
        c0 = RENDER_CYCLES();
    }
    switch (mode) {
    case RENDER_MODE0: case RENDER_MODE1: case RENDER_MODE2: case RENDER_MODE2_10:
        if (!(r->tables_built & 1) || r->table_mode[0] != mode) {
            static const uint32_t px[4] = {8, 4, 2, 1};
            uint32_t n = px[mode == RENDER_MODE0 ? 0 : mode == RENDER_MODE1 ? 1 : mode == RENDER_MODE2 ? 2 : 3];
            generate_2mhz(r, r->table_2mhz, n);
            r->palette_dirty = 0;
            r->stat_tables++;
            r->stat_clk_tables += RENDER_CYCLES() - c0;
            r->tables_built |= 1;
            r->table_mode[0] = mode;
        }
        r->cur_2mhz = r->table_2mhz;
        break;
    default:
        if (!(r->tables_built & 2) || r->table_mode[1] != mode) {
            uint32_t n = mode == RENDER_MODE4_80 ? 16 : mode == RENDER_MODE4 ? 8 : mode == RENDER_MODE5 ? 4 : 2;
            generate_1mhz(r, r->table_1mhz, n);
            r->palette_dirty = 0;
            r->stat_tables++;
            r->stat_clk_tables += RENDER_CYCLES() - c0;
            r->tables_built |= 2;
            r->table_mode[1] = mode;
        }
        r->cur_1mhz = r->table_1mhz;
        break;
    }
}

static inline RENDER_FN void select_func(struct render *r)
{
    if (r->mode == RENDER_MODE7) {
        r->selected_blank = 0;
    } else if (r->is_dispen && !(r->row_address & 0x08)) {
        r->selected_blank = 0;
    } else {
        r->selected_blank = 1;
    }
}

static RENDER_FN void mode_recalc(struct render *r)
{
    if (!r->needs_mode_recalc) {
        return;
    }
    r->needs_mode_recalc = 0;
    if (r->is_teletext) {
        r->mode = RENDER_MODE7;
    } else if (r->is_clock_2mhz) {
        static const int m[4] = {RENDER_MODE2_10, RENDER_MODE2, RENDER_MODE1, RENDER_MODE0};
        r->mode = m[r->chars_per_line & 3];
    } else {
        static const int m[4] = {RENDER_MODE8, RENDER_MODE5, RENDER_MODE4, RENDER_MODE4_80};
        r->mode = m[r->chars_per_line & 3];
    }
    select_func(r);
    reset_render_pos(r);
}

static RENDER_FN void set_mode(struct render *r, int clock_2mhz, int chars_per_line, int is_teletext)
{
    if (clock_2mhz != r->is_clock_2mhz || chars_per_line != r->chars_per_line) {
        r->tables_built = 0;
    }
    r->is_clock_2mhz = clock_2mhz;
    r->chars_per_line = chars_per_line;
    r->is_teletext = is_teletext;
    r->needs_mode_recalc = 1;
}

static RENDER_FN void set_flash(struct render *r, int is_flash)
{
    if (r->is_flash == is_flash) {
        return;
    }
#ifdef RENDER_INDEXED
    end_span(r);
    r->is_flash = is_flash;
    RENDER_PALETTE_HOOK(r);
#else
    for (int i = 0; i < 16; i++) {
        if (r->logical_to_physical[i] & 8) {
            r->tables_built = 0;
            break;
        }
    }
    r->is_flash = is_flash;
#endif
}

static inline RENDER_FN void check_cursor(struct render *r, render_px *pos, render_px *next, int advance)
{
    if (r->cursor_segment_index == -1) {
        return;
    }
    if (r->cursor_segments[r->cursor_segment_index]) {
        for (int i = 0; i < 8; i++) {
            pos[i] = PX_INVERT(pos[i]);
            next[i] = PX_INVERT(next[i]);
        }
    }
    if (!advance) {
        return;
    }
    if (++r->cursor_segment_index == 4) {
        r->cursor_segment_index = -1;
    }
}

static void do_vsync(struct render *r);

static RENDER_FN void do_hsync(struct render *r, uint32_t pulse_ticks)
{
    flush_line(r);
    RENDER_LINE_HOOK(r);
    if (r->horiz_beam_pos >= 1536 && r->vert_beam_pos <= 8) {
        r->is_crt_grayscale_fakeout = 1;
    }
    r->horiz_beam_pos = (pulse_ticks & 1) ? -4 : 0;
    r->vert_beam_pos += 2;
    r->cursor_segment_index = -1;
    if (r->vert_beam_pos >= 768) {
        do_vsync(r);
    }
    reset_render_pos(r);
}

static RENDER_FN void do_vsync(struct render *r)
{
    if (r->vert_beam_pos < 64) {
        return;
    }
    flush_line(r);
    r->frames++;
    RENDER_FRAME_HOOK(r);
    if (r->compat_clear && (r->framing_changed || r->compat_clear == 2) && r->buffer) {
        for (uint32_t i = 0; i < RENDER_WIDTH * RENDER_HEIGHT; i++) {
            r->buffer[i] = BG;
        }
    }
    r->framing_changed = 0;
    r->vert_beam_pos = 0;
    r->is_crt_grayscale_fakeout = 0;  // the grayscale conversion is not ported
    if (r->horiz_beam_pos >= 512) {
        r->vert_beam_pos = -1;  // even to odd interlace field
    }
    reset_render_pos(r);
}

static RENDER_FN void render_tick(struct render *r, uint8_t data, int is_odd_tick)
{
    r->horiz_beam_pos += 8;
    if (r->render_pos <= RENDER_WIDTH - 8) {
        render_px *pos = r->line + r->render_pos;
        render_px *next = r->line2 + r->render_pos;
        if (r->mode == RENDER_MODE7) {
            teletext_render(&r->teletext, is_odd_tick, pos, next);
            check_cursor(r, pos, next, is_odd_tick);
        } else if (r->is_clock_2mhz) {
            const render_px *v = r->selected_blank ? r->black : r->cur_2mhz[data];
            PX_COPY8(pos, v);
            PX_COPY8(next, v);
            check_cursor(r, pos, next, 1);
        } else {
            const render_px *v = r->selected_blank ? r->black : (r->cur_1mhz[data] + (is_odd_tick ? 8 : 0));
            PX_COPY8(pos, v);
            PX_COPY8(next, v);
            check_cursor(r, pos, next, is_odd_tick);
        }
        r->render_pos += 8;
        r->span_end = r->render_pos;
    } else if ((r->horiz_beam_pos & ~7) == r->horiz_window_start) {
        reset_render_pos(r);
    } else if (r->horiz_beam_pos >= 1536) {
        do_hsync(r, 0);
    }
}

// A run of ticks with DISPEN low (VS_SKIP). The teletext pipeline sees at
// most three of them: after that it is at a fixed point (a byte entering
// with DISPEN low is forced and never decoded, and the end of line has
// been detected). Then the beam: black inside the window, a count
// outside, with the window start and the forced hsync honoured tick by
// tick as the per-tick paths do. With the cursor being drawn or a mode
// change pending, the ticks go through the exact path instead.
RENDER_FN void render_word(struct render *r, uint16_t w);
static RENDER_FN void render_skip(struct render *r, uint32_t ticks)
{
    if (r->cursor_segment_index != -1 || r->needs_mode_recalc) {
        while (ticks--) {
            render_word(r, 0);   // a tick: data 0, DISPEN low, even
        }
        return;
    }
    struct teletext *tt = &r->teletext;
    // Three idle calls bring the pipeline to its fixed point (data pipe
    // 0x4040, DISPEN pipe 0, nothing decoded, the end of line past), and
    // idle_calls counts consecutive ones: an earlier skip often did it.
    uint32_t calls = ticks < 3 ? ticks : 3;
    for (uint32_t i = 0; i < calls && tt->idle_calls < 3; i++) {
        teletext_data(tt, 0, 0, TT_DECODE(r->mode == RENDER_MODE7));
    }
    while (ticks) {
        int32_t pos = r->render_pos;
        if (pos <= RENDER_WIDTH - 8) {
            uint32_t k = (uint32_t)(RENDER_WIDTH - pos) / 8;
            if (k > ticks) {
                k = ticks;
            }
            render_px *line = r->line + pos;
            for (uint32_t i = 0; i < 8 * k; i++) {
                line[i] = BG;
            }
            if (r->line2_wanted) {
                render_px *line2 = r->line2 + pos;
                for (uint32_t i = 0; i < 8 * k; i++) {
                    line2[i] = BG;
                }
            }
            r->render_pos = pos + 8 * (int32_t)k;
            r->span_end = (uint32_t)r->render_pos;
            r->horiz_beam_pos += 8 * (int32_t)k;
            r->stat_fast_words += k;
            ticks -= k;
            continue;
        }
        // Outside the window: the beam advances until it re-enters the
        // window or the forced hsync.
        // By arithmetic: the tick the beam's cell reaches the window start,
        // the tick it reaches the forced hsync, whichever is first (the
        // window start wins a tie, as in the per-tick order).
        int32_t horiz = r->horiz_beam_pos;
        int32_t cell = horiz & ~7;
        uint32_t k_ws = 0xffffffffu, k_hs = 0xffffffffu;
        if (cell < r->horiz_window_start) {
            k_ws = (uint32_t)(r->horiz_window_start - cell) / 8;
        }
        if (horiz < 1536) {
            k_hs = (uint32_t)(1536 - horiz + 7) / 8;
        }
        if (k_ws <= k_hs && k_ws <= ticks) {
            horiz += 8 * (int32_t)k_ws;
            ticks -= k_ws;
            r->horiz_beam_pos = horiz;
            reset_render_pos(r);
        } else if (k_hs <= ticks) {
            horiz += 8 * (int32_t)k_hs;
            ticks -= k_hs;
            r->horiz_beam_pos = horiz;
            do_hsync(r, 0);
        } else {
            r->horiz_beam_pos = horiz + 8 * (int32_t)ticks;
            ticks = 0;
        }
        r->stat_fast_words++;
    }
}

RENDER_FN void render_word(struct render *r, uint16_t w)
{
    if (!(w & VS_EVENT)) {  // tick
        uint8_t data = w >> 8;
        int odd = w & VS_ODD;
        int dispen = (w & VS_DISPEN) != 0;
        int ma13 = (w & VS_MA13) != 0;
        if (r->needs_mode_recalc) {
            mode_recalc(r);
        }
        prepare_tables(r);
        // beebjit's rendering tick: DISPEN, then the teletext data pipeline
        // on even ticks, then the render.
        if (r->is_dispen != dispen) {
            r->is_dispen = dispen;
            select_func(r);
        }
        if (!odd && (TT_RUN(ma13, r->teletext.idle_calls))) {
            teletext_data(&r->teletext, ma13 ? data : 0, ma13 ? dispen : 0, TT_DECODE(r->mode == RENDER_MODE7));
        }
        render_tick(r, data, odd);
        if (w & VS_PAIR) {
            render_tick(r, data, 1);
        }
        return;
    }
    uint8_t payload = w >> 8;
    r->stat_events++;
    switch (w & 0xff) {
    case VS_HSYNC:
        do_hsync(r, (uint32_t)payload << (r->is_clock_2mhz ? 0 : 1));
        break;
    case VS_VSYNC:
        if (payload) {
            do_vsync(r);
        } else {
            teletext_vsync_changed(&r->teletext, 0);
        }
        break;
    case VS_RA:
        teletext_ra_isv_changed(&r->teletext, payload & 0x1f, (payload & 0x80) != 0);
        r->row_address = payload & 0x1f;
        select_func(r);
        break;
    case VS_CURSOR:
        r->cursor_segment_index = 0;
        break;
    case VS_ULACTRL:
        r->stat_ulactrl++;
        set_mode(r, (payload >> 4) & 1, (payload >> 2) & 3, (payload >> 1) & 1);
        set_flash(r, payload & 1);
        r->cursor_segments[0] = (payload >> 7) & 1;
        r->cursor_segments[1] = (payload >> 6) & 1;
        r->cursor_segments[2] = (payload >> 5) & 1;
        r->cursor_segments[3] = (payload >> 5) & 1;
        break;
    case VS_ULAPAL:
        r->stat_ulapal++;
#ifdef RENDER_INDEXED
        end_span(r);   // what is rendered keeps the old palette
        r->logical_to_physical[payload >> 4] = payload & 0x0f;
        RENDER_PALETTE_ENTRY_HOOK(r, payload >> 4);
#else
        r->logical_to_physical[payload >> 4] = payload & 0x0f;
        r->palette_dirty |= (uint16_t)(1u << (payload >> 4));
#endif
        break;
    case VS_FRAMING:
        r->framing_changed = 1;
        break;
    case VS_SKIP:
        render_skip(r, payload);
        break;
    default:
        break;
    }
}

void render_flush(struct render *r)
{
    end_span(r);
}

// A row address or framing event, taken by the run loops in passing (RVI
// has one every two ticks): as render_word, less the select_func, which
// the caller does. 0 for any other word.
static inline RENDER_FN int run_event(struct render *r, uint16_t w)
{
    uint32_t ty = w & 0xff;
    if (ty == VS_RA) {
        uint8_t payload = (uint8_t)(w >> 8);
        teletext_ra_isv_changed(&r->teletext, payload & 0x1f, (payload & 0x80) != 0);
        r->row_address = payload & 0x1f;
    } else if (ty == VS_FRAMING) {
        r->framing_changed = 1;
    } else {
        return 0;
    }
    r->stat_events++;
    return 1;
}

// The per-word path above costs a few hundred clocks on the board; most
// words are plain bitmap ticks inside the window with nothing changing,
// which this loop renders with everything hoisted. It gives exactly what
// render_word would (the frame tests run through it).
// Measurement only: when set every word takes the slow path. Never set in
// normal use.
volatile int render_bench_skip;

// A word through render_word, counted as the slow path.
static RENDER_FN void consume_slow(struct render *r, uint16_t w)
{
    r->stat_slow_words++;
    uint32_t c0 = RENDER_CYCLES();
    render_word(r, w);
    uint32_t dc = RENDER_CYCLES() - c0;
    r->stat_clk_slow += dc;
    int ty = (w & VS_EVENT) ? (w & 15) : 0;
    r->stat_ev_n[ty]++;
    r->stat_ev_clk[ty] += dc;
}

RENDER_FN void render_consume_n(struct render *r, struct vstream *vs, int n)
{
    uint32_t t = vs->tail;
    const int all_slow = render_bench_skip;   // bit 0 (or 2): everything down the slow path; 3, 4: see the mode 7 loop
    while (n > 0) {
        uint16_t w = vs->ring[t & (VS_RING - 1)];
        if ((w & VS_EVENT) && !(all_slow & 5) && run_event(r, w)) {
            select_func(r);
            t++;
            n--;
            vs->tail = t;
            continue;
        }
        if ((w & VS_EVENT) || r->needs_mode_recalc || r->cursor_segment_index != -1 || (all_slow & 5)) {
            t++;
            n--;
            vs->tail = t;
            consume_slow(r, w);
            continue;
        }
        const int dispen = (w & VS_DISPEN) != 0;
        const int ma13 = (w & VS_MA13) != 0;
        if (r->is_dispen != dispen) {
            r->is_dispen = dispen;
            select_func(r);
        }
        const uint32_t run_flags = w & (VS_DISPEN | VS_MA13);
        const int mode7 = r->mode == RENDER_MODE7;
        struct teletext *tt = &r->teletext;
        int32_t horiz = r->horiz_beam_pos;
        if (r->render_pos > RENDER_WIDTH - 8) {
            // Outside the window: the beam advances and the teletext
            // pipeline runs, nothing is drawn. Same as render_tick's other
            // branches, for a run of ticks.
            // With the pipeline idle (MA13 clear) DISPEN changes nothing
            // here, so the run goes on through them and through row
            // address and framing events (RVI's short lines start outside
            // the window).
            const int tt_idle = !(TT_RUN(ma13, tt->idle_calls));
            int d = dispen;
            uint32_t t0 = t;
            for (;;) {
                int32_t step = 8;
                if (w & VS_PAIR) {
                    // A pair whose first tick lands on the window start or
                    // the forced hsync is two ticks for the slow path.
                    if (((horiz + 8) & ~7) == r->horiz_window_start || horiz + 8 >= 1536) {
                        r->horiz_beam_pos = horiz;
                        break;
                    }
                    step = 16;
                }
                d = (w & VS_DISPEN) != 0;
                if (!(w & VS_ODD) && (TT_RUN(ma13, tt->idle_calls))) {
                    teletext_data(tt, ma13 ? (uint8_t)(w >> 8) : 0, ma13 ? dispen : 0, TT_DECODE(mode7));
                }
                horiz += step;
                t++;
                n--;
                if ((horiz & ~7) == r->horiz_window_start) {
                    r->horiz_beam_pos = horiz;
                    reset_render_pos(r);
                    break;
                }
                if (horiz >= 1536) {
                    r->horiz_beam_pos = horiz;
                    do_hsync(r, 0);
                    break;
                }
                if (n <= 0) {
                    r->horiz_beam_pos = horiz;
                    break;
                }
                w = vs->ring[t & (VS_RING - 1)];
                if (tt_idle) {
                    while ((w & VS_EVENT) && run_event(r, w)) {
                        t++;
                        if (--n <= 0) {
                            break;
                        }
                        w = vs->ring[t & (VS_RING - 1)];
                    }
                    if (n <= 0 || (w & (VS_EVENT | VS_MA13))) {
                        r->horiz_beam_pos = horiz;
                        break;
                    }
                } else if ((w & VS_EVENT) || (w & (VS_DISPEN | VS_MA13)) != run_flags) {
                    r->horiz_beam_pos = horiz;
                    break;
                }
            }
            if (r->is_dispen != d || tt_idle) {
                r->is_dispen = d;
                select_func(r);
            }
            if (t == t0) {
                // A pair the loop leaves (its first tick on the window
                // start or the forced hsync): two ticks in render_word.
                t++;
                n--;
                vs->tail = t;
                consume_slow(r, w);
                continue;
            }
            r->stat_fast_words += t - t0;
            vs->tail = t;
            continue;
        }
        prepare_tables(r);
        const uint32_t t_in = t;
        const int blank = r->selected_blank;
        const int fast2 = r->is_clock_2mhz;
        const int want2 = r->line2_wanted;
        const render_px *black = r->black;
        const render_px (*t2)[8] = r->cur_2mhz;
        const render_px (*t1)[16] = r->cur_1mhz;
        render_px *line = r->line;
        render_px *line2 = r->line2;
        int32_t pos = r->render_pos;
        const int32_t pos0 = pos;
        if (!mode7 && !blank && !(TT_RUN(ma13, tt->idle_calls))) {
            // Bitmap pixels with the teletext pipeline idle: a table copy
            // per word, the clock and row-doubling variants hoisted out.
            // The same words as the general loop below renders.
#define RUN_2MHZ(DOUBLE) \
            while (!(w & VS_PAIR)) { \
                const render_px *v = t2[w >> 8]; \
                PX_COPY8(line + pos, v); \
                if (DOUBLE) { \
                    PX_COPY8(line2 + pos, v); \
                } \
                pos += 8; \
                horiz += 8; \
                t++; \
                n--; \
                if (n <= 0 || pos > RENDER_WIDTH - 8) { \
                    break; \
                } \
                w = vs->ring[t & (VS_RING - 1)]; \
                if ((w & VS_EVENT) || (w & (VS_DISPEN | VS_MA13)) != run_flags) { \
                    break; \
                } \
            }
#define RUN_1MHZ(DOUBLE) \
            for (;;) { \
                const render_px *v = t1[w >> 8]; \
                if (w & VS_PAIR) { \
                    if (pos > RENDER_WIDTH - 16) { \
                        break; \
                    } \
                    PX_COPY16(line + pos, v); \
                    if (DOUBLE) { \
                        PX_COPY16(line2 + pos, v); \
                    } \
                    pos += 16; \
                    horiz += 16; \
                } else { \
                    v += (w & VS_ODD) ? 8 : 0; \
                    PX_COPY8(line + pos, v); \
                    if (DOUBLE) { \
                        PX_COPY8(line2 + pos, v); \
                    } \
                    pos += 8; \
                    horiz += 8; \
                } \
                t++; \
                n--; \
                if (n <= 0 || pos > RENDER_WIDTH - 8) { \
                    break; \
                } \
                w = vs->ring[t & (VS_RING - 1)]; \
                if ((w & VS_EVENT) || (w & (VS_DISPEN | VS_MA13)) != run_flags) { \
                    break; \
                } \
            }
            if (fast2) {
                if (want2) {
                    RUN_2MHZ(1);
                } else {
                    RUN_2MHZ(0);
                }
            } else {
                if (want2) {
                    RUN_1MHZ(1);
                } else {
                    RUN_1MHZ(0);
                }
            }
#undef RUN_2MHZ
#undef RUN_1MHZ
        }
        if (!mode7 && fast2 && !(TT_RUN(ma13, tt->idle_calls)) && n > 0 && pos <= RENDER_WIDTH - 8 &&
            ((w & VS_EVENT) || !(w & (VS_PAIR | VS_MA13)))) {
            // 2 MHz bitmap ticks whose DISPEN changes, with row address and
            // framing events between them (RVI: two-character lines, each
            // a row address event, a tick with DISPEN and one without).
            // The teletext pipeline is idle (MA13 clear), so DISPEN and
            // the row address only choose between the table and black.
            int d = r->is_dispen;
            uint32_t rab = r->row_address & 8;
            for (;;) {
                if (w & VS_EVENT) {
                    if (!run_event(r, w)) {
                        break;
                    }
                    rab = r->row_address & 8;
                } else {
                    if (w & (VS_PAIR | VS_MA13)) {
                        break;
                    }
                    d = (w & VS_DISPEN) != 0;
                    const render_px *v = (d && !rab) ? t2[w >> 8] : black;
                    PX_COPY8(line + pos, v);
                    if (want2) {
                        PX_COPY8(line2 + pos, v);
                    }
                    pos += 8;
                    horiz += 8;
                }
                t++;
                n--;
                if (n <= 0 || pos > RENDER_WIDTH - 8) {
                    break;
                }
                w = vs->ring[t & (VS_RING - 1)];
            }
            r->is_dispen = d;
            select_func(r);
        }
        if (mode7 && !blank && pos == pos0) {
            // Teletext characters, one pair word each. In bulk through the
            // fused run (state in locals) whenever it will take them; one
            // word at a time while the DISPEN pipeline settles, or for the
            // boot bench's split variants, then back to the run.
            const int fused_ok = !(all_slow & 24) && run_flags == (VS_DISPEN | VS_MA13);
            while ((w & VS_PAIR) && pos <= RENDER_WIDTH - 16) {
                uint32_t c = 0;
                if (fused_ok) {
                    uint32_t kmax = (uint32_t)(RENDER_WIDTH - pos) / 16;
                    if (kmax > (uint32_t)n) {
                        kmax = (uint32_t)n;
                    }
                    uint32_t rc0 = RENDER_CYCLES();
                    c = teletext_run(tt, vs->ring, VS_RING - 1, t, kmax, line + pos, line2 + pos, want2);
                    r->stat_clk_ttrun += RENDER_CYCLES() - rc0;
                }
                if (c == 0) {
                    // (render_bench_skip bits 3 and 4 drop the pipeline or the
                    // pixels for the boot-time bench's split.)
                    if (!(all_slow & 8)) {
                        teletext_data(tt, ma13 ? (uint8_t)(w >> 8) : 0, ma13 ? dispen : 0, TT_DECODE(1));
                    }
                    if (!(all_slow & 16)) {
                        teletext_render_pair(tt, line + pos, line2 + pos, want2);
                    }
                    c = 1;
                }
                pos += 16 * (int32_t)c;
                horiz += 16 * (int32_t)c;
                t += c;
                n -= (int)c;
                if (n <= 0 || pos > RENDER_WIDTH - 8) {
                    break;
                }
                w = vs->ring[t & (VS_RING - 1)];
                if ((w & VS_EVENT) || (w & (VS_DISPEN | VS_MA13)) != run_flags) {
                    break;
                }
            }
        }
        if (pos == pos0) do {
            uint8_t data = (uint8_t)(w >> 8);
            int odd = w & VS_ODD;
            if (w & VS_PAIR) {
                // Teletext pairs after some single ticks (the DISPEN
                // settling at the start of a line): hand back so the outer
                // loop takes them through the mode 7 run.
                if (mode7 && pos != pos0) {
                    break;
                }
                // A whole 1 MHz character. At the right edge, or if the
                // renderer thinks the clock is 2 MHz, the slow path does
                // it as two ticks.
                if (pos > RENDER_WIDTH - 16 || fast2) {
                    break;
                }
                if (TT_RUN(ma13, tt->idle_calls)) {
                    teletext_data(tt, ma13 ? data : 0, ma13 ? dispen : 0, TT_DECODE(mode7));
                }
                if (mode7) {
                    teletext_render_pair(tt, line + pos, line2 + pos, want2);
                } else if (blank) {
                    PX_COPY8(line + pos, black);
                    PX_COPY8(line + pos + 8, black);
                    if (want2) {
                        PX_COPY8(line2 + pos, black);
                        PX_COPY8(line2 + pos + 8, black);
                    }
                } else {
                    PX_COPY16(line + pos, t1[data]);
                    if (want2) {
                        PX_COPY16(line2 + pos, t1[data]);
                    }
                }
                pos += 16;
                horiz += 16;
                t++;
                n--;
                if (n <= 0 || pos > RENDER_WIDTH - 8) {
                    break;
                }
                w = vs->ring[t & (VS_RING - 1)];
                if ((w & VS_EVENT) || (w & (VS_DISPEN | VS_MA13)) != run_flags) {
                    break;
                }
                continue;
            }
            if (!odd && (TT_RUN(ma13, tt->idle_calls))) {
                teletext_data(tt, ma13 ? data : 0, ma13 ? dispen : 0, TT_DECODE(mode7));
            }
            if (mode7) {
                teletext_render(tt, odd, line + pos, line2 + pos);
            } else {
                const render_px *v = blank ? black : (fast2 ? t2[data] : t1[data] + (odd ? 8 : 0));
                PX_COPY8(line + pos, v);
                if (want2) {
                    PX_COPY8(line2 + pos, v);
                }
            }
            pos += 8;
            horiz += 8;
            t++;
            n--;
            if (n <= 0 || pos > RENDER_WIDTH - 8) {
                break;
            }
            w = vs->ring[t & (VS_RING - 1)];
        } while (!(w & VS_EVENT) && (w & (VS_DISPEN | VS_MA13)) == run_flags);
        if (t == t_in) {
            // A word none of the loops takes: a pair at the right edge
            // (half of it fits) or one the renderer, still at 2 MHz, must
            // split. render_word does it as two ticks. (Before this the
            // loop came back to the same word for ever: evil-inf at 362 s,
            // a MODE 5 pair at render_pos 760.)
            t++;
            n--;
            vs->tail = t;
            consume_slow(r, w);
            continue;
        }
        r->stat_fast_words += (uint32_t)(pos - pos0) / 8;
        r->render_pos = pos;
        r->span_end = (uint32_t)pos;
        r->horiz_beam_pos = horiz;
        vs->tail = t;
    }
}

void render_consume(struct render *r, struct vstream *vs)
{
    int n;
    while ((n = vs_available(vs)) > 0) {
        render_consume_n(r, vs, n);
    }
}
