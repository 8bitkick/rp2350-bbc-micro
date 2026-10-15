// esp32-beeb: WD1770 and drives. See fdc.h. Port of beebjit's wd_fdc.c
// and disc_drive.c; the EMU NOTEs there are the reasons for the odd bits.

#include <string.h>

#include "fdc.h"

#ifndef FDC_API
#define FDC_API
#endif

enum {
    CMD_RESTORE = 0x00, CMD_SEEK = 0x10,
    CMD_STEP_IN = 0x40, CMD_STEP_IN_UPDATE = 0x50,
    CMD_STEP_OUT = 0x60, CMD_STEP_OUT_UPDATE = 0x70,
    CMD_READ_SECTOR = 0x80, CMD_READ_SECTOR_MULTI = 0x90,
    CMD_WRITE_SECTOR = 0xa0, CMD_WRITE_SECTOR_MULTI = 0xb0,
    CMD_READ_ADDRESS = 0xc0, CMD_FORCE_INTERRUPT = 0xd0,
    CMD_READ_TRACK = 0xe0, CMD_WRITE_TRACK = 0xf0,
};
enum {
    CMDBIT_MULTI = 0x10, CMDBIT_NO_SPIN_UP = 0x08, CMDBIT_VERIFY = 0x04,
    CMDBIT_SETTLE = 0x04, CMDBIT_DELETED = 0x01,
};
enum {
    CTRL_RESET = 0x20, CTRL_DENSITY = 0x08, CTRL_SIDE = 0x04,
    CTRL_DRIVE_1 = 0x02, CTRL_DRIVE_0 = 0x01,
};
enum {
    ST_MOTOR_ON = 0x80, ST_WRITE_PROTECT = 0x40, ST_SPIN_UP_DONE = 0x20,
    ST_DELETED_MARK = 0x20, ST_RNF = 0x10, ST_CRC_ERROR = 0x08,
    ST_TRACK_0 = 0x04, ST_LOST_BYTE = 0x04, ST_INDEX = 0x02, ST_DRQ = 0x02,
    ST_BUSY = 0x01,
};
enum {
    S_NULL = 0, S_IDLE, S_TIMER_WAIT, S_SPIN_UP_WAIT, S_WAIT_INDEX,
    S_SEARCH_ID, S_IN_ID, S_SEARCH_DATA, S_IN_DATA, S_IN_READ_TRACK,
    S_WRITE_SECTOR_DELAY, S_WRITE_SECTOR_LEAD_IN_FM, S_WRITE_SECTOR_LEAD_IN_MFM,
    S_WRITE_SECTOR_MARKER_FM, S_WRITE_SECTOR_MARKER_MFM, S_WRITE_SECTOR_BODY,
    S_WRITE_TRACK_SETUP, S_IN_WRITE_TRACK, S_CHECK_MULTI, S_DONE,
};
enum { T_NONE = 0, T_SETTLE, T_SEEK, T_DONE };

#define SETTLE_MS 30
#define INDEX_MS 4

// ---- drives (disc_drive.c) ----

static uint32_t drive_track_length(const struct fdc_drive *d)
{
    if (d->disc) {
        return d->disc->length[d->side][d->track];
    }
    return DISC_BYTES_PER_TRACK;
}

// beebjit's double arithmetic, step for step, so the tick counts match.
static double position_fraction(uint32_t length, uint32_t head, uint32_t pulse)
{
    double ret = head;
    ret += pulse / (double)32;
    ret = ret / length;
    return ret;
}

static uint32_t time_for_position_double(uint32_t length, uint32_t head, uint32_t pulse)
{
    double fraction = position_fraction(length, head, pulse);
    fraction *= DISC_TICKS_PER_REV;
    return (uint32_t)fraction;
}

// The drive tick computes two positions' times and asks whether the head
// is in the index pulse, all in doubles: on the ESP32-S3, which has no
// double-precision FPU, that was 14 soft-float calls per 64 us tick. For
// the standard track length a position's time is the exact quotient
// (head * 32 + pulse) * K, K = ticks per revolution / (32 * length), an
// integer here, or one less where the doubles round down; which positions
// round down, and the first head past the index pulse, are worked out
// once with the same double expressions, so the results are identical.
// pulse is 0 or 16, so a position is head * 2 + pulse / 16.
#define TIMING_K (DISC_TICKS_PER_REV / (32 * DISC_BYTES_PER_TRACK))
#define TIMING_POSITIONS (DISC_BYTES_PER_TRACK * 2 + 2)
static uint8_t timing_round_down[(TIMING_POSITIONS + 7) / 8];
static uint32_t timing_index_heads;   // heads below this are in the index pulse
static int timing_ok;

static void timing_init(void)
{
    if (timing_ok || DISC_TICKS_PER_REV % (32 * DISC_BYTES_PER_TRACK) != 0) {
        return;
    }
    for (uint32_t i = 0; i < TIMING_POSITIONS; i++) {
        uint32_t head = i >> 1, pulse = (i & 1) * 16;
        uint32_t exact = (head * 32 + pulse) * TIMING_K;
        uint32_t t = time_for_position_double(DISC_BYTES_PER_TRACK, head, pulse);
        if (t == exact - 1) {
            timing_round_down[i >> 3] |= (uint8_t)(1 << (i & 7));
        } else if (t != exact) {
            return;   // not of that form: keep the doubles
        }
    }
    uint32_t h = 0;
    while (h < (DISC_BYTES_PER_TRACK * (INDEX_MS / (double)200))) {
        h++;
    }
    timing_index_heads = h;
    timing_ok = 1;
}

static uint32_t time_for_position(uint32_t length, uint32_t head, uint32_t pulse)
{
    if (length == DISC_BYTES_PER_TRACK && timing_ok) {
        uint32_t i = head * 2 + (pulse >> 4);
        return (head * 32 + pulse) * TIMING_K - ((timing_round_down[i >> 3] >> (i & 7)) & 1);
    }
    return time_for_position_double(length, head, pulse);
}

static uint32_t drive_quasi_random_pulses(uint32_t ticks)
{
    uint8_t fm = (uint8_t)ticks;
    fm ^= (uint8_t)(ticks >> 8);
    fm ^= (uint8_t)(ticks >> 16);
    fm ^= (uint8_t)(ticks >> 24);
    return disc_fm_to_pulses(0xff, fm);
}

static void drive_set_position_fraction(struct fdc_drive *d, double fraction)
{
    uint32_t length = drive_track_length(d);
    d->head_position = (uint32_t)(length * fraction);
    d->pulse_position = 0;
}

static double drive_position_fraction(const struct fdc_drive *d)
{
    return position_fraction(drive_track_length(d), d->head_position, d->pulse_position);
}

static int drive_is_index_pulse(const struct fdc_drive *d)
{
    if (!d->disc) {
        return 1;   // no disc: INDEX held
    }
    uint32_t length = drive_track_length(d);
    if (length == DISC_BYTES_PER_TRACK && timing_ok) {
        return d->head_position < timing_index_heads;
    }
    return d->head_position < (length * (INDEX_MS / (double)200));
}

static int drive_write_protect(const struct fdc_drive *d)
{
    return d->disc ? !d->disc->writeable : 0;
}

static void drive_select_side(struct fdc_drive *d, int side)
{
    double fraction = drive_position_fraction(d);
    d->side = side;
    drive_set_position_fraction(d, fraction);
}

static void drive_select_track(struct fdc_drive *d, int32_t track)
{
    double fraction = drive_position_fraction(d);
    if (track < 0) {
        track = 0;
    } else if (track >= DISC_TRACKS) {
        track = DISC_TRACKS - 1;
    }
    d->track = (uint32_t)track;
    drive_set_position_fraction(d, fraction);
}

static void drive_write_pulses(struct fdc_drive *d, uint32_t pulses)
{
    if (!d->disc || drive_write_protect(d)) {
        return;
    }
    uint32_t *p = disc_track(d->disc, d->side, d->track) + d->head_position;
    if (d->is_32us) {
        uint32_t have = *p;
        if (d->pulse_position == 0) {
            pulses = (have & 0x0000ffff) | (pulses << 16);
        } else {
            pulses = (have & 0xffff0000) | pulses;
        }
    }
    *p = pulses;
}

// ---- controller ----

static void fdc_update_nmi(struct fdc *f)
{
    int level = f->drq | f->intrq;
    if (level != f->nmi) {
        f->nmi = level;
        f->set_nmi(f->ctx, level);
    }
}

static void set_intrq(struct fdc *f, int level)
{
    f->intrq = level;
    fdc_update_nmi(f);
}

static void set_drq(struct fdc *f, int level)
{
    f->drq = level;
    if (level) {
        if (f->status & ST_DRQ) {
            f->status |= ST_LOST_BYTE;
        }
        f->status |= ST_DRQ;
    } else {
        f->status &= ~ST_DRQ;
    }
    fdc_update_nmi(f);
}

static void set_state(struct fdc *f, uint32_t state)
{
    f->state = state;
    f->state_count = 0;
}

static void clear_timer(struct fdc *f)
{
    f->timer_state = T_NONE;
    f->timer = 0;
}

static void clear_state(struct fdc *f)
{
    set_state(f, S_IDLE);
    clear_timer(f);
    f->index_pulse_count = 0;
}

static void update_type_I_status(struct fdc *f)
{
    if (f->command_type != 1 || !f->cur) {
        return;
    }
    f->status &= ~(ST_TRACK_0 | ST_INDEX);
    if (f->cur->track == 0) {
        f->status |= ST_TRACK_0;
    }
    if (drive_is_index_pulse(f->cur)) {
        f->status |= ST_INDEX;
    }
}

static void start_timer(struct fdc *f, uint32_t timer_state, uint32_t wait_us)
{
    f->timer_state = timer_state;
    f->state = S_TIMER_WAIT;
    f->timer = wait_us * 2 + (uint32_t)f->in_io;
    f->timer_seq = ++f->seq;
}

static void command_done(struct fdc *f, int do_raise_intrq)
{
    f->do_raise_intrq = do_raise_intrq;
    start_timer(f, T_DONE, 32);
}

static void command_done_timer(struct fdc *f)
{
    f->status &= ~ST_BUSY;
    clear_state(f);
    update_type_I_status(f);
    // DRQ is left as it is.
    if (f->do_raise_intrq) {
        set_intrq(f, 1);
    }
}

static void check_verify(struct fdc *f)
{
    if (f->cmd_verify) {
        f->index_pulse_count = 0;
        set_state(f, S_SEARCH_ID);
    } else {
        command_done(f, 1);
    }
}

static void do_seek_step(struct fdc *f, int direction, int update_tr)
{
    struct fdc_drive *d = f->cur;
    int32_t new_track = (int32_t)d->track + direction;
    drive_select_track(d, new_track);
    if (update_tr) {
        f->track_reg = (uint8_t)(f->track_reg + direction);
    }
    update_type_I_status(f);   // TRK0 may have changed
    start_timer(f, T_SEEK, f->step_rate_ms * 1000);
}

static void do_seek_step_or_verify(struct fdc *f)
{
    if (f->track_reg == f->data_reg) {
        check_verify(f);
        return;
    }
    int direction = (f->track_reg > f->data_reg) ? -1 : 1;
    if (f->cur->track == 0 && direction == -1) {
        f->track_reg = 0;
        check_verify(f);
        return;
    }
    do_seek_step(f, direction, 1);
}

static void dispatch_command(struct fdc *f)
{
    if (f->cmd_write && drive_write_protect(f->cur)) {
        f->status |= ST_WRITE_PROTECT;
        command_done(f, 1);
        return;
    }
    switch (f->command) {
    case CMD_RESTORE:
        f->track_reg = 0xff;
        f->data_reg = 0;
        // fall through
    case CMD_SEEK:
        do_seek_step_or_verify(f);
        break;
    case CMD_STEP_IN: do_seek_step(f, 1, 0); break;
    case CMD_STEP_IN_UPDATE: do_seek_step(f, 1, 1); break;
    case CMD_STEP_OUT: do_seek_step(f, -1, 0); break;
    case CMD_STEP_OUT_UPDATE: do_seek_step(f, -1, 1); break;
    case CMD_READ_SECTOR: case CMD_READ_SECTOR_MULTI:
    case CMD_WRITE_SECTOR: case CMD_WRITE_SECTOR_MULTI:
    case CMD_READ_ADDRESS:
        set_state(f, S_SEARCH_ID);
        f->index_pulse_count = 0;
        break;
    case CMD_READ_TRACK:
        set_state(f, S_WAIT_INDEX);
        f->index_pulse_count = 0;
        break;
    case CMD_WRITE_TRACK:
        set_state(f, S_WRITE_TRACK_SETUP);
        f->index_pulse_count = 0;
        break;
    default:
        break;
    }
}

static void timer_fired(struct fdc *f)
{
    uint32_t which = f->timer_state;
#ifdef MACHINE_DEBUG_FDC
    fprintf(stderr, "fdc timer %u fired, state %u status %02x\n", which, f->state, f->status);
#endif
    clear_timer(f);
    switch (which) {
    case T_SETTLE:
        dispatch_command(f);
        break;
    case T_SEEK:
        if (f->command == CMD_STEP_IN || f->command == CMD_STEP_IN_UPDATE ||
            f->command == CMD_STEP_OUT || f->command == CMD_STEP_OUT_UPDATE) {
            check_verify(f);
        } else {
            do_seek_step_or_verify(f);
        }
        break;
    case T_DONE:
        command_done_timer(f);
        break;
    default:
        break;
    }
}

static int is_reset(uint8_t cr) { return !(cr & CTRL_RESET); }   // active low
static int is_mfm_ctrl(uint8_t cr) { return !(cr & CTRL_DENSITY); } // active low

static void drive_start_spinning(struct fdc *f, struct fdc_drive *d)
{
    d->spinning = 1;
    d->timer = 1 + (uint32_t)f->in_io;
    d->timer_seq = ++f->seq;
}

static void drive_stop_spinning(struct fdc_drive *d)
{
    d->spinning = 0;
    d->timer = 0;
}

static void write_control(struct fdc *f, uint8_t val)
{
    int motor_on = (f->status & ST_MOTOR_ON) != 0;
    if (f->cur && f->cur->spinning) {
        drive_stop_spinning(f->cur);
    }
    // Either select bit picks a drive, 0 winning if both are set (beebjit).
    if (val & (CTRL_DRIVE_0 | CTRL_DRIVE_1)) {
        f->cur = (val & CTRL_DRIVE_0) ? &f->drive[0] : &f->drive[1];
    } else {
        f->cur = NULL;
    }
    if (f->cur) {
        if (motor_on) {
            drive_start_spinning(f, f->cur);
        }
        drive_select_side(f->cur, (val & CTRL_SIDE) != 0);
    }
    int mfm = is_mfm_ctrl(val);
    f->drive[0].is_32us = mfm;
    f->drive[1].is_32us = mfm;
    f->control = val;

    if (is_reset(val)) {
        clear_state(f);
        if (f->cur && motor_on) {
            drive_stop_spinning(f->cur);
        }
        f->status = 0;
        // The reset line holds the sector register at 1 and leaves track
        // and data alone (beebjit, from a real machine).
        f->sector_reg = 1;
        f->intrq = 0;
        f->drq = 0;
        fdc_update_nmi(f);
        f->mark_detector = 0;
        f->data_shifter = 0;
        f->data_shift_count = 0;
        f->is_index_pulse = 0;
        f->last_mfm_bit = 0;
        f->deliver_data = 0;
        f->deliver_is_marker = 0;
    }
}

void fdc_break_reset(struct fdc *f)
{
    write_control(f, 0);
}

void fdc_power_on_reset(struct fdc *f)
{
    fdc_break_reset(f);
    f->track_reg = 0;
    f->data_reg = 0;
}

void fdc_init(struct fdc *f, void (*set_nmi)(void *, int), void *ctx)
{
    memset(f, 0, sizeof *f);
    timing_init();
    f->set_nmi = set_nmi;
    f->ctx = ctx;
    f->state = S_IDLE;
}

static void do_command(struct fdc *f, uint8_t val)
{
    uint8_t command = val & 0xf0;
    struct fdc_drive *d = f->cur;
    if (!d) {
        return;   // beebjit bails here; nothing sensible to do
    }
    if (command == CMD_FORCE_INTERRUPT) {
        uint8_t bits = val & 0x0f;
        if (f->status & ST_BUSY) {
            command_done(f, 0);
        } else {
            f->index_pulse_count = 0;
            f->command_type = 1;
            f->status &= ST_MOTOR_ON;
            if (!(f->status & ST_MOTOR_ON)) {
                f->status |= ST_MOTOR_ON;
                drive_start_spinning(f, d);
            }
        }
        if (bits == 0) {
            f->interrupt_on_index = 0;
        } else if (bits == 4) {
            f->interrupt_on_index = 1;
        }
        return;
    }
    if (f->status & ST_BUSY) {
        return;   // ignored while busy, as beebjit does
    }
    f->command = command;
    f->cmd_settle = f->cmd_write = f->cmd_verify = f->cmd_multi = f->cmd_deleted = 0;
    f->interrupt_on_index = 0;
    f->write_track_crc_second = 0;
    uint32_t step_rate_ms = 0;
    switch (command) {
    case CMD_RESTORE: case CMD_SEEK:
    case CMD_STEP_IN: case CMD_STEP_IN_UPDATE:
    case CMD_STEP_OUT: case CMD_STEP_OUT_UPDATE:
        f->command_type = 1;
        f->cmd_verify = (val & CMDBIT_VERIFY) != 0;
        switch (val & 3) {
        case 0: step_rate_ms = 6; break;
        case 1: step_rate_ms = 12; break;
        case 2: step_rate_ms = 20; break;   // 1770 (a 1772 is 2)
        default: step_rate_ms = 30; break;  // 1770 (a 1772 is 3)
        }
        f->step_rate_ms = step_rate_ms;
        break;
    case CMD_READ_SECTOR: case CMD_READ_SECTOR_MULTI:
    case CMD_WRITE_SECTOR: case CMD_WRITE_SECTOR_MULTI:
        f->command_type = 2;
        f->cmd_multi = (val & CMDBIT_MULTI) != 0;
        break;
    case CMD_READ_ADDRESS: case CMD_READ_TRACK: case CMD_WRITE_TRACK:
        f->command_type = 3;
        break;
    default:
        return;
    }
    if ((f->command_type == 2 || f->command_type == 3) && (val & CMDBIT_SETTLE)) {
        f->cmd_settle = 1;
    }
    if (command == CMD_WRITE_SECTOR || command == CMD_WRITE_SECTOR_MULTI || command == CMD_WRITE_TRACK) {
        f->cmd_write = 1;
        f->cmd_deleted = (val & CMDBIT_DELETED) != 0;
    }
    // Every command but force interrupt: clear INTRQ and DRQ and the
    // result bits, go busy, spin up unless already spinning.
    set_drq(f, 0);
    set_intrq(f, 0);
    f->status &= ST_MOTOR_ON;
    f->status |= ST_BUSY;
    f->index_pulse_count = 0;
    if (f->status & ST_MOTOR_ON) {
        dispatch_command(f);
    } else {
        f->status |= ST_MOTOR_ON;
        drive_start_spinning(f, d);
        if (val & CMDBIT_NO_SPIN_UP) {
            f->index_pulse_count = 6;
            dispatch_command(f);
        } else {
            set_state(f, S_SPIN_UP_WAIT);
        }
    }
}

// The Master's control register bits to the chip's.
static uint8_t master_remap(uint8_t val)
{
    uint8_t v = 0;
    if (val & 0x04) v |= CTRL_RESET;
    if (val & 0x01) v |= CTRL_DRIVE_0;
    if (val & 0x02) v |= CTRL_DRIVE_1;
    if (val & 0x10) v |= CTRL_SIDE;
    if (val & 0x20) v |= CTRL_DENSITY;
    return v;
}

FDC_API uint8_t fdc_read(struct fdc *f, uint16_t addr)
{
    switch (addr & 7) {
    case 4:
        set_intrq(f, 0);   // reading status clears INTRQ
        return f->status;
    case 5:
        return f->track_reg;
    case 6:
        return f->sector_reg;
    case 7:
        if (f->command_type == 2 || f->command_type == 3) {
            set_drq(f, 0);
        }
        return f->data_reg;
    default:
        return 0xfe;   // the control register is not readable
    }
}

FDC_API void fdc_write(struct fdc *f, uint16_t addr, uint8_t val)
{
    // The 1770's registers (FE28-FE2B) are 1 MHz: beebjit does the write
    // at the end of the stretched access, one cycle after our write cycle,
    // so a timer started here fires one later. The control register (FE24)
    // is a 2 MHz address and lines up.
    f->in_io = (addr & 7) >= 4;
    switch (addr & 7) {
    case 0: case 1: case 2: case 3:
        write_control(f, master_remap(val));
        break;
    case 4:
        if (!is_reset(f->control)) {
            do_command(f, val);
        }
        break;
    case 5:
        f->track_reg = val;
        break;
    case 6:
        if (!is_reset(f->control)) {
            f->sector_reg = val;
        }
        break;
    case 7:
        if (f->command_type == 2 || f->command_type == 3) {
            set_drq(f, 0);
        }
        f->data_reg = val;
        break;
    }
    f->in_io = 0;
}

static void send_data_to_host(struct fdc *f, uint8_t data)
{
    set_drq(f, 1);
    f->data_reg = data;
}

static void byte_received(struct fdc *f, int index_edge)
{
    uint8_t command_type = f->command_type;
    int is_read_address = (f->command == CMD_READ_ADDRESS);
    uint8_t data = f->deliver_data;
    int is_marker = f->deliver_is_marker;
    int is_mfm = is_mfm_ctrl(f->control);

    f->deliver_is_marker = 0;

    switch (f->state) {
    case S_SEARCH_ID:
        if (!is_marker || data != DISC_ID_MARK) {
            break;
        }
        set_state(f, S_IN_ID);
        f->crc = disc_crc_init(is_mfm);
        f->crc = disc_crc_add(f->crc, DISC_ID_MARK);
        break;
    case S_IN_ID:
        switch (f->state_count) {
        case 0:
            f->on_disc_track = data;
            if (is_read_address) {
                f->sector_reg = data;   // the datasheet's quirk
            }
            break;
        case 2:
            f->on_disc_sector = data;
            break;
        case 3:
            f->on_disc_length = 128u << (data & 3);
            break;
        default:
            break;
        }
        if (is_read_address) {
            send_data_to_host(f, data);   // CRC bytes included
        }
        if (f->state_count < 4) {
            f->crc = disc_crc_add(f->crc, data);
        } else {
            f->on_disc_crc = (uint16_t)((f->on_disc_crc << 8) | data);
        }
        f->state_count++;
        if (f->state_count != 6) {
            break;
        }
        {
            int crc_error = (f->crc != f->on_disc_crc);
            if (is_read_address) {
                if (crc_error) {
                    f->status |= ST_CRC_ERROR;
                }
                set_state(f, S_DONE);   // busy drops a byte after the last DRQ
                break;
            }
            if (f->track_reg != f->on_disc_track) {
                set_state(f, S_SEARCH_ID);
                break;
            }
            if (command_type == 2 && f->sector_reg != f->on_disc_sector) {
                set_state(f, S_SEARCH_ID);
                break;
            }
            if (crc_error) {
                f->status |= ST_CRC_ERROR;
                set_state(f, S_SEARCH_ID);   // the 1770 keeps looking
                break;
            }
            if (command_type == 1) {
                command_done(f, 1);
            } else if (f->cmd_write) {
                set_state(f, S_WRITE_SECTOR_DELAY);
            } else {
                set_state(f, S_SEARCH_DATA);
            }
        }
        break;
    case S_SEARCH_DATA:
        f->state_count++;
        // The data mark counts only after 14 bytes, and it gives up after 31.
        if (f->state_count <= 14u * (is_mfm + 1)) {
            break;
        }
        if (f->state_count >= 31u * (is_mfm + 1)) {
            set_state(f, S_SEARCH_ID);
            break;
        }
        if (!is_marker) {
            break;
        }
        if (data == DISC_DATA_MARK) {
            // normal
        } else if (data == DISC_DELETED_DATA_MARK) {
            f->status |= ST_DELETED_MARK;   // visible at once, and sticky
        } else {
            break;
        }
        set_state(f, S_IN_DATA);
        f->status &= ~ST_CRC_ERROR;   // a header CRC error is forgotten here
        f->crc = disc_crc_init(is_mfm);
        f->crc = disc_crc_add(f->crc, data);
        break;
    case S_IN_DATA:
        f->state_count++;
        if (f->state_count <= f->on_disc_length) {
            f->crc = disc_crc_add(f->crc, data);
            send_data_to_host(f, data);
            break;
        } else if (f->state_count <= f->on_disc_length + 2) {
            f->on_disc_crc = (uint16_t)((f->on_disc_crc << 8) | data);
            break;
        }
        if (f->crc != f->on_disc_crc) {
            f->status |= ST_CRC_ERROR;
            command_done(f, 1);   // terminal, even for a multi-sector read
            break;
        }
        set_state(f, S_CHECK_MULTI);
        break;
    case S_IN_READ_TRACK:
        if (!index_edge) {
            send_data_to_host(f, data);
            break;
        }
        command_done(f, 1);
        break;
    default:
        break;
    }
}

static int mark_detector_triggered(struct fdc *f)
{
    uint64_t md = f->mark_detector;
    if (is_mfm_ctrl(f->control)) {
        if ((md & 0x00000000ffffffffull) == 0x00000000aaaa4489ull) {
            f->deliver_data = 0xa1;
            return 1;
        }
        if ((md & 0xffffffffffff0000ull) == 0x4489448944890000ull) {
            f->deliver_is_marker = 1;
        }
    } else {
        // FM: four data bits of 0 with clocks of 1, then a C7 clock pattern.
        if ((md & 0x0000ffff00000000ull) == 0x0000888800000000ull) {
            uint8_t clocks, data;
            int iffy;
            disc_pulses_to_fm(&clocks, &data, &iffy, (uint32_t)md);
            if (!iffy && clocks == DISC_MARK_CLOCKS) {
                if (data == 0xf8 || data == 0xfb || data == 0xfe) {
                    f->deliver_data = data;
                    f->deliver_is_marker = 1;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void bit_received(struct fdc *f, int bit, uint32_t now)
{
    f->mark_detector = (f->mark_detector << 1) | (uint32_t)bit;
    if (mark_detector_triggered(f)) {
        f->data_shifter = 0;
        f->data_shift_count = 0;
        return;
    }
    uint32_t sh = (f->data_shifter << 1) | (uint32_t)bit;
    f->data_shifter = sh;
    f->data_shift_count++;
    if (is_mfm_ctrl(f->control)) {
        if (f->data_shift_count == 16) {
            f->deliver_data = disc_pulses_to_mfm((uint16_t)sh);
            f->data_shifter = 0;
            f->data_shift_count = 0;
        }
    } else if (f->data_shift_count == 32) {
        uint8_t clocks;
        int iffy;
        disc_pulses_to_fm(&clocks, &f->deliver_data, &iffy, sh);
        if (iffy) {
            // MFM read as FM: fuzzy bits, an unstable read.
            sh = drive_quasi_random_pulses(now);
            disc_pulses_to_fm(&clocks, &f->deliver_data, &iffy, sh);
        }
        f->data_shifter = 0;
        f->data_shift_count = 0;
    }
}

static void bitstream_received(struct fdc *f, uint32_t pulses, uint32_t count, int index_edge, uint32_t now)
{
    pulses <<= (32 - count);
    for (uint32_t i = 0; i < count; i++) {
        bit_received(f, (pulses & 0x80000000u) != 0, now);
        pulses <<= 1;
    }
    byte_received(f, index_edge);
}

static void write_byte(struct fdc *f, int is_mfm, uint8_t byte, int is_marker)
{
    uint32_t pulses;
    if (is_mfm) {
        if (is_marker) {
            pulses = (byte == 0xa1) ? DISC_MFM_A1_SYNC : DISC_MFM_C2_SYNC;
        } else {
            pulses = disc_mfm_to_pulses(&f->last_mfm_bit, byte);
        }
    } else {
        uint8_t clocks = 0xff;
        if (is_marker) {
            clocks = (byte == 0xfc) ? 0xd7 : DISC_MARK_CLOCKS;
        }
        pulses = disc_fm_to_pulses(clocks, byte);
    }
    drive_write_pulses(f->cur, pulses);
}

// One position of the disc has passed under the head (the drive's timer).
static void pulses_callback(struct fdc *f, uint32_t pulses, uint32_t count, uint32_t now)
{
    struct fdc_drive *d = f->cur;
    int was_index = f->is_index_pulse;
    int index_edge = 0;
    int is_mfm = (count == 16);
    uint8_t data_byte;

    int is_index = drive_is_index_pulse(d);
    f->is_index_pulse = is_index;
    if (is_index && !was_index) {
        index_edge = 1;
    }
    if (f->interrupt_on_index && index_edge) {
        set_intrq(f, 1);
    }
    update_type_I_status(f);
#ifdef MACHINE_DEBUG_FDC
    if (index_edge) {
        fprintf(stderr, "fdc index edge, count %u state %u at %u\n", f->index_pulse_count, f->state, now);
    }
#endif

    switch (f->state) {
    case S_IDLE:
        if (f->index_pulse_count == 10) {
            // Automatic motor off.
            drive_stop_spinning(d);
            f->status &= ~ST_MOTOR_ON;
            if (f->command_type == 1) {
                f->status &= ~(ST_TRACK_0 | ST_INDEX);
            }
        }
        break;
    case S_TIMER_WAIT:
        break;
    case S_SPIN_UP_WAIT:
        if (f->index_pulse_count != 6) {
            break;
        }
        if (f->command_type == 1) {
            f->status |= ST_SPIN_UP_DONE;
        }
        if (f->cmd_settle) {
            start_timer(f, T_SETTLE, SETTLE_MS * 1000);
        } else {
            dispatch_command(f);
        }
        break;
    case S_WAIT_INDEX:
        if (!index_edge) {
            break;
        }
        set_state(f, S_IN_READ_TRACK);
        bitstream_received(f, pulses, count, 0, now);   // this byte is included
        break;
    case S_SEARCH_ID: case S_IN_ID: case S_SEARCH_DATA: case S_IN_DATA: case S_IN_READ_TRACK:
        bitstream_received(f, pulses, count, index_edge, now);
        if (f->index_pulse_count == 6) {
            f->status |= ST_RNF;
            command_done(f, 1);
        }
        break;
    case S_WRITE_SECTOR_DELAY:
        if (f->state_count == 0) {
            f->index_pulse_count = 0;
        } else if (f->state_count == 1) {
            set_drq(f, 1);
        } else if (f->state_count == 10 && (f->status & ST_DRQ)) {
            f->status |= ST_LOST_BYTE;
            command_done(f, 1);
        }
        f->state_count++;
        if (f->state_count == 12) {
            set_state(f, is_mfm ? S_WRITE_SECTOR_LEAD_IN_MFM : S_WRITE_SECTOR_LEAD_IN_FM);
        }
        break;
    case S_WRITE_SECTOR_LEAD_IN_FM:
        write_byte(f, is_mfm, 0x00, 0);
        f->state_count++;
        if (f->state_count == 6) {
            set_state(f, S_WRITE_SECTOR_MARKER_FM);
        }
        break;
    case S_WRITE_SECTOR_LEAD_IN_MFM:
        if (f->state_count >= 11) {
            write_byte(f, is_mfm, 0x00, 0);
        }
        f->state_count++;
        if (f->state_count == 23) {
            set_state(f, S_WRITE_SECTOR_MARKER_MFM);
        }
        break;
    case S_WRITE_SECTOR_MARKER_FM:
        data_byte = f->cmd_deleted ? DISC_DELETED_DATA_MARK : DISC_DATA_MARK;
        f->crc = disc_crc_init(0);
        f->crc = disc_crc_add(f->crc, data_byte);
        write_byte(f, 0, data_byte, 1);
        set_state(f, S_WRITE_SECTOR_BODY);
        break;
    case S_WRITE_SECTOR_MARKER_MFM:
        if (f->state_count < 3) {
            write_byte(f, 1, 0xa1, 1);
        }
        f->state_count++;
        if (f->state_count == 4) {
            data_byte = f->cmd_deleted ? DISC_DELETED_DATA_MARK : DISC_DATA_MARK;
            f->crc = disc_crc_init(1);
            f->crc = disc_crc_add(f->crc, data_byte);
            write_byte(f, 1, data_byte, 0);
            set_state(f, S_WRITE_SECTOR_BODY);
        }
        break;
    case S_WRITE_SECTOR_BODY:
        if (f->state_count < f->on_disc_length) {
            data_byte = f->data_reg;
            if (f->status & ST_DRQ) {
                data_byte = 0;
                f->status |= ST_LOST_BYTE;
            }
            f->crc = disc_crc_add(f->crc, data_byte);
            write_byte(f, is_mfm, data_byte, 0);
            if (f->state_count != f->on_disc_length - 1) {
                set_drq(f, 1);
            }
        } else if (f->state_count < f->on_disc_length + 2) {
            write_byte(f, is_mfm, (uint8_t)(f->crc >> 8), 0);
            f->crc <<= 8;
        } else {
            write_byte(f, is_mfm, 0xff, 0);
            set_state(f, S_CHECK_MULTI);
        }
        f->state_count++;
        break;
    case S_CHECK_MULTI:
        if (f->cmd_multi) {
            f->sector_reg++;
            f->index_pulse_count = 0;
            set_state(f, S_SEARCH_ID);
        } else {
            command_done(f, 1);
        }
        break;
    case S_WRITE_TRACK_SETUP:
        if (f->state_count == 0) {
            f->index_pulse_count = 0;
            set_drq(f, 1);
        } else if (f->state_count == 3) {
            if (f->status & ST_DRQ) {
                f->status |= ST_LOST_BYTE;
                command_done(f, 1);
            } else {
                set_state(f, S_IN_WRITE_TRACK);
                break;
            }
        }
        f->state_count++;
        break;
    case S_IN_WRITE_TRACK: {
        if (f->state_count == 0 && !index_edge) {
            break;
        }
        if (index_edge && f->state_count > 0) {
            command_done(f, 1);
            break;
        }
        if (f->write_track_crc_second) {
            write_byte(f, is_mfm, (uint8_t)f->crc, 0);
            f->write_track_crc_second = 0;
            set_drq(f, 1);
            break;
        }
        data_byte = f->data_reg;
        if (f->status & ST_DRQ) {
            data_byte = 0;
            f->status |= ST_LOST_BYTE;
        }
        int is_marker = 0, preset_crc = 0;
        switch (data_byte) {
        case 0xf5:
            if (is_mfm) { is_marker = 1; preset_crc = 1; data_byte = 0xa1; }
            break;
        case 0xf6:
            if (is_mfm) { is_marker = 1; data_byte = 0xc2; }
            break;
        case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfe:
            if (!is_mfm) { is_marker = 1; preset_crc = 1; }
            break;
        case 0xfc:
            if (!is_mfm) { is_marker = 1; }
            break;
        default:
            break;
        }
        if (preset_crc) {
            f->crc = disc_crc_init(is_mfm);
        }
        if (data_byte == 0xf7) {
            write_byte(f, is_mfm, (uint8_t)(f->crc >> 8), 0);
            f->write_track_crc_second = 1;
        } else {
            write_byte(f, is_mfm, data_byte, is_marker);
            if (!(is_mfm && preset_crc)) {
                f->crc = disc_crc_add(f->crc, data_byte);
            }
            set_drq(f, 1);
        }
        f->state_count++;
        break;
    }
    case S_DONE:
        command_done(f, 1);
        break;
    default:
        break;
    }

    if (index_edge) {
        f->index_pulse_count++;
    }
}

// The drive's timer: a position (or half of one in double density) has
// passed; hand its pulses to the controller and schedule the next.
static void drive_tick(struct fdc *f, struct fdc_drive *d, uint32_t now)
{
    uint32_t pulses = 0;
    uint32_t num_pulses;
    uint32_t head = d->head_position;
    uint32_t pulse_pos = d->pulse_position;

#ifdef MACHINE_DEBUG_FDC
    fprintf(stderr, "drv %u %u %u %u" "\n", now, d->track, head, pulse_pos);
#endif
    if (d->disc) {
        pulses = disc_track(d->disc, d->side, d->track)[head];
    }
    if (pulse_pos == 16 || d->is_32us) {
        num_pulses = 16;
        if (pulse_pos == 0) {
            pulses >>= 16;
        } else {
            pulses &= 0xffff;
        }
    } else {
        num_pulses = 32;
    }
    if (pulses == 0) {
        pulses = drive_quasi_random_pulses(now);   // weak bits in a gap
    }
    if (d == f->cur) {
        pulses_callback(f, pulses, num_pulses, now);
    }

    // The callback may have moved the head (a seek) or stopped the motor.
    // A stopped drive still moves on by this position, as beebjit's does:
    // where it restarts depends on it.
    head = d->head_position;
    pulse_pos = d->pulse_position;
    uint32_t length = drive_track_length(d);
    uint32_t this_ticks = time_for_position(length, head, pulse_pos);
    if (num_pulses == 16) {
        if (pulse_pos == 0) {
            pulse_pos = 16;
        } else {
            pulse_pos = 0;
            head++;
        }
    } else {
        head++;
    }
    uint32_t next_ticks = time_for_position(length, head, pulse_pos);
    if (head == length) {
        head = 0;
    }
    d->head_position = head;
    d->pulse_position = pulse_pos;
    if (d->spinning) {
        d->timer = next_ticks - this_ticks;   // from this firing, so the phase holds
        d->timer_seq = ++f->seq;
    }
}

// Runs once per machine chunk: hot, unlike the rest of this file.
#ifndef FDC_HOT_API
#define FDC_HOT_API FDC_API
#endif
FDC_HOT_API uint32_t fdc_ticks_to_event(const struct fdc *f)
{
    uint32_t e = 0xffffffffu;
    if (f->timer_state != T_NONE && f->timer < e) {
        e = f->timer;
    }
    for (int i = 0; i < 2; i++) {
        if (f->drive[i].spinning && f->drive[i].timer < e) {
            e = f->drive[i].timer;
        }
    }
    return e;
}

FDC_API void fdc_advance(struct fdc *f, uint32_t n, uint32_t now)
{
    if (n == 0) {
        return;
    }
    // Count everything down first, then fire what is due in the order it
    // was armed (beebjit's expiry list puts an equal expiry after the ones
    // already there). A callback may re-arm something: that is for a later
    // round, so the due set is fixed before firing.
    int fire_cmd = 0, fire_drive[2] = {0, 0};
    if (f->timer_state != T_NONE) {
        f->timer -= n;
        fire_cmd = (f->timer == 0);
    }
    for (int i = 0; i < 2; i++) {
        if (f->drive[i].spinning) {
            f->drive[i].timer -= n;
            fire_drive[i] = (f->drive[i].timer == 0);
        }
    }
    uint32_t cmd_seq = fire_cmd ? f->timer_seq : 0;
    for (int round = 0; round < 3; round++) {
        // Pick the oldest due item each round.
        int best = -1;
        uint32_t best_seq = 0;
        if (fire_cmd && (best < 0 || (int32_t)(cmd_seq - best_seq) < 0)) {
            best = 2;
            best_seq = cmd_seq;
        }
        for (int i = 0; i < 2; i++) {
            if (fire_drive[i] && (best < 0 || (int32_t)(f->drive[i].timer_seq - best_seq) < 0)) {
                best = i;
                best_seq = f->drive[i].timer_seq;
            }
        }
        if (best < 0) {
            break;
        }
        if (best == 2) {
            fire_cmd = 0;
            if (f->timer_state != T_NONE && f->timer == 0) {
                timer_fired(f);
            }
        } else {
            fire_drive[best] = 0;
            if (f->drive[best].spinning) {
                drive_tick(f, &f->drive[best], now);
            }
        }
    }
}
