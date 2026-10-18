/*
 * Turns beebjit's rendered rows (ARGB, 640 visible pixels) into the Presto
 * panel's framebuffer format: 480 4-bit colour codes per row (bit 0 blue,
 * 1 green, 2 red), 8 to a word, then an end-of-row word (see board.h: BOARD_DISPLAY_PACKED4_ROWS).
 * Plain C with no hardware dependencies, so the test harness can run it too.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_PANEL_H
#define BEEBJIT_PANEL_H

#include <stdint.h>

enum {
  k_beebjit_panel_width = 480,
  k_beebjit_panel_row_words = (480 / 8 + 1),
  k_beebjit_panel_row_end = 0x8,
  /* One framebuffer row per BBC scanline: render's window is 256 rows. */
  k_beebjit_panel_rows = 256,
  /* beebjit's render width with video:border-chars=0. */
  k_beebjit_panel_src_width = 640,
};

struct beebjit_panel {
  uint32_t (*p_rows)[k_beebjit_panel_row_words];
  uint32_t num_rows;
  uint32_t rows_written;
};

void beebjit_panel_init(struct beebjit_panel* p_panel,
                        uint32_t (*p_rows)[k_beebjit_panel_row_words],
                        uint32_t num_rows);

/* A render row callback (see render_set_row_buffer()); p is the panel. */
void beebjit_panel_row(void* p, uint32_t row, uint32_t* p_pixels);

#endif
