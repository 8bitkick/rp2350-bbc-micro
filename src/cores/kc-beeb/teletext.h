// esp32-beeb: SAA5050 teletext character generator.
//
// A port of beebjit's teletext.c (GPL, see docs/decisions.md on licensing)
// with the same pipeline, control code handling, hold graphics, double
// height, flash and glyph generation (b-em's glyphs, doubled, smoothed and
// stretched 12 to 16 pixels with anti-aliasing), so that frame diffs against
// beebjit are pixel exact. Output is 8 ARGB pixels per call.

#ifndef TELETEXT_H
#define TELETEXT_H

#include <stdint.h>

#include "pixel.h"

struct teletext {
    // The two-character data pipeline as bytes 0..2 of a word, and the
    // DISPEN pipeline as bits: 0 and 1 the two stages, 2 the current one.
    uint32_t data_pipe;
    uint32_t dispen_pipe;
    // Consecutive teletext_data calls with no data and no DISPEN (bitmap
    // modes): after two the pipeline is at its fixed point and callers skip
    // the rest, which leaves the state exactly as the calls would.
    uint32_t idle_calls;
    render_px palette[8];
    uint32_t flash_count;
    int flash_visible_this_frame;
    uint32_t scanline;
    const uint8_t *active_characters;
    int is_graphics_active;
    int is_separated_active;
    int is_concealed;
    int double_active;
    int flash_active;
    int had_double_active_this_scanline;
    int second_character_row_of_double;
    render_px fg_color;
    render_px bg_color;
    int is_hold_graphics;
    const uint8_t *held_character;
    int crtc_ra0;
    int is_isv;
    // Glyph row byte offsets for this scanline, normal and double height,
    // first and second raster; recomputed when the scanline state changes.
    uint32_t row_off[2][2];
    const uint8_t *render_character;
    render_px render_fg_color;
    render_px black;
    // Worked out on the even tick of a character, reused on the odd one.
    const uint8_t *row_glyph;      // row-adjusted glyph for the first raster
    const uint8_t *row_glyph2;     // and for the second
    render_px row_fg, row_bg;
    int row_blank;
    uint32_t stat_run_words, stat_run_controls, stat_run_calls, stat_run_zero;   // teletext_run profile
};

// The per-tick entry points are compiled into the renderer's translation
// unit (render.c and teletext.c are always included together) so the run
// loop can inline them.
#define TT_API static inline RENDER_FN

// Generates the glyph tables once (92 KB on the host, 11 KB on the board).
void teletext_generate(void);
void teletext_init(struct teletext *t);
// One character cell's byte and DISPEN into the pipeline. decode = 0 keeps
// only the pipeline and scanline tracking (bitmap modes: what the SAA5050
// makes of the bytes is not seen); beebjit always decodes, which differs
// only in the scanline a mode change to teletext lands on.
TT_API void teletext_data(struct teletext *t, uint8_t data, int is_dispen, int decode);
TT_API void teletext_ra_isv_changed(struct teletext *t, uint8_t ra, int is_isv);
TT_API void teletext_vsync_changed(struct teletext *t, int value);
// Renders 8 pixels to out, and to next_out (the following raster) if given.
TT_API void teletext_render(struct teletext *t, int is_odd_tick, render_px *out, render_px *next_out);
TT_API void teletext_render_pair(struct teletext *t, render_px *out, render_px *next_out, int want_next);
// A run of character words (VS_PAIR, DISPEN and MA13 set, no events) from
// the stream ring starting at index `tail`, at most `max` of them: each is
// teletext_data then teletext_render_pair, with the state in locals. Stops
// at the first word that is not such a pair; returns how many it took (0
// if the DISPEN pipeline is not settled yet, for the caller's own path).
TT_API uint32_t teletext_run(struct teletext *t, const uint16_t *ring, uint32_t mask, uint32_t tail,
                             uint32_t max, render_px *out, render_px *next_out, int want_next);

#endif
