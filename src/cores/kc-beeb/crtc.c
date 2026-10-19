// esp32-beeb: 6845 CRTC. See crtc.h. Included into the machine's translation
// unit after the hook macros are defined; crtc_tick() is static inline there.

#include "crtc.h"

#ifndef CRTC_API
#define CRTC_API
#endif

#ifndef CRTC_FETCH
#define CRTC_FETCH(c, ma, ra, odd) ((void)(ma), (void)(ra), (void)(odd), (uint8_t)0)
#endif
#ifndef CRTC_RENDER
#define CRTC_RENDER(c, data, dispen, odd, ma13) ((void)0)
#endif
#ifndef CRTC_HSYNC
#define CRTC_HSYNC(c, width) ((void)0)
#endif
#ifndef CRTC_VSYNC
#define CRTC_VSYNC(c, level) ((void)0)
#endif
#ifndef CRTC_CURSOR
#define CRTC_CURSOR(c) ((void)0)
#endif
#ifndef CRTC_TELETEXT
#define CRTC_TELETEXT(c, data, dispen) ((void)0)
#endif
#ifndef CRTC_CYCLES
#define CRTC_CYCLES() 0u
#endif
#ifndef CRTC_RA
#define CRTC_RA(c, ra, isv) ((void)0)
#endif

#define CRTC_INLINE static inline __attribute__((always_inline))

#ifdef MACHINE_STATS
// Ticks and calls through crtc_short_lines, and of those through crtc_short_block
// (rp2350: here rather than in struct crtc, so struct machine's layout stays as it was).
static uint32_t crtc_stat_short, crtc_stat_short_calls, crtc_stat_block, crtc_stat_block_calls;
static uint64_t crtc_stat_clk_short;
#endif

enum { DISPEN_HORIZ = 1, DISPEN_VERT = 2, DISPEN_ALL = 3 };

static CRTC_API void crtc_update_cursor_disabled(struct crtc *c)
{
    c->cursor_disabled = ((c->reg[CRTC_R10_CSTART] & 0x60) == 0x20) ||
                         ((c->reg[CRTC_R8_INTERLACE] & 0xc0) == 0xc0);
}

CRTC_INLINE void crtc_update_vsync(struct crtc *c)
{
    int level = (c->is_interlace && !c->is_odd_frame) ? c->is_even_vsync : c->is_odd_vsync;
    if (level == c->in_vsync) {
        return;
    }
    c->in_vsync = level;
    c->vsync_changed = 1;
    CRTC_VSYNC(c, level);
}

static CRTC_API void crtc_start_new_frame(struct crtc *c)
{
    c->scanline_counter = 0;
    c->vert_counter = 0;
    c->vert_adjust_counter = 0;
    c->had_odd_vsync_this_row = 0;
    c->had_even_vsync_this_row = 0;
    c->is_in_vert_adjust = 0;
    c->in_dummy_raster = 0;
    c->is_odd_frame = c->frames & 1;
    c->has_hit_cursor_line_start = 0;
    c->has_hit_cursor_line_end = 0;
    c->is_end_of_main_latched = 0;
    c->is_end_of_vert_adjust_latched = 0;
    c->is_end_of_frame_latched = 0;
    c->is_first_frame_scanline = 1;
    c->display_enable_bits |= DISPEN_VERT;
    c->address_counter = (c->reg[CRTC_R12_MAH] << 8) | c->reg[CRTC_R13_MAL];
    c->address_counter_saved = c->address_counter;
}

// beebjit's check_r6 / check_r7 tail: run at the start of every scanline and
// once at reset, and again after an R7 write (which can start a vsync at
// once on the Hitachi part).
static CRTC_API void crtc_line_start_checks(struct crtc *c, int from_r6)
{
    if (from_r6) {
        int r6_hit = (c->vert_counter == c->reg[CRTC_R6_VDISP]);
        if (r6_hit && (c->display_enable_bits & DISPEN_VERT) && !c->is_first_frame_scanline) {
            c->display_enable_bits &= ~DISPEN_VERT;
            c->frames++;  // the Hitachi counts frames on R6 hit
        }
    }
    int r7_hit = (c->vert_counter == c->reg[CRTC_R7_VSYNC]);
    if (r7_hit && !c->had_odd_vsync_this_row) {
        c->had_odd_vsync_this_row = 1;
        c->is_odd_vsync = 1;
        if (!c->in_vsync) {
            c->vsync_scanline_counter = c->vsync_width;
        }
        crtc_update_vsync(c);
    }
    {
        uint8_t ra = c->scanline_counter;
        if (c->is_isv) {
            ra |= c->is_odd_frame;
        }
        if (c->scanline_counter == c->cursor_start_line) {
            c->has_hit_cursor_line_start = 1;
        }
        CRTC_RA(c, ra, c->is_isv);
    }
    c->r4_hit = (c->vert_counter == c->reg[CRTC_R4_VTOTAL]);
    c->r5_hit = (c->vert_adjust_counter == c->reg[CRTC_R5_VADJ]);
    c->r9_hit = (c->scanline_counter == (c->reg[CRTC_R9_MAXRA] & c->scanline_mask));
}

CRTC_INLINE uint8_t crtc_rendering_tick(struct crtc *c, uint8_t horiz_counter,
                                        int r0_hit, int r9_hit, int is_odd_tick)
{
    int r1_hit = (horiz_counter == c->reg[CRTC_R1_HDISP]);
    if (r1_hit && r9_hit) {
        c->address_counter_saved = c->address_counter;
    }
    if (r1_hit || r0_hit) {
        c->display_enable_bits &= ~DISPEN_HORIZ;
    }

    if (c->in_hsync) {
        if (--c->hsync_tick_counter == 0) {
            c->in_hsync = 0;
        }
    } else if (horiz_counter == c->reg[CRTC_R2_HSYNC] && c->hsync_width > 0) {
        CRTC_HSYNC(c, c->hsync_width);
        c->in_hsync = 1;
        c->hsync_tick_counter = c->hsync_width;
    }

    // DISPEN history for the R8 skew.
    c->dispen_shifts[2] = c->dispen_shifts[1];
    c->dispen_shifts[1] = c->dispen_shifts[0];
    c->dispen_shifts[0] = (c->display_enable_bits == DISPEN_ALL);
    int dispen = c->dispen_shifts[c->skew_dispen_index];

    if (!c->cursor_disabled) {
        uint16_t cursor_addr = (c->reg[CRTC_R14_CAH] << 8) | c->reg[CRTC_R15_CAL];
        if (c->address_counter == cursor_addr && c->has_hit_cursor_line_start &&
            !c->has_hit_cursor_line_end &&
            (!c->cursor_flashing || (c->frames & c->cursor_flash_mask))) {
            c->cursor_skew_counter = c->cursor_skew;
        }
        if (c->cursor_skew_counter >= 0) {
            c->cursor_skew_counter--;
            if (c->cursor_skew_counter == -1 && c->dispen_shifts[c->cursor_skew]) {
                CRTC_CURSOR(c);
            }
        }
    }

    uint8_t ra = c->scanline_counter;
    if (c->is_isv) {
        ra |= c->is_odd_frame;
    }
    uint16_t ma = c->address_counter;
    uint8_t data = CRTC_FETCH(c, ma, ra, is_odd_tick);
    c->data_ma13 = (ma >> 13) & 1;
    if (!is_odd_tick) {
        // The IC15 latch only passes data and DISPEN to the teletext chip
        // under MODE 7 style (MA13 set) addressing.
        if (ma & 0x2000) {
            CRTC_TELETEXT(c, data, dispen);
        } else {
            CRTC_TELETEXT(c, 0, 0);
        }
    }
    c->address_counter = (ma + 1) & 0x3fff;
    (void)dispen;
    c->data_byte = data;
    return data;
}

// One step of the start-of-line chain: beebjit's four-tick latch sequence
// after the R0 hit, which decides vertical adjust and the end of frame
// from the hits latched at the line start. It runs on counting ticks
// (every tick at 2 MHz, the odd ticks at 1 MHz).
CRTC_INLINE uint8_t crtc_chain_step(struct crtc *c, uint8_t checks)
{
    if (checks & 8) {
        if (c->is_end_of_vert_adjust_latched) {
            c->is_end_of_frame_latched = 1;
        }
        checks &= ~8;
    }
    if (checks & 4) {
        if (c->is_end_of_main_latched) {
            if (c->r5_hit) {
                c->is_end_of_vert_adjust_latched = 1;
            } else if (!c->is_in_vert_adjust && !c->in_dummy_raster) {
                c->is_vert_adjust_pending = 1;
            }
        }
        checks &= ~4;
        checks |= 8;
    }
    if (checks & 2) {
        if (c->r4_hit && c->r9_hit) {
            c->is_end_of_main_latched = 1;
        }
        checks &= ~2;
        checks |= 4;
    }
    if (checks & 1) {
        checks &= ~1;
        checks |= 2;
    }
    return checks;
}

// The end of a line: the tick after the R0 hit's counting step. `checks`
// is the start-of-line chain as that step left it.
CRTC_INLINE void crtc_end_of_line(struct crtc *c, uint8_t r0, uint8_t checks)
{
    uint8_t horiz_counter;
    // End of horizontal line.
    horiz_counter = 0;
    c->display_enable_bits |= DISPEN_HORIZ;
    checks |= 1;

    c->is_first_frame_scanline = 0;
    if (c->scanline_counter == c->reg[CRTC_R11_CEND]) {
        c->has_hit_cursor_line_end = 1;
    }

    if (c->in_vsync) {
        if (c->vsync_scanline_counter > 0) {
            c->vsync_scanline_counter--;
        }
        if (c->vsync_scanline_counter == 0) {
            c->is_odd_vsync = 0;
            crtc_update_vsync(c);
        }
    }

    if (r0 != 0) {
        c->scanline_counter = (c->scanline_counter + c->scanline_stride) & c->scanline_mask;
    }
    c->address_counter = c->address_counter_saved;

    if (c->r9_hit && !c->is_in_vert_adjust) {
        c->has_hit_cursor_line_start = 0;
        c->has_hit_cursor_line_end = 0;
        c->had_odd_vsync_this_row = 0;
        c->had_even_vsync_this_row = 0;
        c->scanline_counter = 0;
        c->vert_counter = (c->vert_counter + 1) & 0x7f;
    }

    c->horiz_counter = horiz_counter;
    c->start_of_line_state_checks = checks;

    if (c->in_dummy_raster) {
        crtc_start_new_frame(c);
        crtc_line_start_checks(c, 0);
        return;
    } else if (c->is_vert_adjust_pending) {
        c->is_vert_adjust_pending = 0;
        c->is_in_vert_adjust = 1;
    } else if (c->is_end_of_frame_latched) {
        if (c->is_interlace && (c->frames & 1)) {
            c->in_dummy_raster = 1;
        } else {
            crtc_start_new_frame(c);
            crtc_line_start_checks(c, 0);
            return;
        }
    }

    if (c->is_in_vert_adjust) {
        c->vert_adjust_counter = (c->vert_adjust_counter + 1) & 0x1f;
    }

    crtc_line_start_checks(c, 1);
}

// One 2 MHz system tick. `tick` is the index of the tick being processed.
CRTC_INLINE void crtc_tick(struct crtc *c, uint32_t tick)
{
    int is_odd_tick = tick & 1;
    int is_1mhz = !c->ula_clock_fast;
    uint8_t r0 = c->reg[CRTC_R0_HTOTAL];
    uint8_t horiz_counter = c->horiz_counter;
    int r0_hit = (horiz_counter == r0);

    if (!is_1mhz || !is_odd_tick) {
        c->data_byte = crtc_rendering_tick(c, horiz_counter, r0_hit, c->r9_hit, is_odd_tick);
#ifdef CRTC_FETCH_LOG
        CRTC_FETCH_LOG(c, tick, horiz_counter, (c->address_counter - 1) & 0x3fff, c->data_byte);
#endif
    }
    CRTC_RENDER(c, c->data_byte, c->dispen_shifts[c->skew_dispen_index], is_odd_tick, c->data_ma13);

    if (is_1mhz && !is_odd_tick) {
        return;
    }

    uint8_t checks = c->start_of_line_state_checks;
    if (checks) {
        checks = crtc_chain_step(c, checks);
    }

    horiz_counter++;  // wraps at 256

    if ((c->vert_counter == c->reg[CRTC_R7_VSYNC]) || c->is_even_vsync) {
        if (horiz_counter == c->half_r0) {
            if (!c->had_even_vsync_this_row) {
                c->had_even_vsync_this_row = 1;
                c->is_even_vsync = 1;
            } else if (c->vsync_scanline_counter == 0) {
                c->is_even_vsync = 0;
            }
            crtc_update_vsync(c);
        }
    }

    if (!r0_hit) {
        c->horiz_counter = horiz_counter;
        c->start_of_line_state_checks = checks;
        return;
    }
    crtc_end_of_line(c, r0, checks);
}

// How many line ends, at least, before the row counter can equal R7 at a
// line start (where a vsync starts): rows advance it by one at a row end,
// and a frame start sets it to 0 at any line end. A row is R9 / stride + 1
// lines from a frame start or a row end; the current one ends at the line
// whose start had an R9 hit. With R0 = 0 the scanline counter stands
// still, so every line end counts.
CRTC_INLINE uint32_t crtc_line_ends_to_r7(const struct crtc *c)
{
    uint32_t vc = c->vert_counter, r7 = c->reg[CRTC_R7_VSYNC];
    if (vc == r7 || c->reg[CRTC_R0_HTOTAL] == 0) {
        return 1;
    }
    uint32_t stride = c->scanline_stride, mask = c->scanline_mask;
    uint32_t target = c->reg[CRTC_R9_MAXRA] & mask;
    uint32_t row = target / stride + 1;
    uint32_t cur = c->r9_hit ? 1 : (((target - c->scanline_counter) & mask) / stride + 1);
    uint32_t via_reset = 1 + r7 * row;   // a frame start at the next line end, then R7 rows
    if (r7 < vc) {
        return via_reset;
    }
    uint32_t direct = cur + (r7 - vc - 1) * row;
    return direct < via_reset ? direct : via_reset;
}

// Ticks until the next tick at which this CRTC could change the vsync
// level: the half-R0 point in a row that is at or in vsync, any line end
// in a vsync, otherwise the line end at which the row counter could first
// reach R7 (RVI's two-character lines end every other tick, far from it).
// Register writes reset the estimate through the machine. Never less
// than 1.
CRTC_INLINE uint32_t crtc_ticks_to_check(const struct crtc *c, uint32_t tick)
{
    uint32_t hc = c->horiz_counter;
    uint32_t r0 = c->reg[CRTC_R0_HTOTAL];
    uint32_t to_eol = (hc <= r0) ? (r0 - hc + 1) : (256 - hc + r0 + 1);
    uint32_t t = to_eol;
    if (!c->in_vsync && !c->is_even_vsync) {
        uint32_t lines = crtc_line_ends_to_r7(c);
        if (lines > 1) {
            uint32_t more = (lines - 1) * (r0 + 1);
            t = (more > 4096) ? 4096 : to_eol + more;
        }
    }
    if ((c->vert_counter == c->reg[CRTC_R7_VSYNC]) || c->is_even_vsync) {
        uint32_t half = c->half_r0;
        uint32_t to_half = (hc < half) ? (half - hc) : (256 - hc + half);
        if (to_half < t) {
            t = to_half;
        }
    }
    // 1 MHz mode: counters step on the odd system ticks, so from an odd
    // tick the t-th step is 2t - 1 ticks away (as crtc_fast_ticks); 2t
    // overshot by one and a CRTC left behind could run over a vsync change.
    if (!c->ula_clock_fast) {
        t = 2 * t - (tick & 1);
    }
    return t ? t : 1;
}

// How many system ticks from `tick` can run the fast path: no counter hit
// (R0, R1, R2, the half-R0 vsync point), no start-of-line chain, no hsync
// in progress, no cursor activity, and a settled DISPEN pipeline. Zero
// means the next tick must take the exact path.
CRTC_INLINE uint32_t crtc_fast_ticks(const struct crtc *c, uint32_t tick)
{
    // (An hsync in progress and the start-of-line chain are bookkeeping
    // the fast path does itself.)
    if (c->cursor_skew_counter >= 0) {
        return 0;
    }
    // (A DISPEN pipeline still settling is stepped by the fast path too.)
    uint32_t hc = c->horiz_counter;
    uint32_t r0 = c->reg[CRTC_R0_HTOTAL];
    uint32_t l = (hc <= r0) ? (r0 - hc) : (256 - hc + r0);
    uint32_t r1 = c->reg[CRTC_R1_HDISP];
    if (hc <= r1 && r1 - hc < l) {
        l = r1 - hc;
    }
    uint32_t r2 = c->reg[CRTC_R2_HSYNC];
    if (hc <= r2 && r2 - hc < l && c->hsync_width > 0) {
        l = r2 - hc;
    }
    if ((c->vert_counter == c->reg[CRTC_R7_VSYNC]) || c->is_even_vsync) {
        uint32_t half = c->half_r0;
        if (hc + 1 <= half) {
            uint32_t d = half - 1 - hc;
            if (d < l) {
                l = d;
            }
        }
    }
    if (!c->cursor_disabled && c->has_hit_cursor_line_start && !c->has_hit_cursor_line_end) {
        uint32_t cursor = (c->reg[CRTC_R14_CAH] << 8) | c->reg[CRTC_R15_CAL];
        uint32_t d = (cursor - c->address_counter) & 0x3fff;
        if (d < l) {
            l = d;
        }
    }
    if (c->ula_clock_fast) {
        return l;
    }
    // 1 MHz: an even tick fetches, the odd tick after it counts.
    return l ? (2 * l - (tick & 1)) : 0;
}

// A run of renders may batch its stream writes (machine.c does).
#ifndef CRTC_RUN_BEGIN
#define CRTC_RUN_BEGIN(c) ((void)0)
#define CRTC_RUN_RENDER(c, data, dispen, odd, ma13) CRTC_RENDER(c, data, dispen, odd, ma13)
#define CRTC_RUN_RENDER_PAIR(c, data, dispen, ma13) do { \
        CRTC_RENDER(c, data, dispen, 0, ma13); \
        CRTC_RENDER(c, data, dispen, 1, ma13); \
    } while (0)
#define CRTC_RUN_END(c) ((void)0)
#endif
// Linear fetch runs: the owner may give the fast path a pointer to the
// byte for MA, the step to the next character's byte and how many
// characters (at least 1) the run stays linear for, so the loop is a load
// and a store per character. Without that, one character at a time
// through CRTC_FETCH.
#ifndef CRTC_FETCH_RUN
#define CRTC_FETCH_RUN(c, ma, ra, tick, pp, pstride, pchars) do { \
        (void)(c); (void)(ma); (void)(ra); (void)(tick); \
        *(pp) = 0; *(pstride) = 0; *(pchars) = 1; \
    } while (0)
// `chars` characters at 2 MHz from `ma`, one tick each; leaves the last byte in `data`.
#define CRTC_RUN_RENDER_CHARS(c, p, stride, chars, dispen, tick, ma13, ma, ra, data) do { \
        for (uint32_t k_ = 0; k_ < (chars); k_++) { \
            int odd_ = ((tick) + k_) & 1; \
            data = CRTC_FETCH(c, (uint16_t)(((ma) + k_) & 0x3fff), ra, odd_); \
            CRTC_RUN_RENDER(c, data, dispen, odd_, ma13); \
        } \
    } while (0)
// `chars` characters at 1 MHz from `ma`, two ticks each, as pair words.
#define CRTC_RUN_RENDER_PAIRS(c, p, stride, chars, dispen, ma13, ma, ra, data) do { \
        for (uint32_t k_ = 0; k_ < (chars); k_++) { \
            data = CRTC_FETCH(c, (uint16_t)(((ma) + k_) & 0x3fff), ra, 0); \
            CRTC_RUN_RENDER_PAIR(c, data, dispen, ma13); \
        } \
    } while (0)
#endif

// The fast path: `n` ticks that only fetch, render and count. Fetches go
// in linear runs (CRTC_FETCH_RUN) cut at the addressing boundaries; MA13
// is constant within one, so the stream flags are too.
CRTC_INLINE void crtc_fast(struct crtc *c, uint32_t tick, uint32_t n)
{
    CRTC_RUN_BEGIN(c);
    uint8_t ra = c->scanline_counter;
    if (c->is_isv) {
        ra |= c->is_odd_frame;
    }
    int dispen = c->dispen_shifts[c->skew_dispen_index];
    uint16_t ma = c->address_counter;
    uint8_t data = c->data_byte;
    uint8_t ma13 = c->data_ma13;
    uint8_t hc = c->horiz_counter;
    uint32_t i = 0;
    if (c->start_of_line_state_checks) {
        // The chain's inputs are latched at the line start and only a
        // register write changes them, which syncs the machine first and
        // so ends the run: step it once per counting tick.
        uint32_t counting = c->ula_clock_fast ? n : ((tick & 1) ? (n + 1) / 2 : n / 2);
        uint8_t checks = c->start_of_line_state_checks;
        while (checks && counting) {
            checks = crtc_chain_step(c, checks);
            counting--;
        }
        c->start_of_line_state_checks = checks;
    }
    {
        // The DISPEN pipeline (R8 skew) settles over the three fetch ticks
        // after a display-enable change: step those one at a time, as the
        // exact tick does, with the pipeline's value for each.
        uint8_t cur = (c->display_enable_bits == DISPEN_ALL);
        while (i < n && (c->dispen_shifts[0] != cur || c->dispen_shifts[1] != cur || c->dispen_shifts[2] != cur)) {
            uint32_t t = tick + i;
            if (c->ula_clock_fast || !(t & 1)) {
                c->dispen_shifts[2] = c->dispen_shifts[1];
                c->dispen_shifts[1] = c->dispen_shifts[0];
                c->dispen_shifts[0] = cur;
                dispen = c->dispen_shifts[c->skew_dispen_index];
                data = CRTC_FETCH(c, ma, ra, t & 1);
                ma13 = (ma >> 13) & 1;
                ma = (ma + 1) & 0x3fff;
                CRTC_RUN_RENDER(c, data, dispen, t & 1, ma13);
                if (c->ula_clock_fast) {
                    hc++;
                }
            } else {
                CRTC_RUN_RENDER(c, data, dispen, 1, ma13);
                hc++;
            }
            i++;
        }
        dispen = c->dispen_shifts[c->skew_dispen_index];
    }
    if (c->in_hsync) {
        // The hsync counter steps once per fetch tick (every tick at 2 MHz,
        // the even ticks at 1 MHz). A new hsync can only start at an R2
        // hit, which is never inside a run.
        uint32_t fetches = c->ula_clock_fast ? n : ((tick & 1) ? n / 2 : (n + 1) / 2);
        if (c->hsync_tick_counter > fetches) {
            c->hsync_tick_counter -= fetches;
        } else {
            c->hsync_tick_counter = 0;
            c->in_hsync = 0;
        }
    }
    if (c->ula_clock_fast) {
        uint32_t i0 = i;
        while (i < n) {
            const uint8_t *p;
            uint32_t stride, chars;
            CRTC_FETCH_RUN(c, ma, ra, tick + i, &p, &stride, &chars);
            if (chars > n - i) {
                chars = n - i;
            }
            ma13 = (ma >> 13) & 1;
            CRTC_RUN_RENDER_CHARS(c, p, stride, chars, dispen, tick + i, ma13, ma, ra, data);
            ma = (ma + chars) & 0x3fff;
            i += chars;
        }
        hc = (uint8_t)(hc + (n - i0));
    } else {
        // 1 MHz: the even tick fetches, the odd one counts. Both ticks of a
        // character inside the run go out as one pair word.
        if (i < n && ((tick + i) & 1)) {
            CRTC_RUN_RENDER(c, data, dispen, 1, ma13);   // the odd tick of the current character
            hc++;
            i++;
        }
        while (i + 1 < n) {
            const uint8_t *p;
            uint32_t stride, chars;
            CRTC_FETCH_RUN(c, ma, ra, tick + i, &p, &stride, &chars);
            uint32_t avail = (n - i) / 2;
            if (chars > avail) {
                chars = avail;
            }
            ma13 = (ma >> 13) & 1;
            CRTC_RUN_RENDER_PAIRS(c, p, stride, chars, dispen, ma13, ma, ra, data);
            ma = (ma + chars) & 0x3fff;
            i += 2 * chars;
            hc = (uint8_t)(hc + chars);
        }
        if (i < n) {
            // A lone even tick: fetch and render the first half.
            data = CRTC_FETCH(c, ma, ra, 0);
            ma13 = (ma >> 13) & 1;
            ma = (ma + 1) & 0x3fff;
            CRTC_RUN_RENDER(c, data, dispen, 0, ma13);
            i++;
        }
    }
    CRTC_RUN_END(c);
    c->address_counter = ma;
    c->data_byte = data;
    c->data_ma13 = ma13;
    c->horiz_counter = hc;
}

// Lines this short (R0 below it) go through crtc_short_lines.
#ifndef CRTC_SHORT_R0
#define CRTC_SHORT_R0 8
#endif
#ifndef CRTC_RUN_RELOAD
#define CRTC_RUN_RELOAD(c) ((void)0)
#endif
#ifndef CRTC_RUN_RA
#define CRTC_RUN_RA(c, ra, isv) CRTC_RA(c, ra, isv)
#endif
#ifndef CRTC_RUN_HSYNC
#define CRTC_RUN_HSYNC(c, width) CRTC_HSYNC(c, width)
#endif

// Plain two-character lines (R0 = 1): RVI's steady state. Each line is a
// tick with the row's first byte, one with its second and the row address
// event of a plain line end; nothing else can happen in them when, as
// checked first, the start-of-line chain has nothing latched, R1 and R2
// are past the line, the cursor is off and the row is at neither R6 nor
// R7. From either tick of a line (palette writes catch the CRTC up
// mid-line, so runs start and end there): the rest of that line, whole
// lines up to the line end that reaches R9 (the next end is a row end),
// and a first tick of the next if the run ends there. The rest is worked
// out from the counts: the cursor line flags, the chain bits, the hsync
// count down and the DISPEN history (the same every line from the second
// whole line). crtc_short_lines' loop keeps most of its state on the
// stack on the LX7 (14 usable registers for about 20 values); this leaf
// keeps a dozen. Returns the ticks run; 0 if it could not start here.
static CRTC_API __attribute__((noinline)) uint32_t crtc_short_block(struct crtc *c, uint32_t tick, uint32_t n)
{
    uint32_t sc0 = c->scanline_counter;
    uint32_t saved = c->address_counter_saved;
    uint32_t bits = c->display_enable_bits;
    uint32_t hc = c->horiz_counter;
    if (n == 0 || hc > 1 || c->reg[CRTC_R0_HTOTAL] != 1 || c->scanline_stride != 1 ||
        c->reg[CRTC_R1_HDISP] <= 1 || (c->hsync_width > 0 && c->reg[CRTC_R2_HSYNC] <= 1) ||
        c->r9_hit || !c->cursor_disabled || c->cursor_skew_counter >= 0 || !(bits & DISPEN_HORIZ) ||
        c->in_vsync || c->is_even_vsync || c->in_dummy_raster || c->is_vert_adjust_pending ||
        c->is_end_of_frame_latched || c->is_in_vert_adjust || c->is_end_of_main_latched ||
        c->is_end_of_vert_adjust_latched || c->vert_counter == c->reg[CRTC_R7_VSYNC] ||
        (c->vert_counter == c->reg[CRTC_R6_VDISP] && (bits & DISPEN_VERT)) ||
        c->address_counter != ((saved + hc) & 0x3fff) || ((saved | (saved + 1)) & 0x2000)) {
        return 0;
    }
    const uint8_t *p0;
    uint32_t pstride, chars;
    CRTC_FETCH_RUN(c, (uint16_t)saved, 0, tick, &p0, &pstride, &chars);
    if (chars < 2) {
        return 0;
    }
    const uint8_t *p1 = p0 + pstride;
    uint32_t r9m = c->reg[CRTC_R9_MAXRA] & 0x1f;
    uint32_t to_r9 = ((r9m - sc0 - 1) & 0x1f) + 1;   // line ends up to the one reaching R9
    // The run: lead (the line's second tick), whole lines, trail (a first tick).
    uint32_t lead = hc, ends, lines, trail;
    lines = (n - lead) / 2;
    if (lead + lines > to_r9) {
        lines = to_r9 - lead;
    }
    ends = lead + lines;
    trail = (ends < to_r9 && n - lead - 2 * lines > 0) ? 1 : 0;
    uint32_t ticks = lead + 2 * lines + trail;
    uint32_t isv = c->is_isv, oddf = isv ? c->is_odd_frame : 0;
    uint32_t cur = (bits == DISPEN_ALL);
    uint32_t skew = c->skew_dispen_index;
    uint32_t checks = c->start_of_line_state_checks;
    // The DISPEN history, newest in bit 0. A line's first tick shifts in
    // cur, its second (the R0 hit) 0; every whole line after the first
    // sees 5 * cur and 2 * cur and leaves 2 * cur.
    uint32_t pipe = (c->dispen_shifts[0] ? 1 : 0) | (c->dispen_shifts[1] ? 2 : 0) | (c->dispen_shifts[2] ? 4 : 0);
    uint32_t sc = sc0, data = c->data_byte, odd = tick & 1, t = tick;
    CRTC_RUN_BEGIN(c);
#ifdef CRTC_FETCH_LOG
#define BLOCK_LOG(k) CRTC_FETCH_LOG(c, t, k, (saved + (k)) & 0x3fff, data)
#else
#define BLOCK_LOG(k) ((void)0)
#endif
    if (lead) {
        pipe = (pipe << 1) & 7;
        data = p1[(sc | oddf) & 7];
        BLOCK_LOG(1);
        CRTC_RUN_RENDER(c, data, (pipe >> skew) & 1, odd, 0);
        sc = (sc + 1) & 0x1f;
        CRTC_RUN_RA(c, sc | oddf, isv);
        checks = ((checks << 1) & 0xe) | 1;
        odd ^= 1;
        t++;
    }
    if (lines) {
        // The first whole line from the history as it is, the rest steady.
        uint32_t f = (sc | oddf) & 7;
        pipe = ((pipe << 1) | cur) & 7;
        data = p0[f];
        BLOCK_LOG(0);
        CRTC_RUN_RENDER(c, data, (pipe >> skew) & 1, odd, 0);
        pipe = (pipe << 1) & 7;
        t++;
        data = p1[f];
        BLOCK_LOG(1);
        CRTC_RUN_RENDER(c, data, (pipe >> skew) & 1, odd ^ 1, 0);
        t++;
        sc = (sc + 1) & 0x1f;
        CRTC_RUN_RA(c, sc | oddf, isv);
        checks = ((checks << 2) & 0xc) | 1;
        uint32_t b0 = ((5 * cur) >> skew) & 1, b1 = ((2 * cur) >> skew) & 1;
        for (uint32_t j = 1; j < lines; j++) {
            f = (sc | oddf) & 7;
            data = p0[f];
            BLOCK_LOG(0);
            CRTC_RUN_RENDER(c, data, b0, odd, 0);
            t++;
            data = p1[f];
            BLOCK_LOG(1);
            CRTC_RUN_RENDER(c, data, b1, odd ^ 1, 0);
            t++;
            sc = (sc + 1) & 0x1f;
            CRTC_RUN_RA(c, sc | oddf, isv);
        }
        if (lines > 1) {
            pipe = cur << 1;
            checks = 5;
        }
    }
    if (trail) {
        pipe = ((pipe << 1) | cur) & 7;
        data = p0[(sc | oddf) & 7];
        BLOCK_LOG(0);
        CRTC_RUN_RENDER(c, data, (pipe >> skew) & 1, odd, 0);
        checks = (checks << 1) & 0xe;
    }
#undef BLOCK_LOG
    CRTC_RUN_END(c);
    c->dispen_shifts[0] = pipe & 1;
    c->dispen_shifts[1] = (pipe >> 1) & 1;
    c->dispen_shifts[2] = (pipe >> 2) & 1;
    c->start_of_line_state_checks = (uint8_t)checks;
    if (c->in_hsync) {
        if (c->hsync_tick_counter > ticks) {
            c->hsync_tick_counter = (uint8_t)(c->hsync_tick_counter - ticks);
        } else {
            c->hsync_tick_counter = 0;
            c->in_hsync = 0;
        }
    }
    if (ends) {
        // The ends saw scanlines sc0 .. sc0 + ends - 1 before the step and
        // one more after it.
        uint32_t r11 = c->reg[CRTC_R11_CEND], csl = c->cursor_start_line;
        if (r11 <= 0x1f && ((r11 - sc0) & 0x1f) < ends) {
            c->has_hit_cursor_line_end = 1;
        }
        if (csl <= 0x1f && ((csl - sc0 - 1) & 0x1f) < ends) {
            c->has_hit_cursor_line_start = 1;
        }
        c->scanline_counter = (uint8_t)sc;
        c->is_first_frame_scanline = 0;
        c->r4_hit = (c->vert_counter == c->reg[CRTC_R4_VTOTAL]);
        c->r5_hit = (c->vert_adjust_counter == c->reg[CRTC_R5_VADJ]);
        c->r9_hit = (uint8_t)(sc == r9m);
    }
    c->horiz_counter = (uint8_t)trail;
    c->address_counter = (uint16_t)((saved + trail) & 0x3fff);
    c->data_byte = (uint8_t)data;
    c->data_ma13 = 0;
#ifdef MACHINE_STATS
    crtc_stat_block += ticks;
    crtc_stat_block_calls++;
#endif
    return ticks;
}

// Very short lines at 2 MHz (RVI runs two-character lines for a quarter of
// each scanline): every tick is within a few of an R0 hit, so the fast
// path's runs are one tick long and each costs a plan. This steps up to n
// ticks exactly as crtc_tick does, with the counters in locals, for as
// long as nothing it leaves out can happen: cursor activity, a row at R7
// or an even-field vsync (the half-R0 check). (Single exact ticks of
// longer lines through it cost more than crtc_tick: evil-inf's quiet part
// 57 -> 60 on the board.) A plain line end (not the row's last line,
// no vsync, adjust or frame end pending) is done here, crtc_end_of_line
// and crtc_line_start_checks inlined and cut to what can happen; any other
// goes through crtc_end_of_line. Returns the ticks run, 0 if the first
// could not be; stops after a line end that changed the vsync level.
static CRTC_API __attribute__((noinline)) uint32_t crtc_short_lines(struct crtc *c, uint32_t tick, uint32_t n)
{
    uint32_t i = 0;
    int yield = 0, block_failed = 0;
    CRTC_RUN_BEGIN(c);
    for (;;) {
        if (yield) {
            // A line start the block may take (see the plain line end).
            yield = 0;
            CRTC_RUN_END(c);
            uint32_t done = crtc_short_block(c, tick + i, n - i);
            CRTC_RUN_RELOAD(c);
            if (done) {
                i += done;
                if (i >= n) {
                    break;
                }
            } else {
                block_failed = 1;   // not again in this call
            }
        }
        // State only a line end changes.
        if (c->cursor_skew_counter >= 0 || c->vert_counter == c->reg[CRTC_R7_VSYNC] ||
            c->is_even_vsync || c->reg[CRTC_R0_HTOTAL] == 0 ||
            (!c->cursor_disabled && c->has_hit_cursor_line_start && !c->has_hit_cursor_line_end)) {
            break;
        }
        uint32_t r0 = c->reg[CRTC_R0_HTOTAL], r1 = c->reg[CRTC_R1_HDISP];
        uint32_t r2 = (c->hsync_width > 0) ? c->reg[CRTC_R2_HSYNC] : 0x100;   // never hit
        uint32_t in_hsync = c->in_hsync, hsync_count = c->hsync_tick_counter;
        uint32_t hc = c->horiz_counter;
        uint32_t checks = c->start_of_line_state_checks;
        uint32_t r9_hit = c->r9_hit;
        uint32_t bits = c->display_enable_bits;
        // The DISPEN pipeline as three bits, newest in bit 0.
        uint32_t pipe = (c->dispen_shifts[0] ? 1 : 0) | (c->dispen_shifts[1] ? 2 : 0) | (c->dispen_shifts[2] ? 4 : 0);
        uint32_t skew = c->skew_dispen_index;
        uint32_t ma = c->address_counter;
        uint32_t sc = c->scanline_counter;
        uint32_t odd_frame = c->is_isv ? c->is_odd_frame : 0;
        uint32_t data = c->data_byte, ma13 = c->data_ma13;
        int plain_ok = !c->in_vsync;
        int eol = 0;
        while (i < n) {
            int r0_hit = (hc == r0);
            if (hc == r1) {
                if (r9_hit) {
                    c->address_counter_saved = (uint16_t)ma;
                }
                bits &= ~DISPEN_HORIZ;
            } else if (r0_hit) {
                bits &= ~DISPEN_HORIZ;
            }
            if (in_hsync) {
                if (--hsync_count == 0) {
                    in_hsync = 0;
                }
            } else if (hc == r2) {
                hsync_count = c->hsync_width;
                CRTC_RUN_HSYNC(c, hsync_count);
                in_hsync = 1;
            }
            pipe = ((pipe << 1) | (bits == DISPEN_ALL)) & 7;
            int dispen = (pipe >> skew) & 1;
            uint32_t t = tick + i;
            int odd = t & 1;
            data = CRTC_FETCH(c, (uint16_t)ma, (uint8_t)(sc | odd_frame), odd);
            ma13 = (ma >> 13) & 1;
            if (!odd) {
                if (ma & 0x2000) {
                    CRTC_TELETEXT(c, data, dispen);
                } else {
                    CRTC_TELETEXT(c, 0, 0);
                }
            }
#ifdef CRTC_FETCH_LOG
            CRTC_FETCH_LOG(c, t, hc, ma, data);
#endif
            ma = (ma + 1) & 0x3fff;
            CRTC_RUN_RENDER(c, data, dispen, odd, ma13);
            if (checks) {
                checks = crtc_chain_step(c, (uint8_t)checks);
            }
            i++;
            if (!r0_hit) {
                hc++;
                continue;
            }
            if (!plain_ok || r9_hit || c->in_dummy_raster || c->is_vert_adjust_pending ||
                c->is_end_of_frame_latched || c->is_in_vert_adjust) {
                eol = 1;
                break;
            }
            // A plain line end (crtc_end_of_line with no row end, vsync,
            // adjust or frame end; then crtc_line_start_checks(c, 1) with
            // the row known not to be at R7).
            hc = 0;
            bits |= DISPEN_HORIZ;
            checks |= 1;
            c->is_first_frame_scanline = 0;
            if (sc == c->reg[CRTC_R11_CEND]) {
                c->has_hit_cursor_line_end = 1;
            }
            sc = (sc + c->scanline_stride) & c->scanline_mask;
            ma = c->address_counter_saved;
            if (c->vert_counter == c->reg[CRTC_R6_VDISP] && (bits & DISPEN_VERT)) {
                bits &= ~DISPEN_VERT;
                c->frames++;
            }
            if (sc == c->cursor_start_line) {
                c->has_hit_cursor_line_start = 1;
            }
            CRTC_RUN_RA(c, sc | odd_frame, c->is_isv);
            c->r4_hit = (c->vert_counter == c->reg[CRTC_R4_VTOTAL]);
            c->r5_hit = (c->vert_adjust_counter == c->reg[CRTC_R5_VADJ]);
            r9_hit = (sc == (c->reg[CRTC_R9_MAXRA] & c->scanline_mask));
            c->r9_hit = (uint8_t)r9_hit;   // the chain reads it
            if (!c->cursor_disabled && c->has_hit_cursor_line_start && !c->has_hit_cursor_line_end) {
                break;   // cursor activity from here: the outer check stops
            }
            if (r0 == 1 && !r9_hit && !block_failed && n - i >= 2 && c->cursor_disabled &&
                !c->is_end_of_main_latched && !c->is_end_of_vert_adjust_latched) {
                yield = 1;   // whole plain lines next, perhaps: the block
                break;
            }
        }
        c->horiz_counter = (uint8_t)hc;
        c->start_of_line_state_checks = (uint8_t)checks;
        c->display_enable_bits = (uint8_t)bits;
        c->in_hsync = (uint8_t)in_hsync;
        c->hsync_tick_counter = (uint8_t)hsync_count;
        c->dispen_shifts[0] = pipe & 1;
        c->dispen_shifts[1] = (pipe >> 1) & 1;
        c->dispen_shifts[2] = (pipe >> 2) & 1;
        c->address_counter = (uint16_t)ma;
        c->scanline_counter = (uint8_t)sc;
        c->r9_hit = (uint8_t)r9_hit;
        c->data_byte = (uint8_t)data;
        c->data_ma13 = (uint8_t)ma13;
        if (!eol) {
            if (i < n) {
                continue;   // stopped for the cursor: the check above ends it
            }
            break;
        }
        CRTC_RUN_END(c);
        crtc_end_of_line(c, (uint8_t)r0, (uint8_t)checks);
        CRTC_RUN_RELOAD(c);
        if (c->vsync_changed || i >= n) {
            break;
        }
    }
    CRTC_RUN_END(c);
    return i;
}

// Runs up to n ticks starting at tick index `first`, stopping after a tick
// that changed the vsync level so the caller can record the IRQ change.
// Returns the number of ticks run (at least 1 when n > 0).
CRTC_INLINE uint32_t crtc_run(struct crtc *c, uint32_t first, uint32_t n)
{
    uint32_t i = 0;
    c->vsync_changed = 0;
#ifdef CRTC_RUN_LOG
    CRTC_RUN_LOG(c, first, n);
#endif
    while (i < n) {
#ifdef MACHINE_STATS
        uint64_t p0 = CRTC_CYCLES();
#endif
        if (c->reg[CRTC_R0_HTOTAL] < CRTC_SHORT_R0 && c->ula_clock_fast) {
            uint32_t done = 0;
            if (c->reg[CRTC_R0_HTOTAL] == 1) {
                done = crtc_short_block(c, first + i, n - i);
            }
            if (!done) {
                done = crtc_short_lines(c, first + i, n - i);
            }
            if (done) {
                i += done;
#ifdef MACHINE_STATS
                crtc_stat_short += done;
                crtc_stat_short_calls++;
                crtc_stat_clk_short += CRTC_CYCLES() - p0;
#endif
                if (c->vsync_changed) {
#ifdef CRTC_VSYNC_LOG
                    CRTC_VSYNC_LOG(c, first + i);
#endif
                    break;
                }
                continue;
            }
        }
        uint32_t fast = crtc_fast_ticks(c, first + i);
        if (fast > n - i) {
            fast = n - i;
        }
#ifdef CRTC_FETCH_LOG
        if (CRTC_FETCH_LOG_EXACT(first + i, fast)) {
            fast = 0;   // host debugging: every tick through crtc_tick
        }
#endif
#ifdef MACHINE_STATS
        uint64_t p1 = CRTC_CYCLES();
        c->stat_clk_plan += p1 - p0;
#endif
        if (fast) {
            crtc_fast(c, first + i, fast);
            i += fast;
#ifdef MACHINE_STATS
            c->stat_fast += fast;
            c->stat_runs++;
            c->stat_clk_fast += CRTC_CYCLES() - p1;
#endif
            continue;
        }
#ifdef MACHINE_STATS
        {
            // Why the exact path: 0 line-start chain, 1 hsync, 2 cursor skew,
            // 3 dispen pipeline, 4 counter hit (R0/R1/R2/half-R0/cursor), 5 other
            int why = 5;
            uint32_t hc = c->horiz_counter;
            uint8_t dispen = (c->display_enable_bits == DISPEN_ALL);
            if (c->start_of_line_state_checks) {
                why = 0;
            } else if (c->in_hsync) {
                why = 1;
            } else if (c->cursor_skew_counter >= 0) {
                why = 2;
            } else if (c->dispen_shifts[0] != dispen || c->dispen_shifts[1] != dispen || c->dispen_shifts[2] != dispen) {
                why = 3;
            } else if (hc == c->reg[CRTC_R0_HTOTAL] || hc == c->reg[CRTC_R1_HDISP] || hc == c->reg[CRTC_R2_HSYNC] ||
                       hc + 1 == c->half_r0) {
                why = 4;
            }
            c->stat_slow_why[why]++;
        }
#endif
        crtc_tick(c, first + i);
        i++;
#ifdef MACHINE_STATS
        c->stat_slow++;
        c->stat_clk_slow += CRTC_CYCLES() - p1;
#endif
        if (c->vsync_changed) {
#ifdef CRTC_VSYNC_LOG
            CRTC_VSYNC_LOG(c, first + i);
#endif
            break;
        }
    }
    return i;
}

CRTC_API void crtc_set_ula(struct crtc *c, uint8_t ula_control)
{
    c->ula_clock_fast = (ula_control >> 4) & 1;
    c->ula_teletext = (ula_control >> 1) & 1;
}

CRTC_API uint8_t crtc_read(const struct crtc *c, int addr)
{
    if (addr == 0) {
        return 0;
    }
    int reg = c->addr_reg & 0x1f;
    if (reg >= CRTC_R12_MAH && reg <= CRTC_R17_LPL) {
        return c->reg[reg];
    }
    return 0;
}

CRTC_API void crtc_write(struct crtc *c, int addr, uint8_t val)
{
    if (addr == 0) {
        c->addr_reg = val;
        return;
    }
    int reg = c->addr_reg & 0x1f;
    if (reg >= CRTC_NUM_REGS) {
        return;
    }
#ifdef CRTC_WRITE_LOG
    CRTC_WRITE_LOG(c, reg, val);
#endif
    uint8_t mask = 0xff;
    switch (reg) {
    case CRTC_R4_VTOTAL: case CRTC_R6_VDISP: case CRTC_R7_VSYNC:
    case CRTC_R10_CSTART:
        mask = 0x7f;
        break;
    case CRTC_R5_VADJ: case CRTC_R9_MAXRA: case CRTC_R11_CEND:
        mask = 0x1f;
        break;
    case CRTC_R12_MAH: case CRTC_R14_CAH:
        mask = 0x3f;
        break;
    case CRTC_R16_LPH: case CRTC_R17_LPL:
        return;  // read only
    default:
        break;
    }
    c->reg[reg] = val & mask;

    switch (reg) {
    case CRTC_R0_HTOTAL:
        c->half_r0 = (c->reg[CRTC_R0_HTOTAL] + 1) / 2;
        break;
    case CRTC_R3_SYNCW:
        c->hsync_width = val & 0x0f;
        c->vsync_width = (val >> 4) ? (val >> 4) : 16;
        break;
    case CRTC_R8_INTERLACE:
        c->is_interlace = val & 1;
        c->is_isv = ((val & 3) == 3);
        c->skew_dispen_index = (val >> 4) & 3;
        c->cursor_skew = (val >> 6) & 3;
        if (c->is_isv) {
            c->scanline_stride = 2;
            c->scanline_mask = 0x1e;
        } else {
            c->scanline_stride = 1;
            c->scanline_mask = 0x1f;
        }
        crtc_update_cursor_disabled(c);
        crtc_update_vsync(c);
        break;
    case CRTC_R10_CSTART:
        c->cursor_flashing = (val >> 6) & 1;
        c->cursor_flash_mask = (val & 0x20) ? 0x10 : 0x08;
        c->cursor_start_line = val & 0x1f;
        crtc_update_cursor_disabled(c);
        break;
    default:
        break;
    }

    if (c->is_isv) {
        c->scanline_counter &= ~1;  // odd R9 in ISV mode is not modelled
    }
    // beebjit re-evaluates its line-start latches (R4/R5/R9 hits, the R6
    // and R7 checks) at the start of every video advance, which follows any
    // register write within a few cycles (the next RAM write by the CPU
    // triggers one). A new R7 can start a vsync at once, mid scanline; a
    // new R6 can end the display at once. Do it here, at the write.
    crtc_line_start_checks(c, 1);
}

CRTC_API void crtc_reset(struct crtc *c)
{
    // beebjit's choice of power-on registers: a MODE 7 setup, so timing is
    // deterministic (real chips come up with random values).
    static const uint8_t init[CRTC_NUM_REGS] = {
        63, 40, 51, 0x24, 30, 2, 25, 28, 3 | (1 << 4) | (2 << 6), 18,
        0x72, 0x13, 0, 0, 0, 0, 0, 0,
    };
    uint8_t ula_clock_fast = c->ula_clock_fast;
    uint8_t ula_teletext = c->ula_teletext;
    uint16_t wrap = c->screen_wrap_add;
    *c = (struct crtc){0};
    c->ula_clock_fast = ula_clock_fast;
    c->ula_teletext = ula_teletext;
    c->screen_wrap_add = wrap;
    for (int i = 0; i < CRTC_NUM_REGS; i++) {
        c->reg[i] = init[i];
    }
    c->is_interlace = 1;
    c->is_isv = 1;
    c->scanline_stride = 2;
    c->scanline_mask = 0x1e;
    c->hsync_width = 4;
    c->vsync_width = 2;
    c->half_r0 = 32;
    c->cursor_flashing = 1;
    c->cursor_flash_mask = 0x10;
    c->cursor_start_line = 18;
    c->skew_dispen_index = 1;
    c->cursor_skew = 2;
    c->display_enable_bits = DISPEN_VERT;
    c->start_of_line_state_checks = 1;
    c->is_first_frame_scanline = 1;
    c->cursor_skew_counter = -1;
    crtc_line_start_checks(c, 1);
}
