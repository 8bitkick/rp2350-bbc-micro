/*
 * Starting and running beebjit on an embedded host. Follows upstream's main.c
 * (beebjit_main()), minus the command line, window, sound and poller.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "beebjit_embedded.h"

#if BEEBJIT_RENDER_WIRE
#include "beebjit_wire.h"
#endif

#include "bbc.h"
#include "cpu_driver.h"
#include "log.h"
#include "os_channel.h"
#include "os_terminal.h"
#include "render.h"
#include "serial_ula.h"
#include "util.h"

#include "os_embedded.h"

#include <stdio.h>
#include <string.h>

static struct beebjit_embedded_config s_config;
/* bbc.c keeps using this (Master: HAZEL), so it lives as long as the machine;
 * it is rarely read, so it goes in the big region if there is one.
 */
static uint8_t* s_os_rom;
static intptr_t s_handle_channel_read_ui;
static uint32_t* s_p_row_buffer;

/* The "UI thread": runs as soon as the emulator posts a message. */
static void
beebjit_embedded_ui_handler(void* p) {
  struct bbc_message message;
  struct bbc_struct* p_bbc = (struct bbc_struct*) p;

  bbc_client_receive_message(p_bbc, &message);
  if (message.data[0] == k_message_exited) {
    return;
  }
  if (message.data[0] != k_message_vsync) {
    util_bail("unexpected message from BBC");
  }
  /* Row mode renders as it goes, so there is no frame to paint or clear. */
  if (s_config.p_frame_callback != NULL) {
    s_config.p_frame_callback(s_config.p_frame_callback_object,
                              message.data[3]);
  }
  if (message.data[4]) {
    message.data[0] = k_message_render_done;
    bbc_client_send_message(p_bbc, &message);
  }
}

#ifndef BEEBJIT_JIT_ENCODED_CALLBACKS
#define BEEBJIT_JIT_ENCODED_CALLBACKS 1
#endif

struct bbc_struct*
beebjit_embedded_create(const struct beebjit_embedded_config* p_config) {
  struct bbc_struct* p_bbc;
  struct render_struct* p_render;
  intptr_t handle_channel_write_bbc;
  intptr_t handle_channel_read_bbc;
  intptr_t handle_channel_write_ui;
  uint32_t i;
  int mode;
  size_t read_ret;
  int has_sideways_ram = 0;
  uint8_t* p_load_rom;
  static char s_opt_flags[512];

  s_config = *p_config;
  mode = s_config.cpu_mode;
  if (mode == 0) {
    mode = k_cpu_mode_interp;
  }
  if (mode == k_cpu_mode_jit) {
    /* Turn off the video memory sync: with it, beebjit keeps a non-fast
     * (accurate) machine in the interpreter, syncing every 6502 write to the
     * CRTC's reads, and the JIT would never run.
     * BEEBJIT_JIT_ENCODED_CALLBACKS=0: hardware register accesses from JIT
     * code go to the interpreter instead of being compiled as calls.
     */
    (void) snprintf(s_opt_flags,
                    sizeof(s_opt_flags),
                    "%s,%svideo:no-memory-sync",
                    (s_config.p_opt_flags ? s_config.p_opt_flags : ""),
                    (BEEBJIT_JIT_ENCODED_CALLBACKS ? "" :
                     "jit:no-encoded-callback,"));
    s_config.p_opt_flags = s_opt_flags;
  }

  s_os_rom = beebjit_embedded_malloc_big(k_bbc_rom_size);
  (void) memset(s_os_rom, '\0', k_bbc_rom_size);
  read_ret = util_file_read_fully(s_config.p_os_rom,
                                  s_os_rom,
                                  k_bbc_rom_size);
  if (read_ret != k_bbc_rom_size) {
    log_do_log(k_log_misc, k_log_error, "can't load OS ROM %s",
               s_config.p_os_rom);
    return NULL;
  }
  if (s_config.terminal_flag &&
      (memcmp(&s_os_rom[0x2825], "OS 1.2", 6) == 0)) {
    /* As upstream's -terminal: *FX2,1 (RS423 input), receive interrupts on
     * and *FX3,5 (screen and RS423 output) by default.
     */
    s_os_rom[0xD981 - 0xC000] = 1;
    s_os_rom[0xD990 - 0xC000] = 0x96;
    s_os_rom[0xD9BC - 0xC000] = 5;
  }

  for (i = 0; i < 16; ++i) {
    if (s_config.is_sideways_ram[i]) {
      has_sideways_ram = 1;
    }
  }

  p_bbc = bbc_create(mode,
                     0, /* is_master */
                     has_sideways_ram,
                     s_os_rom,
                     0, /* wd_1770_type: 8271 */
                     0, /* debug_flag */
                     0, /* run_flag */
                     0, /* print_flag */
                     s_config.fast_flag,
                     s_config.accurate_flag,
                     0, /* fasttape_flag */
                     0, /* test_map_flag */
                     (s_config.p_opt_flags ? s_config.p_opt_flags : ""),
                     (s_config.p_log_flags ? s_config.p_log_flags : ""));
  if (p_bbc == NULL) {
    return NULL;
  }

  for (i = 0; i < k_beebjit_embedded_max_discs; ++i) {
    if (s_config.p_disc_names[i] != NULL) {
      bbc_add_disc(p_bbc, s_config.p_disc_names[i], i, 0, 0, 0, 0, 0);
    }
  }

  if (s_config.autoboot_flag) {
    bbc_set_autoboot(p_bbc, 1);
  }
  if (s_config.stop_cycles != 0) {
    bbc_set_stop_cycles(p_bbc, s_config.stop_cycles);
  }

  /* The render buffer must be set before the power on reset, so that video
   * knows to render.
   */
  p_render = bbc_get_render(p_bbc);
#if BEEBJIT_RENDER_WIRE
  /* The host set the wire up (beebjit_wire_init()) before this. */
  beebjit_wire_attach(p_render);
#endif
  if (s_config.p_row_callback != NULL) {
    s_p_row_buffer = util_mallocz(render_get_stride(p_render) * 2 *
                                  sizeof(uint32_t));
    render_set_row_buffer(p_render,
                          s_p_row_buffer,
                          s_config.p_row_callback,
                          s_config.p_row_callback_object);
    if (s_config.is_row_double_buffered) {
      render_set_row_buffer_other(
          p_render,
          util_mallocz(render_get_stride(p_render) * 2 * sizeof(uint32_t)));
    }
  }

  bbc_power_on_reset(p_bbc);

  /* Loaded through PSRAM: on the Presto a 16 KB SRAM buffer here was what
   * made the SRAM heap peak at boot, and the ROM is copied out of it once. */
  p_load_rom = beebjit_embedded_malloc_big(k_bbc_rom_size);
  for (i = 0; i < 16; ++i) {
    const char* p_rom_name = s_config.p_rom_names[i];
    if (p_rom_name != NULL) {
      (void) memset(p_load_rom, '\0', k_bbc_rom_size);
      (void) util_file_read_fully(p_rom_name, p_load_rom, k_bbc_rom_size);
      bbc_load_rom(p_bbc, i, p_load_rom);
    }
    if (s_config.is_sideways_ram[i]) {
      bbc_make_sideways_ram(p_bbc, i);
    }
  }
  util_free(p_load_rom);

  if (s_config.terminal_flag) {
    struct serial_ula_struct* p_serial_ula = bbc_get_serial_ula(p_bbc);
    intptr_t stdin_handle = os_terminal_get_stdin_handle();
    intptr_t stdout_handle = os_terminal_get_stdout_handle();
    os_terminal_setup(stdin_handle);
    serial_ula_set_io_handles(p_serial_ula, stdin_handle, stdout_handle);
  }

  os_channel_get_handles(&s_handle_channel_read_ui,
                         &handle_channel_write_bbc,
                         &handle_channel_read_bbc,
                         &handle_channel_write_ui);
  bbc_set_channel_handles(p_bbc,
                          handle_channel_read_bbc,
                          handle_channel_write_bbc,
                          s_handle_channel_read_ui,
                          handle_channel_write_ui);
  os_channel_embedded_set_handler(s_handle_channel_read_ui,
                                  beebjit_embedded_ui_handler,
                                  p_bbc);

  return p_bbc;
}

uint32_t
beebjit_embedded_run(struct bbc_struct* p_bbc) {
  /* With os_embedded's threads, this runs the emulator to completion. */
  bbc_run_async(p_bbc);
  return bbc_get_run_result(p_bbc);
}
