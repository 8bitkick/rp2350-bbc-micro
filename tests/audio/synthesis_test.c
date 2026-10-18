/* Exercise the actual SN generator and nonblocking submission, not hardware. */
#include <stdio.h>
#include <stdlib.h>
#include "../../src/beebjit/sound.c"
static uint64_t test_ticks;
static uint32_t written;
static int16_t captured[1024];
uint64_t timing_get_scaled_total_timer_ticks(struct timing_struct* p) {
  (void) p; return test_ticks;
}
void util_bail(const char* fmt, ...) {
  (void) fmt; abort();
}
void log_do_log_max_count(uint32_t* c, int m, int s, const char* fmt, ...) {
  (void) c; (void) m; (void) s; (void) fmt;
}
void os_sound_write(struct os_sound_struct* d, int16_t* frames, uint32_t n) {
  (void) d;
  assert(written + n <= 1024);
  memcpy(captured + written, frames, n * sizeof(*frames));
  written += n;
}
int main(void) {
  struct sound_struct s = {0};
  int16_t sn[8192], pcm[1024];
  s.synchronous = 1;
  s.p_driver = (struct os_sound_struct*) &s;
  s.p_sn_frames = sn;
  s.p_driver_frames = pcm;
  s.driver_buffer_size = 1024;
  s.driver_period_size = 256;
  s.sn_frames_per_driver_buffer_size = 8192;
  s.sn_frames_per_driver_frame = 8.0;
  /* A tone on channel zero, other channels silent. */
  s.volume_outputs[0] = 8191;
  for (unsigned i = 0; i < 4; ++i) {
    s.volume[i] = 15;
    s.period[i] = 100;
    s.counter[i] = 100;
  }
  s.volume[0] = 0;
  for (unsigned i = 0; i < 5; ++i) {
    test_ticks += 4000; /* 2 ms of BBC time -> 62.5 samples. */
    sound_tick_nonblocking(&s);
  }
  assert(written == 256);
  assert(s.driver_buffer_index == 56);
  int16_t lo = captured[0], hi = captured[0];
  for (unsigned i = 0; i < written; ++i) {
    if (captured[i] < lo) lo = captured[i];
    if (captured[i] > hi) hi = captured[i];
  }
  assert(lo == 0 && hi == 8191);
  sound_discard_pending(&s);
  assert(s.driver_buffer_index == 0 && s.sn_frames_filled == 0);
  assert(s.accumulated_count == 0 && s.prev_system_ticks == test_ticks);
  assert(s.period[0] == 100 && s.volume[0] == 0);
  /* A hard reset rewinds emulated time; discard must reset the baseline. */
  test_ticks = 0;
  sound_discard_pending(&s);
  assert(s.prev_system_ticks == 0);
  test_ticks = 4000;
  sound_tick_nonblocking(&s);
  assert(s.driver_buffer_index == 62);
  puts("PASS: SN tone, sample count, nonblocking submission and reset baseline");
  return 0;
}
