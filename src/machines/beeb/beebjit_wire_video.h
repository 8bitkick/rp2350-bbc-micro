/*
 * Included by video.c (only) when built with BEEBJIT_RENDER_WIRE: its render
 * and teletext calls become commands on the render wire. See beebjit_wire.h.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_WIRE_VIDEO_H
#define BEEBJIT_WIRE_VIDEO_H

#include "beebjit_wire.h"

#include <string.h>

/* The producer's view of the teletext pipeline: how many teletext_data()
 * calls in a row have been fed (0, 0). After 4, the pipeline is idle
 * whatever it held (see teletext_data()); before that, video.c is told it
 * isn't, which is safe (it then feeds the zeros itself).
 */
extern uint32_t g_beebjit_wire_teletext_zeros;

static inline void
beebjit_wire_teletext_note(uint8_t data, int is_dispen) {
  if ((data == 0) && !is_dispen) {
    if (g_beebjit_wire_teletext_zeros < 4) {
      g_beebjit_wire_teletext_zeros++;
    }
  } else {
    g_beebjit_wire_teletext_zeros = 0;
  }
}

/* The note for a run of n bytes with DISPEN as k_wire_char_run's bits and
 * off from off_from: only the trailing (0, 0) bytes count.
 */
static inline void
beebjit_wire_teletext_note_run(const uint8_t* p_data,
                               uint32_t n,
                               uint32_t dispen,
                               uint32_t dispen_settle,
                               uint32_t dispen_pattern,
                               uint32_t dispen_off_from) {
  uint32_t zeros = 0;
  uint32_t i = n;
  while ((i > 0) && (zeros < 4)) {
    uint32_t d;
    i--;
    d = BEEBJIT_WIRE_CHAR_RUN_DISPEN(dispen, dispen_settle, dispen_pattern, i);
    if (i >= dispen_off_from) {
      d = 0;
    }
    if ((p_data[i] != 0) || d) {
      g_beebjit_wire_teletext_zeros = zeros;
      return;
    }
    zeros++;
  }
  if (i == 0) {
    zeros += g_beebjit_wire_teletext_zeros;
  }
  g_beebjit_wire_teletext_zeros = ((zeros > 4) ? 4 : zeros);
}

#define render_power_on_reset(p) \
    ((void) (p), beebjit_wire_put1(k_wire_power_on_reset, 0))
#define render_set_flyback_callback(p, f, o) \
    do { \
      (void) (p); \
      g_beebjit_wire.p_flyback_callback = (f); \
      g_beebjit_wire.p_flyback_object = (o); \
    } while (0)
#define render_set_mode(p, clock, chars, teletext) \
    ((void) (p), \
     beebjit_wire_put1(k_wire_mode, \
                       ((uint32_t) (clock) | ((uint32_t) (chars) << 4) | \
                        ((uint32_t) !!(teletext) << 8))))
#define render_set_flash(p, v) \
    ((void) (p), beebjit_wire_put1(k_wire_flash, !!(v)))
#define render_set_physical_color(p, l, c) \
    ((void) (p), \
     beebjit_wire_put1(k_wire_physical_color, \
                       ((uint32_t) (uint8_t) (l) | \
                        ((uint32_t) (uint8_t) (c) << 8))))
#define render_set_palette(p, i, rgba) \
    ((void) (p), beebjit_wire_put2(k_wire_palette, (uint8_t) (i), (rgba)))
#define render_set_cursor_segments(p, s0, s1, s2, s3) \
    ((void) (p), \
     beebjit_wire_put1(k_wire_cursor_segments, \
                       ((uint32_t) !!(s0) | ((uint32_t) !!(s1) << 1) | \
                        ((uint32_t) !!(s2) << 2) | \
                        ((uint32_t) !!(s3) << 3))))
#define render_set_DISPEN(p, v) \
    ((void) (p), beebjit_wire_put1(k_wire_dispen, !!(v)))
#define render_set_RA(p, v) \
    ((void) (p), beebjit_wire_put1(k_wire_ra, (uint32_t) (v)))
#define render_prepare(p) \
    ((void) (p), beebjit_wire_put1(k_wire_prepare, 0))
#define render_render(p, d, odd) \
    ((void) (p), \
     beebjit_wire_put1(k_wire_render, \
                       ((uint32_t) (uint8_t) (d) | \
                        ((uint32_t) !!(odd) << 8))))
#define render_get_bitmap_run_capacity(p) ((void) (p), (uint32_t) 0x10000)
#define render_get_blank_run_capacity(p) ((void) (p), (uint32_t) 0x10000)
#define render_render_bitmap_run(p, p_data, n) \
    do { \
      uint32_t wire_n = (n); \
      uint8_t* p_wire_bytes; \
      (void) (p); \
      p_wire_bytes = \
          beebjit_wire_reserve_bytes(k_wire_bitmap_run, wire_n, wire_n); \
      (void) memcpy(p_wire_bytes, (p_data), wire_n); \
      beebjit_wire_commit_bytes(wire_n); \
    } while (0)
#define render_render_blank_run(p, n) \
    ((void) (p), beebjit_wire_put1(k_wire_blank_run, (uint32_t) (n)))
#define render_hsync(p, ticks) \
    ((void) (p), beebjit_wire_put1(k_wire_hsync, (uint32_t) (ticks)))
#define render_vsync(p) ((void) (p), beebjit_wire_put1(k_wire_vsync, 0))
#define render_horiz_line(p, argb) \
    ((void) (p), beebjit_wire_put2(k_wire_horiz_line, 0, (argb)))
#define render_cursor(p) ((void) (p), beebjit_wire_put1(k_wire_cursor, 0))
#define render_set_beam_pos(p, h, v) \
    do { \
      uint32_t* p_wire_words; \
      (void) (p); \
      p_wire_words = beebjit_wire_reserve(3); \
      p_wire_words[0] = k_wire_beam_pos; \
      p_wire_words[1] = (uint32_t) (h); \
      p_wire_words[2] = (uint32_t) (v); \
      beebjit_wire_commit(3); \
    } while (0)

#define teletext_is_idle(p) ((void) (p), (g_beebjit_wire_teletext_zeros >= 4))
#define teletext_data(p, d, dispen) \
    do { \
      uint8_t wire_d = (d); \
      int wire_dispen = !!(dispen); \
      (void) (p); \
      beebjit_wire_teletext_note(wire_d, wire_dispen); \
      beebjit_wire_put1(k_wire_teletext_data, \
                        (wire_d | ((uint32_t) wire_dispen << 8))); \
    } while (0)
#define teletext_RA_ISV_changed(p, ra, isv) \
    ((void) (p), \
     beebjit_wire_put1(k_wire_teletext_ra_isv, \
                       ((uint32_t) (uint8_t) (ra) | \
                        ((uint32_t) !!(isv) << 8))))
#define teletext_VSYNC_changed(p, v) \
    ((void) (p), beebjit_wire_put1(k_wire_teletext_vsync, !!(v)))

#endif /* BEEBJIT_WIRE_VIDEO_H */
