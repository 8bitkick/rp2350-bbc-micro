#ifndef BEEBJIT_TELETEXT_H
#define BEEBJIT_TELETEXT_H

#include <stdint.h>

struct teletext_struct;

struct render_character_2MHz;
struct video_struct;

struct teletext_struct* teletext_create(void);
void teletext_destroy(struct teletext_struct* p_teletext);

void teletext_set_black_rgb(struct teletext_struct* p_teletext, uint32_t rgb);

/* Whether teletext_data(p, 0, 0) would change nothing. */
int teletext_is_idle(struct teletext_struct* p_teletext);
void teletext_data(struct teletext_struct* p_teletext,
                   uint8_t data,
                   int is_dispen);
void teletext_RA_ISV_changed(struct teletext_struct* p_teletext,
                             uint8_t ra,
                             int is_isv);
void teletext_VSYNC_changed(struct teletext_struct* p_teletext, int value);

/* A run of n characters, as the Presto's render wire draws them
 * (beebjit_wire.c): each through teletext_data() on its even tick (DISPEN at
 * tick i: 0 from dispen_off_from on, else pattern bit i while i < settle,
 * else dispen; p_data NULL is bitmap addressing, zeros with DISPEN low),
 * then the first row of its
 * deinterlaced pair into p_out, a word per 8 pixels (two a character at
 * 1MHz), or nothing drawn when p_out is NULL. Packed-4 renderers only. Keeps a cache of runs: a character row
 * sends the same bytes from the same state for each of its scanlines.
 */
void teletext_run(struct teletext_struct* p_teletext,
                  const uint8_t* p_data,
                  uint32_t n,
                  uint32_t dispen,
                  uint32_t dispen_settle,
                  uint32_t dispen_pattern,
                  uint32_t dispen_off_from,
                  int is_1MHz,
                  uint32_t ticks_odd,
                  uint32_t* p_out);

/* As teletext_render() with a second row, but only the first. */
void teletext_render_first_of_pair(struct teletext_struct* p_teletext,
                                   int is_odd_tick,
                                   struct render_character_2MHz* p_out);
void teletext_render(struct teletext_struct* p_teletext,
                     int is_odd_tick,
                     struct render_character_2MHz* p_out,
                     struct render_character_2MHz* p_next_out);

#endif /* BEEBJIT_TELETEXT_H */
