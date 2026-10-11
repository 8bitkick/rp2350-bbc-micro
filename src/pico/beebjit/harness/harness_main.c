/*
 * Test harness: beebjit's core plus the embedded platform layer, built for
 * Cortex-M33 exactly as for the RP2350, run without the board: as a 32-bit
 * Linux process on an Arm64 host (a32/), bare metal on QEMU's mps2-an505 (qemu/)
 * or natively on the host for profiling (native_host.c).
 *
 *   harness [-cycles N] [-terminal] [-type text] [-type-after N] [-fast]
 *           [-keys frame:KEY,...]
 *           [-disc f.ssd] [-autoboot] [-rom slot file] [-os file]
 *           [-mode7] [-ppm out.ppm] [-log-allocs] [-opt flags] [-log flags]
 *           [-no-render | -null-rows] [-spin] [-fake-time] [-frame-stats N]
 *           [-profile file] [-frame-crc] [-jit]
 *
 * -mode7 prints the MODE 7 screen memory (&7C00) as text at exit; -ppm saves
 * the last frame of the Presto panel framebuffer (480 wide, one row per
 * scanline) as it would be shown. -type feeds text (with \n escapes) to the
 * BBC's RS423 input, starting after -type-after cycles (default 4000000); it
 * implies -terminal. -fake-time runs emulated time flat out (the clock only
 * moves when beebjit sleeps): automatic where there's no real clock (QEMU).
 * -frame-stats N reports the host's cycle/instruction count per emulated frame
 * from frame N on, where the host has a counter. -profile samples the PC
 * from that frame on (QEMU) and saves the samples to a file; see
 * harness/profile.py. -frame-crc prints a running CRC of the panel
 * framebuffer over every frame, to compare runs frame by frame.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "harness_host.h"

#include "../beebjit_embedded.h"
#include "cpu_driver.h"
#if BEEBJIT_ASM_THUMB2
#include "asm/thumb2/thumb2_defs.h"
#endif
#include "../beebjit_host.h"
#include "../beebjit_panel.h"
#include "../os_embedded.h"
#if BEEBJIT_RENDER_WIRE
#include "../beebjit_wire.h"
#endif

#include "bbc.h"
#include "keyboard.h"
#include "timing.h"
#include "video.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct bbc_struct* s_p_bbc;

/* ---- beebjit_host.h ---- */

static int s_fake_time;
static int s_spin;
static uint64_t s_fake_us;
static uint64_t s_slept_us;

uint64_t
beebjit_host_time_us(void) {
  if (s_fake_time) {
    return s_fake_us;
  }
  return harness_host_time_us();
}

/* -spin: busy-wait instead of sleeping, so the host CPU stays at full clock
 * and the busy fraction can be measured (as presto_beebjit's #stats does).
 */
void
beebjit_host_sleep_us(uint64_t us) {
  s_slept_us += us;
  if (s_fake_time) {
    s_fake_us += us;
  } else if (s_spin) {
    uint64_t end = (harness_host_time_us() + us);
    while (harness_host_time_us() < end) {
    }
  } else {
    harness_host_sleep_us(us);
  }
}

static const char* s_p_type;
static uint64_t s_type_after = 4000000;

int
beebjit_host_term_read(uint8_t* p_byte) {
  if (s_p_type != NULL) {
    uint64_t cycles;
    if (*s_p_type == '\0') {
      return 0;
    }
    cycles = timing_get_total_timer_ticks(bbc_get_timing(s_p_bbc));
    if (cycles < s_type_after) {
      return 0;
    }
    if ((s_p_type[0] == '\\') && (s_p_type[1] == 'n')) {
      *p_byte = '\r';
      s_p_type += 2;
    } else {
      *p_byte = (uint8_t) *s_p_type++;
    }
    return 1;
  }
  return harness_host_read_stdin(p_byte);
}

void
beebjit_host_term_write(uint8_t byte) {
  harness_host_write_stdout(byte);
}

void*
beebjit_host_fixed_region(void* p_addr, size_t size) {
  return harness_host_fixed_region(p_addr, size);
}

size_t
beebjit_host_big_region(uint8_t** p_base) {
  return harness_host_big_region(p_base);
}

/* ---- Frames ---- */

static uint32_t s_fb[k_beebjit_panel_rows][k_beebjit_panel_row_words];
static struct beebjit_panel s_panel;
static uint32_t s_frames;
static uint32_t s_stats_from_frame;
static int s_do_frame_stats;
static uint64_t s_last_frame_cycles;
static uint64_t s_frame_min = ~0ull;
static uint64_t s_frame_max;
static uint64_t s_frame_total;
#if BEEBJIT_RENDER_WIRE
/* The render wire runs synchronously here: time spent draining it is what
 * the second core would do.
 */
static uint32_t s_wire_buf[BEEBJIT_WIRE_WORDS];
static uint64_t s_wire_cycles;
static uint64_t s_wire_drain_start;
static int s_wire_in_drain;
static uint64_t s_wire_at_stats_start;
static uint64_t s_wire_at_last_frame;

static void
wire_drain_hook(int is_end) {
  uint64_t now = harness_host_cycles();
  if (!is_end) {
    s_wire_drain_start = now;
    s_wire_in_drain = 1;
  } else {
    s_wire_cycles += (now - s_wire_drain_start);
    s_wire_in_drain = 0;
  }
}

static uint64_t
wire_cycles_now(void) {
  uint64_t cycles = s_wire_cycles;
  if (s_wire_in_drain) {
    cycles += (harness_host_cycles() - s_wire_drain_start);
  }
  return cycles;
}
#endif
static uint32_t s_frame_count;
static const char* s_p_profile;
static int s_frame_skip;
static uint32_t s_fb_dump_frame;
static char s_opt_flags[256];
static int s_do_frame_crc;
static int s_do_frame_trace;
static uint32_t s_ram_dump_frame;
static uint32_t s_frame_crc;

/* -keys: BBC key presses by frame (of 40,000 cycles, BBC time), "frame:KEY,frame:KEY,...", each held for
 * k_key_hold_frames. KEY is SPACE, RETURN, ESCAPE, F0..F9, UP, DOWN, LEFT,
 * RIGHT, SHIFT, CTRL, or one character (letters as capitals).
 */
enum { k_max_key_events = 128, k_key_hold_frames = 5 };
static uint32_t s_key_frames[k_max_key_events];
static uint8_t s_key_done[k_max_key_events];
static uint8_t s_key_codes[k_max_key_events];
static uint32_t s_num_keys;

static int
key_code(const char* p_name, size_t len) {
  static const struct { const char* p_name; int code; } names[] = {
    { "SPACE", ' ' }, { "RETURN", k_keyboard_key_enter },
    { "ESCAPE", k_keyboard_key_escape }, { "UP", k_keyboard_key_arrow_up },
    { "DOWN", k_keyboard_key_arrow_down }, { "LEFT", k_keyboard_key_arrow_left },
    { "RIGHT", k_keyboard_key_arrow_right },
    { "SHIFT", k_keyboard_key_shift_left }, { "CTRL", k_keyboard_key_ctrl },
  };
  size_t i;
  for (i = 0; i < (sizeof(names) / sizeof(names[0])); ++i) {
    if ((strlen(names[i].p_name) == len) &&
        !strncmp(names[i].p_name, p_name, len)) {
      return names[i].code;
    }
  }
  if ((len == 2) && (p_name[0] == 'F') && (p_name[1] >= '0') &&
      (p_name[1] <= '9')) {
    return (k_keyboard_key_f0 + (p_name[1] - '0'));
  }
  if (len == 1) {
    return (uint8_t) p_name[0];
  }
  return -1;
}

static void
parse_keys(const char* p_spec) {
  while (*p_spec && (s_num_keys < k_max_key_events)) {
    char* p_end;
    const char* p_name;
    size_t len;
    int code;
    uint32_t frame = strtoul(p_spec, &p_end, 0);
    if (*p_end != ':') {
      break;
    }
    p_name = (p_end + 1);
    len = strcspn(p_name, ",");
    code = key_code(p_name, len);
    if (code < 0) {
      (void) printf("-keys: unknown key %.*s\n", (int) len, p_name);
      exit(1);
    }
    s_key_frames[s_num_keys] = frame;
    s_key_codes[s_num_keys] = (uint8_t) code;
    s_num_keys++;
    p_spec = (p_name + len);
    if (*p_spec == ',') {
      p_spec++;
    }
  }
}

static uint32_t s_key_timer_id;

/* The key events, at their exact cycles: a timer, restarted for each. */
static void
key_timer_fired(void* p) {
  struct keyboard_struct* p_keyboard = bbc_get_keyboard(s_p_bbc);
  struct timing_struct* p_timing = bbc_get_timing(s_p_bbc);
  uint64_t now = timing_get_total_timer_ticks(p_timing);
  int64_t next = -1;
  uint32_t i;
  (void) p;
  (void) timing_stop_timer(p_timing, s_key_timer_id);
  for (i = 0; i < s_num_keys; ++i) {
    uint64_t t_down = ((uint64_t) s_key_frames[i] * 40000);
    uint64_t t_up = (t_down + ((uint64_t) k_key_hold_frames * 40000));
    if (!(s_key_done[i] & 1)) {
      if (t_down <= now) {
        s_key_done[i] |= 1;
        keyboard_system_key_pressed(p_keyboard, s_key_codes[i]);
      } else if ((next < 0) || ((int64_t) t_down < next)) {
        next = (int64_t) t_down;
      }
    }
    if ((s_key_done[i] & 1) && !(s_key_done[i] & 2)) {
      if (t_up <= now) {
        s_key_done[i] |= 2;
        keyboard_system_key_released(p_keyboard, s_key_codes[i]);
      } else if ((next < 0) || ((int64_t) t_up < next)) {
        next = (int64_t) t_up;
      }
    }
  }
  if (next >= 0) {
    (void) timing_start_timer_with_value(p_timing, s_key_timer_id,
                                         (next - (int64_t) now));
  }
}

static void
keys_start(void) {
  struct timing_struct* p_timing = bbc_get_timing(s_p_bbc);
  if (s_num_keys == 0) {
    return;
  }
  s_key_timer_id = timing_register_timer(p_timing, "harness_keys",
                                         key_timer_fired, NULL);
  (void) timing_start_timer_with_value(p_timing, s_key_timer_id, 1);
}

static void
frame_callback(void* p, uint64_t cycles) {
  uint64_t now;
  (void) p;
  (void) cycles;
  s_frames++;
  if (s_num_keys != 0) {
    }
  if (s_do_frame_crc) {
    /* Not byte-at-a-time CRC32 (util_crc32_add would be slow under QEMU):
     * a word-wise FNV-style hash is enough to tell frames apart.
     */
    const uint32_t* p_words = &s_fb[0][0];
    uint32_t n = (sizeof(s_fb) / sizeof(uint32_t));
    uint32_t h = s_frame_crc;
    uint32_t j;
    for (j = 0; j < n; ++j) {
      h = ((h ^ p_words[j]) * 16777619u);
    }
    s_frame_crc = h;
  }
  if (s_do_frame_trace) {
    /* Per frame: the frame's own hash, and a hash of the 6502's RAM and
     * registers, to find where two runs (e.g. interpreter and JIT) part.
     */
    const uint32_t* p_words = &s_fb[0][0];
    const uint8_t* p_mem = bbc_get_mem_read(s_p_bbc);
    uint32_t n = (sizeof(s_fb) / sizeof(uint32_t));
    uint32_t hf = 2166136261u;
    uint32_t hm = 2166136261u;
    uint32_t j;
    for (j = 0; j < n; ++j) {
      hf = ((hf ^ p_words[j]) * 16777619u);
    }
    for (j = 0; j < 0x8000; ++j) {
      hm = ((hm ^ p_mem[j]) * 16777619u);
    }
    if (s_frames == s_ram_dump_frame) {
      for (j = 0; j < 0x8000; ++j) {
        (void) printf("%s%02x", ((j & 31) ? "" : "\nram "), p_mem[j]);
      }
      (void) printf("\n");
    }
    if (s_frames == s_fb_dump_frame) {
      /* The panel's rows, a line each. */
      for (j = 0; j < k_beebjit_panel_rows; ++j) {
        uint32_t k;
        (void) printf("fb %03lu", (unsigned long) j);
        for (k = 0; k < (k_beebjit_panel_row_words - 1); ++k) {
          (void) printf(" %08lx", (unsigned long) s_fb[j][k]);
        }
        (void) printf("\n");
      }
    }
    (void) printf("frame %lu cycles %llu fb %08lx ram %08lx hw %lu\n",
                  (unsigned long) s_frames,
                  (unsigned long long) cycles,
                  (unsigned long) hf,
                  (unsigned long) hm,
                  (unsigned long) video_get_num_hw_writes(
                      bbc_get_video(s_p_bbc)));
  }
  if (!s_do_frame_stats) {
    return;
  }
  now = harness_host_cycles();
  if ((s_frames == s_stats_from_frame) && (s_p_profile != NULL)) {
    harness_host_profile(1);
  }
#if BEEBJIT_RENDER_WIRE
  if (s_frames == s_stats_from_frame) {
    s_wire_at_stats_start = wire_cycles_now();
  }
  s_wire_at_last_frame = wire_cycles_now();
#endif
  if (s_frames > s_stats_from_frame) {
    uint64_t delta = (now - s_last_frame_cycles);
    if (delta < s_frame_min) {
      s_frame_min = delta;
    }
    if (delta > s_frame_max) {
      s_frame_max = delta;
    }
    s_frame_total += delta;
    s_frame_count++;
  }
  s_last_frame_cycles = now;
}

static void
null_row(void* p, uint32_t row, uint32_t* p_pixels) {
  (void) p;
  (void) row;
  (void) p_pixels;
}

static void
save_ppm(const char* p_name) {
  static const uint8_t k_rgb[8][3] = {
    {0, 0, 0}, {0, 0, 255}, {0, 255, 0}, {0, 255, 255},
    {255, 0, 0}, {255, 0, 255}, {255, 255, 0}, {255, 255, 255},
  };
  static uint8_t s_line[k_beebjit_panel_width * 3];
  uint32_t x;
  uint32_t y;
  FILE* p_file = fopen(p_name, "wb");
  if (p_file == NULL) {
    (void) printf("can't write %s\n", p_name);
    return;
  }
  (void) fprintf(p_file,
                 "P6 %d %d 255\n",
                 k_beebjit_panel_width,
                 k_beebjit_panel_rows);
  for (y = 0; y < k_beebjit_panel_rows; ++y) {
    for (x = 0; x < k_beebjit_panel_width; ++x) {
      uint32_t code = ((s_fb[y][x / 8] >> ((x % 8) * 4)) & 7);
      (void) memcpy(&s_line[x * 3], k_rgb[code], 3);
    }
    (void) fwrite(s_line, sizeof(s_line), 1, p_file);
  }
  (void) fclose(p_file);
}

static void
print_mode7(struct bbc_struct* p_bbc) {
  uint32_t x;
  uint32_t y;
  uint8_t* p_mem = bbc_get_mem_read(p_bbc);
  (void) printf("+----------------------------------------+\n");
  for (y = 0; y < 25; ++y) {
    (void) printf("|");
    for (x = 0; x < 40; ++x) {
      uint8_t c = (p_mem[0x7C00 + (y * 40) + x] & 0x7F);
      (void) printf("%c", ((c >= 32) && (c < 127)) ? c : ' ');
    }
    (void) printf("|\n");
  }
  (void) printf("+----------------------------------------+\n");
}

int
main(int argc, char** argv) {
  int i;
  uint64_t start_us;
  uint64_t elapsed_us;
  uint64_t start_cycles;
  uint64_t host_cycles;
  uint64_t cycles;
  size_t big_used;
  size_t big_peak;
  size_t small_total;
  struct beebjit_embedded_config config;
  int do_mode7 = 0;
  int render_mode = 2;
  const char* p_ppm = NULL;
  uint32_t num_discs = 0;
  const char* p_unit = harness_host_cycles_unit();

  (void) memset(&config, '\0', sizeof(config));
  config.p_os_rom = "roms/os/os12.rom";
  config.p_rom_names[15] = "roms/general/basic2.rom";
  config.p_rom_names[14] = "roms/general/dfs09.rom";
  config.p_opt_flags = "video:border-chars=0,video:no-deinterlace-bitmap";
  config.p_log_flags = "";

  for (i = 1; i < argc; ++i) {
    const char* p_arg = argv[i];
    int has_1 = ((i + 1) < argc);
    if (has_1 && !strcmp(p_arg, "-cycles")) {
      config.stop_cycles = strtoull(argv[++i], NULL, 0);
    } else if (has_1 && !strcmp(p_arg, "-os")) {
      config.p_os_rom = argv[++i];
    } else if (((i + 2) < argc) && !strcmp(p_arg, "-rom")) {
      int slot = (int) strtol(argv[i + 1], NULL, 16);
      config.p_rom_names[slot & 15] = argv[i + 2];
      i += 2;
    } else if (has_1 && !strcmp(p_arg, "-disc")) {
      if (num_discs < k_beebjit_embedded_max_discs) {
        config.p_disc_names[num_discs++] = argv[i + 1];
      }
      ++i;
    } else if (has_1 && !strcmp(p_arg, "-ppm")) {
      p_ppm = argv[++i];
    } else if (!strcmp(p_arg, "-jit")) {
      config.cpu_mode = k_cpu_mode_jit;
#if BEEBJIT_ASM_THUMB2
      if (harness_host_needs_code_flush()) {
        g_t2.p_flush_code = harness_host_flush_code;
      }
#endif
    } else if (has_1 && !strcmp(p_arg, "-opt")) {
      config.p_opt_flags = argv[++i];
    } else if (has_1 && !strcmp(p_arg, "-log")) {
      config.p_log_flags = argv[++i];
    } else if (has_1 && !strcmp(p_arg, "-type")) {
      s_p_type = argv[++i];
      config.terminal_flag = 1;
    } else if (has_1 && !strcmp(p_arg, "-keys")) {
      parse_keys(argv[++i]);
    } else if (has_1 && !strcmp(p_arg, "-type-after")) {
      s_type_after = strtoull(argv[++i], NULL, 0);
    } else if (has_1 && !strcmp(p_arg, "-frame-stats")) {
      s_do_frame_stats = 1;
      s_stats_from_frame = strtoul(argv[++i], NULL, 0);
    } else if (has_1 && !strcmp(p_arg, "-profile")) {
      s_p_profile = argv[++i];
    } else if (!strcmp(p_arg, "-frame-trace")) {
      s_do_frame_trace = 1;
    } else if (has_1 && !strcmp(p_arg, "-fb-dump-frame")) {
      s_do_frame_trace = 1;
      s_fb_dump_frame = strtoul(argv[++i], NULL, 0);
    } else if (has_1 && !strcmp(p_arg, "-ram-dump-frame")) {
      s_do_frame_trace = 1;
      s_ram_dump_frame = strtoul(argv[++i], NULL, 0);
    } else if (!strcmp(p_arg, "-frame-crc")) {
      s_do_frame_crc = 1;
      s_frame_crc = 2166136261u;
    } else if (!strcmp(p_arg, "-terminal")) {
      config.terminal_flag = 1;
    } else if (!strcmp(p_arg, "-fast")) {
      config.fast_flag = 1;
    } else if (!strcmp(p_arg, "-autoboot")) {
      config.autoboot_flag = 1;
    } else if (!strcmp(p_arg, "-spin")) {
      s_spin = 1;
    } else if (!strcmp(p_arg, "-fake-time")) {
      s_fake_time = 1;
    } else if (!strcmp(p_arg, "-frame-skip")) {
      s_frame_skip = 1;
    } else if (!strcmp(p_arg, "-no-render")) {
      render_mode = 0;
    } else if (!strcmp(p_arg, "-null-rows")) {
      render_mode = 1;
    } else if (!strcmp(p_arg, "-mode7")) {
      do_mode7 = 1;
    } else if (!strcmp(p_arg, "-log-allocs")) {
      beebjit_embedded_log_allocs = 1;
    } else {
      (void) printf("unknown option %s\n", p_arg);
      return 1;
    }
  }
  if (harness_host_time_us() == 0) {
    /* No real clock: run on emulated time. */
    s_fake_time = 1;
  }
  if ((s_p_profile != NULL) && !s_do_frame_stats) {
    s_do_frame_stats = 1;
    s_stats_from_frame = 100;
  }
  if (p_unit == NULL) {
    s_do_frame_stats = 0;
  }

  if (s_frame_skip) {
    (void) snprintf(s_opt_flags, sizeof(s_opt_flags), "%s,video:frame-skip",
                    config.p_opt_flags);
    config.p_opt_flags = s_opt_flags;
  }
  beebjit_panel_init(&s_panel, s_fb, k_beebjit_panel_rows);
  if (render_mode == 2) {
    config.p_row_callback = beebjit_panel_row;
    config.p_row_callback_object = &s_panel;
  } else if (render_mode == 1) {
    config.p_row_callback = null_row;
  }
  config.p_frame_callback = frame_callback;

#if BEEBJIT_RENDER_WIRE
  beebjit_wire_init(s_wire_buf, 1);
  g_beebjit_wire.p_wait_hook = wire_drain_hook;
#endif
  s_p_bbc = beebjit_embedded_create(&config);
  if (s_p_bbc == NULL) {
    (void) printf("create failed\n");
    return 1;
  }
  keys_start();
  beebjit_embedded_get_alloc_stats(&big_used, &big_peak, &small_total);
  (void) printf("memory after create: heap %u, small allocs %u, "
                "big region %u\n",
                (unsigned) harness_host_heap_used(),
                (unsigned) small_total,
                (unsigned) big_used);

  start_us = harness_host_time_us();
  start_cycles = harness_host_cycles();
  s_last_frame_cycles = start_cycles;
  (void) beebjit_embedded_run(s_p_bbc);
#if BEEBJIT_RENDER_WIRE
  /* Draw what's still queued. */
  beebjit_wire_sync();
#endif
  elapsed_us = (harness_host_time_us() - start_us);
  host_cycles = (harness_host_cycles() - start_cycles);
  cycles = timing_get_total_timer_ticks(bbc_get_timing(s_p_bbc));

  if (s_do_frame_trace) {
    /* The 6502's RAM at the end, at the same instant whatever was drawn. */
    const uint8_t* p_mem = bbc_get_mem_read(s_p_bbc);
    uint32_t hm = 2166136261u;
    uint32_t j;
    for (j = 0; j < 0x8000; ++j) {
      hm = ((hm ^ p_mem[j]) * 16777619u);
    }
    (void) printf("end cycles %llu ram %08lx\n",
                  (unsigned long long) cycles,
                  (unsigned long) hm);
  }
  (void) printf("\nran %llu cycles (%.2f s of BBC time), %u frames, %u rows\n",
                (unsigned long long) cycles,
                (cycles / 2e6),
                s_frames,
                s_panel.rows_written);
  if (elapsed_us > 0) {
    (void) printf("took %.3f s: %.1fx; busy %.1f%% (slept %.3f s)\n",
                  (elapsed_us / 1e6),
                  ((cycles / 2.0) / (double) elapsed_us),
                  (100.0 * (elapsed_us - (double) s_slept_us) / elapsed_us),
                  (s_slept_us / 1e6));
  }
  if (p_unit != NULL) {
    (void) printf("%llu %s in all: %.1f M per BBC second\n",
                  (unsigned long long) host_cycles,
                  p_unit,
                  ((host_cycles / 1e6) / (cycles / 2e6)));
  }
  if (s_do_frame_stats && (s_frame_count > 0)) {
    (void) printf("per frame (%u frames from frame %u): min %llu avg %llu "
                  "max %llu %s\n",
                  s_frame_count,
                  s_stats_from_frame,
                  (unsigned long long) s_frame_min,
                  (unsigned long long) (s_frame_total / s_frame_count),
                  (unsigned long long) s_frame_max,
                  p_unit);
  }
#if BEEBJIT_RENDER_WIRE
  if (p_unit != NULL) {
    (void) printf("render wire consumer: %llu %s in all",
                  (unsigned long long) s_wire_cycles,
                  p_unit);
    if (s_do_frame_stats && (s_frame_count > 0)) {
      uint64_t per = ((s_wire_at_last_frame - s_wire_at_stats_start) /
                      s_frame_count);
      (void) printf("; per frame: consumer %llu, producer %llu",
                    (unsigned long long) per,
                    (unsigned long long) ((s_frame_total / s_frame_count) -
                                          per));
    }
    (void) printf("\n");
  }
#endif
  if (s_do_frame_crc) {
    (void) printf("frame hash over %u frames: %08lx\n",
                  s_frames,
                  (unsigned long) s_frame_crc);
  }
  if (s_p_profile != NULL) {
    harness_host_profile(0);
    harness_host_profile_save(s_p_profile);
  }
  if (do_mode7) {
    print_mode7(s_p_bbc);
  }
  if (p_ppm != NULL) {
    save_ppm(p_ppm);
  }
  return 0;
}
