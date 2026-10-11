/*
 * The render wire's consumer, and the producer's slow paths. See
 * beebjit_wire.h.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#if BEEBJIT_RENDER_WIRE

#include "beebjit_wire.h"

#include "render.h"
#include "teletext.h"

#include <assert.h>
#include <stddef.h>

#ifndef BEEBJIT_WIRE_FUNC
#define BEEBJIT_WIRE_FUNC(x) x
#endif

struct beebjit_wire g_beebjit_wire;
uint32_t g_beebjit_wire_teletext_zeros;

/* Consumer state: what the renderer was last told about the clock. */
static int s_is_2MHz;

static inline void
beebjit_wire_wfe(void) {
#if defined(__arm__)
  __asm__ volatile ("wfe");
#endif
}

static inline void
beebjit_wire_sev(void) {
#if defined(__arm__)
  __asm__ volatile ("sev");
#endif
}

void
beebjit_wire_init(uint32_t* p_buf, int is_synchronous) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  p_wire->p_buf = p_buf;
  p_wire->head = 0;
  p_wire->published = 0;
  p_wire->tail = 0;
  p_wire->flybacks = 0;
  p_wire->flybacks_seen = 0;
  p_wire->is_synchronous = is_synchronous;
  p_wire->is_attached = 0;
  p_wire->is_draining = 0;
  g_beebjit_wire_teletext_zeros = 0;
  s_is_2MHz = 0;
}

static void
beebjit_wire_flyback(void* p) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  (void) p;
  if (p_wire->is_synchronous) {
    /* Same core, same moment in the picture: paint now. */
    if (p_wire->p_flyback_callback != NULL) {
      p_wire->p_flyback_callback(p_wire->p_flyback_object);
    }
    return;
  }
  p_wire->flybacks++;
}

void
beebjit_wire_attach(struct render_struct* p_render) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  p_wire->p_render = p_render;
  p_wire->p_teletext = render_get_teletext(p_render);
  render_set_flyback_callback(p_render, beebjit_wire_flyback, NULL);
  p_wire->is_attached = 1;
  /* Whatever video queued while being created. */
  beebjit_wire_publish();
  (void) beebjit_wire_drain();
}

static void
beebjit_wire_bitmap_run(struct render_struct* p_render,
                        const uint8_t* p_data,
                        uint32_t n) {
  /* As video.c would: runs as long as the renderer takes them, else a
   * render_render() a tick.
   */
  while (n > 0) {
    uint32_t capacity = render_get_bitmap_run_capacity(p_render);
    if (!s_is_2MHz) {
      capacity /= 2;
    }
    if (capacity > 0) {
      if (capacity > n) {
        capacity = n;
      }
      render_render_bitmap_run(p_render, p_data, capacity);
      p_data += capacity;
      n -= capacity;
    } else if (render_get_blank_run_capacity(p_render) > 0) {
      /* Blanked (RA >= 8 in a bitmap mode): every call draws black. */
      render_render_blank_run(p_render, (s_is_2MHz ? n : (2 * n)));
      n = 0;
    } else {
      render_render(p_render, p_data[0], 0);
      if (!s_is_2MHz) {
        render_render(p_render, p_data[0], 1);
      }
      p_data++;
      n--;
    }
  }
}

static void
beebjit_wire_blank_run(struct render_struct* p_render, uint32_t count) {
  uint32_t done = 0;
  while (count > 0) {
    uint32_t capacity = render_get_blank_run_capacity(p_render);
    if (capacity > 0) {
      if (capacity > count) {
        capacity = count;
      }
      render_render_blank_run(p_render, capacity);
      count -= capacity;
      done += capacity;
    } else {
      render_render(p_render, 0, (done & 1));
      count--;
      done++;
    }
  }
}

static void
beebjit_wire_fast_line(struct render_struct* p_render,
                       uint32_t arg,
                       const uint32_t* p) {
  /* As video_do_fast_line() renders directly. */
  uint32_t n = (arg & 0xFF);
  uint32_t mul = (((arg >> 8) & 1) ? 2 : 1);
  uint32_t w = p[0];
  uint32_t pre = (w & 0xFF);
  if (pre > 0) {
    render_set_DISPEN(p_render, 0);
    beebjit_wire_blank_run(p_render, (pre * mul));
  }
  if (n > 0) {
    render_set_DISPEN(p_render, 1);
    beebjit_wire_bitmap_run(p_render, (const uint8_t*) &p[1], n);
  }
  render_set_DISPEN(p_render, 0);
  beebjit_wire_blank_run(p_render, (((w >> 8) & 0xFF) * mul));
  if ((arg >> 9) & 1) {
    render_hsync(p_render, (w >> 24));
    beebjit_wire_blank_run(p_render, (((w >> 16) & 0xFF) * mul));
  }
}

static void
beebjit_wire_char_run(struct render_struct* p_render,
                      struct teletext_struct* p_teletext,
                      uint32_t arg,
                      const uint8_t* p_data) {
  /* As video_do_char_run(). */
  uint32_t n = (arg & 0xFF);
  uint32_t ticks = ((arg >> 8) & 1);
  int is_1MHz = ((arg >> 9) & 1);
  int is_teletext_addressed = ((arg >> 10) & 1);
  int dispen = ((arg >> 11) & 1);
  uint32_t i;

  if (n == 0) {
    n = 256;
  }
#if BEEBJIT_RENDER_PACKED4
  {
    /* Teletext on screen with no cursor: the glyphs straight into the row. */
    uint32_t calls = (n * (is_1MHz ? 2 : 1));
    uint32_t* p_pos = render_teletext_run_start(p_render, calls);
    if (p_pos != NULL) {
      for (i = 0; i < n; ++i) {
        uint8_t data = p_data[i];
        if (!(ticks & 1)) {
          if (is_teletext_addressed) {
            teletext_data(p_teletext, data, dispen);
          } else {
            teletext_data(p_teletext, 0, 0);
          }
        }
        teletext_render_first_of_pair(p_teletext,
                                      (ticks & 1),
                                      (struct render_character_2MHz*) p_pos);
        p_pos += k_render_words_per_8_pixels;
        ticks++;
        if (is_1MHz) {
          teletext_render_first_of_pair(
              p_teletext, 1, (struct render_character_2MHz*) p_pos);
          p_pos += k_render_words_per_8_pixels;
          ticks++;
        }
      }
      render_teletext_run_end(p_render, calls);
      return;
    }
  }
#endif
  for (i = 0; i < n; ++i) {
    uint8_t data = p_data[i];
    int is_odd_tick = (ticks & 1);
    if (!is_odd_tick) {
      if (is_teletext_addressed) {
        teletext_data(p_teletext, data, dispen);
      } else {
        teletext_data(p_teletext, 0, 0);
      }
    }
    render_render(p_render, data, is_odd_tick);
    ticks++;
    if (is_1MHz) {
      render_render(p_render, data, 1);
      ticks++;
    }
  }
}

uint32_t
BEEBJIT_WIRE_FUNC(beebjit_wire_drain)(void) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  struct render_struct* p_render = p_wire->p_render;
  struct teletext_struct* p_teletext = p_wire->p_teletext;
  uint32_t* p_buf = p_wire->p_buf;
  uint32_t tail = p_wire->tail;
  uint32_t head;
  uint32_t start = tail;

  if (!p_wire->is_attached || p_wire->is_draining) {
    /* (A flyback's paint queueing more would land here.) */
    assert(p_wire->is_attached);
    assert(!p_wire->is_draining);
    return 0;
  }
  p_wire->is_draining = 1;
  head = p_wire->published;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  while (tail != head) {
    uint32_t pos = (tail & (BEEBJIT_WIRE_WORDS - 1));
    uint32_t* p = &p_buf[pos];
    uint32_t word = p[0];
    uint32_t arg = (word >> 8);
    uint32_t len = 1;
    switch (word & 0xFF) {
    case k_wire_pad:
      len = (BEEBJIT_WIRE_WORDS - pos);
      break;
    case k_wire_render:
      render_render(p_render, (arg & 0xFF), ((arg >> 8) & 1));
      break;
    case k_wire_bitmap_run:
      beebjit_wire_bitmap_run(p_render, (const uint8_t*) &p[1], arg);
      len += ((arg + 3) / 4);
      break;
    case k_wire_blank_run:
      beebjit_wire_blank_run(p_render, arg);
      break;
    case k_wire_hsync:
      render_hsync(p_render, arg);
      break;
    case k_wire_vsync:
      render_vsync(p_render);
      break;
    case k_wire_dispen:
      render_set_DISPEN(p_render, arg);
      break;
    case k_wire_ra:
      render_set_RA(p_render, arg);
      break;
    case k_wire_mode:
      s_is_2MHz = (arg & 1);
      render_set_mode(p_render,
                      (arg & 0xF),
                      ((arg >> 4) & 0xF),
                      ((arg >> 8) & 1));
      break;
    case k_wire_flash:
      render_set_flash(p_render, arg);
      break;
    case k_wire_physical_color:
      render_set_physical_color(p_render, (arg & 0xFF), ((arg >> 8) & 0xFF));
      break;
    case k_wire_palette:
      render_set_palette(p_render, arg, p[1]);
      len = 2;
      break;
    case k_wire_cursor_segments:
      render_set_cursor_segments(p_render,
                                 (arg & 1),
                                 ((arg >> 1) & 1),
                                 ((arg >> 2) & 1),
                                 ((arg >> 3) & 1));
      break;
    case k_wire_cursor:
      render_cursor(p_render);
      break;
    case k_wire_prepare:
      render_prepare(p_render);
      break;
    case k_wire_beam_pos:
      render_set_beam_pos(p_render, (int32_t) p[1], (int32_t) p[2]);
      len = 3;
      break;
    case k_wire_horiz_line:
      render_horiz_line(p_render, p[1]);
      len = 2;
      break;
    case k_wire_power_on_reset:
      render_power_on_reset(p_render);
      break;
    case k_wire_teletext_data:
      teletext_data(p_teletext, (arg & 0xFF), ((arg >> 8) & 1));
      break;
    case k_wire_teletext_ra_isv:
      teletext_RA_ISV_changed(p_teletext, (arg & 0xFF), ((arg >> 8) & 1));
      break;
    case k_wire_teletext_vsync:
      teletext_VSYNC_changed(p_teletext, arg);
      break;
    case k_wire_frame_tick:
      beebjit_wire_flyback(NULL);
      break;
    case k_wire_fast_line_ra:
      teletext_RA_ISV_changed(p_teletext,
                              ((arg >> 10) & 0x1F),
                              ((arg >> 15) & 1));
      render_set_RA(p_render, ((arg >> 10) & 0x1F));
      /* Fall through. */
    case k_wire_fast_line:
      beebjit_wire_fast_line(p_render, arg, &p[1]);
      len += (1 + (((arg & 0xFF) + 3) / 4));
      break;
    case k_wire_char_run:
      beebjit_wire_char_run(p_render,
                            p_teletext,
                            arg,
                            (const uint8_t*) &p[1]);
      len += ((((arg & 0xFF) ? (arg & 0xFF) : 256) + 3) / 4);
      break;
    default:
      assert(0);
      break;
    }
    tail += len;
    /* Hand the space back as we go. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p_wire->tail = tail;
  }
  p_wire->is_draining = 0;
  if (!p_wire->is_synchronous) {
    beebjit_wire_sev();
  }
  return (tail - start);
}

void
beebjit_wire_consumer_loop(void) {
  while (1) {
    if (beebjit_wire_drain() == 0) {
      beebjit_wire_wfe();
    }
  }
}

void
beebjit_wire_wait(uint32_t words) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  beebjit_wire_publish();
  if (p_wire->is_synchronous) {
    if (p_wire->p_wait_hook != NULL) {
      p_wire->p_wait_hook(0);
    }
    (void) beebjit_wire_drain();
    if (p_wire->p_wait_hook != NULL) {
      p_wire->p_wait_hook(1);
    }
    /* Not attached yet: too much queued before the renderer exists. */
    assert((BEEBJIT_WIRE_WORDS - (p_wire->head - p_wire->tail)) >= words);
    return;
  }
  if (p_wire->p_wait_hook != NULL) {
    p_wire->p_wait_hook(0);
  }
  while ((BEEBJIT_WIRE_WORDS - (p_wire->head - p_wire->tail)) < words) {
    beebjit_wire_wfe();
  }
  if (p_wire->p_wait_hook != NULL) {
    p_wire->p_wait_hook(1);
  }
}

void
beebjit_wire_sync(void) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  beebjit_wire_publish();
  if (p_wire->is_synchronous) {
    if (p_wire->p_wait_hook != NULL) {
      p_wire->p_wait_hook(0);
    }
    (void) beebjit_wire_drain();
    if (p_wire->p_wait_hook != NULL) {
      p_wire->p_wait_hook(1);
    }
    return;
  }
  if (p_wire->p_wait_hook != NULL) {
    p_wire->p_wait_hook(0);
  }
  while (p_wire->tail != p_wire->head) {
    beebjit_wire_wfe();
  }
  if (p_wire->p_wait_hook != NULL) {
    p_wire->p_wait_hook(1);
  }
}

void
beebjit_wire_poll_flybacks(void) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  while (p_wire->flybacks_seen != p_wire->flybacks) {
    p_wire->flybacks_seen++;
    if (p_wire->p_flyback_callback != NULL) {
      p_wire->p_flyback_callback(p_wire->p_flyback_object);
    }
  }
}

#endif /* BEEBJIT_RENDER_WIRE */
