/*
 * beebjit on the Pimoroni Presto (RP2350): beebjit (its JIT, with the thumb2
 * backend, or its interpreter) and hardware models on core 0, rendering
 * scanline by scanline into the panel's 4-bit framebuffer, which PIO and DMA
 * scan out with no CPU help (presto_lcd.c). With PRESTO_BEEBJIT_WIRE, the
 * drawing moves to core 1: video hands it its render calls through the render
 * wire (beebjit_wire.h). The front end (the web panel, the SD card, the USB
 * channel) is in src/pico/presto, through presto_machine.c.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "presto_beebjit.h"
#include "../beebjit_embedded.h"
#include "../beebjit_host.h"
#include "../beebjit_panel.h"
#include "../os_embedded.h"

#include "bbc.h"
#include "cpu_driver.h"
#include "log.h"
#include "timing.h"

#include "presto_audio.h"
#if PRESTO_BEEBJIT_AMBIENT
#include "presto_ambient.h"
#endif
#include "presto_boot.h"
#include "presto_lcd.h"
#include "presto_machine.h"
#include "presto_psram.h"
#include "presto_sd.h"

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/timer.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#if PRESTO_BEEBJIT_WIRE
#include "../beebjit_wire.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/structs/systick.h"
#endif

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PRESTO_SYS_CLOCK_KHZ
#define PRESTO_SYS_CLOCK_KHZ 250000
#endif
/* Print emulation speed and load every this many frames (0: never). */
#ifndef PRESTO_BEEBJIT_STATS_FRAMES
#define PRESTO_BEEBJIT_STATS_FRAMES 250
#endif
/* Start in turbo (as fast as it goes, every frame drawn). */
#ifndef PRESTO_BEEBJIT_TURBO
#define PRESTO_BEEBJIT_TURBO 0
#endif
/* USB serial input is the BBC's RS423 (as upstream's -terminal) rather than
 * the USB channel's commands and keys.
 */
#ifndef PRESTO_BEEBJIT_TERMINAL
#define PRESTO_BEEBJIT_TERMINAL 0
#endif

static_assert(k_beebjit_panel_row_words == PRESTO_LCD_ROW_WORDS,
              "panel row format");
static_assert(k_beebjit_panel_row_end == PRESTO_LCD_ROW_END,
              "panel row format");

/* One row per BBC scanline; the panel's row table stretches them to 480. */
static uint32_t s_fb[k_beebjit_panel_rows][k_beebjit_panel_row_words];
static const uint16_t* s_row_table[PRESTO_LCD_HEIGHT + 1];
static struct beebjit_panel s_panel;

#if PRESTO_BEEBJIT_PANEL_PIO
/* ---- 640 -> 480 on PIO0 ----
 *
 * A finished row of packed 4-bit codes (80 words) is copied out of the
 * renderer's row (which it clears straight away) and goes by DMA through a
 * PIO0 program that keeps 3 pixels of every 4; a second DMA takes the 60
 * words out into the framebuffer row. The program moves 3 pixels (12 bits)
 * at a time: per 4 words in (8 groups of 3 kept + 1 dropped) it pushes 3
 * words, the ISR filling to exactly 32 bits each time (the groups that
 * straddle a word are split). 20 instructions, ~5 cycles a word: a row in
 * ~1.6us, where a CPU loop took ~600 cycles of core 1.
 */
static uint16_t s_pack_insns[20];
static int s_pack_sm = -1;
static uint s_pack_offset;
static int s_copy_dma = -1;
static int s_pack_dma = -1;
static uint32_t s_pack_timeouts;

/* From the top of the program, with nothing in flight. */
static void __not_in_flash_func(panel_pio_restart)(void) {
  uint sm = (uint) s_pack_sm;
  dma_channel_abort((uint) s_copy_dma);
  dma_channel_abort((uint) s_pack_dma);
  pio_sm_set_enabled(pio0, sm, false);
  pio_sm_clear_fifos(pio0, sm);
  pio_sm_restart(pio0, sm);
  pio_sm_exec(pio0, sm, pio_encode_jmp(s_pack_offset));
  pio_sm_set_enabled(pio0, sm, true);
}

static void
panel_pio_init(void) {
  static const uint8_t ins[20][2] = {
    /* in osr, n; out null, n (bits): 12+12 | 8 (push) 4 | 12+12 | 4 (push)
     * 8 | 12+12 (push).
     */
    { 12, 16 }, { 12, 16 }, { 8, 8 }, { 4, 8 }, { 12, 16 }, { 12, 16 },
    { 4, 4 }, { 8, 12 }, { 12, 16 }, { 12, 16 },
  };
  struct pio_program prog;
  pio_sm_config c;
  dma_channel_config dc;
  uint32_t i;
  for (i = 0; i < 10; ++i) {
    s_pack_insns[i * 2] = pio_encode_in(pio_osr, ins[i][0]);
    s_pack_insns[(i * 2) + 1] = pio_encode_out(pio_null, ins[i][1]);
  }
  prog.instructions = s_pack_insns;
  prog.length = 20;
  prog.origin = -1;
  s_pack_offset = pio_add_program(pio0, &prog);
  s_pack_sm = pio_claim_unused_sm(pio0, true);
  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, s_pack_offset, (s_pack_offset + 19));
  /* First pixel in the low nibble, in and out. */
  sm_config_set_out_shift(&c, true, true, 32);
  sm_config_set_in_shift(&c, true, true, 32);
  pio_sm_init(pio0, (uint) s_pack_sm, s_pack_offset, &c);
  pio_sm_set_enabled(pio0, (uint) s_pack_sm, true);

  s_copy_dma = dma_claim_unused_channel(true);
  dc = dma_channel_get_default_config((uint) s_copy_dma);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pio0, (uint) s_pack_sm, true));
  dma_channel_configure((uint) s_copy_dma, &dc, &pio0->txf[s_pack_sm], NULL,
                        0, false);
  s_pack_dma = dma_claim_unused_channel(true);
  dc = dma_channel_get_default_config((uint) s_pack_dma);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, false);
  channel_config_set_write_increment(&dc, true);
  channel_config_set_dreq(&dc, pio_get_dreq(pio0, (uint) s_pack_sm, false));
  dma_channel_configure((uint) s_pack_dma, &dc, NULL, &pio0->rxf[s_pack_sm],
                        0, false);
}

/* The renderer's row callback (beebjit_panel_row's job), on core 1. */
/* ---- Frame lock ----
 * The panel refreshes at the BBC's frame rate (presto_lcd.c), and the BBC's
 * clock is steered, a little each frame, so its vsync leads the start of the
 * panel's display area by s_lock_lead_us. Core 1 then draws each row (as the
 * emulator reaches it) before the panel scans it, and every frame is shown
 * once and whole: no frame shown twice, none torn. The lead covers the BBC's
 * own vertical blanking before its first displayed row (~2.6 ms), the
 * emulator running up to a 4 ms slice (BEEBJIT_VIDEO_SLICE_LINES) ahead or
 * behind, and some lateness on core 1; the rows are drawn at about the
 * panel's pace (16.4 ms for 256 against 16.3 for 480), so they can't lap it.
 * "!lock [lead us]" reads and sets it, and counts the rows that were late.
 */
#ifndef PRESTO_BEEBJIT_LOCK_LEAD_US
#define PRESTO_BEEBJIT_LOCK_LEAD_US 11000
#endif
static void lock_frame(void);
static int32_t s_lock_lead_us = PRESTO_BEEBJIT_LOCK_LEAD_US;
static int32_t s_lock_err_us;
static int32_t s_lock_err_min_us;
static int32_t s_lock_err_max_us;
static uint32_t s_lock_frames;
/* Core 1's: rows drawn, rows drawn after the panel had scanned them, and
 * how far the panel's scan was from each row as it was drawn (panel rows;
 * negative: the row was drawn that far ahead of the scan).
 */
static volatile uint32_t s_lock_rows;
static volatile uint32_t s_lock_late_rows;
static volatile uint32_t s_lock_near_rows;   /* under 40 lines ahead of the scan */
/* How a frame's rows arrive: from the first row to the last, in us. */
static uint32_t s_lock_span_first_us;
static uint32_t s_lock_span_last_us;
static uint32_t s_lock_span_last_row;
static volatile uint32_t s_lock_span_frames;
static volatile uint32_t s_lock_span_min_us;
static volatile uint32_t s_lock_span_max_us;
static volatile uint64_t s_lock_span_sum_us;
static volatile int32_t s_lock_first_scan_sum;   /* the scan's line when the first row went */
static volatile int32_t s_lock_margin_min;
static volatile int32_t s_lock_margin_max;
static volatile int64_t s_lock_margin_sum;


static void __not_in_flash_func(panel_pio_row)(void* p,
                                               uint32_t row,
                                               uint32_t* p_pixels) {
  struct beebjit_panel* p_panel = (struct beebjit_panel*) p;
  uint32_t t0;

  row >>= 1;
  if (row >= p_panel->num_rows) {
    return;
  }
  /* The frame lock's check: a row the panel has already scanned this frame
   * shows a frame late (a torn frame).
   */
  {
    uint32_t now = time_us_32();
    int32_t scan = presto_lcd_scan_row(now);
    int32_t margin = (scan - (int32_t) (row * PRESTO_LCD_HEIGHT / p_panel->num_rows));
    if (row <= s_lock_span_last_row) {
      /* A new frame: the previous one's span. */
      if (s_lock_rows != 0) {
        uint32_t span = (s_lock_span_last_us - s_lock_span_first_us);
        if ((s_lock_span_frames == 0) || (span < s_lock_span_min_us)) {
          s_lock_span_min_us = span;
        }
        if ((s_lock_span_frames == 0) || (span > s_lock_span_max_us)) {
          s_lock_span_max_us = span;
        }
        s_lock_span_sum_us += span;
        s_lock_span_frames++;
      }
      s_lock_span_first_us = now;
      s_lock_first_scan_sum += scan;
    }
    s_lock_span_last_us = now;
    s_lock_span_last_row = row;
    int32_t lines = (int32_t) presto_lcd_frame_lines();
    /* To the nearest scan of this row: the one before (negative: the row
     * was drawn that many lines ahead of it) or the one after.
     */
    if (margin >= (lines / 2)) {
      margin -= lines;
    } else if (margin < -(lines / 2)) {
      margin += lines;
    }
    if (s_lock_rows == 0) {
      s_lock_margin_min = s_lock_margin_max = margin;
      s_lock_margin_sum = 0;
    }
    s_lock_rows++;
    if (margin >= 0) {
      s_lock_late_rows++;
    } else if (margin > -40) {
      s_lock_near_rows++;
    }
    if (margin < s_lock_margin_min) {
      s_lock_margin_min = margin;
    }
    if (margin > s_lock_margin_max) {
      s_lock_margin_max = margin;
    }
    s_lock_margin_sum += margin;
  }
  /* The last row's (long done; if it somehow isn't, start the PIO afresh
   * rather than hang).
   */
  t0 = time_us_32();
  while (dma_channel_is_busy((uint) s_copy_dma) ||
         dma_channel_is_busy((uint) s_pack_dma)) {
    if ((time_us_32() - t0) > 500) {
      s_pack_timeouts++;
      panel_pio_restart();
      break;
    }
  }
  dma_channel_set_write_addr((uint) s_pack_dma, p_panel->p_rows[row], false);
  dma_channel_set_trans_count((uint) s_pack_dma,
                              (k_beebjit_panel_width / 8), true);
  dma_channel_set_read_addr((uint) s_copy_dma, p_pixels, false);
  dma_channel_set_trans_count((uint) s_copy_dma,
                              (k_beebjit_panel_src_width / 8), true);
  p_panel->rows_written++;
}
#endif

/* ---- beebjit_host.h ---- */

static uint64_t s_slept_us;
static bool s_turbo = PRESTO_BEEBJIT_TURBO;
/* Turbo: beebjit's clock runs on by the time it would have slept, so it
 * paces itself as if it had (and has nothing to catch up when turbo ends).
 */
static uint64_t s_turbo_time_us;

#if PRESTO_BEEBJIT_WIRE
/* ---- The render wire: core 0 emulates, core 1 draws. ---- */

static uint32_t s_wire_buf[BEEBJIT_WIRE_WORDS];
/* 4 KB: its high-water mark is ~0.6 KB over every scene (STACKS, after !mem). */
static uint32_t s_core1_stack[4 * 1024 / 4];
/* Time core 1 waited for work, and core 0 waited for core 1. */
static uint64_t s_core0_wait_us;
static uint32_t s_core0_wait_start;

/* Core 1's busy time, in clk_sys cycles by its SysTick (which stops while
 * the core sleeps in wfe, so idle time can't be timed with it).
 */
static volatile uint64_t s_core1_busy_cycles;

#if PRESTO_BEEBJIT_PROF && PRESTO_BEEBJIT_PROF_CORE
static void prof_init(void);
#endif

static void __not_in_flash_func(core1_main)(void) {
  /* Let core 0 park this core while it writes the flash (saving WiFi details). */
  (void) flash_safe_execute_core_init();
#if PRESTO_BEEBJIT_SOUND
  /* The sound: its DMA interrupt runs here. */
  presto_audio_init();
#endif
#if PRESTO_BEEBJIT_PROF && PRESTO_BEEBJIT_PROF_CORE
  prof_init();
#endif
  systick_hw->rvr = 0xFFFFFF;
  systick_hw->cvr = 0;
  systick_hw->csr = 0x5;   /* enabled, processor clock, no interrupt */
  while (1) {
    uint32_t t0 = systick_hw->cvr;
    if (beebjit_wire_drain() == 0) {
#if PRESTO_BEEBJIT_AMBIENT
      if (presto_ambient_due()) {
        presto_ambient_update();
        s_core1_busy_cycles += ((t0 - systick_hw->cvr) & 0xFFFFFF);
        continue;
      }
#endif
      __wfe();
    } else {
      s_core1_busy_cycles += ((t0 - systick_hw->cvr) & 0xFFFFFF);
    }
  }
}

static void __not_in_flash_func(wire_wait_hook)(int is_end) {
  /* Wall time (core 0 sleeps in wfe while it waits). Whole microseconds,
   * but both ends are cut the same way, so the sum comes out right.
   */
  if (!is_end) {
    s_core0_wait_start = time_us_32();
  } else {
    s_core0_wait_us += (time_us_32() - s_core0_wait_start);
  }
}
#endif

uint64_t
beebjit_host_time_us(void) {
  return (time_us_64() + s_turbo_time_us);
}

static void
lock_frame(void) {
  int32_t period = (int32_t) presto_lcd_frame_period_us();
  /* Where in the panel's frame this vsync fell, against where it should. */
  int32_t phase = (int32_t) (time_us_32() - presto_lcd_frame_time_us());
  int32_t err = (phase - (period - s_lock_lead_us));
  int32_t step;
  if (s_turbo || (period == 0)) {
    return;
  }
  while (err > (period / 2)) {
    err -= period;
  }
  while (err < -(period / 2)) {
    err += period;
  }
  /* Late (err > 0): advance the BBC's clock, so it sleeps less and the next
   * vsync comes sooner. A quarter of the error a frame, at most 250 us.
   */
  step = (err / 4);
  if (step > 250) {
    step = 250;
  } else if (step < -250) {
    step = -250;
  }
  s_turbo_time_us += step;
  s_lock_err_us = err;
  if ((s_lock_frames == 0) || (err < s_lock_err_min_us)) {
    s_lock_err_min_us = err;
  }
  if ((s_lock_frames == 0) || (err > s_lock_err_max_us)) {
    s_lock_err_max_us = err;
  }
  s_lock_frames++;
}

void
presto_beebjit_lock(const char* p_arg) {
  if ((p_arg != NULL) && (*p_arg != '\0')) {
    s_lock_lead_us = atoi(p_arg);
  }
  (void) printf("LOCK lead=%ld panel=%lu us err=%ld (min %ld max %ld over %lu frames) "
                "rows %lu: late %lu, near %lu, lines from the scan min %ld avg %ld max %ld; "
                "a frame's rows span %lu/%lu/%lu us, first at scan line %ld, over %lu frames\n",
                (long) s_lock_lead_us,
                (unsigned long) presto_lcd_frame_period_us(),
                (long) s_lock_err_us,
                (long) s_lock_err_min_us,
                (long) s_lock_err_max_us,
                (unsigned long) s_lock_frames,
                (unsigned long) s_lock_rows,
                (unsigned long) s_lock_late_rows,
                (unsigned long) s_lock_near_rows,
                (long) s_lock_margin_min,
                (long) (s_lock_rows ? (s_lock_margin_sum / (int64_t) s_lock_rows) : 0),
                (long) s_lock_margin_max,
                (unsigned long) s_lock_span_min_us,
                (unsigned long) (s_lock_span_frames ? (s_lock_span_sum_us / s_lock_span_frames) : 0),
                (unsigned long) s_lock_span_max_us,
                (long) (s_lock_span_frames ? (s_lock_first_scan_sum / (int32_t) s_lock_span_frames) : 0),
                (unsigned long) s_lock_span_frames);
  s_lock_span_frames = 0;
  s_lock_span_sum_us = 0;
  s_lock_first_scan_sum = 0;
  s_lock_frames = 0;
  s_lock_late_rows = 0;
  s_lock_near_rows = 0;
  s_lock_rows = 0;
}

void
beebjit_host_sleep_us(uint64_t us) {
  uint64_t t0;
  uint64_t end;
  if (s_turbo) {
    /* Every frame still emulated and drawn, just no waiting. */
    s_turbo_time_us += us;
    return;
  }
  /* While waiting: the USB channel and the network. */
  t0 = time_us_64();
  end = (t0 + us);
  presto_machine_poll();
  while (1) {
    uint64_t now = time_us_64();
    if (now >= end) {
      break;
    }
    sleep_us(((end - now) > 1000) ? 1000 : (end - now));
    presto_machine_poll();
    watchdog_update();
  }
  s_slept_us += (time_us_64() - t0);
}

int
beebjit_host_term_read(uint8_t* p_byte) {
#if PRESTO_BEEBJIT_TERMINAL
  int c = getchar_timeout_us(0);
  if (c >= 0) {
    *p_byte = (uint8_t) c;
    return 1;
  }
#endif
  (void) p_byte;
  return 0;
}

void
beebjit_host_term_write(uint8_t byte) {
  putchar_raw(byte);
}

/* The PSRAM (8MB at 0x11000000, through the cached XIP window) is split: the
 * thumb2 JIT's fixed region at the bottom (4MB of code, then its no-code and
 * stub pages; see asm/asm_platform.h), beebjit's big allocations above, and
 * at the top the disc RAM (presto_psram.h: disc images arriving from the SD
 * card or the web) and a snapshot after it.
 */
#define PRESTO_JIT_REGION_START 0x11000000u
#define PRESTO_JIT_REGION_END   0x11410000u
#define PRESTO_DISC_RAM_SIZE    (PRESTO_DISC_MAX + 0x10000)

uint8_t*
presto_disc_ram(void) {
  size_t size = presto_psram_size();
  if ((presto_psram() == NULL) || (size < (8u << 20))) {
    return NULL;
  }
  return (presto_psram() + size - PRESTO_DISC_RAM_SIZE);
}

void*
beebjit_host_fixed_region(void* p_addr, size_t size) {
  uintptr_t start = (uintptr_t) p_addr;
  if ((presto_psram() == NULL) ||
      (presto_psram_size() < (8u << 20)) ||
      (start < PRESTO_JIT_REGION_START) ||
      ((start + size) > PRESTO_JIT_REGION_END)) {
    return NULL;
  }
  return p_addr;
}

size_t
beebjit_host_big_region(uint8_t** p_base) {
  size_t size = presto_psram_size();
  if ((presto_psram() == NULL) || (size < (8u << 20))) {
    *p_base = presto_psram();
    return size;
  }
  *p_base = (uint8_t*) PRESTO_JIT_REGION_END;
  return (size - (PRESTO_JIT_REGION_END - PRESTO_JIT_REGION_START) -
          PRESTO_DISC_RAM_SIZE);
}

/* ---- Clocks ---- */

/* Slow the flash interface before raising the clock: on RP2350 nothing else
 * sets it for the app, so it would run at clk_sys/2. Runs from RAM.
 */
static void __no_inline_not_in_flash_func(slow_flash_timing)(void) {
  const uint clkdiv = 3;
  const uint rxdelay = 2;
  hw_write_masked(&qmi_hw->m[0].timing,
                  ((clkdiv << QMI_M0_TIMING_CLKDIV_LSB) &
                   QMI_M0_TIMING_CLKDIV_BITS) |
                  ((rxdelay << QMI_M0_TIMING_RXDELAY_LSB) &
                   QMI_M0_TIMING_RXDELAY_BITS),
                  QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
}

static void
setup_clock(void) {
  slow_flash_timing();
  vreg_set_voltage(VREG_VOLTAGE_1_25);
  set_sys_clock_khz(PRESTO_SYS_CLOCK_KHZ, true);
}

/* ---- A bigger stack than SCRATCH_Y's 4K, for the emulator. ---- */

/* 8 KB: its high-water mark is ~3.2 KB over every scene (STACKS, after !mem). */
static uint32_t s_emulator_stack[8 * 1024 / 4] __attribute__((aligned(8)));

/* How much of a stack (bss, so zero at boot; grows down) has been touched:
 * from its first non-zero word to the top. For the SRAM audit (!mem).
 */
static uint32_t
stack_high_water(const uint32_t* p_stack, uint32_t words) {
  uint32_t i;
  for (i = 0; i < words; ++i) {
    if (p_stack[i] != 0) {
      break;
    }
  }
  return ((words - i) * 4);
}

void
presto_beebjit_print_stacks(void) {
  extern uint32_t __scratch_y_start__;
  extern uint32_t __StackTop;
  (void) printf("STACKS emulator %u of %u, core 1 %u of %u, main (scratch Y) %u of %u\n",
                (unsigned) stack_high_water(s_emulator_stack,
                                            count_of(s_emulator_stack)),
                (unsigned) sizeof(s_emulator_stack),
                (unsigned) stack_high_water(s_core1_stack,
                                            count_of(s_core1_stack)),
                (unsigned) sizeof(s_core1_stack),
                (unsigned) stack_high_water(&__scratch_y_start__,
                                            (&__StackTop - &__scratch_y_start__)),
                (unsigned) ((&__StackTop - &__scratch_y_start__) * 4));
}

static void __attribute__((naked, noinline))
call_on_stack(void (*p_func)(void), void* p_stack_top) {
  __asm volatile(
      "mov r2, sp\n"
      "mov sp, r1\n"
      "push {r2, lr}\n"
      "blx r0\n"
      "pop {r2, r3}\n"
      "mov sp, r2\n"
      "bx r3\n");
}

#ifndef PRESTO_BEEBJIT_PROF_CORE
#define PRESTO_BEEBJIT_PROF_CORE 0
#endif
/* ---- Sampling profiler (PRESTO_BEEBJIT_PROF=1; PRESTO_BEEBJIT_PROF_CORE=1
 * samples core 1 instead) ----
 * A timer interrupt on core 0 records the address it interrupted, at a
 * jittered ~10 kHz, starting 30 s after boot. Once full, the samples are
 * printed as "#p <hex>" lines; map them to functions with the ELF (the
 * port diary has the command).
 */
#ifndef PRESTO_BEEBJIT_PROF
#define PRESTO_BEEBJIT_PROF 0
#endif
#if PRESTO_BEEBJIT_PROF
#define PROF_SAMPLES 8192
#define PROF_MAGIC 0x50524F46u
#define PROF_PERIOD_US 100
#ifndef PROF_START_US
#define PROF_START_US 30000000   /* well after Elite has loaded */
#endif
/* In the disc RAM (PSRAM), idle while profiling: SRAM is short. */
static uint32_t* s_prof;
static volatile uint32_t s_prof_count;
static int s_prof_alarm = -1;
static uint32_t s_prof_jitter = 0x12345678;

static void __not_in_flash_func(prof_arm)(void) {
  s_prof_jitter = (s_prof_jitter * 1664525u) + 1013904223u;
  timer_hw->alarm[s_prof_alarm] = (timer_hw->timerawl + (PROF_PERIOD_US / 2) +
                                   (s_prof_jitter % PROF_PERIOD_US));
}

void __not_in_flash_func(prof_sample)(uint32_t* p_frame) {
  timer_hw->intr = (1u << s_prof_alarm);
  /* The exception frame: r0-r3, r12, lr, pc, xpsr. */
  if (s_prof_count < PROF_SAMPLES) {
    s_prof[s_prof_count++] = p_frame[6];
    s_prof[PROF_SAMPLES + 1] = s_prof_count;
    s_prof[PROF_SAMPLES] = PROF_MAGIC;
    prof_arm();
  }
}

static void __attribute__((naked)) __not_in_flash_func(prof_isr)(void) {
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "b prof_sample\n");
}

/* The samples sit in PSRAM, with a header after them, and are printed at
 * the next boot too: a run that hangs (and so reboots by the watchdog) still
 * reports what it was doing.
 */

static void
prof_dump_previous(void) {
  uint32_t* p_prof = (uint32_t*) presto_disc_ram();
  uint32_t i;
  if ((p_prof == NULL) || (p_prof[PROF_SAMPLES] != PROF_MAGIC)) {
    return;
  }
  (void) printf("#prof %lu (from the previous boot)\n",
                (unsigned long) p_prof[PROF_SAMPLES + 1]);
  for (i = 0; i < p_prof[PROF_SAMPLES + 1]; ++i) {
    (void) printf("#p %08lx\n", (unsigned long) p_prof[i]);
  }
  (void) printf("#prof end\n");
  p_prof[PROF_SAMPLES] = 0;
}

static void
prof_init(void) {
  uint irq;
  s_prof = (uint32_t*) presto_disc_ram();
  if (s_prof == NULL) {
    return;
  }
  prof_dump_previous();
  s_prof[PROF_SAMPLES] = 0;
  s_prof_alarm = hardware_alarm_claim_unused(true);
  irq = hardware_alarm_get_irq_num(s_prof_alarm);
  irq_set_exclusive_handler(irq, prof_isr);
  irq_set_priority(irq, 0);
  hw_set_bits(&timer_hw->inte, (1u << s_prof_alarm));
  irq_set_enabled(irq, true);
  timer_hw->alarm[s_prof_alarm] = (timer_hw->timerawl + PROF_START_US);
}

/* "!prof": sample again, from now (the boot-time run's samples are gone);
 * "!profdump": "PROF <n>" and the samples as raw little endian words, for
 * tools/prof.py.
 */
static int s_prof_by_command;

void
presto_prof_restart(void) {
  if ((s_prof == NULL) || (s_prof_alarm < 0)) {
    (void) printf("PROF none\n");
    return;
  }
  s_prof_by_command = 1;
  s_prof_count = 0;
  s_prof[PROF_SAMPLES] = 0;
  prof_arm();
}

void
presto_prof_dump(void) {
  uint32_t n = ((s_prof != NULL) ? s_prof_count : 0);
  (void) printf("PROF %lu\n", (unsigned long) n);
  (void) fflush(stdout);
  stdio_set_translate_crlf(&stdio_usb, false);
  (void) fwrite(s_prof, 4, n, stdout);
  (void) fflush(stdout);
  stdio_set_translate_crlf(&stdio_usb, true);
}

static void
prof_poll(void) {
  static int s_printed;
  uint32_t i;
  if (s_printed || s_prof_by_command || (s_prof_count < PROF_SAMPLES)) {
    return;
  }
  s_printed = 1;
  s_prof[PROF_SAMPLES] = 0;
  (void) printf("#prof %lu\n", (unsigned long) s_prof_count);
  for (i = 0; i < s_prof_count; ++i) {
    (void) printf("#p %08lx\n", (unsigned long) s_prof[i]);
  }
  (void) printf("#prof end\n");
}
#endif

/* ---- Frame statistics ---- */

/* Until the web panel's code disarms the boot guard (presto_net_frame, at
 * 100 frames), keep its watchdog fed.
 */
#define PRESTO_BEEBJIT_GUARD_FRAMES 100

/* Free SRAM as the web panel counts it (presto_net.c): free heap blocks plus
 * the unclaimed space between the heap and the stack.
 */
static uint32_t
free_ram(void) {
  extern char __StackLimit;
  struct mallinfo m = mallinfo();
  return (uint32_t) (m.fordblks + (uint32_t) (&__StackLimit - (char*) sbrk(0)));
}

static uint32_t s_frames;
static uint64_t s_stats_time_us;
static uint64_t s_stats_slept_us;
static uint64_t s_stats_cycles;
#if PRESTO_BEEBJIT_WIRE
static uint64_t s_stats_core1_busy_cycles;
static uint64_t s_stats_core0_wait_us;
#endif

/* ---- The figures for the web panel and !stats: each second ---- */

static uint32_t s_perf_core0;
static uint32_t s_perf_core1;
static uint32_t s_perf_speed;
static bool s_overlay;

static void
perf_frame(uint64_t cycles) {
  static uint32_t s_perf_frames;
  static uint64_t s_time_us;
  static uint64_t s_slept;
  static uint64_t s_cycles;
#if PRESTO_BEEBJIT_WIRE
  static uint64_t s_core1_busy;
  static uint64_t s_core0_wait;
#endif
  uint64_t now;
  uint64_t elapsed;
  if (++s_perf_frames < 50) {
    return;
  }
  s_perf_frames = 0;
  now = time_us_64();
  elapsed = (now - s_time_us);
  /* (A power-on reset starts the cycle count again.) */
  if ((s_time_us != 0) && (elapsed != 0) && (cycles >= s_cycles)) {
    uint64_t busy = (elapsed - (s_slept_us - s_slept));
#if PRESTO_BEEBJIT_WIRE
    uint64_t core0_wait = (s_core0_wait_us - s_core0_wait);
    busy -= core0_wait;
    s_perf_core1 = (uint32_t) ((((s_core1_busy_cycles - s_core1_busy) /
                                 (PRESTO_SYS_CLOCK_KHZ / 1000)) * 100) /
                               elapsed);
#endif
    s_perf_core0 = (uint32_t) ((busy * 100) / elapsed);
    /* 2MHz is 100%. */
    s_perf_speed = (uint32_t) (((cycles - s_cycles) * 50) / elapsed);
  }
  s_time_us = now;
  s_slept = s_slept_us;
  s_cycles = cycles;
#if PRESTO_BEEBJIT_WIRE
  s_core1_busy = s_core1_busy_cycles;
  s_core0_wait = s_core0_wait_us;
#endif
}

void
presto_perf_get(uint* p_core0, uint* p_core1, uint* p_speed) {
  *p_core0 = s_perf_core0;
  *p_core1 = s_perf_core1;
  *p_speed = s_perf_speed;
}

bool
presto_turbo_get(void) {
  return s_turbo;
}

void
presto_turbo_set(bool on) {
  s_turbo = on;
}

bool
presto_overlay_get(void) {
  return s_overlay;
}

void
presto_overlay_set(bool on) {
  s_overlay = on;
}

void
presto_beebjit_print_stats(void) {
  (void) printf("STATS frames=%lu late=0 lcd_frames=%lu avg_busy_us=0 "
                "core0=%lu%% core1=%lu%% speed=%lu%%%s\n",
                (unsigned long) s_frames,
                (unsigned long) presto_lcd_frame_count(),
                (unsigned long) s_perf_core0,
                (unsigned long) s_perf_core1,
                (unsigned long) s_perf_speed,
                (s_turbo ? " (turbo)" : ""));
}

static void
frame_callback(void* p, uint64_t cycles) {
  uint64_t now;
  uint64_t elapsed;
  (void) p;

  s_frames++;
  /* The boot guard's watchdog until presto_net_frame() disarms it at
   * PRESTO_BEEBJIT_GUARD_FRAMES; then one of our own for good: a hang
   * (no frame, no idle poll for 8 s) reboots, so the board is always
   * reachable to reflash.
   */
  if (s_frames == (PRESTO_BEEBJIT_GUARD_FRAMES + 1)) {
    watchdog_enable(8000, 1);
  }
  watchdog_update();
  perf_frame(cycles);
#if PRESTO_BEEBJIT_SOUND
  presto_audio_frame(cycles, s_turbo);
#endif
#if PRESTO_BEEBJIT_AMBIENT
  /* CAPS LOCK and SHIFT LOCK are lit when their IC32 latch bits are low. */
  presto_ambient_frame((uint8_t) (((IC32 & 0x40) ? 0 : 1) | ((IC32 & 0x80) ? 0 : 2)));
#endif
  presto_machine_frame();
#if PRESTO_BEEBJIT_PROF
  prof_poll();
#endif
  if ((PRESTO_BEEBJIT_STATS_FRAMES == 0) ||
      ((s_frames % PRESTO_BEEBJIT_STATS_FRAMES) != 0)) {
    return;
  }
  now = time_us_64();
  elapsed = (now - s_stats_time_us);
  if (s_stats_time_us != 0) {
    uint64_t busy = (elapsed - (s_slept_us - s_stats_slept_us));
#if PRESTO_BEEBJIT_WIRE
    uint64_t core1_busy = ((s_core1_busy_cycles - s_stats_core1_busy_cycles) /
                           (PRESTO_SYS_CLOCK_KHZ / 1000));
    uint64_t core0_wait = (s_core0_wait_us - s_stats_core0_wait_us);
    (void) printf("#stats frames %lu: %.2f MHz, core 0 busy %lu%% "
                  "(waiting for core 1 %lu%%), core 1 busy %lu%%, free RAM %lu KB\n",
                  (unsigned long) s_frames,
                  ((cycles - s_stats_cycles) / (double) elapsed),
                  (unsigned long) (((busy - core0_wait) * 100) / elapsed),
                  (unsigned long) ((core0_wait * 100) / elapsed),
                  (unsigned long) ((core1_busy * 100) / elapsed),
                  (unsigned long) (free_ram() / 1024));
#else
    (void) printf("#stats frames %lu: %.2f MHz, core 0 busy %lu%%, free RAM %lu KB\n",
                  (unsigned long) s_frames,
                  ((cycles - s_stats_cycles) / (double) elapsed),
                  (unsigned long) ((busy * 100) / elapsed),
                  (unsigned long) (free_ram() / 1024));
#endif
  }
#if PRESTO_BEEBJIT_WIRE
  s_stats_core1_busy_cycles = s_core1_busy_cycles;
  s_stats_core0_wait_us = s_core0_wait_us;
#endif
  {
    /* The XIP cache (flash and PSRAM, shared by both cores): JIT code and
     * inturbo run from PSRAM through it.
     */
    uint32_t acc = xip_ctrl_hw->ctr_acc;
    uint32_t hit = xip_ctrl_hw->ctr_hit;
    if ((s_stats_time_us != 0) && (acc != 0)) {
      (void) printf("#xip %lu accesses a frame, hit %lu.%lu%%, %lu misses a frame\n",
                    (unsigned long) (acc / PRESTO_BEEBJIT_STATS_FRAMES),
                    (unsigned long) (((uint64_t) hit * 100) / acc),
                    (unsigned long) ((((uint64_t) hit * 1000) / acc) % 10),
                    (unsigned long) ((acc - hit) / PRESTO_BEEBJIT_STATS_FRAMES));
    }
    xip_ctrl_hw->ctr_acc = 0;
    xip_ctrl_hw->ctr_hit = 0;
  }
#if PRESTO_BEEBJIT_PANEL_PIO
  if (s_pack_timeouts != 0) {
    (void) printf("#panel PIO timeouts %lu\n", (unsigned long) s_pack_timeouts);
  }
#endif
  s_stats_time_us = now;
  s_stats_slept_us = s_slept_us;
  s_stats_cycles = cycles;
}

static size_t s_psram;

static void
run_beebjit(void) {
  struct bbc_struct* p_bbc;
  struct beebjit_embedded_config config;

  (void) memset(&config, '\0', sizeof(config));
  config.p_os_rom = "roms/os12.rom";
  config.p_rom_names[15] = "roms/basic2.rom";
  config.p_rom_names[14] = "roms/dfs09.rom";
  /* 64K of sideways RAM in sockets 4-7, as a RAM board gives a Model B:
   * demos load music and data into it (BEEB-NICCC's music, for one).
   */
  config.is_sideways_ram[4] = 1;
  config.is_sideways_ram[5] = 1;
  config.is_sideways_ram[6] = 1;
  config.is_sideways_ram[7] = 1;
  config.terminal_flag = PRESTO_BEEBJIT_TERMINAL;
#ifdef PRESTO_BEEBJIT_DISC
  config.p_disc_names[0] = PRESTO_BEEBJIT_DISC;
  config.autoboot_flag = 1;
#endif
  config.p_opt_flags = "video:border-chars=0,video:no-deinterlace-bitmap"
#if PRESTO_BEEBJIT_FRAME_SKIP
                       ",video:frame-skip"
#endif
                       ;
#if PRESTO_BEEBJIT_JIT
  config.cpu_mode = k_cpu_mode_jit;
#endif
#if PRESTO_BEEBJIT_FAST
  /* Unthrottled, to measure the top speed. */
  config.fast_flag = 1;
#endif
  config.p_log_flags = "";
#if PRESTO_BEEBJIT_PANEL_PIO
  config.p_row_callback = panel_pio_row;
  /* The DMA reads the row the renderer gave it while the next is drawn. */
  config.is_row_double_buffered = 1;
#else
  config.p_row_callback = beebjit_panel_row;
#endif
  config.p_row_callback_object = &s_panel;
  config.p_frame_callback = frame_callback;

  presto_boot_step(4);
#if PRESTO_BEEBJIT_WIRE
#if PRESTO_BEEBJIT_SOUND
  presto_audio_set_ring(beebjit_embedded_malloc_big(presto_audio_ring_size()));
#endif
  beebjit_wire_init(s_wire_buf, 0);
  g_beebjit_wire.p_wait_hook = wire_wait_hook;
  g_beebjit_wire.p_vsync_hook = lock_frame;
#endif
  p_bbc = beebjit_embedded_create(&config);
#if PRESTO_BEEBJIT_WIRE
  if (p_bbc != NULL) {
    multicore_launch_core1_with_stack(core1_main,
                                      s_core1_stack,
                                      sizeof(s_core1_stack));
  }
#endif
  presto_boot_step(5);
  if (p_bbc == NULL) {
    (void) printf("#beebjit_embedded_create failed\n");
  } else {
    size_t big_used;
    size_t big_peak;
    size_t small_total;
    beebjit_embedded_get_alloc_stats(&big_used, &big_peak, &small_total);
    (void) printf("#memory: %lu bytes in SRAM, %lu in PSRAM\n",
                  (unsigned long) small_total,
                  (unsigned long) big_used);
#ifdef PRESTO_BEEBJIT_DISC
    presto_machine_init(p_bbc, PRESTO_BEEBJIT_DISC);
#else
    presto_machine_init(p_bbc, NULL);
#endif
    (void) beebjit_embedded_run(p_bbc);
    (void) printf("#beebjit exited\n");
  }
}

int
main(void) {
  uint32_t i;

  presto_boot_begin();   /* reports instead, if the last boot died */
  setup_clock();
  stdio_init_all();
#if PRESTO_BEEBJIT_LOG_ALLOCS
  /* Diagnosis (build with -DPRESTO_BEEBJIT_LOG_ALLOCS=1): wait for the USB
   * console, then log every allocation, with the SRAM heap's state. */
  while (!stdio_usb_connected() && (to_ms_since_boot(get_absolute_time()) < 6000)) {
    sleep_ms(10);
  }
  beebjit_embedded_log_allocs = 1;
#endif
  presto_boot_step(1);

  s_psram = presto_psram_init();
  presto_boot_step(2);

  beebjit_panel_init(&s_panel, s_fb, k_beebjit_panel_rows);
  /* 256 BBC lines over 480 panel rows. */
  for (i = 0; i < PRESTO_LCD_HEIGHT; ++i) {
    s_row_table[i] = (const uint16_t*) s_fb[(i * 256) / PRESTO_LCD_HEIGHT];
  }
  s_row_table[PRESTO_LCD_HEIGHT] = NULL;
  presto_lcd_init(s_row_table);
#if PRESTO_BEEBJIT_PANEL_PIO
#if PRESTO_BEEBJIT_AMBIENT
  /* First: it moves PIO0 to GPIO 16-47, which it can only do before PIO0 has a program. */
  presto_ambient_init();
#endif
  panel_pio_init();
#endif
  presto_lcd_set_backlight(255);
  presto_boot_step(3);

  (void) printf("#beebjit on Presto: %lu MHz, PSRAM %lu KB\n",
                (unsigned long) (clock_get_hz(clk_sys) / 1000000),
                (unsigned long) (s_psram / 1024));

#if PRESTO_BEEBJIT_PROF && !PRESTO_BEEBJIT_PROF_CORE
  prof_init();
#endif
  call_on_stack(run_beebjit,
                &s_emulator_stack[count_of(s_emulator_stack)]);

  while (1) {
    tight_loop_contents();
  }
}
