#ifndef BEEBJIT_RENDER_H
#define BEEBJIT_RENDER_H

#include <stdint.h>

struct render_struct;

struct bbc_options;
struct teletext_struct;

/* BEEBJIT_RENDER_PACKED4: for hosts short of memory and CPU, host pixels are
 * 4-bit codes (bit 0 blue, bit 1 green, bit 2 red: the top bit of each
 * channel), 8 to a 32-bit word with the leftmost pixel in the low nibble,
 * instead of one ARGB word each. Rows, tables and render calls all shrink 8x.
 * Not supported: video:double-size, half-character horizontal offsets.
 */
#ifndef BEEBJIT_RENDER_PACKED4
#define BEEBJIT_RENDER_PACKED4 0
#endif

enum {
  /* 32-bit words holding 8 host pixels. */
  k_render_words_per_8_pixels = (BEEBJIT_RENDER_PACKED4 ? 1 : 8),
};

struct render_character_2MHz {
  uint32_t host_pixels[k_render_words_per_8_pixels];
};

struct render_character_1MHz {
  struct render_character_2MHz first;
  struct render_character_2MHz second;
};

struct render_table_2MHz {
  struct render_character_2MHz values[256];
};

struct render_table_1MHz {
  struct render_character_1MHz values[256];
};

struct render_struct* render_create(struct teletext_struct* p_teletext,
                                    struct bbc_options* p_options);
void render_destroy(struct render_struct* p_render);

void render_power_on_reset(struct render_struct* p_render);

void render_set_flyback_callback(struct render_struct* p_render,
                                 void (*p_flyback_callback)(void* p),
                                 void* p_callback_object);

uint32_t render_get_width(struct render_struct* p_render);
uint32_t render_get_height(struct render_struct* p_render);
/* 32-bit words per row of host pixels. */
uint32_t render_get_stride(struct render_struct* p_render);
/* A host pixel value for an ARGB color. */
uint32_t render_pixel_from_argb(uint32_t argb);
uint32_t render_get_buffer_size(struct render_struct* p_render);
uint32_t render_get_horiz_pos(struct render_struct* p_render);
uint32_t render_get_vert_pos(struct render_struct* p_render);

uint32_t render_get_buffer_crc32(struct render_struct* p_render);

uint32_t* render_get_buffer(struct render_struct* p_render);
void render_set_buffer(struct render_struct* p_render, uint32_t* p_buffer);
int render_has_buffer(struct render_struct* p_render);
void render_create_internal_buffer(struct render_struct* p_render);
/* Row mode, for hosts without memory for a whole frame: p_buffer holds 2 rows
 * (render_get_width() pixels each, one interlace pair: render_get_stride()
 * words a row). As the beam leaves a
 * pair, p_row_callback gets the row that was drawn (the first, or with
 * video:no-deinterlace-* the one for the current field); "row" is the pair's
 * vertical position in the window (0, 2, 4, ...). The row is cleared to black
 * after.
 */
void render_set_row_buffer(struct render_struct* p_render,
                           uint32_t* p_buffer,
                           void (*p_row_callback)(void* p,
                                                  uint32_t row,
                                                  uint32_t* p_pixels),
                           void* p_row_callback_object);
/* A second row pair of the same size, after render_set_row_buffer(): the
 * renderer then alternates between the two at each callback, so the host can
 * go on reading the row it was given (e.g. by DMA) until the next callback.
 */
void render_set_row_buffer_other(struct render_struct* p_render,
                                 uint32_t* p_buffer_other);

void render_set_mode(struct render_struct* p_render,
                     int clock_speed,
                     int chars_per_line,
                     int is_teletext);
void render_set_flash(struct render_struct* p_render, int is_flash);

void render_set_physical_color(struct render_struct* p_render,
                               uint8_t logical_color,
                               uint8_t physical_color);
void render_set_palette(struct render_struct* p_render,
                        uint8_t index,
                        uint32_t rgba);
void render_set_cursor_segments(struct render_struct* p_render,
                                int s0,
                                int s1,
                                int s2,
                                int s3);
void render_set_DISPEN(struct render_struct* p_render, int is_enabled);
void render_set_RA(struct render_struct* p_render, uint32_t row_address);

/* Call render_prepare() before a sequence of render_render() to ensure that
 * all pending pixel table rebuilds are taken care of.
 */
void render_prepare(struct render_struct* p_render);
void render_render(struct render_struct* p_render,
                   uint8_t data,
                   int is_odd_tick);
/* For video's fast path through ordinary stretches of bitmap scanline: how
 * many render_render() calls in a row, from now, render_render_bitmap_run()
 * can stand in for (0 when per-call handling is needed: teletext, blanking,
 * cursor, beam outside the window).
 */
/* A run of calls render_render() calls of deinterlaced teletext drawn by the
 * caller (the first row of each pair, k_render_words_per_8_pixels a call,
 * from the pointer returned); NULL if this run can't be (then render_render()
 * as usual). render_teletext_run_end() moves on past it.
 */
uint32_t* render_teletext_run_start(struct render_struct* p_render,
                                    uint32_t calls);
/* Whether calls render_render() calls would all fall outside the window; if
 * so, the beam has moved past them (nothing to draw).
 */
int render_teletext_run_outside(struct render_struct* p_render,
                                uint32_t calls);
/* How many render_render() calls render_teletext_run_start() would take from
 * here (0 outside the row).
 */
uint32_t render_get_teletext_run_capacity(struct render_struct* p_render);
/* Teletext characters outside the window up to its start: the render_render()
 * calls to get there (0 if it isn't ahead), then those calls in one go.
 */
uint32_t render_teletext_calls_to_window(struct render_struct* p_render);
void render_teletext_run_to_window(struct render_struct* p_render,
                                   uint32_t calls);
void render_teletext_run_end(struct render_struct* p_render, uint32_t calls);
uint32_t render_get_bitmap_run_capacity(struct render_struct* p_render);
/* As render_render() for each of count data bytes: one call per byte at 2MHz,
 * two (first and second half) at 1MHz.
 */
void render_render_bitmap_run(struct render_struct* p_render,
                              const uint8_t* p_data,
                              uint32_t count);
/* The same for blanked (display disabled) render_render() calls. */
uint32_t render_get_blank_run_capacity(struct render_struct* p_render);
void render_render_blank_run(struct render_struct* p_render, uint32_t count);

void render_clear_buffer(struct render_struct* p_render);
void render_process_full_buffer(struct render_struct* p_render);
void render_hsync(struct render_struct* p_render, uint32_t hsync_pulse_ticks);
void render_vsync(struct render_struct* p_render);
void render_horiz_line(struct render_struct* p_render, uint32_t argb);
void render_cursor(struct render_struct* p_render);
void render_set_beam_pos(struct render_struct* p_render,
                         int32_t horiz,
                         int32_t vert);

#if BEEBJIT_RENDER_WIRE
/* For the render wire's consumer (port specific). */
struct teletext_struct* render_get_teletext(struct render_struct* p_render);
#endif

#endif /* BEEBJIT_RENDER_H */
