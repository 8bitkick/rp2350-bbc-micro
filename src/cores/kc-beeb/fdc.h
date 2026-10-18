// esp32-beeb: WD1770 floppy controller and the two drives on it.
//
// A port of beebjit's wd_fdc.c and disc_drive.c (GPL, see decisions.md):
// the controller sees the disc as pulses, one 64 us position at a time,
// recovers marks, bytes and CRCs from them, and runs its command state
// machine on each byte; its own timers do spin-up, seek steps, head
// settle and the 32 us command completion. Both are plain countdowns in
// 2 MHz cycles that the machine folds into its event scheduling:
// fdc_ticks_to_event() says how long until something fires,
// fdc_advance() moves time on and fires it. INTRQ and DRQ drive the NMI.
//
// Master register map: control at FE24 (reset, drive select, side,
// density), the 1770's own four registers at FE28 to FE2B.

#ifndef FDC_H
#define FDC_H

#include <stdint.h>

#include "disc.h"

struct fdc_drive {
    struct disc *disc;         // NULL: no disc
    int side;
    uint32_t track;            // physical, 0..DISC_TRACKS-1
    uint32_t head_position;    // position on the track, 0..length-1
    uint32_t pulse_position;   // 0 or 16: half a position, for MFM
    int is_32us;               // double density: half a position per tick
    int spinning;
    uint32_t timer;            // cycles to the next position, if spinning
    uint32_t timer_seq;        // when it was last (re)armed, see fdc_advance
};

struct fdc {
    struct fdc_drive drive[2];
    struct fdc_drive *cur;     // selected drive or NULL

    uint8_t control;
    uint8_t status;
    uint8_t track_reg;
    uint8_t sector_reg;
    uint8_t data_reg;
    int intrq, drq;
    int nmi;                   // the NMI line as last reported
    int do_raise_intrq;

    int is_index_pulse;
    int interrupt_on_index;
    int write_track_crc_second;
    uint8_t command;
    uint8_t command_type;
    int cmd_settle, cmd_write, cmd_verify, cmd_multi, cmd_deleted;
    uint32_t step_rate_ms;
    uint32_t state;
    uint32_t timer_state;      // which timer is running, 0 none
    uint32_t timer;            // cycles until it fires
    uint32_t timer_seq;
    uint32_t seq;              // arming order: beebjit fires ties oldest first
    uint32_t state_count;
    uint32_t index_pulse_count;
    uint64_t mark_detector;
    uint32_t data_shifter;
    uint32_t data_shift_count;
    uint8_t deliver_data;
    int deliver_is_marker;
    uint16_t crc;
    uint8_t on_disc_track, on_disc_sector;
    uint32_t on_disc_length;
    uint16_t on_disc_crc;
    int last_mfm_bit;

    // Set while a register write is being handled: timers started from a
    // write fire one cycle later than ones started from a callback (how
    // beebjit's timing lands relative to our write cycle; measured).
    int in_io;
    // The machine's NMI line callback: level 0 or 1.
    void (*set_nmi)(void *ctx, int level);
    void *ctx;
};

void fdc_init(struct fdc *f, void (*set_nmi)(void *, int), void *ctx);
void fdc_power_on_reset(struct fdc *f);
void fdc_break_reset(struct fdc *f);
// addr: 0..3 control (Master layout), 4..7 the 1770 registers.
uint8_t fdc_read(struct fdc *f, uint16_t addr);
void fdc_write(struct fdc *f, uint16_t addr, uint8_t val);
// Cycles until the next timer fires, or 0xffffffff.
// (defined by fdc.c as FDC_HOT_API, which the machine makes static inline in IRAM)
// Moves `n` cycles on (n at most fdc_ticks_to_event) and fires what is
// due. `now` is the cycle count after the advance, for the drive's noise.
void fdc_advance(struct fdc *f, uint32_t n, uint32_t now);

#endif
