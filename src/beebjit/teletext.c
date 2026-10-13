#include "teletext.h"

/* Needed before teletext_glyphs.h */
#include <stdint.h>

/* Teletext raw glyph data.
 * With thanks,
 * from B-em v2.2. (Called bbctext.h there.)
 * GPL licensed.
 * (c) Sarah Walker.
 */
#include "teletext_glyphs.h"

#include "render.h"
#include "util.h"
#include "video.h"

#include <assert.h>
#include <string.h>

/* Used for stretching 12 pixels wide into 16. */
static const uint8_t k_stretch_data[] = {
  0,  255, 0,  0,
  0,  85,  1,  170,
  1,  170, 2,  85,
  2,  255, 0,  0,

  3,  255, 0,  0,
  3,  85,  4,  170,
  4,  170, 5,  85,
  5,  255, 0,  0,

  6,  255, 0,  0,
  6,  85,  7,  170,
  7,  170, 8,  85,
  8,  255, 0,  0,

  9,  255, 0,  0,
  9,  85,  10, 170,
  10, 170, 11, 85,
  11, 255, 0,  0,
};

static int s_teletext_was_generated;

/* Generated glyphs: 96 characters of 20 rows of 16 pixels. Normally a byte of
 * intensity per pixel, blended between foreground and background; with
 * BEEBJIT_RENDER_PACKED4, a bit per pixel (intensity >= 128), so a row is 2
 * bytes (8 pixels each, leftmost in bit 0) and the tables are 8x smaller.
 */
enum {
  k_teletext_row_bytes = (BEEBJIT_RENDER_PACKED4 ? 2 : 16),
  k_teletext_half_row_bytes = (k_teletext_row_bytes / 2),
  k_teletext_glyph_bytes = (k_teletext_row_bytes * 20),
};

static uint8_t s_teletext_generated_glyphs[96 * k_teletext_glyph_bytes];
static uint8_t s_teletext_generated_gfx[96 * k_teletext_glyph_bytes];
static uint8_t s_teletext_generated_sep_gfx[96 * k_teletext_glyph_bytes];
#if BEEBJIT_RENDER_PACKED4
/* 8 pixel bits -> a word with 0xF in each set pixel's nibble. */
static uint32_t s_teletext_expand[256];
#endif


struct teletext_struct {
  struct render_character_2MHz render_character_2MHz_black;
  uint32_t background_color;
  uint8_t data_pipeline[3];
  uint8_t dispen_pipeline[2];
  uint32_t palette[8];
  uint32_t flash_count;
  int flash_visible_this_frame;
  uint32_t scanline;
  uint8_t* p_active_characters;
  int is_graphics_active;
  int is_separated_active;
  int is_concealed;
  int double_active;
  int flash_active;
  int had_double_active_this_scanline;
  int second_character_row_of_double;
  uint32_t fg_color;
  uint32_t bg_color;
  int is_hold_graphics;
  uint8_t* p_held_character;
  int crtc_ra0;
  int is_isv;
  int curr_dispen;
  uint8_t* p_render_character;
  uint32_t render_fg_color;
  uint8_t last_data_byte;
  /* For the run cache: scanline advances so far, and the double-height
   * flag at the last one.
   */
  uint32_t advance_count;
  int had_double_at_advance;
};

static void
teletext_draw_block(uint8_t* p_glyph,
                    uint32_t x,
                    uint32_t y,
                    uint32_t w,
                    uint32_t h) {
  uint32_t i_width;
  uint32_t i_height;

  for (i_height = 0; i_height < h; ++i_height) {
    for (i_width = 0; i_width < w; ++i_width) {
      p_glyph[((y + i_height) * 6) + x + i_width] = 1;
    }
  }
}

static void
teletext_double_up_pixels(uint8_t* p_dest, uint8_t* p_src) {
  uint32_t x;
  uint32_t y;
  for (y = 0; y < 10; ++y) {
    for (x = 0; x < 6; ++x) {
      uint8_t* p_pixel_block = (p_dest + (y * 12 * 2) + (x * 2));
      uint8_t val = p_src[(y * 6) + x];
      *p_pixel_block = val;
      *(p_pixel_block + 1) = val;
      *(p_pixel_block + 12) = val;
      *(p_pixel_block + 12 + 1) = val;
    }
  }
}

static void
teletext_stretch_12_to_16(uint8_t* p_dest, uint8_t* p_src) {
  uint32_t x;
  uint32_t y;
  for (y = 0; y < 20; ++y) {
    const uint8_t* p_stretch_data = &k_stretch_data[0];
    for (x = 0; x < 16; ++x) {
      uint8_t val = (p_src[p_stretch_data[0]] * p_stretch_data[1]);
      val += (p_src[p_stretch_data[2]] * p_stretch_data[3]);
      *p_dest++ = val;
      p_stretch_data += 4;
    }
    p_src += 12;
  }
}

static void
teletext_smooth_diagonals(uint8_t* p_dest, uint8_t* p_src) {
  uint32_t x;
  uint32_t y;

  (void) memcpy(p_dest, p_src, (12 * 20));

  /* NOTE: could visit less rows / columns here but keeping it simple. */
  for (y = 1; y < 19; ++y) {
    for (x = 1; x < 11; ++x) {
      uint8_t* p_pixel = (p_src + (y * 12) + x);
      int up = *(p_pixel - 12);
      int down = *(p_pixel + 12);
      int left = *(p_pixel - 1);
      int right = *(p_pixel + 1);
      int down_left = *(p_pixel + 11);
      int up_right = *(p_pixel - 11);
      int down_right = *(p_pixel + 13);
      int up_left = *(p_pixel - 13);
      int smooth = 0;
      if (down && left && !down_left) {
        smooth = 1;
      }
      if (up && right && !up_right) {
        smooth = 1;
      }
      if (down && right && !down_right) {
        smooth = 1;
      }
      if (up && left && !up_left) {
        smooth = 1;
      }
      if (smooth) {
        p_dest[(y * 12) + x] = 1;
      }
    }
  }
}

/* Stretch a 12x20 glyph to 16x20 and store it in the table format. */
static void
teletext_store_glyph(uint8_t* p_dest, uint8_t* p_src) {
#if BEEBJIT_RENDER_PACKED4
  uint32_t y;
  uint32_t x;
  uint8_t stretched[16 * 20];
  teletext_stretch_12_to_16(&stretched[0], p_src);
  for (y = 0; y < 20; ++y) {
    uint32_t bits = 0;
    for (x = 0; x < 16; ++x) {
      if (stretched[(y * 16) + x] >= 128) {
        bits |= (1u << x);
      }
    }
    p_dest[(y * 2) + 0] = (bits & 0xFF);
    p_dest[(y * 2) + 1] = (bits >> 8);
  }
#else
  teletext_stretch_12_to_16(p_dest, p_src);
#endif
}

static BEEBJIT_COLD void
teletext_generate(void) {
  uint32_t i;

  s_teletext_was_generated = 1;

  /* Make the ROM glyphs pretty. */
  for (i = 0; i < 96; ++i) {
    uint8_t double_glyph[12 * 20];
    uint8_t pretty_glyph[12 * 20];
    uint8_t* p_glyph = &teletext_characters[i * 60];

    /* 6x10 to 12x20. */
    teletext_double_up_pixels(&double_glyph[0], p_glyph);

    /* Smooth the diagonals. */
    teletext_smooth_diagonals(&pretty_glyph[0], &double_glyph[0]);

    /* Stretch 12 pixels wide to 16, with anti-aliasing. */
    teletext_store_glyph(&s_teletext_generated_glyphs[i *
                                                      k_teletext_glyph_bytes],
                         &pretty_glyph[0]);
  }

  /* 0x40 - 0x5F in graphics modes use the character glyph. */
  (void) memcpy(&s_teletext_generated_gfx[(0x40 - 0x20) *
                                         k_teletext_glyph_bytes],
                &s_teletext_generated_glyphs[(0x40 - 0x20) *
                                             k_teletext_glyph_bytes],
                (0x20 * k_teletext_glyph_bytes));
  (void) memcpy(&s_teletext_generated_sep_gfx[(0x40 - 0x20) *
                                             k_teletext_glyph_bytes],
                &s_teletext_generated_glyphs[(0x40 - 0x20) *
                                             k_teletext_glyph_bytes],
                (0x20 * k_teletext_glyph_bytes));

  /* Generate the graphics glphys, which are on a 2x3 grid. */
  for (i = 0; i < 0x40; ++i) {
    uint8_t glyph_index;
    uint8_t gfx_buf[6 * 10];
    uint8_t sep_gfx_buf[6 * 10];
    uint8_t doubled_gfx_glyph[12 * 20];

    (void) memset(gfx_buf, '\0', sizeof(gfx_buf));
    (void) memset(sep_gfx_buf, '\0', sizeof(sep_gfx_buf));

    if (i & 0x01) {
      teletext_draw_block(&gfx_buf[0], 0, 0, 3, 3);
      teletext_draw_block(&sep_gfx_buf[0], 1, 0, 2, 2);
    }
    if (i & 0x02) {
      teletext_draw_block(&gfx_buf[0], 3, 0, 3, 3);
      teletext_draw_block(&sep_gfx_buf[0], 4, 0, 2, 2);
    }
    if (i & 0x04) {
      teletext_draw_block(&gfx_buf[0], 0, 3, 3, 4);
      teletext_draw_block(&sep_gfx_buf[0], 1, 3, 2, 3);
    }
    if (i & 0x08) {
      teletext_draw_block(&gfx_buf[0], 3, 3, 3, 4);
      teletext_draw_block(&sep_gfx_buf[0], 4, 3, 2, 3);
    }
    if (i & 0x10) {
      teletext_draw_block(&gfx_buf[0], 0, 7, 3, 3);
      teletext_draw_block(&sep_gfx_buf[0], 1, 7, 2, 2);
    }
    if (i & 0x20) {
      teletext_draw_block(&gfx_buf[0], 3, 7, 3, 3);
      teletext_draw_block(&sep_gfx_buf[0], 4, 7, 2, 2);
    }

    glyph_index = i;
    if (i >= 0x20) {
      glyph_index += 0x20;
    }

    teletext_double_up_pixels(&doubled_gfx_glyph[0], &gfx_buf[0]);
    teletext_store_glyph(&s_teletext_generated_gfx[glyph_index *
                                                   k_teletext_glyph_bytes],
                         &doubled_gfx_glyph[0]);

    teletext_double_up_pixels(&doubled_gfx_glyph[0], &sep_gfx_buf[0]);
    teletext_store_glyph(&s_teletext_generated_sep_gfx[glyph_index *
                                                       k_teletext_glyph_bytes],
                         &doubled_gfx_glyph[0]);
  }

#if BEEBJIT_RENDER_PACKED4
  for (i = 0; i < 256; ++i) {
    uint32_t j;
    uint32_t word = 0;
    for (j = 0; j < 8; ++j) {
      if (i & (1u << j)) {
        word |= (0xFu << (j * 4));
      }
    }
    s_teletext_expand[i] = word;
  }
#endif
}

static inline void
teletext_set_active_characters(struct teletext_struct* p_teletext) {
  if (p_teletext->is_graphics_active) {
    if (p_teletext->is_separated_active) {
      p_teletext->p_active_characters = &s_teletext_generated_sep_gfx[0];
    } else {
      p_teletext->p_active_characters = &s_teletext_generated_gfx[0];
    }
  } else {
    p_teletext->p_active_characters = &s_teletext_generated_glyphs[0];
  }
}

static inline void
teletext_reset_scanline_state(struct teletext_struct* p_teletext) {
  p_teletext->is_graphics_active = 0;
  p_teletext->is_separated_active = 0;
  p_teletext->is_concealed = 0;
  p_teletext->double_active = 0;
  p_teletext->flash_active = 0;
  p_teletext->fg_color = p_teletext->palette[7];
  p_teletext->bg_color = p_teletext->palette[0];
  p_teletext->is_hold_graphics = 0;
  /* This is space. */
  p_teletext->p_held_character = &s_teletext_generated_glyphs[0];

  teletext_set_active_characters(p_teletext);
}

static inline void
teletext_advance_scanline(struct teletext_struct* p_teletext) {
  p_teletext->advance_count++;
  p_teletext->had_double_at_advance =
      p_teletext->had_double_active_this_scanline;
  p_teletext->scanline++;
  if (p_teletext->scanline == 10) {
    p_teletext->scanline = 0;
    if (p_teletext->second_character_row_of_double) {
      p_teletext->second_character_row_of_double = 0;
    } else if (p_teletext->had_double_active_this_scanline) {
      p_teletext->second_character_row_of_double = 1;
    }
  }

  p_teletext->had_double_active_this_scanline = 0;
}

static inline void
teletext_new_frame_started(struct teletext_struct* p_teletext) {
  p_teletext->scanline = 0;
  p_teletext->second_character_row_of_double = 0;

  /* Flash counter is a simple 6 bits on the silicon.
   * See SAA5050 reverse engineering thead:
   * https://stardot.org.uk/forums/viewtopic.php?t=21608
   */
  p_teletext->flash_count++;
  p_teletext->flash_count &= 63;
  p_teletext->flash_visible_this_frame = (p_teletext->flash_count >= 16);
}

static void
teletext_generate_black_character(struct teletext_struct* p_teletext) {
  uint32_t i;
  uint32_t rgba = (0xff000000 | p_teletext->background_color);

  for (i = 0; i < k_render_words_per_8_pixels; ++i) {
    p_teletext->render_character_2MHz_black.host_pixels[i] =
        render_pixel_from_argb(rgba);
  }

}

struct teletext_struct*
teletext_create(void) {
  uint32_t i;
  struct teletext_struct* p_teletext;

  if (!s_teletext_was_generated) {
    teletext_generate();
  }

  p_teletext = util_mallocz(sizeof(struct teletext_struct));

  p_teletext->flash_count = 0;
  p_teletext->scanline = 0;

  teletext_reset_scanline_state(p_teletext);
  teletext_new_frame_started(p_teletext);

  for (i = 0; i < 8; ++i) {
    uint32_t color = 0;
    if (i & 1) {
      color |= 0x010000;
    }
    if (i & 2) {
      color |= 0x000100;
    }
    if (i & 4) {
      color |= 0x000001;
    }
    if (BEEBJIT_RENDER_PACKED4) {
      /* Channels of 0 / 1 -> 0 / 255, then to a packed pixel word. */
      color = render_pixel_from_argb(color * 255);
    }
    p_teletext->palette[i] = color;
  }

  p_teletext->background_color = 0;
  teletext_generate_black_character(p_teletext);

  return p_teletext;
}

void
teletext_destroy(struct teletext_struct* p_teletext) {
  util_free(p_teletext);
}

void
teletext_set_black_rgb(struct teletext_struct* p_teletext, uint32_t rgb) {
  p_teletext->background_color = rgb;
  teletext_generate_black_character(p_teletext);
}

static inline void
teletext_handle_control_character(struct teletext_struct* p_teletext,
                                  uint8_t src_char) {
  switch (src_char) {
  case 0x00:
    /* NOTE: SAA5050 appears to be a pre-2.5 presentation level, which doesn't
     * have the ability to select black.
     * See: https://www.etsi.org/deliver/etsi_i_ets/300700_300799/300706/01_60/ets_300706e01p.pdf
     */
    break;
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x04:
  case 0x05:
  case 0x06:
  case 0x07:
    p_teletext->is_graphics_active = 0;
    p_teletext->fg_color = p_teletext->palette[src_char];
    if (p_teletext->is_concealed) {
      p_teletext->is_concealed = 0;
      /* Current character remains concealed. */
      p_teletext->render_fg_color = p_teletext->bg_color;
    }
    break;
  case 0x08:
    p_teletext->flash_active = 1;
    break;
  case 0x09:
    p_teletext->flash_active = 0;
    break;
  case 0x0C:
    p_teletext->double_active = 0;
    break;
  case 0x0D:
    p_teletext->double_active = 1;
    p_teletext->had_double_active_this_scanline = 1;
    break;
  case 0x10:
    /* Can't select black graphics -- see above. */
    break;
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x17:
    p_teletext->is_graphics_active = 1;
    p_teletext->fg_color = p_teletext->palette[(src_char & 7)];
    if (p_teletext->is_concealed) {
      p_teletext->is_concealed = 0;
      /* Current character remains concealed. */
      p_teletext->render_fg_color = p_teletext->bg_color;
    }
    break;
  case 0x18:
    /* Not commonly seen but needed e.g. by the JCB Digger MODE7 intro
     * animation.
     */
    p_teletext->is_concealed = 1;
    break;
  case 0x19:
    p_teletext->is_separated_active = 0;
    break;
  case 0x1A:
    p_teletext->is_separated_active = 1;
    break;
  case 0x1C:
    p_teletext->bg_color = p_teletext->palette[0];
    break;
  case 0x1D:
    p_teletext->bg_color = p_teletext->fg_color;
    break;
  case 0x1E:
    p_teletext->is_hold_graphics = 1;
    break;
  case 0x1F:
    p_teletext->is_hold_graphics = 0;
    break;
  }

  teletext_set_active_characters(p_teletext);
}

static inline void
teletext_do_data_byte(struct teletext_struct* p_teletext, uint8_t data) {
  /* Foreground color and active characters are set-after so load them before
   * potentially processing a control code.
   */
  int is_hold_graphics = p_teletext->is_hold_graphics;
  p_teletext->render_fg_color = p_teletext->fg_color;

  data &= 0x7F;

  if (data >= 0x20) {
    uint8_t* p_render_character = p_teletext->p_active_characters;
    p_render_character += (k_teletext_glyph_bytes * (data - 0x20));
    p_teletext->p_render_character = p_render_character;
    /* EMU NOTE: from the Teletext spec, "the "Held-Mosaic" character inserted
     * is the most recent mosaics character with bit 6 = '1' in its code on
     * that row".
     * This mostly matches the chip in the beeb, but one notable exception is
     * that control codes _outside_ of hold graphics reset the held character
     * to space.
     * Also: "The "Held-Mosaic" character is reset to "SPACE" at the start of
     * each row, on a change of alphanumeric/mosaics mode..."
     */
    if (p_teletext->is_graphics_active) {
      if (data & 0x20) {
        p_teletext->p_held_character = p_render_character;
      }
    } else {
      p_teletext->p_held_character = &s_teletext_generated_glyphs[0];
    }
  } else {
    uint8_t* p_held_character = p_teletext->p_held_character;
    int is_graphics_active = p_teletext->is_graphics_active;
    int is_double_active = p_teletext->double_active;

    teletext_handle_control_character(p_teletext, data);
    /* Hold on is set-at and hold off is set-after. */
    is_hold_graphics |= p_teletext->is_hold_graphics;
    if (is_graphics_active &&
        is_hold_graphics &&
        (p_teletext->double_active == is_double_active)) {
      p_teletext->p_render_character = p_held_character;
    } else {
      p_teletext->p_render_character = &s_teletext_generated_glyphs[0];
      p_teletext->p_held_character = &s_teletext_generated_glyphs[0];
    }
  }
}

void
teletext_data(struct teletext_struct* p_teletext, uint8_t data, int is_dispen) {
  /* This function handles the pipelining of incoming signals, which is to say
   * that in real hardware, incoming signals have their visible effect some
   * number of cycles after initial receipt.
   * There are two components potentially involved in the pipelining: the
   * IC15 latch (between the 6845 and SAA5050) and the SAA5050 chip itself.
   * In addition to the pipelining, the beeb teletext mode also uses 6845
   * "skew", which delays the 6845 DISPEN signal by 1 clock.
   * All together, the data bytes appear to hit the screen 3 clocks after the
   * 6845 selects them, and the display enable triggers after 1 clock of 6845
   * skew and 2 clocks of pipeline latency.
   */
  uint8_t queue_data = p_teletext->data_pipeline[0];
  int is_pipelined_dispen = p_teletext->dispen_pipeline[0];

  /* NOTE: this logic is outside the SAA5050 chip, but put here in the
   * pipeline handling logic for convenience.
   * A logic gate in IC37 and IC36 is used to set bit 6 in the pipelined data
   * byte if current DISPEN is low. This has the effect of avoiding control
   * codes in the off-screen data bytes leading up to the start of a line.
   */
  if (!is_dispen) {
    p_teletext->data_pipeline[2] |= 0x40;
  }

  teletext_do_data_byte(p_teletext, queue_data);
  p_teletext->last_data_byte = queue_data;

  p_teletext->data_pipeline[0] = p_teletext->data_pipeline[1];
  p_teletext->data_pipeline[1] = p_teletext->data_pipeline[2];
  p_teletext->data_pipeline[2] = data;

  /* The SAA5050 end-of-scanline logic triggers on LOSE going low, which is
   * connected to the 6845 DISPEN.
   * The logic is not simple.
   * For the per-scanline state (colors, graphics mode, etc.) to be reset,
   * the LOSE pin needs to be low for at least 2 cycles. We model this.
   * For the scanline counter to increment, there is a complicated interplay
   * between DEW (wired to hsync) and LOSE. Approximately: DEW resets a
   * 1/4 speed (1MHz / 4) clock that looks for LOSE being low. If LOSE is
   * only low briefly, the 1/4 speed clocking might actually miss it! We
   * do not model this, but if DISPEN is only low for 1 clock, we don't
   * advance the scanline counter.
   * We should also hold the scanline state in reset for the duration of LOSE
   * being low, but we currently only trigger on the negative going edge.
   */
  if ((is_pipelined_dispen == 0) &&
      (p_teletext->dispen_pipeline[1] == 0) &&
      (p_teletext->curr_dispen == 1)) {
    teletext_reset_scanline_state(p_teletext);
    teletext_advance_scanline(p_teletext);
  }
  p_teletext->curr_dispen = is_pipelined_dispen;

  p_teletext->dispen_pipeline[0] = p_teletext->dispen_pipeline[1];
  p_teletext->dispen_pipeline[1] = is_dispen;
}

int
teletext_is_idle(struct teletext_struct* p_teletext) {
  /* Fed zero data with DISPEN low (bitmap addressing), the pipeline settles
   * here: each further call processes 0x40 again and changes nothing.
   */
  return ((p_teletext->data_pipeline[0] == 0x40) &&
          (p_teletext->data_pipeline[1] == 0x40) &&
          (p_teletext->data_pipeline[2] == 0x00) &&
          (p_teletext->dispen_pipeline[0] == 0) &&
          (p_teletext->dispen_pipeline[1] == 0) &&
          (p_teletext->curr_dispen == 0) &&
          (p_teletext->last_data_byte == 0x40));
}

static inline void
teletext_render_internal(struct teletext_struct* p_teletext,
                         int is_odd_tick,
                         struct render_character_2MHz* p_out,
                         struct render_character_2MHz* p_next_out,
                         int is_pair) {
  uint32_t i;
  uint32_t src_data_scanline;
  int do_render_rounded_scanline;
  uint8_t* p_src_data = p_teletext->p_render_character;
  uint32_t render_fg_color = p_teletext->render_fg_color;
  uint32_t bg_color = p_teletext->bg_color;
  struct render_character_2MHz* p_black =
      &p_teletext->render_character_2MHz_black;

  if (!p_teletext->curr_dispen) {
    if (p_out) {
      *p_out = *p_black;
    }
    if (p_next_out) {
      *p_next_out = *p_black;
    }
    return;
  }

  if ((p_teletext->flash_active && !p_teletext->flash_visible_this_frame) ||
      (p_teletext->second_character_row_of_double &&
       !p_teletext->double_active)) {
    /* Re-route to space. */
    p_src_data = &s_teletext_generated_glyphs[0];
  }

  if (p_teletext->is_concealed) {
    /* Re-route to space. */
    p_src_data = &s_teletext_generated_glyphs[0];
  }

  src_data_scanline = p_teletext->scanline;
  if (!p_teletext->double_active) {
    src_data_scanline *= 2;
  } else {
    if (p_teletext->second_character_row_of_double) {
      src_data_scanline += 10;
    }
  }

  /* Handle non-interlaced teletext (the glyphs end up weird because character
   * rounded scanline rows aren't selected correctly).
   */
  do_render_rounded_scanline = 0;
  if (!p_teletext->is_isv &&
      p_teletext->crtc_ra0 &&
      !p_teletext->double_active) {
    do_render_rounded_scanline = 1;
  }

  /* Handle interlaced rendering. This is a non-default configuration where
   * the renderer only rasters every other line in the canvas.
   */
  if (!is_pair &&
      p_teletext->crtc_ra0 &&
      !p_teletext->double_active) {
    do_render_rounded_scanline = 1;
  }

  if (do_render_rounded_scanline) {
    src_data_scanline++;
  }

  assert(src_data_scanline < 20);
  p_src_data += (src_data_scanline * k_teletext_row_bytes);

  if (is_odd_tick) {
    p_src_data += k_teletext_half_row_bytes;
  }

#if BEEBJIT_RENDER_PACKED4
  (void) i;
  {
    uint32_t mask = s_teletext_expand[p_src_data[0]];
    p_out->host_pixels[0] = ((render_fg_color & mask) | (bg_color & ~mask));
    p_out = p_next_out;
    if (p_out == NULL) {
      return;
    }
    if (p_teletext->is_isv && !p_teletext->double_active) {
      p_src_data += k_teletext_row_bytes;
    }
    mask = s_teletext_expand[p_src_data[0]];
    p_out->host_pixels[0] = ((render_fg_color & mask) | (bg_color & ~mask));
    return;
  }
#endif

  for (i = 0; i < 8; ++i) {
    uint32_t color;
    uint8_t val = p_src_data[i];

    color = (val * render_fg_color);
    color += ((255 - val) * bg_color);

    p_out->host_pixels[i] = (color | 0xff000000);
  }

  p_out = p_next_out;
  if (p_out == NULL) {
    return;
  }

  /* Another condition to handle non-interlaced teletext. */
  if (p_teletext->is_isv && !p_teletext->double_active) {
    p_src_data += k_teletext_row_bytes;
  }

  for (i = 0; i < 8; ++i) {
    uint32_t color;
    uint8_t val = p_src_data[i];

    color = (val * render_fg_color);
    color += ((255 - val) * bg_color);

    p_out->host_pixels[i] = (color | 0xff000000);
  }
}

#if BEEBJIT_RENDER_PACKED4
/* The run cache (teletext_run()). What a run of bytes does in teletext_data()
 * is a function of the chip's state below and the bytes alone, so when a
 * character row's next scanline sends the same run from the same state,
 * every character's rendering inputs (glyph, colours) are what they were,
 * as is the state left behind; only the glyph row follows the scanline.
 * One line's worth of runs is kept: up to the hsync and past it, with one
 * to spare.
 */
/* 0: every run character by character, through the renderer (a reference
 * to check the cache and the short way for undrawn runs against).
 */
#ifndef BEEBJIT_TELETEXT_RUN_CACHE
#define BEEBJIT_TELETEXT_RUN_CACHE 1
#endif

enum {
  k_teletext_run_max = 64,
  k_teletext_run_entries = 3,
};

struct teletext_run_state {
  uint8_t data_pipeline[3];
  uint8_t dispen_pipeline[2];
  uint8_t curr_dispen;
  uint8_t last_data_byte;
  uint8_t is_graphics_active;
  uint8_t is_separated_active;
  uint8_t is_concealed;
  uint8_t double_active;
  uint8_t flash_active;
  uint8_t had_double_active_this_scanline;
  uint8_t second_character_row_of_double;
  uint8_t flash_visible_this_frame;
  uint8_t is_hold_graphics;
  uint32_t fg_color;
  uint32_t bg_color;
  uint32_t render_fg_color;
  uint8_t* p_held_character;
  uint8_t* p_active_characters;
  uint8_t* p_render_character;
};

struct teletext_run_entry {
  struct teletext_struct* p_teletext;
  uint32_t n;
  uint32_t dispen_arg;
  struct teletext_run_state start;
  struct teletext_run_state end;
  uint8_t advances;
  uint8_t had_double_at_advance;
  uint8_t bytes[k_teletext_run_max];
  /* fg code << 4 | bg code (the packed word is the code in every nibble);
   * 0xFF: black, DISPEN low.
   */
  uint8_t fgbg[k_teletext_run_max];
  uint8_t is_double[k_teletext_run_max];
  const uint8_t* p_glyph[k_teletext_run_max];
};

static struct teletext_run_entry s_teletext_runs[k_teletext_run_entries];
static uint32_t s_teletext_run_next;
uint32_t g_teletext_run_hits;
uint32_t g_teletext_run_misses;

static void
teletext_run_state_get(struct teletext_struct* p_teletext,
                       struct teletext_run_state* p_state) {
  (void) memcpy(p_state->data_pipeline, p_teletext->data_pipeline, 3);
  (void) memcpy(p_state->dispen_pipeline, p_teletext->dispen_pipeline, 2);
  p_state->curr_dispen = p_teletext->curr_dispen;
  p_state->last_data_byte = p_teletext->last_data_byte;
  p_state->is_graphics_active = p_teletext->is_graphics_active;
  p_state->is_separated_active = p_teletext->is_separated_active;
  p_state->is_concealed = p_teletext->is_concealed;
  p_state->double_active = p_teletext->double_active;
  p_state->flash_active = p_teletext->flash_active;
  p_state->had_double_active_this_scanline =
      p_teletext->had_double_active_this_scanline;
  p_state->second_character_row_of_double =
      p_teletext->second_character_row_of_double;
  p_state->flash_visible_this_frame = p_teletext->flash_visible_this_frame;
  p_state->is_hold_graphics = p_teletext->is_hold_graphics;
  p_state->fg_color = p_teletext->fg_color;
  p_state->bg_color = p_teletext->bg_color;
  p_state->render_fg_color = p_teletext->render_fg_color;
  p_state->p_held_character = p_teletext->p_held_character;
  p_state->p_active_characters = p_teletext->p_active_characters;
  p_state->p_render_character = p_teletext->p_render_character;
}

static void
teletext_run_state_set(struct teletext_struct* p_teletext,
                       const struct teletext_run_state* p_state) {
  (void) memcpy(p_teletext->data_pipeline, p_state->data_pipeline, 3);
  (void) memcpy(p_teletext->dispen_pipeline, p_state->dispen_pipeline, 2);
  p_teletext->curr_dispen = p_state->curr_dispen;
  p_teletext->last_data_byte = p_state->last_data_byte;
  p_teletext->is_graphics_active = p_state->is_graphics_active;
  p_teletext->is_separated_active = p_state->is_separated_active;
  p_teletext->is_concealed = p_state->is_concealed;
  p_teletext->double_active = p_state->double_active;
  p_teletext->flash_active = p_state->flash_active;
  p_teletext->had_double_active_this_scanline =
      p_state->had_double_active_this_scanline;
  p_teletext->second_character_row_of_double =
      p_state->second_character_row_of_double;
  p_teletext->flash_visible_this_frame = p_state->flash_visible_this_frame;
  p_teletext->is_hold_graphics = p_state->is_hold_graphics;
  p_teletext->fg_color = p_state->fg_color;
  p_teletext->bg_color = p_state->bg_color;
  p_teletext->render_fg_color = p_state->render_fg_color;
  p_teletext->p_held_character = p_state->p_held_character;
  p_teletext->p_active_characters = p_state->p_active_characters;
  p_teletext->p_render_character = p_state->p_render_character;
}

/* Whether the chip is in the state p_state records (without copying it).
 * With is_quiet (teletext_run_is_quiet()), the fields such a run overwrites
 * before they can show don't count.
 */
static inline int
teletext_run_state_equals(const struct teletext_struct* p_teletext,
                          const struct teletext_run_state* p_state,
                          int is_quiet) {
  if (is_quiet) {
    if (!(p_state->data_pipeline[0] & 0x40)) {
      return 0;
    }
  } else if ((p_teletext->p_render_character != p_state->p_render_character) ||
             (p_teletext->data_pipeline[0] != p_state->data_pipeline[0]) ||
             (p_teletext->last_data_byte != p_state->last_data_byte)) {
    return 0;
  }
  return ((p_teletext->data_pipeline[1] == p_state->data_pipeline[1]) &&
          (p_teletext->data_pipeline[2] == p_state->data_pipeline[2]) &&
          (p_teletext->dispen_pipeline[0] == p_state->dispen_pipeline[0]) &&
          (p_teletext->dispen_pipeline[1] == p_state->dispen_pipeline[1]) &&
          (p_teletext->curr_dispen == p_state->curr_dispen) &&
          (p_teletext->is_graphics_active == p_state->is_graphics_active) &&
          (p_teletext->is_separated_active ==
           p_state->is_separated_active) &&
          (p_teletext->is_concealed == p_state->is_concealed) &&
          (p_teletext->double_active == p_state->double_active) &&
          (p_teletext->flash_active == p_state->flash_active) &&
          (p_teletext->had_double_active_this_scanline ==
           p_state->had_double_active_this_scanline) &&
          (p_teletext->second_character_row_of_double ==
           p_state->second_character_row_of_double) &&
          (p_teletext->flash_visible_this_frame ==
           p_state->flash_visible_this_frame) &&
          (p_teletext->is_hold_graphics == p_state->is_hold_graphics) &&
          (p_teletext->fg_color == p_state->fg_color) &&
          (p_teletext->bg_color == p_state->bg_color) &&
          (p_teletext->render_fg_color == p_state->render_fg_color) &&
          (p_teletext->p_held_character == p_state->p_held_character) &&
          (p_teletext->p_active_characters == p_state->p_active_characters));
}

/* A run whose first byte (the one waiting at the head of the chip's
 * pipeline) goes through drawn black (DISPEN off in the pipeline), outside
 * graphics, marked off-screen (bit 6 set, so a printable character): all it
 * does is set the character to draw, the last byte seen and the held
 * character to space, and the next byte overwrites the first two. So with 2
 * bytes or more, it, the character and the last byte aren't part of the
 * run's key. (A character row's first scanline starts with an off-screen
 * byte from the line before it: without this, every row's second scanline
 * missed the cache as well as its first.)
 */
static inline int
teletext_run_is_quiet(const struct teletext_struct* p_teletext, uint32_t n) {
  return ((n >= 2) &&
          (p_teletext->dispen_pipeline[0] == 0) &&
          !p_teletext->is_graphics_active &&
          (p_teletext->data_pipeline[0] & 0x40));
}

/* What teletext_render_internal() would use for the character just fed. */
static inline void
teletext_run_capture(struct teletext_struct* p_teletext,
                     struct teletext_run_entry* p_entry,
                     uint32_t i) {
  const uint8_t* p_src_data = p_teletext->p_render_character;
  if (!p_teletext->curr_dispen) {
    /* Black: drawn as a space in black on black. */
    uint32_t black = p_teletext->render_character_2MHz_black.host_pixels[0];
    assert((black & 0xF) * 0x11111111u == black);
    p_entry->fgbg[i] = (((black & 0xF) << 4) | (black & 0xF));
    p_entry->p_glyph[i] = &s_teletext_generated_glyphs[0];
    p_entry->is_double[i] = 0;
    return;
  }
  if ((p_teletext->flash_active && !p_teletext->flash_visible_this_frame) ||
      (p_teletext->second_character_row_of_double &&
       !p_teletext->double_active) ||
      p_teletext->is_concealed) {
    p_src_data = &s_teletext_generated_glyphs[0];
  }
  assert((p_teletext->render_fg_color & 0xF) * 0x11111111u ==
         p_teletext->render_fg_color);
  assert((p_teletext->bg_color & 0xF) * 0x11111111u == p_teletext->bg_color);
  p_entry->fgbg[i] = (((p_teletext->render_fg_color & 0xF) << 4) |
                      (p_teletext->bg_color & 0xF));
  p_entry->p_glyph[i] = p_src_data;
  p_entry->is_double[i] = !!p_teletext->double_active;
}

/* The rows of glyph data teletext_render_internal() would draw from now:
 * the chip's scanline for a row's lines, or of a double-height pair.
 */
static inline void
teletext_run_rows(const struct teletext_struct* p_teletext,
                  uint32_t second_character_row_of_double,
                  uint32_t* p_row_single,
                  uint32_t* p_row_double) {
  uint32_t scanline = p_teletext->scanline;
  uint32_t row_single = ((scanline * 2) +
                         ((!p_teletext->is_isv && p_teletext->crtc_ra0) ? 1 : 0));
  uint32_t row_double = (scanline + (second_character_row_of_double ? 10 : 0));
  assert(row_single < 20);
  assert(row_double < 20);
  *p_row_single = (row_single * k_teletext_row_bytes);
  *p_row_double = (row_double * k_teletext_row_bytes);
}

/* One captured character, drawn (both ticks of a 1MHz character); p_rows:
 * the single and double height glyph rows.
 */
static inline void
teletext_run_draw(const struct teletext_run_entry* p_entry,
                  uint32_t i,
                  const uint32_t* p_rows,
                  uint32_t* p_out) {
  uint32_t fgbg = p_entry->fgbg[i];
  const uint8_t* p_src = (p_entry->p_glyph[i] + p_rows[p_entry->is_double[i]]);
  uint32_t bg = ((fgbg & 0xF) * 0x11111111u);
  uint32_t fg_bg = (((fgbg >> 4) * 0x11111111u) ^ bg);
  p_out[0] = (bg ^ (fg_bg & s_teletext_expand[p_src[0]]));
  p_out[1] = (bg ^ (fg_bg & s_teletext_expand[p_src[1]]));
}

static void
teletext_run_replay(struct teletext_struct* p_teletext,
                    const struct teletext_run_entry* p_entry,
                    uint32_t* p_out) {
  uint32_t i;
  uint32_t n = p_entry->n;
  uint32_t rows[2];

  teletext_run_rows(p_teletext,
                    p_teletext->second_character_row_of_double,
                    &rows[0],
                    &rows[1]);
  for (i = 0; (p_out != NULL) && (i < n); ++i) {
    teletext_run_draw(p_entry, i, rows, p_out);
    p_out += 2;
  }

  teletext_run_state_set(p_teletext, &p_entry->end);
  if (p_entry->advances) {
    /* The run's scanline advance, done on this scanline: the end state has
     * the row's values from when the run was captured.
     */
    p_teletext->second_character_row_of_double =
        p_entry->start.second_character_row_of_double;
    p_teletext->had_double_active_this_scanline =
        p_entry->had_double_at_advance;
    teletext_advance_scanline(p_teletext);
  }
}

/* (Byte loops: newlib's memcmp / memcpy run from flash on the Presto.) */
static inline int
teletext_run_bytes_equal(const uint8_t* p_a, const uint8_t* p_b, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; ++i) {
    if (p_a[i] != p_b[i]) {
      return 0;
    }
  }
  return 1;
}

/* A run with DISPEN low throughout and nothing drawn (outside the window,
 * or past the hsync). Once the chip's pipeline is quiet too (DISPEN low in
 * it and drawn), outside graphics, every byte it takes in goes through
 * marked off-screen (bit 6 set: a printable character), and all a printable
 * character does there is set the character to draw, the last byte seen and
 * the held character (space). So only the last bytes count: the run starts
 * them in the pipeline as if the rest had gone through.
 */
static void
teletext_run_off(struct teletext_struct* p_teletext,
                 const uint8_t* p_data,
                 uint32_t n) {
  uint32_t i = 0;
  while ((i < n) &&
         ((p_teletext->curr_dispen != 0) ||
          (p_teletext->dispen_pipeline[0] != 0) ||
          (p_teletext->dispen_pipeline[1] != 0))) {
    teletext_data(p_teletext, p_data[i], 0);
    i++;
  }
  if (!p_teletext->is_graphics_active && ((n - i) >= 6)) {
    p_teletext->data_pipeline[0] = (p_data[n - 6] | 0x40);
    p_teletext->data_pipeline[1] = (p_data[n - 5] | 0x40);
    p_teletext->data_pipeline[2] = p_data[n - 4];
    i = (n - 3);
  }
  while (i < n) {
    teletext_data(p_teletext, p_data[i], 0);
    i++;
  }
}

void
teletext_run(struct teletext_struct* p_teletext,
             const uint8_t* p_data,
             uint32_t n,
             uint32_t dispen,
             uint32_t dispen_settle,
             uint32_t dispen_pattern,
             uint32_t dispen_off_from,
             int is_1MHz,
             uint32_t ticks_odd,
             uint32_t* p_out) {
  uint32_t i;
  uint32_t ticks = ticks_odd;
  uint32_t advances_before;
  struct teletext_run_entry* p_entry;
  uint32_t dispen_arg;
  uint32_t rows[2];
  int is_quiet;

  if (dispen_off_from > n) {
    dispen_off_from = n;
  }
  dispen_arg = (dispen | (dispen_settle << 1) | (dispen_pattern << 3) |
                (dispen_off_from << 6));

  if (!BEEBJIT_TELETEXT_RUN_CACHE || !is_1MHz || ticks_odd ||
      (n > k_teletext_run_max) || (p_data == NULL)) {
    /* Character by character. */
    for (i = 0; i < n; ++i) {
      if (!(ticks & 1)) {
        if (p_data != NULL) {
          uint32_t d = ((i < dispen_settle) ? ((dispen_pattern >> i) & 1) :
                                              dispen);
          if (i >= dispen_off_from) {
            d = 0;
          }
          teletext_data(p_teletext, p_data[i], d);
        } else {
          teletext_data(p_teletext, 0, 0);
        }
      }
      if (p_out != NULL) {
        teletext_render_first_of_pair(p_teletext,
                                      (ticks & 1),
                                      (struct render_character_2MHz*) p_out);
        p_out += k_render_words_per_8_pixels;
        if (is_1MHz) {
          teletext_render_first_of_pair(p_teletext,
                                        1,
                                        (struct render_character_2MHz*) p_out);
          p_out += k_render_words_per_8_pixels;
        }
      }
      ticks += (is_1MHz ? 2 : 1);
    }
    return;
  }

  if (p_out == NULL) {
    /* Nothing drawn: with DISPEN low throughout, the short way. */
    uint32_t on_ticks = ((dispen_off_from < dispen_settle) ? dispen_off_from :
                                                             dispen_settle);
    if ((dispen_off_from == 0) ||
        ((!dispen || (dispen_settle >= dispen_off_from)) &&
         ((on_ticks >= 32) ||
          !(dispen_pattern & ((1u << on_ticks) - 1))))) {
      teletext_run_off(p_teletext, p_data, n);
      return;
    }
  }

  is_quiet = teletext_run_is_quiet(p_teletext, n);
  for (i = 0; i < k_teletext_run_entries; ++i) {
    struct teletext_run_entry* p_e = &s_teletext_runs[i];
    if ((p_e->p_teletext == p_teletext) &&
        (p_e->n == n) &&
        (p_e->dispen_arg == dispen_arg) &&
        teletext_run_state_equals(p_teletext, &p_e->start, is_quiet) &&
        teletext_run_bytes_equal(&p_e->bytes[0], p_data, n)) {
      teletext_run_replay(p_teletext, p_e, p_out);
      g_teletext_run_hits++;
      return;
    }
  }
  g_teletext_run_misses++;
  p_entry = &s_teletext_runs[s_teletext_run_next];
  s_teletext_run_next++;
  if (s_teletext_run_next == k_teletext_run_entries) {
    s_teletext_run_next = 0;
  }
  p_entry->p_teletext = NULL;
  p_entry->n = n;
  p_entry->dispen_arg = dispen_arg;
  teletext_run_state_get(p_teletext, &p_entry->start);
  for (i = 0; i < n; ++i) {
    p_entry->bytes[i] = p_data[i];
  }
  advances_before = p_teletext->advance_count;

  /* Each character through the chip, captured, and drawn from what was
   * captured (as a replay draws it, with the rows as they are now).
   */
  teletext_run_rows(p_teletext,
                    p_teletext->second_character_row_of_double,
                    &rows[0],
                    &rows[1]);
  for (i = 0; i < n; ++i) {
    uint32_t advances = p_teletext->advance_count;
    uint32_t d = ((i < dispen_settle) ? ((dispen_pattern >> i) & 1) : dispen);
    if (i >= dispen_off_from) {
      d = 0;
    }
    teletext_data(p_teletext, p_data[i], d);
    teletext_run_capture(p_teletext, p_entry, i);
    if (p_out != NULL) {
      if (p_teletext->advance_count != advances) {
        teletext_run_rows(p_teletext,
                          p_teletext->second_character_row_of_double,
                          &rows[0],
                          &rows[1]);
      }
      teletext_run_draw(p_entry, i, rows, p_out);
      p_out += 2;
    }
  }

  {
    uint32_t advances = (p_teletext->advance_count - advances_before);
    if (advances <= 1) {
      p_entry->advances = advances;
      p_entry->had_double_at_advance = p_teletext->had_double_at_advance;
      teletext_run_state_get(p_teletext, &p_entry->end);
      p_entry->p_teletext = p_teletext;
    }
  }
}
#endif

void
teletext_render(struct teletext_struct* p_teletext,
                int is_odd_tick,
                struct render_character_2MHz* p_out,
                struct render_character_2MHz* p_next_out) {
  teletext_render_internal(p_teletext,
                           is_odd_tick,
                           p_out,
                           p_next_out,
                           (p_next_out != NULL));
}

void
teletext_render_first_of_pair(struct teletext_struct* p_teletext,
                              int is_odd_tick,
                              struct render_character_2MHz* p_out) {
  /* teletext_render()'s first row of a deinterlaced pair, exactly, without
   * the second.
   */
  teletext_render_internal(p_teletext, is_odd_tick, p_out, NULL, 1);
}

void
teletext_RA_ISV_changed(struct teletext_struct* p_teletext,
                        uint8_t ra,
                        int is_isv) {
  p_teletext->crtc_ra0 = (ra & 1);
  p_teletext->is_isv = is_isv;
}

void
teletext_VSYNC_changed(struct teletext_struct* p_teletext, int value) {
  /* TODO: we've currently only wired this up to VSYNC lower but it will
   * suffice.
   */
  (void) value;

  /* NOTE: the reverse engineered SAA5050 schematic suggests that the scanline
   * counter is in fact held low while the VSYNC signal is asserted. i.e. it
   * is level triggered whereas here we are edge triggered.
   * Practically they are similar because it is unusual to be rendering pixel
   * output while vsync is active!
   */
  teletext_new_frame_started(p_teletext);
}
