// esp32-beeb: Hitachi 6845 CRTC, stepped once per 2 MHz cycle.
//
// Plain C, no ESP headers. This is beebjit's video.c CRTC state machine
// (video_advance_crtc_timing and friends) restated as a per-tick stepper:
// the same counters, latches and quirks, in the same order, so that the
// trace diff against beebjit holds through vsync. The pixel side is behind
// the CRTC_* hook macros; crtc.c itself only produces the fetched data byte,
// DISPEN, cursor and sync events.
//
// Timing: in 1 MHz mode (video ULA clock bit clear) the counters step on odd
// system ticks and the fetch happens on even ones; in 2 MHz mode both happen
// every tick.
//
// Hooks the including translation unit defines (defaults do nothing):
//   CRTC_FETCH(c, ma, ra, odd)   returns the display byte for MA/RA
//   CRTC_RENDER(c, data, dispen, odd, ma13)  pixel output for one tick
//   CRTC_HSYNC(c, width)         start of horizontal sync pulse
//   CRTC_VSYNC(c, level)         vsync level change (system VIA CA1)
//   CRTC_CURSOR(c)               cursor shown at this character
//   CRTC_TELETEXT(c, data, dispen)  teletext data pipeline, even ticks
//   CRTC_RA(c, ra, isv)          external RA changed (teletext/renderer)

#ifndef CRTC_H
#define CRTC_H

#include <stdint.h>

enum {
    CRTC_R0_HTOTAL = 0, CRTC_R1_HDISP, CRTC_R2_HSYNC, CRTC_R3_SYNCW,
    CRTC_R4_VTOTAL, CRTC_R5_VADJ, CRTC_R6_VDISP, CRTC_R7_VSYNC,
    CRTC_R8_INTERLACE, CRTC_R9_MAXRA, CRTC_R10_CSTART, CRTC_R11_CEND,
    CRTC_R12_MAH, CRTC_R13_MAL, CRTC_R14_CAH, CRTC_R15_CAL,
    CRTC_R16_LPH, CRTC_R17_LPL, CRTC_NUM_REGS,
};

struct crtc {
    uint8_t reg[CRTC_NUM_REGS];
    uint8_t addr_reg;

    // Derived from registers.
    uint8_t is_interlace, is_isv;        // R8 bit 0; R8 bits 0,1 both
    uint8_t scanline_stride, scanline_mask;
    uint8_t hsync_width, vsync_width;
    uint8_t half_r0;
    uint8_t cursor_disabled, cursor_flashing, cursor_flash_mask, cursor_start_line;
    uint8_t skew_dispen_index, cursor_skew;

    // From the video ULA and IC32, kept here by the machine.
    uint8_t ula_clock_fast;              // ULA control bit 4
    uint8_t ula_teletext;                // ULA control bit 1
    uint16_t screen_wrap_add;

    // Counters and latches.
    uint32_t frames;
    uint8_t is_odd_frame;
    uint8_t horiz_counter;               // C0
    uint8_t scanline_counter;            // C9 (external RA before ISV shift)
    uint8_t vert_counter;                // C4
    uint8_t vert_adjust_counter;         // C5
    uint8_t vsync_scanline_counter;
    uint8_t hsync_tick_counter;
    uint16_t address_counter;            // MA
    uint16_t address_counter_saved;
    uint8_t is_vert_adjust_pending, is_in_vert_adjust;
    uint8_t in_vsync, is_even_vsync, is_odd_vsync, in_hsync, in_dummy_raster;
    uint8_t had_odd_vsync_this_row, had_even_vsync_this_row;
    uint8_t display_enable_bits;         // bit 0 horiz, bit 1 vert
    uint8_t has_hit_cursor_line_start, has_hit_cursor_line_end;
    uint8_t is_end_of_main_latched, is_end_of_vert_adjust_latched;
    uint8_t is_end_of_frame_latched;
    uint8_t start_of_line_state_checks;
    uint8_t is_first_frame_scanline;
    int8_t cursor_skew_counter;
    uint8_t dispen_shifts[4];
    uint8_t r4_hit, r5_hit, r9_hit;      // latched per scanline, as beebjit
    uint8_t data_byte;
    uint8_t data_ma13;                   // MA13 of the last fetch
    uint8_t vsync_changed;               // set by crtc_update_vsync, cleared by crtc_run
    uint32_t stat_fast, stat_slow;
    uint64_t stat_clk_fast, stat_clk_slow, stat_clk_plan;   // CRTC_CYCLES() in each
    uint32_t stat_runs, stat_slow_why[8];   // fast runs; exact ticks by cause (see crtc_run)       // ticks by path, with MACHINE_STATS
};

void crtc_reset(struct crtc *c);
void crtc_write(struct crtc *c, int addr, uint8_t val);   // addr 0/1
uint8_t crtc_read(const struct crtc *c, int addr);
void crtc_set_ula(struct crtc *c, uint8_t ula_control);

#endif
