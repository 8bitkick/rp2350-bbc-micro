// esp32-beeb: the BBC Master 128 as a machine around the 65C12 core.
//
// Plain C, no ESP headers. machine.c includes via.c and cpu6502.c into its
// own translation unit so the per-cycle hook and the I/O hooks inline into
// the core; the ESP wrapper and the host harness both just include machine.c
// after defining placement attributes.
//
// Memory (all RAM in internal SRAM on the board):
//   ram[32K]      0x0000-0x7FFF main RAM
//   lynne[20K]    0x3000-0x7FFF shadow RAM, selected by ACCCON X/E rules
//   andy[4K]      0x8000-0x8FFF when ROMSEL bit 7
//   hazel[8K]     0xC000-0xDFFF when ACCCON bit 3
//   swram[4]      16 KB sideways RAM for slots 4 to 7; the platform supplies
//                 the buffers (PSRAM on the board for now, see decisions)
//   rom[16]       16 KB images for the sideways slots (NULL = empty, reads 0)
//   mos           16 KB MOS 3.20
// Paging is pointer-table updates only (machine_remap_*), never copies.

#ifndef MACHINE_H
#define MACHINE_H

#include <stdint.h>
#include "cpu6502.h"
#include "via.h"
#include "crtc.h"
#include "fdc.h"
#include "vstream.h"

#define MACHINE_SWRAM_SLOTS 4   // slots 4..7

struct machine {
    struct cpu6502 cpu;
    uint32_t synced;   // cycle count the peripherals have been stepped to
    uint32_t sync_due; // next cycle the core must catch up at (see machine.c)
    uint16_t wlo, whi; // display addresses fetched before sync_due
    // Tuning statistics, counted only when MACHINE_STATS is defined.
    uint32_t stat_calls, stat_chunks;
    uint32_t stat_sync_boundary, stat_sync_write, stat_sync_io;
    uint32_t stat_due_hist[17];
    uint32_t stat_due_src[8];
    uint64_t stat_clk_advance, stat_clk_crtc, stat_clk_via, stat_clk_fdc, stat_clk_io, stat_clk_due, stat_clk_quiet, stat_clk_wr_outer, stat_clk_sync_outer;   // MACHINE_CYCLES()
    uint64_t stat_clk_sheila[8];   // register handling alone, by (addr >> 4) & 7 within SHEILA
    uint32_t stat_io_page[8];
    uint32_t stat_io_ticks, stat_zero_calls;   // IO_TICK advance calls; calls with nothing to run
    uint32_t stat_via_fast;   // VIA accesses through the quiet-window path
    uint32_t stat_crtc_deferred, stat_crtc_deferred_vsync;   // calls that left the CRTC behind; vsync changes found when catching it up (must be 0)   // which term set the due cycle: 0 cap, 1 via, 2 adc, 3 key, 4 crtc, 5 fdc, 6 nmi

    uint8_t ram[0x8000];
    uint8_t lynne[0x5000];
    uint8_t andy[0x1000];
    uint8_t hazel[0x2000];
    uint8_t *swram[MACHINE_SWRAM_SLOTS];   // 16 KB each, platform allocated
    const uint8_t *rom[16];
    const uint8_t *mos;
    uint8_t bitbucket[256];     // write target for ROM pages
    uint8_t empty_rom[256];     // read source for empty slots (zeros)

    // Page tables: [0] user code, [1] code fetched from 0xC000-0xDFFF.
    uint8_t *rd_tab[2][256];
    uint8_t *wr_tab[2][256];
    // Display memory for addresses 0x3000 and up: main RAM or LYNNE, indexed
    // by the full address (the pointer is pre-offset). Set with ACCCON.
    uint8_t *display_base;

    uint8_t romsel, acccon, ic32;
    uint8_t sound_we, sound_bus;   // the sound chip's write enable (IC32 bit 0 low) and the byte it last took

    struct via sysvia, usrvia;

    // Keyboard matrix: bit (row) set in keys[col] when the key is down.
    uint8_t keys[16];
    // Scheduled key events, applied by the catch-up at their exact cycle
    // (in order of cycle). machine_queue_key() inserts; a 32-bit cycle count.
    struct machine_key_event { uint32_t cycle; uint8_t row, col, down; } key_events[256];
    uint16_t key_head, key_count;

    // CMOS/RTC (HD146818) as driven through the system VIA.
    uint8_t cmos_ram[64];
    uint8_t cmos_addr;
    uint8_t cmos_enabled, cmos_strobe, cmos_data, cmos_read;

    // Serial: ACIA control and the serial ULA control byte. The ACIA status
    // is derived: TDRE unless CTS, and CTS is high whenever RS423 is
    // selected (nothing is on the other end). No tape, so DCD stays low.
    uint8_t acia_ctrl, serial_ula;

    // ADC (uPD7002): a write to the control register starts a conversion of
    // 4 ms (8 bit) or 10 ms (12 bit); completion drops system VIA CB1.
    uint8_t adc_ctrl, adc_busy, adc_ready;
    uint32_t adc_countdown;   // 2 MHz cycles

    struct crtc crtc;
    uint8_t ula_ctrl, ula_pal[16];
    struct vstream *vs;        // video stream to the renderer, or NULL

    // The 1770 and its two drives; discs are attached with machine_set_disc.
    struct fdc fdc;
    // An NMI edge waiting to be handed to the core (which runs on a copy of
    // its state, so the machine cannot poke it directly): the cycle it
    // happened on. Delivered by the catch-up hooks like the IRQ line.
    uint8_t nmi_edge;
    uint8_t irq_late;    // a key event moved the IRQ level after its cycle was recorded
    // The tick the CRTC has reached: at most `synced`. A register access
    // to a device other than the CRTC, the ULA or paging leaves the CRTC
    // behind when no vsync change can fall in the window (machine.c).
    uint32_t crtc_synced;
    uint32_t win_due;    // the sync point the display write window was computed for
    uint8_t quiet_ok;    // at the last full schedule: the 1770 and ADC idle (see io_quiet_window)
    uint32_t nmi_cycle;
};

extern uint8_t machine_profile_skip;   // measurement only, see machine.c
void machine_init(struct machine *m);  // zero state, build tables, VIAs
void machine_reset(struct machine *m); // power-on reset of CPU and VIAs
// Runs until the machine's total cycle count reaches `until`.
uint64_t machine_run(struct machine *m, uint64_t until);
// Presses or releases a key of the BBC matrix (row 0-7, column 0-15); the
// system VIA sees it at once.
void machine_set_key(struct machine *m, int row, int col, int down);
// Puts a disc (disc.h) in drive 0 or 1, or NULL to take it out.
void machine_set_disc(struct machine *m, int drive, struct disc *d);
// Holds shift from power on and releases it at cycle 3000000, as beebjit's
// -autoboot does, so a disc's !BOOT runs.
void machine_autoboot(struct machine *m);
// Schedules a key change for an exact cycle count, kept in cycle order (one
// queued for a cycle already past is never applied). Returns 0 if the
// queue is full.
int machine_queue_key(struct machine *m, uint32_t cycle, int row, int col, int down);
// an analogue input (the joystick port: channels 0-3, 0..0xFFFF, 0x8000 centred), from its next conversion
void machine_set_analogue(struct machine *m, int channel, uint16_t value);

#endif
