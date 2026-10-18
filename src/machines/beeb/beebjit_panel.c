/*
 * beebjit rows -> Presto panel framebuffer rows. See beebjit_panel.h.
 *
 * 3 of every 4 pixels are kept (exact for MODEs 2 and 5, whose pixels are 4
 * render pixels wide). With BEEBJIT_RENDER_PACKED4, beebjit already renders
 * the panel's 4-bit codes and this only drops pixels; otherwise it also takes
 * the top bit of each of R, G and B.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "beebjit_panel.h"

#include "render.h"

#include <string.h>

#ifndef BEEBJIT_PANEL_FUNC
#define BEEBJIT_PANEL_FUNC(x) x
#endif

void
beebjit_panel_init(struct beebjit_panel* p_panel,
                   uint32_t (*p_rows)[k_beebjit_panel_row_words],
                   uint32_t num_rows) {
  uint32_t i;
  p_panel->p_rows = p_rows;
  p_panel->num_rows = num_rows;
  p_panel->rows_written = 0;
  for (i = 0; i < num_rows; ++i) {
    (void) memset(p_rows[i], '\0', sizeof(p_rows[i]));
    p_rows[i][k_beebjit_panel_row_words - 1] = k_beebjit_panel_row_end;
  }
}


#if BEEBJIT_RENDER_PACKED4

/* Rows arrive as packed 4-bit codes already (render.h), 80 words for 640
 * pixels. Keeping nibbles 0, 1, 2 and 4, 5, 6 of each word gives 24 bits, so
 * 4 words in make 3 words out.
 */
void
BEEBJIT_PANEL_FUNC(beebjit_panel_row)(void* p,
                                      uint32_t row,
                                      uint32_t* p_pixels) {
  uint32_t i;
  uint32_t* p_out;
  struct beebjit_panel* p_panel = (struct beebjit_panel*) p;

  row >>= 1;
  if (row >= p_panel->num_rows) {
    return;
  }
  p_out = p_panel->p_rows[row];
  for (i = 0; i < (k_beebjit_panel_src_width / 32); ++i) {
    uint32_t k0 = p_pixels[0];
    uint32_t k1 = p_pixels[1];
    uint32_t k2 = p_pixels[2];
    uint32_t k3 = p_pixels[3];
    k0 = ((k0 & 0xFFF) | ((k0 >> 4) & 0xFFF000));
    k1 = ((k1 & 0xFFF) | ((k1 >> 4) & 0xFFF000));
    k2 = ((k2 & 0xFFF) | ((k2 >> 4) & 0xFFF000));
    k3 = ((k3 & 0xFFF) | ((k3 >> 4) & 0xFFF000));
    p_out[0] = (k0 | (k1 << 24));
    p_out[1] = ((k1 >> 8) | (k2 << 16));
    p_out[2] = ((k2 >> 16) | (k3 << 8));
    p_pixels += 4;
    p_out += 3;
  }
  p_panel->rows_written++;
}

#else /* !BEEBJIT_RENDER_PACKED4 */

static inline uint32_t
beebjit_panel_code(uint32_t argb) {
  /* ARGB: blue bit 7, green bit 15, red bit 23 -> codes 1, 2, 4. */
  return ((argb >> 7) & 1) | ((argb >> 14) & 2) | ((argb >> 21) & 4);
}

void
BEEBJIT_PANEL_FUNC(beebjit_panel_row)(void* p,
                                      uint32_t row,
                                      uint32_t* p_pixels) {
  uint32_t i;
  uint32_t* p_out;
  struct beebjit_panel* p_panel = (struct beebjit_panel*) p;

  /* Rows come in interlace pairs: 0, 2, 4... */
  row >>= 1;
  if (row >= p_panel->num_rows) {
    return;
  }
  p_out = p_panel->p_rows[row];
  /* 640 -> 480: of each 4 source pixels keep 0, 1, 2; that's 6 codes from 8
   * pixels, so 32 source pixels make 3 output words.
   */
  for (i = 0; i < (k_beebjit_panel_src_width / 32); ++i) {
    const uint32_t* p_in = &p_pixels[i * 32];
    uint32_t w0;
    uint32_t w1;
    uint32_t w2;
    w0 = beebjit_panel_code(p_in[0]) |
         (beebjit_panel_code(p_in[1]) << 4) |
         (beebjit_panel_code(p_in[2]) << 8) |
         (beebjit_panel_code(p_in[4]) << 12) |
         (beebjit_panel_code(p_in[5]) << 16) |
         (beebjit_panel_code(p_in[6]) << 20) |
         (beebjit_panel_code(p_in[8]) << 24) |
         (beebjit_panel_code(p_in[9]) << 28);
    w1 = beebjit_panel_code(p_in[10]) |
         (beebjit_panel_code(p_in[12]) << 4) |
         (beebjit_panel_code(p_in[13]) << 8) |
         (beebjit_panel_code(p_in[14]) << 12) |
         (beebjit_panel_code(p_in[16]) << 16) |
         (beebjit_panel_code(p_in[17]) << 20) |
         (beebjit_panel_code(p_in[18]) << 24) |
         (beebjit_panel_code(p_in[20]) << 28);
    w2 = beebjit_panel_code(p_in[21]) |
         (beebjit_panel_code(p_in[22]) << 4) |
         (beebjit_panel_code(p_in[24]) << 8) |
         (beebjit_panel_code(p_in[25]) << 12) |
         (beebjit_panel_code(p_in[26]) << 16) |
         (beebjit_panel_code(p_in[28]) << 20) |
         (beebjit_panel_code(p_in[29]) << 24) |
         (beebjit_panel_code(p_in[30]) << 28);
    p_out[(i * 3) + 0] = w0;
    p_out[(i * 3) + 1] = w1;
    p_out[(i * 3) + 2] = w2;
  }
  p_panel->rows_written++;
}

#endif /* BEEBJIT_RENDER_PACKED4 */
