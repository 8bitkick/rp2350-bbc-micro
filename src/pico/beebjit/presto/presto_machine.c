/*
 * What the Presto's front end asks of the machine, for beebjit: the hooks in
 * presto_machine.h and presto_sd.h (the web panel and the SD card share their
 * code with other emulators), the USB channel's "!" commands (as tools/presto.py
 * speaks them) and BBC Micro Bot snapshots (presto_state.h).
 *
 * All of it runs in the emulator's thread on core 0, in its frame callback or
 * while it waits (presto_machine_frame, presto_machine_poll): inside one of
 * beebjit's timer callbacks, where a disc may be changed (as its own Alt+0
 * does) and a reset requested. Whatever must change the running CPU's view
 * (ROM sockets, a snapshot's memory and registers) waits for that reset and
 * is done in the reset hook, where beebjit's CPU driver resumes from the
 * state it is left.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "presto_beebjit.h"
#include "romfs.h"
#if PRESTO_BEEBJIT_AUDIO
#include "presto_audio.h"
#include "sound.h"
#endif
#include "../os_embedded.h"

#include "bbc.h"
#include "cpu_driver.h"
#include "keyboard.h"
#include "serial_ula.h"
#include "state_6502.h"
#include "via.h"
#include "video.h"

#include "presto_lcd.h"
#include "presto_machine.h"
#include "presto_net.h"
#include "presto_psram.h"
#include "presto_sd.h"
#include "presto_state.h"

#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static struct bbc_struct* s_bbc;
static struct keyboard_struct* s_keyboard;
static const char* s_builtin_disc;
static uint32_t s_frames;

/* ---- Keys ---- */

/* BBC matrix code (row << 4 | column) -> beebjit key (0: none). */
static uint8_t s_matrix_keys[0x80];

static void
keys_init(void) {
  uint32_t key;
  for (key = 1; key < k_keyboard_key_SPECIAL_release_all; ++key) {
    int32_t row;
    int32_t col;
    keyboard_bbc_key_to_rowcol((uint8_t) key, &row, &col);
    if ((row >= 0) && (row < 8) && (col >= 0) && (col < 16) &&
        (s_matrix_keys[(row << 4) | col] == 0)) {
      s_matrix_keys[(row << 4) | col] = (uint8_t) key;
    }
  }
}

static void
key_event(uint8_t key, bool down) {
  if ((s_keyboard == NULL) || (key == 0)) {
    return;
  }
  if (down) {
    keyboard_system_key_pressed(s_keyboard, key);
  } else {
    keyboard_system_key_released(s_keyboard, key);
  }
}

void
key_bbc(int vkey, bool down) {
  if ((vkey >= 0) && (vkey < 0x80)) {
    key_event(s_matrix_keys[vkey], down);
  }
}

/* The USB channel's keys: SDL scancodes, as tools/presto.py sends them. */
static uint8_t
sdl_key(uint32_t sc) {
  static const uint8_t k_keys[] = {
    [40] = k_keyboard_key_enter, [41] = k_keyboard_key_escape,
    [42] = k_keyboard_key_backspace, [43] = k_keyboard_key_tab, [44] = ' ',
    [45] = '-', [46] = '=', [47] = '[', [48] = ']', [49] = '\\',
    [51] = ';', [52] = '\'', [53] = '`', [54] = ',', [55] = '.', [56] = '/',
    [57] = k_keyboard_key_caps_lock, [67] = k_keyboard_key_f0,
    [69] = k_keyboard_key_f12, [77] = k_keyboard_key_end,
    [79] = k_keyboard_key_arrow_right, [80] = k_keyboard_key_arrow_left,
    [81] = k_keyboard_key_arrow_down, [82] = k_keyboard_key_arrow_up,
  };
  if ((sc >= 4) && (sc <= 29)) {
    return (uint8_t) ('A' + (sc - 4));
  }
  if ((sc >= 30) && (sc <= 38)) {
    return (uint8_t) ('1' + (sc - 30));
  }
  if (sc == 39) {
    return '0';
  }
  if ((sc >= 58) && (sc <= 66)) {
    return (uint8_t) (k_keyboard_key_f1 + (sc - 58));
  }
  if ((sc == 224) || (sc == 228)) {
    return k_keyboard_key_ctrl;
  }
  if (sc == 225) {
    return k_keyboard_key_shift_left;
  }
  if (sc == 229) {
    return k_keyboard_key_shift_right;
  }
  if (sc < sizeof(k_keys)) {
    return k_keys[sc];
  }
  return 0;
}

void
presto_break(void) {
  if (s_bbc != NULL) {
    bbc_request_reset(s_bbc, 0);
  }
}

/* SHIFT held for a second after a reset, as SHIFT+BREAK: the disc boots.
 * (A power-on reset clears the keyboard, so the reset hook presses it again.)
 */
enum {
  k_boot_shift_frames = 50,
};
static uint32_t s_boot_shift_frames;

void
presto_boot_shift(void) {
  s_boot_shift_frames = k_boot_shift_frames;
  key_event(k_keyboard_key_shift_left, true);
}

static void
boot_shift_cancel(void) {
  if (s_boot_shift_frames != 0) {
    s_boot_shift_frames = 0;
    key_event(k_keyboard_key_shift_left, false);
  }
}

/* ---- The keyboard's LEDs, the temperature ---- */

uint8_t IC32;

uint8_t
serial_get_reg(void) {
  if (s_bbc == NULL) {
    return 0;
  }
  return serial_ula_is_motor_on(bbc_get_serial_ula(s_bbc)) ? 0x80 : 0;
}

/* In hundredths of a degree (RP2350 datasheet: 27C at 0.706V, -1.721mV/C). */
int
presto_temp_centi(void) {
  static bool s_adc_ready;
  uint32_t sum = 0;
  int32_t uv;
  int i;
  if (!s_adc_ready) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    s_adc_ready = true;
  }
  adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
  for (i = 0; i < 64; ++i) {
    sum += adc_read();
  }
  uv = (int32_t) (((uint64_t) sum * 3300000u) / (64 * 4096));
  return 2700 - (((uv - 706000) * 100) / 1721);
}

/* ---- ROM sockets ----
 * BASIC 15 and DFS 14, as built in, but a snapshot may want its own layout
 * (below, under snapshots): the banks are swapped
 * between sockets, so a change costs no memory. The Graphics Extension ROM,
 * if the build has one, is loaded into its socket only when wanted.
 */
enum {
  k_rom_basic,
  k_rom_dfs,
  k_rom_count,
};
static int8_t s_rom_socket[k_rom_count] = { 15, 14 };
static int8_t s_gxr_socket = -1;
/* Wanted at the next power-on reset. */
static int8_t s_want_socket[k_rom_count] = { 15, 14 };
static int8_t s_want_gxr = -1;

#define GXR_ROM_NAME "roms/gxr.rom"

static void
rom_move(int rom, int to) {
  int from = s_rom_socket[rom];
  int i;
  if (from == to) {
    return;
  }
  bbc_swap_roms(s_bbc, (uint8_t) from, (uint8_t) to);
  for (i = 0; i < k_rom_count; ++i) {
    if (s_rom_socket[i] == to) {
      s_rom_socket[i] = (int8_t) from;
    }
  }
  if (s_gxr_socket == to) {
    s_gxr_socket = (int8_t) from;
  }
  s_rom_socket[rom] = (int8_t) to;
}

static void
gxr_set(int socket) {
  static uint8_t* s_p_zeros;
  const struct romfs_file* p_gxr = romfs_find_builtin(GXR_ROM_NAME);
  if ((p_gxr == NULL) || (socket == s_gxr_socket)) {
    return;
  }
  if (s_gxr_socket >= 0) {
    if (s_p_zeros == NULL) {
      s_p_zeros = beebjit_embedded_malloc_big(16384);
      (void) memset(s_p_zeros, '\0', 16384);
    }
    bbc_load_rom(s_bbc, (uint8_t) s_gxr_socket, s_p_zeros);
    s_gxr_socket = -1;
  }
  if (socket >= 0) {
    bbc_load_rom(s_bbc, (uint8_t) socket, (uint8_t*) p_gxr->p_data);
    s_gxr_socket = (int8_t) socket;
  }
}

static void
roms_apply(void) {
  int i;
  for (i = 0; i < k_rom_count; ++i) {
    rom_move(i, s_want_socket[i]);
  }
  gxr_set(s_want_gxr);
}

void
presto_rom_layout_default(void) {
  s_want_socket[k_rom_basic] = 15;
  s_want_socket[k_rom_dfs] = 14;
  s_want_gxr = -1;
}

/* ---- Discs ----
 * An image from the card or the web lands at the start of the disc RAM;
 * beebjit takes a copy as it goes in (building tracks from it as the drive
 * reaches them), so the disc RAM is free again straight away.
 */
static char s_disc_name[72];

const char*
presto_disc_name(void) {
  return s_disc_name[0] ? s_disc_name : NULL;
}

static void
reset_to_boot(void) {
  presto_rom_layout_default();   /* after a snapshot: our own sockets */
  presto_boot_shift();
  bbc_request_reset(s_bbc, 1);
}

bool
presto_disc_insert(const char* p_name, uint32_t size) {
  uint8_t* p_ram = presto_disc_ram();
  const char* p_dot = strrchr(p_name, '.');
  const char* p_file;
  uint32_t side_sectors;
  uint32_t padded;
  bool dsd;

  if ((p_ram == NULL) || (s_bbc == NULL) || (size == 0) ||
      (size > PRESTO_DISC_MAX)) {
    return false;
  }
  /* DFS single sided (.ssd) or interleaved double sided (.dsd). The name
   * isn't always right (double sided images get called .ssd), so ask the
   * disc too: its catalogue (bytes 6 and 7 of sector 1) says how many sectors
   * a side holds, so an image bigger than one side is two.
   */
  dsd = ((p_dot != NULL) && !strcasecmp(p_dot, ".dsd"));
  side_sectors = 0;
  if (size >= 0x108) {
    side_sectors = (((p_ram[0x106] & 3u) << 8) | p_ram[0x107]);
  }
  if (((side_sectors == 400) || (side_sectors == 800)) &&
      (size > (side_sectors * 256))) {
    dsd = true;
  }
  if (!dsd && (size > (PRESTO_DISC_MAX / 2))) {
    return false;
  }
  /* beebjit wants whole sectors. */
  padded = ((size + 255) & ~255u);
  (void) memset((p_ram + size), '\0', (padded - size));
  p_file = (dsd ? "card.dsd" : "card.ssd");
  romfs_set_ram_file(p_file, p_ram, padded);
  bbc_replace_disc(s_bbc, p_file, 0);
  (void) snprintf(s_disc_name, sizeof(s_disc_name), "%s", p_name);
  reset_to_boot();
  return true;
}

bool
presto_sd_reinsert_disc(void) {
  /* The drive keeps its disc through resets. */
  return (s_disc_name[0] != '\0');
}

void
presto_sd_eject_disc(void) {
  if (s_bbc == NULL) {
    return;
  }
  bbc_replace_disc(s_bbc, s_builtin_disc, 0);
  s_disc_name[0] = '\0';
  reset_to_boot();
  (void) printf("DISC built-in\n");
}

/* ---- BBC Micro Bot snapshots ----
 * The format is tools/presto.py's (it packs the snapshot). The lessons,
 * from the BBC Micro Bot's own restore: our ROMs move to the snapshot's sockets, the
 * machine resets (without booting) and runs for a moment so the OS sets up
 * the hardware, then a BREAK, and in its reset hook the snapshot's memory,
 * VIA timers, paged ROM, screen size, CRTC, ULA and registers go in.
 */
#define STATE_RAM 0x8000
#define VIA_BYTES 22
#define STATE_BYTES (STATE_RAM + 18 + 1 + 16 + 7 + 5 + (2 * VIA_BYTES))
#define RESET_FRAMES 60

enum {
  k_state_idle,
  k_state_received,
  k_state_resetting,
  k_state_applying,
};
static uint8_t s_state_stage;
static uint32_t s_state_frames;

static uint8_t*
state_buf(void) {
  uint8_t* p_ram = presto_disc_ram();
  return (p_ram == NULL) ? NULL : (p_ram + PRESTO_DISC_MAX);
}

void
presto_state_receive(const char* p_args) {
  unsigned long size = strtoul(p_args, NULL, 10);
  uint8_t* p_buf = state_buf();
  uint32_t n;
  if (p_buf == NULL) {
    (void) printf("STATE error: no PSRAM\n");
    return;
  }
  if (size != STATE_BYTES) {
    (void) printf("STATE error: expected %u bytes\n", (unsigned) STATE_BYTES);
    return;
  }
  (void) printf("STATE ready\n");
  (void) fflush(stdout);
  for (n = 0; n < size; ++n) {
    int c = getchar_timeout_us(2000000);
    if (c == PICO_ERROR_TIMEOUT) {
      (void) printf("STATE error: timed out after %lu bytes\n",
                    (unsigned long) n);
      return;
    }
    p_buf[n] = (uint8_t) c;
    if ((n & 0xFF) == 0) {
      watchdog_update();
    }
  }
  s_state_stage = k_state_received;
  (void) printf("STATE ok\n");
}

static uint32_t
get32(const uint8_t* p) {
  return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24));
}

/* Timers and interrupts; never the ports. jsbeeb keeps the
 * latches in 2MHz ticks: beebjit wants them in 1MHz (as its state.c).
 */
static void
restore_via(struct via_struct* p_via, const uint8_t* p, int irq) {
  uint8_t ORA, ORB, DDRA, DDRB, SR, ACR, PCR, IFR, IER, pa, pb;
  uint8_t t1_fired, t2_fired, t1_pb7;
  int32_t t1c, t1l, t2c, t2l;
  via_get_registers(p_via, &ORA, &ORB, &DDRA, &DDRB, &SR, &ACR, &PCR, &IFR,
                    &IER, &pa, &pb, &t1c, &t1l, &t2c, &t2l, &t1_fired,
                    &t2_fired, &t1_pb7);
  via_set_registers(p_via, ORA, ORB, DDRA, DDRB, SR, p[18], p[19],
                    (p[20] & 0x7F), (p[21] & 0x7F), pa, pb,
                    (int32_t) get32(p), ((int32_t) get32(p + 4) >> 1),
                    (int32_t) get32(p + 8), ((int32_t) get32(p + 12) >> 1),
                    p[16], p[17], t1_pb7);
  state_6502_set_irq_level(bbc_get_6502(s_bbc),
                           irq,
                           ((p[20] & p[21] & 0x7F) != 0));
}

static void
state_apply(void) {
  static const uint8_t k_size_for_mode[8] = { 1, 1, 1, 0, 3, 3, 2, 2 };
  uint8_t* p_s = state_buf();
  const uint8_t* p_crtc = (p_s + STATE_RAM);
  const uint8_t* p_ula = (p_crtc + 18);
  const uint8_t* p_pal = (p_ula + 1);
  const uint8_t* p_cpu = (p_pal + 16);
  const uint8_t* p_extra = (p_cpu + 7);
  const uint8_t* p_vias = (p_extra + 5);
  uint8_t paged = p_extra[1];
  uint8_t crtc_reg = p_extra[2];
  uint8_t ic32 = p_extra[3];
  uint8_t flags = p_extra[4];
  struct video_struct* p_video = bbc_get_video(s_bbc);
  uint8_t palette[16];
  uint32_t i;

  bbc_set_memory_block(s_bbc, 0, STATE_RAM, p_s);

  /* VIAs first, before anything else touches the machine. */
  if (flags & 4) {
    restore_via(bbc_get_sysvia(s_bbc), p_vias, k_state_6502_irq_via_1);
  }
  if (flags & 8) {
    restore_via(bbc_get_uservia(s_bbc), (p_vias + VIA_BYTES),
                k_state_6502_irq_via_2);
  }

  bbc_sideways_select(s_bbc, (paged & 15));

  /* The screen's wrap-around size: the system VIA's latch. */
  if (!(flags & 1)) {
    uint8_t size = k_size_for_mode[p_s[0x355] & 7];
    ic32 = (uint8_t) ((bbc_get_IC32(s_bbc) & ~0x30) |
                      ((size & 2) ? 0x10 : 0) |
                      ((size & 1) ? 0x20 : 0));
  }
  bbc_set_IC32(s_bbc, ic32);

  for (i = 0; i < 17; ++i) {
    video_crtc_write(p_video, 0, (uint8_t) i);
    video_crtc_write(p_video, 1, p_crtc[i]);
  }
  if (flags & 2) {
    video_crtc_write(p_video, 0, crtc_reg);
  }
  video_set_ula_control(p_video, p_ula[0]);
  /* Saved as the colours: the ULA takes their low 3 bits inverted. */
  for (i = 0; i < 16; ++i) {
    palette[i] = (uint8_t) ((~p_pal[i] & 7) | (p_pal[i] & 8));
  }
  video_set_ula_full_palette(p_video, palette);

  state_6502_set_registers(bbc_get_6502(s_bbc),
                           p_cpu[0],
                           p_cpu[1],
                           p_cpu[2],
                           p_cpu[3],
                           (p_cpu[4] | 0x20),
                           (uint16_t) (p_cpu[5] | (p_cpu[6] << 8)));
}

void
presto_state_apply_pending(void) {
  if (s_state_stage == k_state_received) {
    const uint8_t* p_s = state_buf();
    uint8_t layout = p_s[STATE_RAM + 18 + 1 + 16 + 7];
    int gxr = ((layout == 1) ? 7 : 13);
    s_want_socket[k_rom_basic] = ((layout == 1) ? 12 : 15);
    s_want_socket[k_rom_dfs] = ((layout == 1) ? 13 : 14);
    /* GXR if the OS's ROM table in the snapshot shows one in its socket. */
    s_want_gxr = (int8_t) ((p_s[0x2A1 + gxr] != 0) ? gxr : -1);
    boot_shift_cancel();
    bbc_request_reset(s_bbc, 1);
    s_state_frames = RESET_FRAMES;
    s_state_stage = k_state_resetting;
  } else if ((s_state_stage == k_state_resetting) && !--s_state_frames) {
    s_state_stage = k_state_applying;
    bbc_request_reset(s_bbc, 0);
  }
}

/* ---- The reset hook ---- */

static void
reset_hook(void* p, uint32_t flags) {
  (void) p;
#if PRESTO_BEEBJIT_AUDIO
  sound_discard_pending(bbc_get_sound(s_bbc));
  presto_audio_flush();
#endif
  if (flags & k_cpu_flag_hard_reset) {
    roms_apply();
    if (s_boot_shift_frames != 0) {
      key_event(k_keyboard_key_shift_left, true);
    }
  }
  if ((flags & k_cpu_flag_soft_reset) &&
      (s_state_stage == k_state_applying)) {
    state_apply();
    s_state_stage = k_state_idle;
  }
}

/* ---- The USB channel ----
 *   0x1a 0x00 <sdl scancode>   key down
 *   0x1a 0x01 <sdl scancode>   key up
 *   !shot [ms]                 screenshot of the panel (then hold the frame ms)
 *   !stats                     speed and load
 *   !crtc                      the CRTC registers
 *   !discs                     the disc images on the SD card
 *   !disc <name>               load one into drive 0 and boot it
 *   !disc                      back to the built-in disc
 *   !put <name> <size>         write a disc image to the card
 *   !state <size>              load a BBC Micro Bot snapshot
 *   !turbo, !perf              toggle turbo, the panel's figures
 *   !wifi <ssid> <password>    join a network; !net its state
 *   !peek <hex addr> [len], !mem, !psram, !temp, !clk, !lcd, !bootsel
 *   !lock [lead us]            the frame lock's phase error (presto_beebjit.c)
 *   !prof, !profdump           (PRESTO_BEEBJIT_PROF) sample core 0 again,
 *                              fetch the samples (tools/prof.py)
 */
static void
binary_out(const void* p_data, uint32_t len) {
  stdio_set_translate_crlf(&stdio_usb, false);
  (void) fwrite(p_data, 1, len, stdout);
  (void) fflush(stdout);
  stdio_set_translate_crlf(&stdio_usb, true);
}

static void
screenshot(void) {
  static uint16_t s_out[PRESTO_LCD_WIDTH];
  int y;
  int x;
  (void) printf("SHOT %d %d\n", PRESTO_LCD_WIDTH, PRESTO_LCD_HEIGHT);
  (void) fflush(stdout);
  for (y = 0; y < PRESTO_LCD_HEIGHT; ++y) {
    const uint16_t* p_src = presto_lcd_row(y, s_out);
    for (x = 0; x < PRESTO_LCD_WIDTH; ++x) {
      s_out[x] = __builtin_bswap16(p_src[x]);
    }
    binary_out(s_out, (PRESTO_LCD_WIDTH * sizeof(uint16_t)));
  }
}

static void
run_command(char* p_cmd) {
  if (!strncmp(p_cmd, "shot", 4) &&
      ((p_cmd[4] == '\0') || (p_cmd[4] == ' '))) {
    /* "!shot [ms]": then hold the machine (and so the panel) still that long,
     * for a camera to take the same frame.
     */
    unsigned hold_ms = 0;
    (void) sscanf(p_cmd + 4, "%u", &hold_ms);
#if PRESTO_BEEBJIT_AUDIO
    presto_audio_set_muted(true);
#endif
    screenshot();
    if (hold_ms > 10000) {
      hold_ms = 10000;
    }
    while (hold_ms > 0) {
      sleep_ms((hold_ms > 100) ? 100 : hold_ms);
      hold_ms -= ((hold_ms > 100) ? 100 : hold_ms);
      watchdog_update();
    }
#if PRESTO_BEEBJIT_AUDIO
    sound_discard_pending(bbc_get_sound(s_bbc));
    presto_audio_set_muted(presto_turbo_get());
#endif
  } else if (!strncmp(p_cmd, "audio gain ", 11)) {
#if PRESTO_BEEBJIT_AUDIO
    char* end;
    unsigned long multiplier = strtoul(p_cmd + 11, &end, 10);
    if (end == p_cmd + 11 || *end != '\0' || multiplier > 32) {
      printf("AUDIO gain must be an integer from 0 to 32\n");
    } else {
      sound_discard_pending(bbc_get_sound(s_bbc));
      presto_audio_set_gain((uint32_t) multiplier * 256u);
      printf("AUDIO gain=%lux\n", multiplier);
    }
#else
    printf("AUDIO disabled; rebuild with scripts/setup.sh --audio\n");
#endif
  } else if (!strcmp(p_cmd, "audio test")) {
#if PRESTO_BEEBJIT_AUDIO
    bool ok = presto_audio_test();
    sound_discard_pending(bbc_get_sound(s_bbc));
    printf("AUDIO test=%s (1 kHz then 2 kHz; two 300 ms tones)\n",
           ok ? "sent" : "failed");
#else
    printf("AUDIO disabled; rebuild with scripts/setup.sh --audio\n");
#endif
  } else if (!strcmp(p_cmd, "audio")) {
#if PRESTO_BEEBJIT_AUDIO
    struct presto_audio_stats stats;
    presto_audio_get_stats(&stats);
    printf("AUDIO v=3 init=%u muted=%u playing=%u ticks=%lu submitted=%lu "
           "pcm_min=%d pcm_max=%d queued=%lu underruns=%lu dropped=%lu adjust_ppm=%ld "
           "gain_q8=%lu clipped=%lu pwm_min=%u pwm_max=%u\n",
           (unsigned) stats.initialized, (unsigned) stats.muted,
           (unsigned) stats.playing, (unsigned long) stats.timer_ticks,
           (unsigned long) stats.submitted_samples,
           (int) stats.pcm_min, (int) stats.pcm_max,
           (unsigned long) stats.queued_samples,
           (unsigned long) stats.underruns,
           (unsigned long) stats.dropped_samples,
           (long) stats.rate_adjust_ppm,
           (unsigned long) stats.gain_q8,
           (unsigned long) stats.clipped_samples,
           (unsigned) stats.pwm_min, (unsigned) stats.pwm_max);
#else
    printf("AUDIO disabled; rebuild with scripts/setup.sh --audio\n");
#endif
  } else if (!strcmp(p_cmd, "stats")) {
    presto_beebjit_print_stats();
#if PRESTO_BEEBJIT_PROF
  } else if (!strncmp(p_cmd, "prof", 4) &&
             ((p_cmd[4] == '\0') || (p_cmd[4] == ' '))) {
    presto_prof_restart();
  } else if (!strcmp(p_cmd, "profdump")) {
    presto_prof_dump();
#endif
  } else if (!strcmp(p_cmd, "crtc")) {
    uint8_t regs[18];
    int i;
    video_get_crtc_registers(bbc_get_video(s_bbc), regs);
    (void) printf("CRTC");
    for (i = 0; i < 18; ++i) {
      (void) printf(" R%d=%d", i, regs[i]);
    }
    (void) printf("\n");
  } else if (!strcmp(p_cmd, "discs")) {
    presto_sd_list_discs();
  } else if (!strncmp(p_cmd, "disc ", 5)) {
    (void) presto_sd_load_disc(p_cmd + 5);
  } else if (!strcmp(p_cmd, "disc")) {
    presto_sd_eject_disc();
  } else if (!strncmp(p_cmd, "put ", 4)) {
    presto_sd_put(p_cmd + 4);
  } else if (!strncmp(p_cmd, "state ", 6)) {
    presto_state_receive(p_cmd + 6);
  } else if (!strcmp(p_cmd, "turbo")) {
    presto_turbo_set(!presto_turbo_get());
    (void) printf("TURBO %s\n", presto_turbo_get() ? "on" : "off");
  } else if (!strcmp(p_cmd, "perf")) {
    presto_overlay_set(!presto_overlay_get());
    (void) printf("PERF overlay %s\n", presto_overlay_get() ? "on" : "off");
  } else if (!strncmp(p_cmd, "wifi ", 5)) {
    presto_net_set_wifi(p_cmd + 5);
  } else if (!strcmp(p_cmd, "net")) {
    presto_net_print_status();
  } else if (!strncmp(p_cmd, "peek ", 5)) {
    const uint8_t* p_mem = bbc_get_mem_read(s_bbc);
    unsigned addr = 0;
    unsigned len = 16;
    unsigned i;
    (void) sscanf(p_cmd + 5, "%x %u", &addr, &len);
    (void) printf("PEEK %04x", addr);
    for (i = 0; (i < len) && ((addr + i) < 0x10000); ++i) {
      (void) printf(" %02x", p_mem[addr + i]);
    }
    (void) printf("\n");
  } else if (!strcmp(p_cmd, "mem")) {
    extern char __StackLimit;
    struct mallinfo m = mallinfo();
    size_t big_used;
    size_t big_peak;
    size_t small_total;
    beebjit_embedded_get_alloc_stats(&big_used, &big_peak, &small_total);
    (void) printf("MEM heap used=%u free_in_heap=%u unclaimed=%u psram_used=%u\n",
                  (unsigned) m.uordblks, (unsigned) m.fordblks,
                  (unsigned) (&__StackLimit - (char*) sbrk(0)),
                  (unsigned) big_used);
    presto_beebjit_print_stacks();
  } else if (!strncmp(p_cmd, "lock", 4) &&
             ((p_cmd[4] == '\0') || (p_cmd[4] == ' '))) {
    presto_beebjit_lock(p_cmd + ((p_cmd[4] == ' ') ? 5 : 4));
  } else if (!strcmp(p_cmd, "psram")) {
    (void) printf("PSRAM %u bytes\n", (unsigned) presto_psram_size());
  } else if (!strcmp(p_cmd, "temp")) {
    int centi = presto_temp_centi();
    (void) printf("TEMP %d.%02dC\n", (centi / 100), (centi % 100));
  } else if (!strcmp(p_cmd, "clk")) {
    /* Measured by the clocks block's frequency counter, against the crystal. */
    (void) printf("CLK sys=%lu kHz (measured), %lu kHz (set)\n",
                  (unsigned long) frequency_count_khz(
                      CLOCKS_FC0_SRC_VALUE_CLK_SYS),
                  (unsigned long) (clock_get_hz(clk_sys) / 1000));
  } else if (!strcmp(p_cmd, "lcd")) {
    presto_lcd_debug();
  } else if (!strcmp(p_cmd, "bootsel")) {
    (void) printf("BOOTSEL\n");
    (void) fflush(stdout);
    sleep_ms(100);
    reset_usb_boot(0, 0);
  } else {
    (void) printf("ERR unknown command '%s'\n", p_cmd);
  }
}

static void
usb_poll(void) {
#if !PRESTO_BEEBJIT_TERMINAL
  static char s_cmd[128];
  static int s_cmd_len = -1;   /* -1 when not in a command */
  static int s_key_state;      /* 1 after 0x1a, 2 / 3 awaiting a scancode */
  int c;
  while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
    if (s_key_state == 1) {
      s_key_state = ((c == 0) ? 2 : ((c == 1) ? 3 : 0));
    } else if (s_key_state >= 2) {
      key_event(sdl_key((uint32_t) c), (s_key_state == 2));
      s_key_state = 0;
    } else if (c == 0x1A) {
      s_key_state = 1;
    } else if (c == '!') {
      s_cmd_len = 0;
    } else if (s_cmd_len >= 0) {
      if ((c == '\n') || (c == '\r')) {
        s_cmd[s_cmd_len] = '\0';
        s_cmd_len = -1;
        run_command(s_cmd);
      } else if (s_cmd_len < (int) (sizeof(s_cmd) - 1)) {
        s_cmd[s_cmd_len++] = (char) c;
      }
    }
  }
#endif
}

/* ---- ---- */

void
presto_machine_init(struct bbc_struct* p_bbc, const char* p_builtin_disc) {
  s_bbc = p_bbc;
  s_keyboard = bbc_get_keyboard(p_bbc);
  s_builtin_disc = p_builtin_disc;
  keys_init();
  bbc_set_host_reset_callback(p_bbc, reset_hook, NULL);
  presto_net_init();
}

void
presto_machine_frame(void) {
  s_frames++;
  /* beebjit's own autoboot boots the built-in disc at power on; after
   * that, resets boot only when asked (presto_boot_shift), as a Model B's
   * keyboard links have it.
   */
  if (s_frames == 200) {
    bbc_set_autoboot(s_bbc, 0);
  }
  if ((s_boot_shift_frames != 0) && (--s_boot_shift_frames == 0)) {
    key_event(k_keyboard_key_shift_left, false);
  }
  IC32 = bbc_get_IC32(s_bbc);
  usb_poll();
  presto_state_apply_pending();
  presto_net_frame();
}

void
presto_machine_poll(void) {
  usb_poll();
  presto_net_poll();
}
