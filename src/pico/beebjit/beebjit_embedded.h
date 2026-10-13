/*
 * Starting and running beebjit on an embedded host: the part of upstream's
 * main.c that a machine without a command line, window or threads needs.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_EMBEDDED_H
#define BEEBJIT_EMBEDDED_H

#include <stdint.h>

struct bbc_struct;

enum {
  k_beebjit_embedded_max_discs = 2,
};

struct beebjit_embedded_config {
  /* File names (opened with fopen; the host provides the files). */
  const char* p_os_rom;
  const char* p_rom_names[16];
  int is_sideways_ram[16];
  const char* p_disc_names[k_beebjit_embedded_max_discs];

  /* As upstream's command line options. */
  int cpu_mode;           /* k_cpu_mode_*; 0 for the interpreter. */
  int fast_flag;
  int accurate_flag;
  int autoboot_flag;
  int terminal_flag;      /* MOS 1.2 talks over RS423 to the host terminal. */
  uint64_t stop_cycles;   /* Exit after this many 2MHz cycles (0: never). */
  const char* p_opt_flags;
  const char* p_log_flags;

  /* Rendering: if p_row_callback is set, the emulated CRT is rendered one
   * scanline pair at a time into an internal buffer (see
   * render_set_row_buffer()); each row is render_get_width() host pixels:
   * ARGB, or packed 4-bit codes with BEEBJIT_RENDER_PACKED4.
   */
  void (*p_row_callback)(void* p, uint32_t row, uint32_t* p_pixels);
  /* Two row pairs, alternating (render_set_row_buffer_other()): the row
   * handed to p_row_callback stays as it is until the next call.
   */
  int is_row_double_buffered;
  void* p_row_callback_object;
  /* Called at each vsync of the emulated CRT. */
  void (*p_frame_callback)(void* p, uint64_t cycles);
  void* p_frame_callback_object;
};

/* Builds the machine. Returns NULL on failure. */
struct bbc_struct* beebjit_embedded_create(
    const struct beebjit_embedded_config* p_config);

/* Runs it until it exits (stop_cycles, or never). Returns the run result. */
uint32_t beebjit_embedded_run(struct bbc_struct* p_bbc);

#endif
