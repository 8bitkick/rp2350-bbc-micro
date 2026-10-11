/*
 * The render wire: beebjit's video (CRTC and ULA timing, on the emulating
 * core) hands its render and teletext calls to the renderer through a ring of
 * commands, so the drawing can run on the other core.
 *
 * video.c's calls are redirected here when it's built with
 * BEEBJIT_RENDER_WIRE (beebjit_wire_video.h). The consumer makes the very
 * same render_*() / teletext_*() calls in the same order, so the picture is
 * the same; it's just drawn later. Nothing the renderer does feeds back into
 * the emulation except:
 *   - how long a run render_render_bitmap_run() / render_render_blank_run()
 *     can take: the producer assumes any, and the consumer breaks a run up
 *     the way video.c's ticking would;
 *   - whether the teletext pipeline is idle: the producer assumes so in
 *     bitmap runs, and the consumer feeds it the zeros video.c would have;
 *   - flyback, which paints: the consumer counts flybacks and the producer
 *     passes them on to video (beebjit_wire_poll()); a synchronous wire (the
 *     test harness, one core) calls the flyback callback right away.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_WIRE_H
#define BEEBJIT_WIRE_H

#include <stdint.h>

struct render_struct;
struct teletext_struct;

enum {
  k_wire_pad = 0,
  k_wire_render,
  k_wire_bitmap_run,
  k_wire_blank_run,
  k_wire_hsync,
  k_wire_vsync,
  k_wire_dispen,
  k_wire_ra,
  k_wire_mode,
  k_wire_flash,
  k_wire_physical_color,
  k_wire_palette,
  k_wire_cursor_segments,
  k_wire_cursor,
  k_wire_prepare,
  k_wire_beam_pos,
  k_wire_horiz_line,
  k_wire_power_on_reset,
  k_wire_teletext_data,
  k_wire_teletext_ra_isv,
  k_wire_teletext_vsync,
  k_wire_char_run,
  /* A whole ordinary bitmap scanline (video_do_fast_line()): arg n shown
   * bytes, bit 8 1MHz, bit 9 hsync; then a word: blank characters before
   * the shown ones (skew), blank characters after them up to the hsync (or
   * the line's end), blank characters from the hsync on, hsync ticks (a byte
   * each); then the n bytes.
   */
  k_wire_fast_line,
  /* As k_wire_fast_line, after a new line's RA: arg bits 10-14 the external
   * RA, bit 15 teletext deinterlace (render_set_RA() and
   * teletext_RA_ISV_changed() first).
   */
  k_wire_fast_line_ra,
  /* A frame with nothing new to draw (video.c's frame skip): a flyback, so
   * the frame is painted (and counted) as every other.
   */
  k_wire_frame_tick,
};

/* Ring size in words; a power of 2. */
#ifndef BEEBJIT_WIRE_WORDS
#define BEEBJIT_WIRE_WORDS 8192
#endif

struct beebjit_wire {
  uint32_t* p_buf;
  /* Producer: next word to write (free running) and how much of it the
   * consumer may see.
   */
  uint32_t head;
  volatile uint32_t published;
  /* Consumer: next word to read (free running). */
  volatile uint32_t tail;
  /* Flybacks the consumer has seen, and the producer has passed on. */
  volatile uint32_t flybacks;
  uint32_t flybacks_seen;
  /* 1: no second core; the producer drains the ring itself when it's full
   * or when asked (beebjit_wire_sync()).
   */
  int is_synchronous;
  int is_attached;
  int is_draining;
  struct render_struct* p_render;
  struct teletext_struct* p_teletext;
  void (*p_flyback_callback)(void*);
  void* p_flyback_object;
  /* Called around the producer's waits for the consumer (synchronous wires:
   * around the drains), e.g. to time them.
   */
  void (*p_wait_hook)(int is_end);
};

extern struct beebjit_wire g_beebjit_wire;

/* Set up the ring (before the emulator's created: video.c talks to it from
 * the start).
 */
void beebjit_wire_init(uint32_t* p_buf, int is_synchronous);
/* Give the consumer its renderer (and its teletext chip), and run what's
 * queued so far (on this core).
 */
void beebjit_wire_attach(struct render_struct* p_render);
/* Consumer: run the commands published so far. Returns how many words. */
uint32_t beebjit_wire_drain(void);
/* Consumer: the second core's loop; never returns. */
void beebjit_wire_consumer_loop(void);
/* Producer: wait until the consumer has caught up (and, synchronous, catch it
 * up).
 */
void beebjit_wire_sync(void);

/* Producer side, used by beebjit_wire_video.h. */
void beebjit_wire_wait(uint32_t words);
void beebjit_wire_poll_flybacks(void);

static inline void
beebjit_wire_publish(void) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  p_wire->published = p_wire->head;
#if defined(__arm__) && !defined(BEEBJIT_WIRE_NO_SEV)
  if (!p_wire->is_synchronous) {
    __asm__ volatile ("sev");
  }
#endif
}

/* Room for a command of words words, contiguous in the ring. */
static inline uint32_t*
beebjit_wire_reserve(uint32_t words) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  uint32_t pos = (p_wire->head & (BEEBJIT_WIRE_WORDS - 1));
  uint32_t need = words;
  if ((pos + words) > BEEBJIT_WIRE_WORDS) {
    need += (BEEBJIT_WIRE_WORDS - pos);
  }
  if ((BEEBJIT_WIRE_WORDS - (p_wire->head - p_wire->tail)) < need) {
    beebjit_wire_wait(need);
  }
  if ((pos + words) > BEEBJIT_WIRE_WORDS) {
    /* Pad to the end; the consumer skips to the start. */
    p_wire->p_buf[pos] = k_wire_pad;
    p_wire->head += (BEEBJIT_WIRE_WORDS - pos);
    pos = 0;
  }
  return &p_wire->p_buf[pos];
}

static inline void
beebjit_wire_commit(uint32_t words) {
  struct beebjit_wire* p_wire = &g_beebjit_wire;
  p_wire->head += words;
  /* Let the consumer start on a long stretch before it's all queued. */
  if ((p_wire->head - p_wire->published) >= 256) {
    beebjit_wire_publish();
  }
}

static inline void
beebjit_wire_put1(uint32_t op, uint32_t arg) {
  uint32_t* p = beebjit_wire_reserve(1);
  p[0] = (op | (arg << 8));
  beebjit_wire_commit(1);
}

static inline void
beebjit_wire_put2(uint32_t op, uint32_t arg, uint32_t w1) {
  uint32_t* p = beebjit_wire_reserve(2);
  p[0] = (op | (arg << 8));
  p[1] = w1;
  beebjit_wire_commit(2);
}

/* A command followed by n bytes; returns where the bytes go (the caller
 * fills them, then calls beebjit_wire_commit_bytes()).
 */
static inline uint8_t*
beebjit_wire_reserve_bytes(uint32_t op, uint32_t arg, uint32_t n) {
  uint32_t* p = beebjit_wire_reserve(1 + ((n + 3) / 4));
  p[0] = (op | (arg << 8));
  return (uint8_t*) &p[1];
}

static inline void
beebjit_wire_commit_bytes(uint32_t n) {
  beebjit_wire_commit(1 + ((n + 3) / 4));
}

#endif /* BEEBJIT_WIRE_H */
