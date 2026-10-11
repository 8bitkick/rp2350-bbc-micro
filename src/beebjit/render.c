#include "render.h"

#include "bbc_options.h"
#include "teletext.h"
#include "util.h"

#include <assert.h>
#include <string.h>

enum {
  k_render_mode0 = 0,
  k_render_mode1 = 1,
  k_render_mode2 = 2,
  k_render_mode4 = 3,
  k_render_mode5 = 4,
  k_render_mode7 = 5,
  k_render_mode8 = 6,
  k_render_mode4_80 = 7,
  k_render_mode2_10 = 8,
};

struct render_struct;
typedef void (*render_func_t)(struct render_struct* p_render,
                              uint8_t data,
                              int is_odd_tick);

struct render_struct {
  void (*p_flyback_callback)(void*);
  void* p_flyback_callback_object;

  uint32_t width;
  uint32_t height;
  /* Words per row: width with ARGB pixels, width / 8 packed. */
  uint32_t stride;

  uint32_t* p_buffer;
  uint32_t* p_buffer_end;
  int is_buffer_owned;

  struct teletext_struct* p_teletext;

  uint8_t logical_to_physical_color[16];
  uint32_t palette[16];
  uint32_t render_tables_built;
  struct render_table_2MHz render_table_mode0;
  struct render_table_2MHz render_table_mode1;
  struct render_table_2MHz render_table_mode2;
  struct render_table_2MHz render_table_mode2_10;
  struct render_table_1MHz render_table_mode4;
  struct render_table_1MHz render_table_mode4_80;
  struct render_table_1MHz render_table_mode5;
  struct render_table_1MHz render_table_mode8;

  struct render_character_2MHz render_character_2MHz_black;

#if BEEBJIT_RENDER_PACKED4
  /* Fast table rebuilds on palette changes: the current mode's pixels as
   * logical color indices (4 bits each, laid out as the table), which only
   * change with the mode, then mapped through the palette 2 pixels at a time.
   */
  /* Per mode (Elite's split screen switches mode twice a frame), built once. */
  uint32_t packed_index_built;
  uint32_t packed_index_words[k_render_mode2_10 + 1][256 * 2];
  /* The codes each mode's table was last built with: palette writes often
   * leave them as they were (Elite rewrites its palettes every frame).
   */
  uint32_t packed_codes_valid;
  uint8_t packed_codes[k_render_mode2_10 + 1][16];
  /* The logical colors each mode's pixels can use: only those codes matter
   * (palettes are often written one entry at a time, e.g. at Elite's split,
   * and a mode with 2 or 4 colors doesn't see most of the steps).
   */
  uint16_t packed_used[k_render_mode2_10 + 1];
#endif

  struct render_table_1MHz* p_render_table_1MHz;
  struct render_table_2MHz* p_render_table_2MHz;

  int render_mode;
  int is_dispen;
  uint32_t row_address;
  render_func_t p_render_func;
  render_func_t p_render_blank_func;
  render_func_t p_selected_render_func;
  int is_clock_2MHz;
  int chars_per_line;
  int is_teletext;
  int is_flash;
  int needs_render_mode_recalc;
  int32_t horiz_beam_pos;
  int32_t vert_beam_pos;
  int32_t horiz_beam_window_start_pos;
  int32_t horiz_beam_window_end_pos;
  int32_t vert_beam_window_start_pos;
  int32_t vert_beam_window_end_pos;
  uint32_t* p_render_pos;
  uint32_t* p_render_pos_row;
  uint32_t* p_render_pos_row_max;
  int32_t cursor_segment_index;
  int cursor_segments[4];
  int is_crt_grayscale_fakeout;

  /* Options. */
  int is_double_size;
  int do_deinterlace_teletext;
  int do_deinterlace_bitmap;

  /* Row mode: render into a buffer of 2 rows (one interlace pair) and hand
   * each finished pair to a callback, instead of keeping a whole frame.
   */
  int is_row_mode;
  int32_t row_window_pos;
  int is_row_odd;
  /* What isn't black yet, so that neither clearing a row nor drawing blanks
   * touches what's black already: this row pair has been drawn (anything but
   * black runs) from p_row_dirty_start to p_row_dirty_end, in row 0's terms,
   * and each row of the pair may hold non-black from row_uncleared_start[]
   * to row_uncleared_end[] words in (from earlier pairs).
   */
  uint32_t* p_row_dirty_start;
  uint32_t* p_row_dirty_end;
  uint32_t row_uncleared_start[2];
  uint32_t row_uncleared_end[2];
  void (*p_row_callback)(void* p, uint32_t row, uint32_t* p_pixels);
  void* p_row_callback_object;
};

struct render_struct*
render_create(struct teletext_struct* p_teletext,
              struct bbc_options* p_options) {
  uint32_t width;
  uint32_t height;
  uint32_t i;
  uint32_t background_color = 0;

  /* The border appears on all screen edges, and 1 character unit is the width /
   * height of a MODE 4 glyph / character.
   */
  uint32_t border_chars = 4;

  /* These numbers, 15 and 4, come from the delta between horiz/vert sync
   * position and of line/frame, in a standard 1MHz MODE.
   * Note that AUG states R7 as 34 for a lot of the screen modes whereas it
   * should be 35!
   */
  uint32_t k_horiz_standard_offset = 15;
  uint32_t k_vert_standard_offset = 4;

  const char* p_opt_flags = p_options->p_opt_flags;

  struct render_struct* p_render = util_mallocz(sizeof(struct render_struct));

  p_render->p_teletext = p_teletext;

  /* "border characters" is the number of MODE1 square 8x8 pixel characters
   * used to pad the display window beyond the standard viewport for MODE1.
   * If set to zero, standard modes will fit perfectly. If set larger than
   * zero, pixels rendered in "overscan" areas will start to be visible.
   */
  (void) util_get_u32_option(&border_chars, p_opt_flags, "video:border-chars=");
  if (border_chars > 16) {
    util_bail("border-chars must be 16 or less");
  }

  p_render->do_deinterlace_teletext = 1;
  if (util_has_option(p_opt_flags, "video:no-deinterlace-teletext")) {
    p_render->do_deinterlace_teletext = 0;
  }
  p_render->do_deinterlace_bitmap = 1;
  if (util_has_option(p_opt_flags, "video:no-deinterlace-bitmap")) {
    p_render->do_deinterlace_bitmap = 0;
  }
  (void) util_get_u32_hex_option(&background_color,
                                 p_opt_flags,
                                 "video:background=");
  background_color |= 0xff000000;

  width = (640 + (border_chars * 2 * 16));
  height = (512 + (border_chars * 2 * 16));

  if (util_has_option(p_opt_flags, "video:double-size")) {
    width *= 2;
    height *= 2;
    p_render->is_double_size = 1;
  } else {
    p_render->is_double_size = 0;
  }

  if (BEEBJIT_RENDER_PACKED4 && p_render->is_double_size) {
    util_bail("video:double-size isn't supported with packed pixels");
  }
  p_render->width = width;
  p_render->height = height;
  p_render->stride = ((width / 8) * k_render_words_per_8_pixels);

  if (border_chars > k_horiz_standard_offset) {
    p_render->horiz_beam_window_start_pos = 0;
  } else {
    p_render->horiz_beam_window_start_pos =
        ((k_horiz_standard_offset - border_chars) * 16);
  }
  p_render->horiz_beam_window_end_pos = (p_render->width +
                                         p_render->horiz_beam_window_start_pos);

  if (border_chars > k_vert_standard_offset) {
    p_render->vert_beam_window_start_pos = 0;
  } else {
    p_render->vert_beam_window_start_pos =
        ((k_vert_standard_offset - border_chars) * 16);
  }
  p_render->vert_beam_window_end_pos = (p_render->height +
                                        p_render->vert_beam_window_start_pos);

  p_render->render_mode = k_render_mode0;
  p_render->is_clock_2MHz = 1;
  p_render->chars_per_line= 3;
  p_render->is_teletext = 0;
  p_render->needs_render_mode_recalc = 1;

  p_render->cursor_segment_index = -1;

  p_render->render_tables_built = 0;
#if BEEBJIT_RENDER_PACKED4
  p_render->packed_index_built = 0;
  p_render->packed_codes_valid = 0;
#endif

  for (i = 0; i < k_render_words_per_8_pixels; ++i) {
    p_render->render_character_2MHz_black.host_pixels[i] =
        render_pixel_from_argb(background_color);
  }

  if (background_color != 0) {
    teletext_set_black_rgb(p_teletext, background_color);
  }

  return p_render;
}

void
render_destroy(struct render_struct* p_render) {
  if (p_render->is_buffer_owned) {
    util_free(p_render->p_buffer);
  }
  util_free(p_render);
}

void
render_power_on_reset(struct render_struct* p_render) {
  uint32_t i;

  for (i = 0; i < 16; ++i) {
    render_set_physical_color(p_render, i, 0);
  }

  /* Default displayed colors. */
  p_render->palette[0] = 0xff000000;
  p_render->palette[1] = 0xffff0000;
  p_render->palette[2] = 0xff00ff00;
  p_render->palette[3] = 0xffffff00;
  p_render->palette[4] = 0xff0000ff;
  p_render->palette[5] = 0xffff00ff;
  p_render->palette[6] = 0xff00ffff;
  p_render->palette[7] = 0xffffffff;
  for (i = 8; i < 16; ++i) {
    p_render->palette[i] = 0;
  }
}

void
render_set_flyback_callback(struct render_struct* p_render,
                            void (*p_flyback_callback)(void* p),
                            void* p_flyback_callback_object) {
  p_render->p_flyback_callback = p_flyback_callback;
  p_render->p_flyback_callback_object = p_flyback_callback_object;
}

#if BEEBJIT_RENDER_WIRE
struct teletext_struct*
render_get_teletext(struct render_struct* p_render) {
  return p_render->p_teletext;
}
#endif

uint32_t
render_get_width(struct render_struct* p_render) {
  return p_render->width;
}

uint32_t
render_get_height(struct render_struct* p_render) {
  return p_render->height;
}

uint32_t
render_get_stride(struct render_struct* p_render) {
  return p_render->stride;
}

uint32_t
render_pixel_from_argb(uint32_t argb) {
  if (BEEBJIT_RENDER_PACKED4) {
    uint32_t code = (((argb >> 7) & 1) | ((argb >> 14) & 2) |
                     ((argb >> 21) & 4));
    /* All 8 pixels of a word. */
    return (code * 0x11111111u);
  }
  return argb;
}

uint32_t*
render_get_buffer(struct render_struct* p_render) {
  return p_render->p_buffer;
}

int
render_has_buffer(struct render_struct* p_render) {
  return (p_render->p_buffer != NULL);
}

uint32_t
render_get_buffer_size(struct render_struct* p_render) {
  if (p_render->is_row_mode) {
    return (p_render->stride * 2 * 4);
  }
  return (p_render->stride * p_render->height * 4);
}

uint32_t
render_get_horiz_pos(struct render_struct* p_render) {
  return p_render->horiz_beam_pos;
}

uint32_t
render_get_vert_pos(struct render_struct* p_render) {
  return p_render->vert_beam_pos;
}

uint32_t
render_get_buffer_crc32(struct render_struct* p_render) {
  uint32_t crc = util_crc32_init();

  if (p_render->p_buffer != NULL) {
    uint32_t bytes = render_get_buffer_size(p_render);
    crc = util_crc32_add(crc, (uint8_t*) p_render->p_buffer, bytes);
  }

  crc = util_crc32_finish(crc);

  return crc;
}

/* Black from p_words up to p_end. */
static inline void
render_fill_black(uint32_t* p_words, uint32_t* p_end) {
  uint32_t black = render_pixel_from_argb(0xff000000);
  while ((p_words + 4) <= p_end) {
    p_words[0] = black;
    p_words[1] = black;
    p_words[2] = black;
    p_words[3] = black;
    p_words += 4;
  }
  while (p_words < p_end) {
    *p_words++ = black;
  }
}

/* How far into a row (words) a pointer into the row pair is. */
static inline uint32_t
render_row_offset(struct render_struct* p_render, uint32_t* p) {
  uint32_t offset = (uint32_t) (p - p_render->p_buffer);
  if (offset > p_render->stride) {
    offset = p_render->stride;
  }
  return offset;
}

static void
render_row_mode_flush(struct render_struct* p_render, int32_t new_row_pos) {
  int is_deinterlaced;
  uint32_t* p_row = p_render->p_buffer;
  uint32_t row_index = 0;

  if (new_row_pos == p_render->row_window_pos) {
    return;
  }
  if (p_render->row_window_pos >= 0) {
    /* Deinterlacing renderers draw both rows of the pair; the interlaced ones
     * draw the odd row on odd fields.
     */
    if (p_render->render_mode == k_render_mode7) {
      is_deinterlaced = p_render->do_deinterlace_teletext;
    } else {
      is_deinterlaced = p_render->do_deinterlace_bitmap;
    }
    if (p_render->is_row_odd && !is_deinterlaced) {
      p_row += p_render->stride;
      row_index = 1;
    }
    {
      /* Rows that the beam doesn't fully cover must come out black. This
       * pair drew (or blanked) everything from dirty start to end, so what
       * must be cleared first is only what earlier pairs left in this row
       * outside that (for most lines, nothing: the same span again).
       */
      uint32_t i;
      uint32_t cur_start = 0;
      uint32_t cur_end = 0;
      uint32_t prev_start = p_render->row_uncleared_start[row_index];
      uint32_t prev_end = p_render->row_uncleared_end[row_index];
      if (p_render->p_row_dirty_end > p_render->p_row_dirty_start) {
        cur_start = render_row_offset(p_render, p_render->p_row_dirty_start);
        cur_end = render_row_offset(p_render, p_render->p_row_dirty_end);
      }
      if (prev_end > prev_start) {
        if (cur_end > cur_start) {
          if (prev_start < cur_start) {
            render_fill_black((p_row + prev_start),
                              (p_row + ((prev_end < cur_start) ? prev_end :
                                                                 cur_start)));
          }
          if (prev_end > cur_end) {
            render_fill_black((p_row + ((prev_start > cur_end) ? prev_start :
                                                                 cur_end)),
                              (p_row + prev_end));
          }
        } else {
          render_fill_black((p_row + prev_start), (p_row + prev_end));
        }
      }
      p_render->p_row_callback(p_render->p_row_callback_object,
                               p_render->row_window_pos,
                               p_row);
      /* This row now holds this pair's span; the other may also hold it. */
      p_render->row_uncleared_start[row_index] = cur_start;
      p_render->row_uncleared_end[row_index] = cur_end;
      i = (row_index ^ 1);
      if (cur_end > cur_start) {
        if (p_render->row_uncleared_end[i] == 0) {
          p_render->row_uncleared_start[i] = cur_start;
          p_render->row_uncleared_end[i] = cur_end;
        } else {
          if (cur_start < p_render->row_uncleared_start[i]) {
            p_render->row_uncleared_start[i] = cur_start;
          }
          if (cur_end > p_render->row_uncleared_end[i]) {
            p_render->row_uncleared_end[i] = cur_end;
          }
        }
      }
    }
  }
  /* Empty: start past the end. */
  p_render->p_row_dirty_start = (p_render->p_buffer + p_render->stride);
  p_render->p_row_dirty_end = p_render->p_buffer;
  p_render->row_window_pos = new_row_pos;
  p_render->is_row_odd = (p_render->vert_beam_pos & 1);
}

/* Drawing anything but black runs: from p_start to p_end (row 0's terms). */
static inline void
render_mark_dirty(struct render_struct* p_render,
                  uint32_t* p_start,
                  uint32_t* p_end) {
  if (p_start < p_render->p_row_dirty_start) {
    p_render->p_row_dirty_start = p_start;
  }
  if (p_end > p_render->p_row_dirty_end) {
    p_render->p_row_dirty_end = p_end;
  }
}

static inline void
render_reset_render_pos(struct render_struct* p_render) {
  uint32_t window_horiz_pos;
  uint32_t window_vert_pos;
  int32_t vert_beam_pos;

  p_render->p_render_pos = (p_render->p_buffer_end + 1);
  p_render->p_render_pos_row = (p_render->p_buffer_end + 1);
  p_render->p_render_pos_row_max = NULL;

  if (p_render->p_buffer == NULL) {
    return;
  }

  /* Align the render pointer to a block of 2 vertical pixels. This gives the
   * renderers flexibility to handle interlace in different ways. It also
   * guards again a switch from interlace to de-interlaced mid-frame causing
   * problems like out-of-bounds accesses.
   */
  vert_beam_pos = (p_render->vert_beam_pos & ~1);
  if ((vert_beam_pos >= p_render->vert_beam_window_end_pos) ||
      (vert_beam_pos < p_render->vert_beam_window_start_pos)) {
    if (p_render->is_row_mode) {
      render_row_mode_flush(p_render, -1);
    }
    return;
  }

  window_vert_pos = (vert_beam_pos - p_render->vert_beam_window_start_pos);
  p_render->p_render_pos_row = p_render->p_buffer;
  if (p_render->is_row_mode) {
    render_row_mode_flush(p_render, window_vert_pos);
  } else {
    p_render->p_render_pos_row += (window_vert_pos * p_render->stride);
  }

  if (p_render->horiz_beam_pos >= p_render->horiz_beam_window_end_pos) {
    return;
  }
  if (p_render->horiz_beam_pos < p_render->horiz_beam_window_start_pos) {
    return;
  }

  window_horiz_pos = (p_render->horiz_beam_pos -
                      p_render->horiz_beam_window_start_pos);

  p_render->p_render_pos = p_render->p_render_pos_row;
  if (BEEBJIT_RENDER_PACKED4) {
    /* Whole words only: no half character offsets. */
    p_render->p_render_pos += (window_horiz_pos / 8);
  } else {
    p_render->p_render_pos += window_horiz_pos;
  }
  p_render->p_render_pos_row_max =
      (p_render->p_render_pos_row + p_render->stride -
       k_render_words_per_8_pixels);
}

static void
render_setup_new_buffer(struct render_struct* p_render) {
  uint32_t* p_buffer = p_render->p_buffer;
  p_render->p_buffer_end = p_buffer;
  if (p_render->is_row_mode) {
    p_render->p_buffer_end += (p_render->stride * 2);
  } else {
    p_render->p_buffer_end += (p_render->stride * p_render->height);
  }

  render_clear_buffer(p_render);

  p_render->horiz_beam_pos = 0;
  p_render->vert_beam_pos = 0;
  render_reset_render_pos(p_render);
}

void
render_set_buffer(struct render_struct* p_render, uint32_t* p_buffer) {
  assert(p_render->p_buffer == NULL);
  assert(p_buffer != NULL);

  p_render->p_buffer = p_buffer;
  p_render->is_buffer_owned = 0;
  render_setup_new_buffer(p_render);
}

void
render_set_row_buffer(struct render_struct* p_render,
                      uint32_t* p_buffer,
                      void (*p_row_callback)(void* p,
                                             uint32_t row,
                                             uint32_t* p_pixels),
                      void* p_row_callback_object) {
  assert(p_render->p_buffer == NULL);
  assert(p_buffer != NULL);

  p_render->p_buffer = p_buffer;
  p_render->is_buffer_owned = 0;
  p_render->is_row_mode = 1;
  p_render->row_window_pos = -1;
  p_render->p_row_dirty_start = (p_buffer + p_render->stride);
  p_render->p_row_dirty_end = p_buffer;
  /* The buffer is cleared (render_setup_new_buffer). */
  p_render->row_uncleared_start[0] = 0;
  p_render->row_uncleared_end[0] = 0;
  p_render->row_uncleared_start[1] = 0;
  p_render->row_uncleared_end[1] = 0;
  p_render->p_row_callback = p_row_callback;
  p_render->p_row_callback_object = p_row_callback_object;
  render_setup_new_buffer(p_render);
}

void
render_create_internal_buffer(struct render_struct* p_render) {
  uint32_t size = render_get_buffer_size(p_render);
  assert(p_render->p_buffer == NULL);

  p_render->p_buffer = util_malloc(size);
  p_render->is_buffer_owned = 1;
  render_setup_new_buffer(p_render);
}

static inline void
render_check_cursor(struct render_struct* p_render,
                    uint32_t* p_render_pos,
                    uint32_t* p_next_render_pos,
                    int do_cursor_segment_advance) {
  if (p_render->cursor_segment_index == -1) {
    return;
  }

  if (p_render->cursor_segments[p_render->cursor_segment_index] &&
      (p_render_pos >= p_render->p_render_pos_row)) {
    uint32_t i;
    /* Invert the color (but not alpha / the unused 4th bit). */
    uint32_t invert = (BEEBJIT_RENDER_PACKED4 ? 0x77777777 : 0x00ffffff);
    for (i = 0; i < k_render_words_per_8_pixels; ++i) {
      p_render_pos[i] ^= invert;
      if (p_next_render_pos != NULL) {
        p_next_render_pos[i] ^= invert;
      }
    }
  }
  if (!do_cursor_segment_advance) {
    return;
  }
  p_render->cursor_segment_index++;
  if (p_render->cursor_segment_index == 4) {
    p_render->cursor_segment_index = -1;
  }
}

static void
render_function_teletext_deinterlaced(struct render_struct* p_render,
                                      uint8_t data,
                                      int is_odd_tick) {
  uint32_t* p_render_pos = p_render->p_render_pos;
  uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
  struct teletext_struct* p_teletext = p_render->p_teletext;

  (void) data;

  teletext_render(p_teletext,
                  is_odd_tick,
                  (struct render_character_2MHz*) p_render_pos,
                  (struct render_character_2MHz*) p_next_render_pos);
  render_check_cursor(p_render, p_render_pos, p_next_render_pos, is_odd_tick);
}

static void
render_function_teletext_interlaced(struct render_struct* p_render,
                                    uint8_t data,
                                    int is_odd_tick) {
  uint32_t* p_render_pos = p_render->p_render_pos;
  struct teletext_struct* p_teletext = p_render->p_teletext;

  (void) data;

  if (p_render->vert_beam_pos & 1) {
    uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
    teletext_render(p_teletext,
                    is_odd_tick,
                    (struct render_character_2MHz*) p_next_render_pos,
                    NULL);
    render_check_cursor(p_render, p_next_render_pos, NULL, is_odd_tick);
  } else {
    teletext_render(p_teletext,
                    is_odd_tick,
                    (struct render_character_2MHz*) p_render_pos,
                    NULL);
    render_check_cursor(p_render, p_render_pos, NULL, is_odd_tick);
  }
}

#if BEEBJIT_RENDER_PACKED4
static int render_check_packed_table(struct render_struct* p_render);
#endif

/* With packed pixels, render_prepare() leaves a table invalidated by a
 * palette or mode write unbuilt until bitmap data is actually drawn with it:
 * a game rewriting the whole palette at a split rebuilt it once per write.
 */
static inline void
render_check_table_built(struct render_struct* p_render) {
#if BEEBJIT_RENDER_PACKED4
  if (!(p_render->render_tables_built & (1u << p_render->render_mode))) {
    (void) render_check_packed_table(p_render);
  }
#else
  (void) p_render;
#endif
}

static void
render_function_1MHz_data_deinterlaced(struct render_struct* p_render,
                                       uint8_t data,
                                       int is_odd_tick) {
  render_check_table_built(p_render);
  uint32_t* p_render_pos = p_render->p_render_pos;
  uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
  struct render_character_2MHz* p_value;

  if (!is_odd_tick) {
    p_value = &p_render->p_render_table_1MHz->values[data].first;
  } else {
    p_value = &p_render->p_render_table_1MHz->values[data].second;
  }
  *(struct render_character_2MHz*) p_render_pos = *p_value;
  *(struct render_character_2MHz*) p_next_render_pos = *p_value;
  render_check_cursor(p_render, p_render_pos, p_next_render_pos, is_odd_tick);
}

static void
render_function_1MHz_data_interlaced(struct render_struct* p_render,
                                     uint8_t data,
                                     int is_odd_tick) {
  render_check_table_built(p_render);
  uint32_t* p_render_pos = p_render->p_render_pos;
  struct render_character_2MHz* p_value;

  if (!is_odd_tick) {
    p_value = &p_render->p_render_table_1MHz->values[data].first;
  } else {
    p_value = &p_render->p_render_table_1MHz->values[data].second;
  }
  if (p_render->vert_beam_pos & 1) {
    uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
    *(struct render_character_2MHz*) p_next_render_pos = *p_value;
    render_check_cursor(p_render, p_next_render_pos, NULL, is_odd_tick);
  } else {
    *(struct render_character_2MHz*) p_render_pos = *p_value;
    render_check_cursor(p_render, p_render_pos, NULL, is_odd_tick);
  }
}

static void
render_function_1MHz_blank_deinterlaced(struct render_struct* p_render,
                                        uint8_t data,
                                        int is_odd_tick) {
  uint32_t* p_render_pos = p_render->p_render_pos;
  uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
  struct render_character_2MHz* p_value =
      &p_render->render_character_2MHz_black;

  (void) data;

  *(struct render_character_2MHz*) p_render_pos = *p_value;
  *(struct render_character_2MHz*) p_next_render_pos = *p_value;
  render_check_cursor(p_render, p_render_pos, p_next_render_pos, is_odd_tick);
}

static void
render_function_1MHz_blank_interlaced(struct render_struct* p_render,
                                      uint8_t data,
                                      int is_odd_tick) {
  uint32_t* p_render_pos = p_render->p_render_pos;
  struct render_character_2MHz* p_value =
      &p_render->render_character_2MHz_black;

  (void) data;

  if (p_render->vert_beam_pos & 1) {
    uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
    *(struct render_character_2MHz*) p_next_render_pos = *p_value;
    render_check_cursor(p_render, p_next_render_pos, NULL, is_odd_tick);
  } else {
    *(struct render_character_2MHz*) p_render_pos = *p_value;
    render_check_cursor(p_render, p_render_pos, NULL, is_odd_tick);
  }
}

static void
render_function_2MHz_data_deinterlaced(struct render_struct* p_render,
                                       uint8_t data,
                                       int is_odd_tick) {
  render_check_table_built(p_render);
  uint32_t* p_render_pos = p_render->p_render_pos;
  uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
  struct render_character_2MHz* p_value =
      &p_render->p_render_table_2MHz->values[data];

  (void) is_odd_tick;

  *(struct render_character_2MHz*) p_render_pos = *p_value;
  *(struct render_character_2MHz*) p_next_render_pos = *p_value;
  render_check_cursor(p_render, p_render_pos, p_next_render_pos, 1);
}

static void
render_function_2MHz_data_interlaced(struct render_struct* p_render,
                                     uint8_t data,
                                     int is_odd_tick) {
  render_check_table_built(p_render);
  uint32_t* p_render_pos = p_render->p_render_pos;
  struct render_character_2MHz* p_value =
      &p_render->p_render_table_2MHz->values[data];

  (void) is_odd_tick;

  if (p_render->vert_beam_pos & 1) {
    uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
    *(struct render_character_2MHz*) p_next_render_pos = *p_value;
    render_check_cursor(p_render, p_next_render_pos, NULL, 1);
  } else {
    *(struct render_character_2MHz*) p_render_pos = *p_value;
    render_check_cursor(p_render, p_render_pos, NULL, 1);
  }
}

static void
render_function_2MHz_blank_deinterlaced(struct render_struct* p_render,
                                        uint8_t data,
                                        int is_odd_tick) {
  uint32_t* p_render_pos = p_render->p_render_pos;
  uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
  struct render_character_2MHz* p_value =
      &p_render->render_character_2MHz_black;

  (void) data;
  (void) is_odd_tick;

  *(struct render_character_2MHz*) p_render_pos = *p_value;
  *(struct render_character_2MHz*) p_next_render_pos = *p_value;
  render_check_cursor(p_render, p_render_pos, p_next_render_pos, 1);
}

static void
render_function_2MHz_blank_interlaced(struct render_struct* p_render,
                                      uint8_t data,
                                      int is_odd_tick) {
  uint32_t* p_render_pos = p_render->p_render_pos;
  struct render_character_2MHz* p_value =
      &p_render->render_character_2MHz_black;

  (void) data;
  (void) is_odd_tick;

  if (p_render->vert_beam_pos & 1) {
    uint32_t* p_next_render_pos = (p_render_pos + p_render->stride);
    *(struct render_character_2MHz*) p_next_render_pos = *p_value;
    render_check_cursor(p_render, p_next_render_pos, NULL, 1);
  } else {
    *(struct render_character_2MHz*) p_render_pos = *p_value;
    render_check_cursor(p_render, p_render_pos, NULL, 1);
  }
}

static uint32_t
render_get_display_color(struct render_struct* p_render, uint8_t bits) {
  uint32_t pixel;
  uint8_t physical_color = p_render->logical_to_physical_color[bits];
  physical_color ^= 0x7;
  pixel = p_render->palette[physical_color];
  if (physical_color & 0x8) {
    if (pixel == 0) {
      /* Only flash the color if we don't have a NuLA palette override. This
       * is approximately how NuLA works. Note that it is possible to have
       * flashing with NuLA colors, but that requires a write to the NuLA
       * control register which is not implemented.
       */
      physical_color &= 0x7;
      if (p_render->is_flash) {
        physical_color ^= 0x7;
      }
      pixel = p_render->palette[physical_color];
    }
  }

  return pixel;
}

static inline void
render_set_table_pixel(struct render_character_2MHz* p_character,
                       uint32_t index,
                       uint32_t argb) {
  if (BEEBJIT_RENDER_PACKED4) {
    uint32_t shift = (index * 4);
    uint32_t word = p_character->host_pixels[0];
    word &= ~(0xFu << shift);
    word |= ((render_pixel_from_argb(argb) & 0xF) << shift);
    p_character->host_pixels[0] = word;
  } else {
    p_character->host_pixels[index] = argb;
  }
}

static void
render_generate_1MHz_table(struct render_struct* p_render,
                           struct render_table_1MHz* p_table,
                           uint32_t num_pixels) {
  uint32_t i;
  uint32_t j;

  uint32_t pixel_stride = (16 / num_pixels);
  uint32_t palette_index = 0;
  uint32_t pixel_value = 0;

  for (i = 0; i < 256; ++i) {
    struct render_character_1MHz* p_character = &p_table->values[i];
    uint8_t shift_register = i;
    for (j = 0; j < 16; ++j) {
      if ((j % pixel_stride) == 0) {
        palette_index = (((shift_register & 0x02) >> 1) |
                         ((shift_register & 0x08) >> 2) |
                         ((shift_register & 0x20) >> 3) |
                         ((shift_register & 0x80) >> 4));
        pixel_value = render_get_display_color(p_render, palette_index);
        shift_register <<= 1;
        shift_register |= 1;
      }
      render_set_table_pixel(((j < 8) ? &p_character->first :
                                        &p_character->second),
                             (j & 7),
                             pixel_value);
    }
  }
}

static void
render_generate_2MHz_table(struct render_struct* p_render,
                           struct render_table_2MHz* p_table,
                           uint32_t num_pixels) {
  uint32_t i;
  uint32_t j;

  uint32_t pixel_stride = (8 / num_pixels);
  uint32_t palette_index = 0;
  uint32_t pixel_value = 0;

  for (i = 0; i < 256; ++i) {
    struct render_character_2MHz* p_character = &p_table->values[i];
    uint8_t shift_register = i;
    for (j = 0; j < 8; ++j) {
      if ((j % pixel_stride) == 0) {
        palette_index = (((shift_register & 0x02) >> 1) |
                         ((shift_register & 0x08) >> 2) |
                         ((shift_register & 0x20) >> 3) |
                         ((shift_register & 0x80) >> 4));
        pixel_value = render_get_display_color(p_render, palette_index);
        shift_register <<= 1;
        shift_register |= 1;
      }
      render_set_table_pixel(p_character, j, pixel_value);
    }
  }
}

#if BEEBJIT_RENDER_PACKED4
static void
render_generate_packed_table(struct render_struct* p_render,
                             uint32_t* p_out_words,
                             uint32_t words_per_entry,
                             uint32_t num_pixels) {
  uint32_t i;
  uint32_t num_words = (256 * words_per_entry);
  uint32_t* p_index_words =
      &p_render->packed_index_words[p_render->render_mode][0];
  uint8_t codes[16];
  uint8_t pair_codes[256];

  if (!(p_render->packed_index_built & (1u << p_render->render_mode))) {
    /* As render_generate_*_table(), but logical indices, not colors. */
    uint32_t pixels_per_entry = (8 * words_per_entry);
    uint32_t pixel_stride = (pixels_per_entry / num_pixels);
    uint16_t used = 0;
    for (i = 0; i < 256; ++i) {
      uint32_t j;
      uint8_t shift_register = i;
      uint32_t palette_index = 0;
      for (j = 0; j < words_per_entry; ++j) {
        p_index_words[(i * words_per_entry) + j] = 0;
      }
      for (j = 0; j < pixels_per_entry; ++j) {
        if ((j % pixel_stride) == 0) {
          palette_index = (((shift_register & 0x02) >> 1) |
                           ((shift_register & 0x08) >> 2) |
                           ((shift_register & 0x20) >> 3) |
                           ((shift_register & 0x80) >> 4));
          shift_register <<= 1;
          shift_register |= 1;
        }
        p_index_words[(i * words_per_entry) + (j / 8)] |=
            (palette_index << ((j & 7) * 4));
        used |= (1u << palette_index);
      }
    }
    p_render->packed_used[p_render->render_mode] = used;
    p_render->packed_index_built |= (1u << p_render->render_mode);
  }

  {
    uint16_t used = p_render->packed_used[p_render->render_mode];
    const uint8_t* p_old = &p_render->packed_codes[p_render->render_mode][0];
    int is_same = !!(p_render->packed_codes_valid &
                     (1u << p_render->render_mode));
    for (i = 0; i < 16; ++i) {
      /* Unused colors are left as black: they never show. */
      codes[i] = 0;
      if (used & (1u << i)) {
        codes[i] = (render_pixel_from_argb(
                        render_get_display_color(p_render, i)) & 0xF);
      }
      if (codes[i] != p_old[i]) {
        is_same = 0;
      }
    }
    if (is_same) {
      /* The table already holds these colors. */
      return;
    }
  }
  (void) memcpy(&p_render->packed_codes[p_render->render_mode][0], codes, 16);
  p_render->packed_codes_valid |= (1u << p_render->render_mode);
  for (i = 0; i < 256; ++i) {
    pair_codes[i] = (codes[i & 0xF] | (codes[i >> 4] << 4));
  }
  for (i = 0; i < num_words; ++i) {
    uint32_t w = p_index_words[i];
    p_out_words[i] = (pair_codes[w & 0xFF] |
                      (pair_codes[(w >> 8) & 0xFF] << 8) |
                      (pair_codes[(w >> 16) & 0xFF] << 16) |
                      ((uint32_t) pair_codes[w >> 24] << 24));
  }
}

static int
render_check_packed_table(struct render_struct* p_render) {
  int mode = p_render->render_mode;
  uint32_t* p_words;
  uint32_t words_per_entry = 1;
  uint32_t num_pixels;

  if (p_render->render_tables_built & (1 << mode)) {
    return 0;
  }
  switch (mode) {
  case k_render_mode0:
    p_words = &p_render->render_table_mode0.values[0].host_pixels[0];
    num_pixels = 8;
    break;
  case k_render_mode1:
    p_words = &p_render->render_table_mode1.values[0].host_pixels[0];
    num_pixels = 4;
    break;
  case k_render_mode2:
    p_words = &p_render->render_table_mode2.values[0].host_pixels[0];
    num_pixels = 2;
    break;
  case k_render_mode2_10:
    p_words = &p_render->render_table_mode2_10.values[0].host_pixels[0];
    num_pixels = 1;
    break;
  case k_render_mode4_80:
    p_words = &p_render->render_table_mode4_80.values[0].first.host_pixels[0];
    words_per_entry = 2;
    num_pixels = 16;
    break;
  case k_render_mode4:
    p_words = &p_render->render_table_mode4.values[0].first.host_pixels[0];
    words_per_entry = 2;
    num_pixels = 8;
    break;
  case k_render_mode5:
    p_words = &p_render->render_table_mode5.values[0].first.host_pixels[0];
    words_per_entry = 2;
    num_pixels = 4;
    break;
  case k_render_mode8:
    p_words = &p_render->render_table_mode8.values[0].first.host_pixels[0];
    words_per_entry = 2;
    num_pixels = 2;
    break;
  default:
    return 0;
  }
  render_generate_packed_table(p_render, p_words, words_per_entry, num_pixels);
  p_render->render_tables_built |= (1 << mode);
  return 1;
}
#endif

static void
render_check_2MHz_render_table(struct render_struct* p_render) {
  int mode = p_render->render_mode;

  /* Packed: built on first use, render_check_table_built(). */
  if (!BEEBJIT_RENDER_PACKED4 &&
      !(p_render->render_tables_built & (1 << mode))) {
    switch (mode) {
    case k_render_mode0:
      render_generate_2MHz_table(p_render, &p_render->render_table_mode0, 8);
      break;
    case k_render_mode1:
      render_generate_2MHz_table(p_render, &p_render->render_table_mode1, 4);
      break;
    case k_render_mode2:
      render_generate_2MHz_table(p_render, &p_render->render_table_mode2, 2);
      break;
    case k_render_mode2_10:
      render_generate_2MHz_table(p_render, &p_render->render_table_mode2_10, 1);
      break;
    default:
      assert(0);
      break;
    }
    p_render->render_tables_built |= (1 << mode);
  }

  switch (mode) {
  case k_render_mode0:
    p_render->p_render_table_2MHz = &p_render->render_table_mode0;
    break;
  case k_render_mode1:
    p_render->p_render_table_2MHz = &p_render->render_table_mode1;
    break;
  case k_render_mode2:
    p_render->p_render_table_2MHz = &p_render->render_table_mode2;
    break;
  case k_render_mode2_10:
    p_render->p_render_table_2MHz = &p_render->render_table_mode2_10;
    break;
  default:
    assert(0);
    break;
  }
}

static void
render_check_1MHz_render_table(struct render_struct* p_render) {
  int mode = p_render->render_mode;

  /* Packed: built on first use, render_check_table_built(). */
  if (!BEEBJIT_RENDER_PACKED4 &&
      !(p_render->render_tables_built & (1 << mode))) {
    switch (mode) {
    case k_render_mode4_80:
      render_generate_1MHz_table(p_render,
                                 &p_render->render_table_mode4_80,
                                 16);
      break;
    case k_render_mode4:
      render_generate_1MHz_table(p_render, &p_render->render_table_mode4, 8);
      break;
    case k_render_mode5:
      render_generate_1MHz_table(p_render, &p_render->render_table_mode5, 4);
      break;
    case k_render_mode8:
      render_generate_1MHz_table(p_render, &p_render->render_table_mode8, 2);
      break;
    default:
      assert(0);
      break;
    }
    p_render->render_tables_built |= (1 << mode);
  }

  switch (mode) {
  case k_render_mode4_80:
    p_render->p_render_table_1MHz = &p_render->render_table_mode4_80;
    break;
  case k_render_mode4:
    p_render->p_render_table_1MHz = &p_render->render_table_mode4;
    break;
  case k_render_mode5:
    p_render->p_render_table_1MHz = &p_render->render_table_mode5;
    break;
  case k_render_mode8:
    p_render->p_render_table_1MHz = &p_render->render_table_mode8;
    break;
  default:
    assert(0);
    break;
  }
}

static inline void
render_select_render_func(struct render_struct* p_render) {
  if (p_render->render_mode == k_render_mode7) {
    p_render->p_selected_render_func = p_render->p_render_func;
  } else if (p_render->is_dispen && !(p_render->row_address & 0x08)) {
    p_render->p_selected_render_func = p_render->p_render_func;
  } else {
    p_render->p_selected_render_func = p_render->p_render_blank_func;
  }
}

void
render_set_mode(struct render_struct* p_render,
                int clock_speed,
                int chars_per_line,
                int is_teletext) {
  if ((clock_speed != p_render->is_clock_2MHz) ||
      (chars_per_line != p_render->chars_per_line)) {
    /* Any change other than flipping teletext means we need to rebuild all
     * the pixel render tables.
     */
    p_render->render_tables_built = 0;
  }

  p_render->is_clock_2MHz = clock_speed;
  p_render->chars_per_line = chars_per_line;
  p_render->is_teletext = is_teletext;

  p_render->needs_render_mode_recalc = 1;
}

void
render_set_flash(struct render_struct* p_render, int is_flash) {
  uint32_t i;

  if (p_render->is_flash == is_flash) {
    return;
  }

  for (i = 0; i < 16; ++i) {
    uint8_t color = p_render->logical_to_physical_color[i];
    if (color & 0x8) {
      p_render->render_tables_built = 0;
      break;
    }
  }

  p_render->is_flash = is_flash;
}

void
render_set_physical_color(struct render_struct* p_render,
                          uint8_t logical_color,
                          uint8_t physical_color) {
  p_render->logical_to_physical_color[logical_color] = physical_color;
  p_render->render_tables_built = 0;
}

void
render_set_palette(struct render_struct* p_render,
                   uint8_t index,
                   uint32_t rgba) {
  p_render->palette[index] = rgba;
  p_render->render_tables_built = 0;
}

void
render_set_cursor_segments(struct render_struct* p_render,
                           int s0,
                           int s1,
                           int s2,
                           int s3) {
  p_render->cursor_segments[0] = s0;
  p_render->cursor_segments[1] = s1;
  p_render->cursor_segments[2] = s2;
  p_render->cursor_segments[3] = s3;
}

void
render_set_DISPEN(struct render_struct* p_render, int is_enabled) {
  p_render->is_dispen = is_enabled;
  render_select_render_func(p_render);
}

void
render_set_RA(struct render_struct* p_render, uint32_t row_address) {
  p_render->row_address = row_address;

  render_select_render_func(p_render);
}

static void
render_check_mode_recalc(struct render_struct* p_render) {
  if (!p_render->needs_render_mode_recalc) {
    return;
  }

  if (p_render->is_teletext) {
    /* For teletext rendeing, we use the same function for data or blank,
     * because the teletext display chain does its own display enable handling.
     */
    if (p_render->do_deinterlace_teletext) {
      p_render->p_render_func = render_function_teletext_deinterlaced;
      p_render->p_render_blank_func = NULL;
    } else {
      p_render->p_render_func = render_function_teletext_interlaced;
      p_render->p_render_blank_func = NULL;
    }
    p_render->render_mode = k_render_mode7;
  } else {
    if (p_render->is_clock_2MHz == 1) {
      /* 2MHz. */
      if (p_render->do_deinterlace_bitmap) {
        p_render->p_render_func = render_function_2MHz_data_deinterlaced;
        p_render->p_render_blank_func = render_function_2MHz_blank_deinterlaced;
      } else {
        p_render->p_render_func = render_function_2MHz_data_interlaced;
        p_render->p_render_blank_func = render_function_2MHz_blank_interlaced;
      }
      switch (p_render->chars_per_line) {
      case 0:
        p_render->render_mode = k_render_mode2_10;
        break;
      case 1:
        p_render->render_mode = k_render_mode2;
        break;
      case 2:
        p_render->render_mode = k_render_mode1;
        break;
      case 3:
        p_render->render_mode = k_render_mode0;
        break;
      default:
        assert(0);
        break;
      }
    } else {
      /* 1MHz. */
      if (p_render->do_deinterlace_bitmap) {
        p_render->p_render_func = render_function_1MHz_data_deinterlaced;
        p_render->p_render_blank_func = render_function_1MHz_blank_deinterlaced;
      } else {
        p_render->p_render_func = render_function_1MHz_data_interlaced;
        p_render->p_render_blank_func = render_function_1MHz_blank_interlaced;
      }
      switch (p_render->chars_per_line) {
      case 0:
        p_render->render_mode = k_render_mode8;
        break;
      case 1:
        p_render->render_mode = k_render_mode5;
        break;
      case 2:
        p_render->render_mode = k_render_mode4;
        break;
      case 3:
        p_render->render_mode = k_render_mode4_80;
        break;
      default:
        assert(0);
        break;
      }
    }
  }

  render_select_render_func(p_render);

  /* Changing 1MHz <-> 2MHz changes the size of the pixel blocks we write, and
   * therefore the bounds.
   */
  render_reset_render_pos(p_render);
}

void
render_prepare(struct render_struct* p_render) {
  render_check_mode_recalc(p_render);
  if (p_render->render_mode == k_render_mode7) {
    /* Nothing to do. */
  } else if (p_render->is_clock_2MHz) {
    render_check_2MHz_render_table(p_render);
  } else {
    render_check_1MHz_render_table(p_render);
  }
}

void
render_render(struct render_struct* p_render, uint8_t data, int is_odd_tick) {
  p_render->horiz_beam_pos += 8;

  if (p_render->p_render_pos <= p_render->p_render_pos_row_max) {

    p_render->p_selected_render_func(p_render, data, is_odd_tick);

    p_render->p_render_pos += k_render_words_per_8_pixels;
    if (p_render->is_row_mode) {
      render_mark_dirty(p_render,
                        (p_render->p_render_pos - k_render_words_per_8_pixels),
                        p_render->p_render_pos);
    }
  } else if ((p_render->horiz_beam_pos & ~7) ==
             p_render->horiz_beam_window_start_pos) {
    render_reset_render_pos(p_render);
  } else if (p_render->horiz_beam_pos >= 1536) {
    render_hsync(p_render, 0);
  }
}

uint32_t*
render_teletext_run_start(struct render_struct* p_render, uint32_t calls) {
  /* calls render_render() calls of deinterlaced teletext, all inside the
   * window, in row mode, with no cursor: the caller draws the first row of
   * each pair itself (teletext_render_first_of_pair(); the row mode flush
   * only hands over the first row when deinterlacing).
   */
  if ((p_render->render_mode != k_render_mode7) ||
      !p_render->is_row_mode ||
      (p_render->p_selected_render_func !=
       render_function_teletext_deinterlaced) ||
      (p_render->cursor_segment_index != -1) ||
      (calls == 0) ||
      (p_render->p_render_pos < p_render->p_render_pos_row) ||
      ((p_render->p_render_pos +
        ((calls - 1) * k_render_words_per_8_pixels)) >
       p_render->p_render_pos_row_max)) {
    return NULL;
  }
  return p_render->p_render_pos;
}

void
render_teletext_run_end(struct render_struct* p_render, uint32_t calls) {
  uint32_t* p_start = p_render->p_render_pos;
  p_render->p_render_pos += (calls * k_render_words_per_8_pixels);
  p_render->horiz_beam_pos += (calls * 8);
  render_mark_dirty(p_render, p_start, p_render->p_render_pos);
}

uint32_t
render_get_bitmap_run_capacity(struct render_struct* p_render) {
  if ((p_render->render_mode == k_render_mode7) ||
      (p_render->p_selected_render_func != p_render->p_render_func) ||
      (p_render->cursor_segment_index != -1) ||
      (p_render->p_render_pos > p_render->p_render_pos_row_max) ||
      (p_render->p_render_pos < p_render->p_render_pos_row)) {
    return 0;
  }
  return (((p_render->p_render_pos_row_max - p_render->p_render_pos) /
           k_render_words_per_8_pixels) + 1);
}

void
render_render_bitmap_run(struct render_struct* p_render,
                         const uint8_t* p_data,
                         uint32_t count) {
  uint32_t i;
  uint32_t* p_pos = p_render->p_render_pos;
  uint32_t stride = p_render->stride;
  /* Which rows of the pair get written: deinterlaced both, else the one for
   * the current field.
   */
  int do_row0 = 1;
  int do_row1 = 0;

  if (p_render->do_deinterlace_bitmap) {
    do_row1 = 1;
  } else if (p_render->vert_beam_pos & 1) {
    do_row0 = 0;
    do_row1 = 1;
  }

  render_check_table_built(p_render);
#if BEEBJIT_RENDER_PACKED4
  if (do_row0 != do_row1) {
    /* One row of the pair (8 pixels a word): the row picked once, two words
     * (1MHz) or one (2MHz) a byte.
     */
    uint32_t* p_dst = (do_row0 ? p_pos : (p_pos + stride));
    if (p_render->is_clock_2MHz) {
      const struct render_character_2MHz* p_values =
          &p_render->p_render_table_2MHz->values[0];
      for (i = 0; i < count; ++i) {
        p_dst[i] = p_values[p_data[i]].host_pixels[0];
      }
      p_pos += count;
      p_render->horiz_beam_pos += (count * 8);
    } else {
      const struct render_character_1MHz* p_values =
          &p_render->p_render_table_1MHz->values[0];
      for (i = 0; i < count; ++i) {
        const struct render_character_1MHz* p_value = &p_values[p_data[i]];
        uint32_t w0 = p_value->first.host_pixels[0];
        uint32_t w1 = p_value->second.host_pixels[0];
        p_dst[0] = w0;
        p_dst[1] = w1;
        p_dst += 2;
      }
      p_pos += (2 * count);
      p_render->horiz_beam_pos += (count * 16);
    }
    if (p_render->is_row_mode) {
      render_mark_dirty(p_render, p_render->p_render_pos, p_pos);
    }
    p_render->p_render_pos = p_pos;
    return;
  }
#endif
  if (p_render->is_clock_2MHz) {
    struct render_table_2MHz* p_table = p_render->p_render_table_2MHz;
    for (i = 0; i < count; ++i) {
      struct render_character_2MHz* p_value = &p_table->values[p_data[i]];
      if (do_row0) {
        *(struct render_character_2MHz*) p_pos = *p_value;
      }
      if (do_row1) {
        *(struct render_character_2MHz*) (p_pos + stride) = *p_value;
      }
      p_pos += k_render_words_per_8_pixels;
    }
    p_render->horiz_beam_pos += (count * 8);
  } else {
    struct render_table_1MHz* p_table = p_render->p_render_table_1MHz;
    for (i = 0; i < count; ++i) {
      struct render_character_1MHz* p_value = &p_table->values[p_data[i]];
      if (do_row0) {
        *(struct render_character_2MHz*) p_pos = p_value->first;
        *(struct render_character_2MHz*) (p_pos + k_render_words_per_8_pixels) =
            p_value->second;
      }
      if (do_row1) {
        *(struct render_character_2MHz*) (p_pos + stride) = p_value->first;
        *(struct render_character_2MHz*) (p_pos + stride +
                                          k_render_words_per_8_pixels) =
            p_value->second;
      }
      p_pos += (2 * k_render_words_per_8_pixels);
    }
    p_render->horiz_beam_pos += (count * 16);
  }
  if (p_render->is_row_mode) {
    render_mark_dirty(p_render, p_render->p_render_pos, p_pos);
  }
  p_render->p_render_pos = p_pos;
}

uint32_t
render_get_blank_run_capacity(struct render_struct* p_render) {
  /* render_render_blank_run() steps through the window's edges and forced
   * hsyncs itself, so any number of calls goes.
   */
  if ((p_render->render_mode == k_render_mode7) ||
      (p_render->p_selected_render_func != p_render->p_render_blank_func) ||
      (p_render->cursor_segment_index != -1)) {
    return 0;
  }
  return 0x10000;
}

void
render_render_blank_run(struct render_struct* p_render, uint32_t count) {
  /* count render_render() calls with the blank function selected: inside the
   * window each draws black; outside, each only moves the beam, except the
   * one landing on the window's start (which enters it) or reaching 1536
   * (a forced hsync), done here one at a time as render_render() would.
   */
  uint32_t done = 0;
  while (count > 0) {
    uint32_t* p_pos = p_render->p_render_pos;
    if (p_pos <= p_render->p_render_pos_row_max) {
      uint32_t i;
      uint32_t n = ((uint32_t) ((p_render->p_render_pos_row_max - p_pos) /
                                k_render_words_per_8_pixels) + 1);
      if (n > count) {
        n = count;
      }
      i = 0;
      if (p_render->is_row_mode &&
          BEEBJIT_RENDER_PACKED4 &&
          (p_render->render_character_2MHz_black.host_pixels[0] ==
           render_pixel_from_argb(0xff000000))) {
        /* Black over black: only where either row of the pair may hold
         * something else does a blank need drawing, and that's a fill of the
         * rows the blank function would write (both deinterlaced, else the
         * field's).
         */
        int do_row0 = 1;
        int do_row1 = 0;
        uint32_t* p_from = p_render->p_row_dirty_start;
        uint32_t* p_to = p_render->p_row_dirty_end;
        uint32_t j;
        for (j = 0; j < 2; ++j) {
          if (p_render->row_uncleared_end[j] != 0) {
            uint32_t* p_s = (p_render->p_buffer +
                             p_render->row_uncleared_start[j]);
            uint32_t* p_e = (p_render->p_buffer +
                             p_render->row_uncleared_end[j]);
            if (p_s < p_from) {
              p_from = p_s;
            }
            if (p_e > p_to) {
              p_to = p_e;
            }
          }
        }
        if (p_render->do_deinterlace_bitmap) {
          do_row1 = 1;
        } else if (p_render->vert_beam_pos & 1) {
          do_row0 = 0;
          do_row1 = 1;
        }
        while (i < n) {
          uint32_t* p_pos_now = p_render->p_render_pos;
          if (((p_pos_now + k_render_words_per_8_pixels) <= p_from) ||
              (p_pos_now >= p_to)) {
            /* Already black: run to the next edge of the span. */
            uint32_t skip = (n - i);
            if ((p_pos_now + k_render_words_per_8_pixels) <= p_from) {
              uint32_t k = (uint32_t) ((p_from - p_pos_now) /
                                       k_render_words_per_8_pixels);
              if (k < skip) {
                skip = k;
              }
            }
            p_render->horiz_beam_pos += (8 * skip);
            p_render->p_render_pos += (k_render_words_per_8_pixels * skip);
            i += skip;
            continue;
          }
          {
            /* Inside: black up to the span's end (or the run's). */
            uint32_t k = (n - i);
            uint32_t to_end = (uint32_t) (((p_to - p_pos_now) +
                                           k_render_words_per_8_pixels - 1) /
                                          k_render_words_per_8_pixels);
            uint32_t words;
            if (to_end < k) {
              k = to_end;
            }
            words = (k * k_render_words_per_8_pixels);
            if (do_row0) {
              render_fill_black(p_pos_now, (p_pos_now + words));
            }
            if (do_row1) {
              render_fill_black((p_pos_now + p_render->stride),
                                (p_pos_now + p_render->stride + words));
            }
            p_render->horiz_beam_pos += (8 * k);
            p_render->p_render_pos += words;
            i += k;
          }
        }
      }
      for (; i < n; ++i) {
        p_render->horiz_beam_pos += 8;
        p_render->p_render_blank_func(p_render, 0, ((done + i) & 1));
        p_render->p_render_pos += k_render_words_per_8_pixels;
      }
      count -= n;
      done += n;
    } else {
      /* Outside: the next call that does more than move the beam. */
      int32_t beam = p_render->horiz_beam_pos;
      uint32_t calls = count;
      int32_t start = p_render->horiz_beam_window_start_pos;
      if (((beam + 8) & ~7) <= start) {
        uint32_t k = ((uint32_t) (start - ((beam + 8) & ~7)) / 8) + 1;
        if (k < calls) {
          calls = k;
        }
      }
      if ((beam + (int32_t) (8 * calls)) >= 1536) {
        uint32_t k = ((beam >= 1536) ? 1 : ((uint32_t) (1536 - beam + 7) / 8));
        if (k < calls) {
          calls = k;
        }
      }
      /* All but the last call only move the beam. */
      p_render->horiz_beam_pos += (8 * (calls - 1));
      render_render(p_render, 0, ((done + calls - 1) & 1));
      count -= calls;
      done += calls;
    }
  }
}

void
render_clear_buffer(struct render_struct* p_render) {
  uint32_t i;
  uint32_t size = (render_get_buffer_size(p_render) / 4);
  uint32_t* p_buf = p_render->p_buffer;

  for (i = 0; i < size; ++i) {
    /* Full alpha and black. */
    p_buf[i] = render_pixel_from_argb(0xff000000);
  }
}

void
render_process_full_buffer(struct render_struct* p_render) {
  uint32_t width;
  uint32_t line_size;
  uint32_t* p_buffer;
  int32_t line;   /* Must be signed. */
  int32_t column; /* Must be signed. */
  int32_t lines;
  uint32_t half_width;

  if (!p_render->is_double_size) {
    return;
  }

  width = p_render->width;
  line_size = (width * sizeof(uint32_t));
  p_buffer = p_render->p_buffer;
  lines = (p_render->height / 2);
  half_width = (width / 2);
  
  for (line = 0; line < lines; ++line) {
    for (column = (half_width - 1); column >= 0; --column) {
      p_buffer[column * 2] = p_buffer[column];
      p_buffer[(column * 2) + 1] = p_buffer[column];
    }
    p_buffer += width;
  }

  p_buffer = p_render->p_buffer;
  for (line = (lines - 1); line >= 0; --line) {
    uint32_t* p_buffer_src = (p_buffer + (line * width));
    uint32_t* p_buffer_dest = (p_buffer + (2 * (line * width)));
    if (line == 0) {
      /* Don't copy line 0 to line 2*0=0 (memcpy overlap = undefined) */
    } else {
      (void) memcpy(p_buffer_dest, p_buffer_src, line_size);
    }
    (void) memcpy((p_buffer_dest + width), p_buffer_src, line_size);
  }
}

void
render_hsync(struct render_struct* p_render, uint32_t hsync_pulse_ticks) {
  /* Another CRT + PAL mystery!
   * Fire Track manages to achieve a black and white display on a color TV by
   * having a very "late" HSYNC directly after the VSYNC. (Hold F5 as you
   * start the game!)
   * This somehow confuses the TV into ignoring the color signal.
   */
  if ((p_render->horiz_beam_pos >= 1536) && (p_render->vert_beam_pos <= 8)) {
    p_render->is_crt_grayscale_fakeout = 1;
  }
  /* A real CRT appears to sync to the middle of the hsync pulse?!! This
   * permits half-character horizontal scrolling.
   * Used by tricky's RallyX demo.
   * For now, handle just the special case.
   */
  p_render->horiz_beam_pos = 0;
  if (hsync_pulse_ticks & 1) {
    p_render->horiz_beam_pos = -4;
  }
  p_render->vert_beam_pos += 2;

  p_render->cursor_segment_index = -1;

  /* If the CRT beam gets too low with no vsync signal in sight, it will do
   * flyback anyway.
   */
  if (p_render->vert_beam_pos >= 768) {
    render_vsync(p_render);
  }

  render_reset_render_pos(p_render);
}

static void
render_convert_to_grayscale(struct render_struct* p_render) {
  uint32_t width;
  uint32_t height;
  uint32_t* p_buffer = p_render->p_buffer;

  if ((p_buffer == NULL) || p_render->is_row_mode || BEEBJIT_RENDER_PACKED4) {
    /* TODO: row mode could pass a grayscale flag to the row callback. */
    return;
  }

  for (height = 0; height < p_render->height; ++height) {
    for (width = 0; width < p_render->width; ++width) {
      uint32_t pixel = *p_buffer;
      uint8_t r = (pixel >> 16);
      uint8_t g = (pixel >> 8);
      uint8_t b = pixel;
      uint8_t grey;
      /* These constants are from: https://en.wikipedia.org/wiki/Grayscale
       * "Luma coding in video systems", for PAL.
       */
      r = (r * 0.29);
      g = (g * 0.58);
      b = (b * 0.11);
      grey = (r + g + b);
      pixel = (grey | (grey << 8) | (grey << 16));
      /* Merge alpha back in. */
      pixel |= 0xff000000;
      *p_buffer = pixel;

      p_buffer++;
    }
  }
}

void
render_vsync(struct render_struct* p_render) {
  /* Ignore any vsyncs that come in shortly after a previous one. A CRT is
   * unlikely to lock to those.
   * This situation should be very rare, but a prototype demo hit this
   * condition when triggering vsyncs in an unusual manner.
   */
  if (p_render->vert_beam_pos < 64) {
    /* Row mode: the rows went out as they were drawn, and the flyback is
     * the host's frame tick, which it gets for every vsync.
     */
    if (p_render->is_row_mode && p_render->p_flyback_callback) {
      p_render->p_flyback_callback(p_render->p_flyback_callback_object);
    }
    return;
  }

  if (p_render->p_flyback_callback) {
    if (p_render->is_crt_grayscale_fakeout) {
      render_convert_to_grayscale(p_render);
    }
    p_render->p_flyback_callback(p_render->p_flyback_callback_object);
  }

  p_render->vert_beam_pos = 0;
  p_render->is_crt_grayscale_fakeout = 0;

  if (p_render->horiz_beam_pos >= 512) {
    /* We're transitioning from the even to the odd interlace frame. */
    p_render->vert_beam_pos = -1;
  }
  render_reset_render_pos(p_render);
}

void
render_horiz_line(struct render_struct* p_render, uint32_t argb) {
  uint32_t i;

  if (p_render->p_render_pos_row >= p_render->p_buffer_end) {
    return;
  }

  /* Paint a red line to edge of canvas denote CRTC frame boundary. */
  for (i = 0; i < p_render->stride; ++i) {
    p_render->p_render_pos_row[i] = render_pixel_from_argb(argb);
  }
}

void
render_cursor(struct render_struct* p_render) {
  p_render->cursor_segment_index = 0;
}

void
render_set_beam_pos(struct render_struct* p_render,
                    int32_t horiz,
                    int32_t vert) {
  p_render->horiz_beam_pos = horiz;
  p_render->vert_beam_pos = vert;
  if (vert <= 0) {
    p_render->is_crt_grayscale_fakeout = 0;
  }
  render_reset_render_pos(p_render);
}
