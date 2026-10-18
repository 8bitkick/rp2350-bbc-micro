// esp32-beeb: pixel renderer driven by the video stream (vstream.h).
//
// A port of beebjit's render.c beam model (GPL lineage, see decisions.md):
// a canvas of 640 + 2 * border character cells wide and 512 + the same
// high, the beam advancing 8 pixels per 2 MHz tick, hsync returning it to
// the left and down two rasters, vsync to the top, each character row drawn
// on two rasters (deinterlaced). Bitmap modes expand bytes through tables
// built from the ULA palette; teletext goes through the SAA5050 port. With
// the same stream beebjit's video code would have produced, the canvas is
// pixel identical to beebjit's, which is what tools/framediff.py checks.
//
// Pixels go into a pair of raster line buffers (line and line2, the two
// rasters beebjit draws per scanline; they differ only in teletext) and are
// emitted at the next hsync through RENDER_EMIT(r, first, count, vert): on
// the host into the canvas, on the board into the panel strips.

#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

#include "pixel.h"
#include "teletext.h"
#include "vstream.h"

#define RENDER_BORDER_CHARS 4
#define RENDER_WIDTH (640 + RENDER_BORDER_CHARS * 2 * 16)
#define RENDER_HEIGHT (512 + RENDER_BORDER_CHARS * 2 * 16)

enum {
    RENDER_MODE0 = 0, RENDER_MODE1, RENDER_MODE2, RENDER_MODE4, RENDER_MODE5,
    RENDER_MODE7, RENDER_MODE8, RENDER_MODE4_80, RENDER_MODE2_10,
};

#define RENDER_LINE_END 0x7fffffff   // render_pos value meaning "not rendering"

struct render {
    render_px *buffer;                // RENDER_WIDTH * RENDER_HEIGHT, or NULL
    render_px line[RENDER_WIDTH] __attribute__((aligned(4)));   // the raster being rendered
    render_px line2[RENDER_WIDTH] __attribute__((aligned(4)));  // its second raster (teletext differs)
    uint32_t span_start, span_end;    // rendered part of line[]
    struct teletext teletext;

    uint8_t logical_to_physical[16];
    render_px palette[16];
    // One expansion table per clock, rebuilt when the mode or palette
    // changes (as a palette write would anyway). tables_built is bit 0 for
    // the 2 MHz table, bit 1 for 1 MHz; table_mode which mode each holds.
    uint32_t tables_built;
    int table_mode[2];
    render_px table_2mhz[256][8] __attribute__((aligned(4)));
    render_px table_1mhz[256][16] __attribute__((aligned(4)));
#ifdef RENDER_INDEXED
    // Indexed, a table depends on the mode alone: a second one of each kind
    // is kept, so a split-mode screen (Elite's MODE 4 top, MODE 5
    // dashboard) builds none per frame. slot_mode: each slot's mode, -1
    // none; [kind][slot], kind 0 the 2 MHz tables, 1 the 1 MHz ones.
    render_px table_2mhz_b[256][8] __attribute__((aligned(4)));
    render_px table_1mhz_b[256][16] __attribute__((aligned(4)));
    int slot_mode[2][2];
#endif
    // Which logical colours each byte's pixels use in the built table, so
    // a palette write rebuilds only the entries it changes (host; the
    // board's tables hold logical colour codes, see pixel.h).
    uint16_t byte_logicals[2][256];
    uint16_t palette_dirty;           // logical colours written since the tables were fixed up
    render_px black[8] __attribute__((aligned(4)));
    const render_px (*cur_2mhz)[8];
    const render_px (*cur_1mhz)[16];

    int mode;
    int is_dispen;
    uint32_t row_address;
    int is_clock_2mhz;
    int chars_per_line;
    int is_teletext;
    int is_flash;
    int needs_mode_recalc;
    int selected_blank;               // render the blank function this tick
    int32_t horiz_beam_pos, vert_beam_pos;
    int32_t horiz_window_start, horiz_window_end;
    int32_t vert_window_start, vert_window_end;
    int32_t render_pos;               // index into line[], or RENDER_LINE_END
    int line2_wanted;                 // the emitter will read line2 for this raster
    int32_t cursor_segment_index;
    int cursor_segments[4];
    int is_crt_grayscale_fakeout;
    uint32_t frames;                  // vsyncs seen (flyback count)
    // beebjit compatibility for frame diffs: clear the canvas at the
    // flyback after a framing change, as beebjit does after its paint (1),
    // or at every flyback (2, beebjit's video:always-clear-frame-buffer).
    int compat_clear;
    int framing_changed;
    uint32_t stat_fast_words, stat_slow_words;   // render_consume_n paths taken
    uint32_t stat_tables, stat_ulactrl, stat_ulapal, stat_events;   // rebuilds and events seen
    uint32_t stat_clk_tables, stat_clk_slow, stat_clk_runs;         // RENDER_CYCLES() in each
    uint32_t stat_ev_n[16], stat_ev_clk[16];
    uint32_t stat_clk_ttrun;   // cycles inside teletext_run   // slow words by event type (low nibble; 0 = a tick)
};

void render_init(struct render *r, render_px *buffer);
void render_reset(struct render *r);   // beebjit's power-on state
// Consumes everything available in the stream.
void render_consume(struct render *r, struct vstream *vs);
// Consumes up to n words, runs of plain bitmap ticks on a fast path.
void render_consume_n(struct render *r, struct vstream *vs, int n);
extern volatile int render_bench_skip;   // measurement only, see render.c
// Processes one stream word.
void render_word(struct render *r, uint16_t w);
// Emits the part of the current raster rendered so far (for mid-line dumps).
void render_flush(struct render *r);

#endif
