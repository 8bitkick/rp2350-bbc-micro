// esp32-beeb: BBC Master 128 machine. See machine.h.
//
// Code shape. The core only counts cycles; the peripherals catch up in
// machine_sync(), called once per instruction, before RAM writes and before
// hardware accesses. One call per cycle was measured at 60+ host clocks on
// the ESP32-S3 because the windowed ABI leaves few registers live across a
// call; one call per instruction amortises that. The out-of-line paths take
// the machine pointer and the cycle count, never the core's state pointer,
// and hand back the IRQ level of every cycle they stepped so the core's
// interrupt poll stays cycle exact.
//
// Timing model. The core calls machine_tick() once per 2 MHz cycle. The CRTC
// steps every cycle; the VIAs and ADC step on odd cycle counts (beebjit's
// VIA timers expire one tick after the 1 MHz boundary). A read or write of a
// 1 MHz address is stretched the way the real bus does it (beebjit's rule,
// confirmed on hardware): with the cycle count c before the access, the CPU
// idles for one cycle if c is even or two if it is odd, performs the access
// at the odd count with that count's 1 MHz step already applied, then the
// data cycle completes. 2 MHz hardware is "tick then access", except the
// video ULA at 0xFE20-0xFE23 which is "access then tick".

#include <stddef.h>
#include <string.h>

#ifndef MACHINE_CODE_ATTR
#define MACHINE_CODE_ATTR
#endif
#define MACHINE_NOINLINE MACHINE_CODE_ATTR __attribute__((noinline))
#ifndef MACHINE_CYCLES
#define MACHINE_CYCLES() 0u
#endif
#define CRTC_CYCLES() MACHINE_CYCLES()
#ifdef MACHINE_STATS
#define STAT(x) ((x)++)
#define STAT_CLK(field, expr) do { uint64_t c0_ = MACHINE_CYCLES(); expr; (field) += MACHINE_CYCLES() - c0_; } while (0)
#else
#define STAT(x) ((void)0)
#define STAT_CLK(field, expr) do { expr; } while (0)
#endif
#define VIA_API static MACHINE_CODE_ATTR
#define CRTC_API MACHINE_CODE_ATTR

#include "machine.h"
#include "via.c"

#define INLINE static inline __attribute__((always_inline))

struct machine;
INLINE uint8_t machine_display_fetch(struct machine *m, uint16_t ma, uint8_t ra, int odd);
INLINE void machine_vsync(struct machine *m, int level);
#define CRTC_FETCH(c, ma, ra, odd) machine_display_fetch(crtc_owner(c), ma, ra, odd)
#define CRTC_VSYNC(c, level) machine_vsync(crtc_owner(c), level)
INLINE struct machine *crtc_owner(struct crtc *c);
// Video stream hooks: one word per tick, tagged events for the rest. With
// no stream attached (m->vs NULL) they cost one compare.
#define VS_PUT(c, w) do { struct machine *mm_ = crtc_owner(c); if (mm_->vs) vs_put(mm_->vs, (w)); } while (0)
#define CRTC_RENDER(c, data, dispen, odd, ma13) \
    VS_PUT(c, (uint16_t)(((data) << 8) | ((odd) ? VS_ODD : 0) | ((dispen) ? VS_DISPEN : 0) | ((ma13) ? VS_MA13 : 0)))
#define CRTC_HSYNC(c, width) VS_PUT(c, VS_EV(VS_HSYNC, (width)))
#define CRTC_CURSOR(c) VS_PUT(c, VS_EV(VS_CURSOR, 0))
#define CRTC_RA(c, ra, isv) VS_PUT(c, VS_EV(VS_RA, (ra) | ((isv) ? 0x80 : 0)))
// The CRTC fast path keeps the ring head in a register for the run.
#define CRTC_RUN_BEGIN(c) \
    struct vstream *vs_ = crtc_owner(c)->vs; \
    uint32_t vh_ = vs_ ? vs_->head : 0
// odd and ma13 are 0 or 1 here, dispen any value; the flag bits are laid
// out so they assemble with shifts and ors alone.
#define CRTC_RUN_RENDER(c, data, dispen, odd, ma13) do { \
        if (vs_) { \
            vs_->ring[vh_ & (VS_RING - 1)] = (uint16_t)(((data) << 8) | (odd) | \
                                                        ((dispen) ? VS_DISPEN : 0) | ((ma13) << 2)); \
            vh_++; \
        } \
    } while (0)
#define CRTC_RUN_RENDER_PAIR(c, data, dispen, ma13) do { \
        if (vs_) { \
            vs_->ring[vh_ & (VS_RING - 1)] = (uint16_t)(((data) << 8) | VS_PAIR | \
                                                        ((dispen) ? VS_DISPEN : 0) | ((ma13) << 2)); \
            vh_++; \
        } \
    } while (0)
#define CRTC_RUN_END(c) do { if (vs_) vs_->head = vh_; } while (0)
// Linear fetch runs: a load and a store per character.
INLINE void machine_display_run(const struct machine *m, uint16_t ma, uint8_t ra, uint32_t tick,
                                const uint8_t **p, uint32_t *stride, uint32_t *chars);
#define CRTC_FETCH_RUN(c, ma, ra, tick, pp, pstride, pchars) \
    machine_display_run(crtc_owner(c), ma, ra, tick, pp, pstride, pchars)
// DISPEN low: one skip word per 255 ticks in place of the tick words.
#define CRTC_RUN_SKIP(ticks) do { \
        uint32_t tk_ = (ticks); \
        while (tk_ > 255) { \
            vs_->ring[vh_ & (VS_RING - 1)] = VS_EV(VS_SKIP, 255); \
            vh_++; \
            tk_ -= 255; \
        } \
        vs_->ring[vh_ & (VS_RING - 1)] = VS_EV(VS_SKIP, tk_); \
        vh_++; \
    } while (0)
#define CRTC_RUN_RENDER_CHARS(c, p, stride, chars, dispen, tick, ma13, ma, ra, data) do { \
        const uint8_t *q_ = (p); \
        uint32_t st_ = (stride), n_ = (chars); \
        if (vs_ && !(dispen)) { \
            CRTC_RUN_SKIP(n_); \
            data = q_[st_ * (n_ - 1)]; \
        } else if (vs_) { \
            uint32_t fl_ = ((dispen) ? VS_DISPEN : 0) | ((ma13) << 2); \
            uint32_t odd_ = (tick) & 1; \
            for (uint32_t k_ = 0; k_ < n_; k_++) { \
                data = *q_; \
                q_ += st_; \
                vs_->ring[vh_ & (VS_RING - 1)] = (uint16_t)((data << 8) | fl_ | odd_); \
                vh_++; \
                odd_ ^= 1; \
            } \
        } else { \
            data = q_[st_ * (n_ - 1)]; \
        } \
    } while (0)
#define CRTC_RUN_RENDER_PAIRS(c, p, stride, chars, dispen, ma13, ma, ra, data) do { \
        const uint8_t *q_ = (p); \
        uint32_t st_ = (stride), n_ = (chars); \
        if (vs_ && !(dispen)) { \
            CRTC_RUN_SKIP(2 * n_); \
            data = q_[st_ * (n_ - 1)]; \
        } else if (vs_) { \
            uint32_t fl_ = VS_PAIR | ((dispen) ? VS_DISPEN : 0) | ((ma13) << 2); \
            for (uint32_t k_ = 0; k_ < n_; k_++) { \
                data = *q_; \
                q_ += st_; \
                vs_->ring[vh_ & (VS_RING - 1)] = (uint16_t)((data << 8) | fl_); \
                vh_++; \
            } \
        } else { \
            data = q_[st_ * (n_ - 1)]; \
        } \
    } while (0)
#include "crtc.c"
// The disc side is cold code (one call per 64 us while a disc spins):
// keep it, and everything it inlines, out of the IRAM functions that call
// it. On the board that leaves it in flash. (A build with the per-byte
// path, 4.2 KB, in IRAM stopped printing after the machine started; not
// diagnosed, and not needed once the drive timing lost its doubles.)
#define DISC_API __attribute__((noinline))
#define FDC_API __attribute__((noinline))
#define FDC_HOT_API static inline MACHINE_CODE_ATTR
#include "disc.c"
#include "fdc.c"

// The 1770's INTRQ or DRQ: the NMI line. The core latches the rising edge
// with the cycle it happened on, for its interrupt poll.
static void machine_fdc_nmi(void *ctx, int level)
{
    struct machine *m = ctx;
#ifdef MACHINE_DEBUG_FDC
    fprintf(stderr, "fdc nmi %d at %u\n", level, m->synced);
#endif
    if (level) {
        m->nmi_edge = 1;
        m->nmi_cycle = m->synced;
    }
}

// Profiling switch for the per-cycle step (bit 0: skip the CRTC, bit 1:
// skip the VIAs). Zero in normal use; main.c sets it while measuring.
uint8_t machine_profile_skip;

// ---- paging -----------------------------------------------------------------

static void map_range(struct machine *m, int set, int first, int count,
                      uint8_t *rd_base, uint8_t *wr_base)
{
    for (int i = 0; i < count; i++) {
        m->rd_tab[set][first + i] = rd_base ? rd_base + i * 256 : 0;
        // A NULL write base means "ROM": every page writes to the bitbucket.
        m->wr_tab[set][first + i] = wr_base ? wr_base + i * 256 : m->bitbucket;
    }
}

static void map_range_const(struct machine *m, int set, int first, int count,
                            const uint8_t *rd_base, int stride)
{
    for (int i = 0; i < count; i++) {
        m->rd_tab[set][first + i] = (uint8_t *)rd_base + i * stride;
        m->wr_tab[set][first + i] = m->bitbucket;
    }
}

// 0x3000-0x7FFF: main RAM or LYNNE, separately for user code and for code
// running in the MOS VDU driver region. beebjit's rule (from b2): user code
// sees LYNNE iff X; MOS code sees LYNNE iff (HAZEL and X) or (not HAZEL and
// E). The surprising case HAZEL=0, X=1, E=0 sends MOS code to main RAM.
static void remap_shadow(struct machine *m)
{
    int x = (m->acccon >> 2) & 1;
    int e = (m->acccon >> 1) & 1;
    int hazel = (m->acccon >> 3) & 1;
    int d = m->acccon & 1;
    // ACCCON D alone picks what the display reads at 0x3000-0x7FFF: LYNNE
    // when set, main RAM when clear. (beebjit's p_mem_base test is D ^ X
    // because its "main" pointer is the CPU's current view, which X swaps.)
    m->display_base = d ? m->lynne - 0x3000 : m->ram;
    int mos_shadow = (hazel && x) || (!hazel && e);
    uint8_t *usr = x ? m->lynne : m->ram + 0x3000;
    uint8_t *mos = mos_shadow ? m->lynne : m->ram + 0x3000;
    map_range(m, 0, 0x30, 0x50, usr, usr);
    map_range(m, 1, 0x30, 0x50, mos, mos);
}

static void remap_sideways(struct machine *m)
{
    int bank = m->romsel & 0x0f;
    uint8_t *swram = 0;
    const uint8_t *rom = m->rom[bank];
    if (bank >= 4 && bank < 4 + MACHINE_SWRAM_SLOTS) {
        swram = m->swram[bank - 4];  // NULL if the platform gave none
    }
    for (int set = 0; set < 2; set++) {
        if (swram) {
            map_range(m, set, 0x80, 0x40, swram, swram);
        } else if (rom) {
            map_range_const(m, set, 0x80, 0x40, rom, 256);
        } else {
            map_range_const(m, set, 0x80, 0x40, m->empty_rom, 0);
        }
        if (m->romsel & 0x80) {
            map_range(m, set, 0x80, 0x10, m->andy, m->andy);
        }
    }
}

static void remap_hazel(struct machine *m)
{
    for (int set = 0; set < 2; set++) {
        if (m->acccon & 0x08) {
            map_range(m, set, 0xc0, 0x20, m->hazel, m->hazel);
        } else {
            map_range_const(m, set, 0xc0, 0x20, m->mos, 256);
        }
    }
}

static void remap_all(struct machine *m)
{
    for (int set = 0; set < 2; set++) {
        map_range(m, set, 0x00, 0x30, m->ram, m->ram);
        map_range_const(m, set, 0xe0, 0x1c, m->mos + 0x2000, 256);
        map_range(m, set, 0xfc, 0x03, 0, 0);
        m->wr_tab[set][0xfc] = m->wr_tab[set][0xfd] = m->wr_tab[set][0xfe] = 0;
        map_range_const(m, set, 0xff, 0x01, m->mos + 0x3f00, 256);
    }
    remap_shadow(m);
    remap_sideways(m);
    remap_hazel(m);
}

// ---- system VIA peripherals: keyboard, IC32 latch, CMOS, sound -------------

// A byte landing on the SN76489 (the platform renders it; see sn76489.h).
#ifndef MACHINE_SOUND_WRITE
#define MACHINE_SOUND_WRITE(m, byte) ((void)0)
#endif

// The chip takes the port A bus value when its write enable (IC32 bit 0,
// active low) goes low, and again whenever the bus changes while it is low.
static void sound_bus_changed(struct machine *m, int we_edge)
{
    uint8_t bus = m->sysvia.bus_a;
    if (!m->sound_we) {
        return;
    }
    if (we_edge || bus != m->sound_bus) {
        m->sound_bus = bus;
        MACHINE_SOUND_WRITE(m, bus);
    }
}

static int key_down(const struct machine *m, int row, int col)
{
    return (m->keys[col & 0xf] >> (row & 7)) & 1;
}

// Row 0 (shift, ctrl, the links) is not wired to the CA2 interrupt: it
// reads as a key but never raises the flag (beebjit's keyboard.c).
static int key_column_down(const struct machine *m, int col)
{
    return (m->keys[col & 0xf] & ~1u) != 0;
}

static int any_key_down(const struct machine *m)
{
    for (int i = 0; i < 16; i++) {
        if (m->keys[i] & ~1u) {
            return 1;
        }
    }
    return 0;
}

static uint8_t cmos_bus_value(const struct machine *m)
{
    if (m->cmos_enabled && !m->cmos_strobe && m->cmos_data && m->cmos_read) {
        return m->cmos_ram[m->cmos_addr & 0x3f];
    }
    return 0xff;
}

// Recomputes port A pin levels: keyboard bit 7, CMOS data, and CA2.
static void sysvia_port_a(struct machine *m)
{
    struct via *via = &m->sysvia;
    uint8_t bus = via_port_a_out(via);
    uint8_t periph = 0xff;
    int fire = 0;

    periph &= cmos_bus_value(m);
    bus &= periph;

    if (!(m->ic32 & 0x08)) {
        int row = (bus >> 4) & 7;
        int col = bus & 0xf;
        if (!key_down(m, row, col)) {
            periph &= 0x7f;
        }
        if (key_column_down(m, col)) {
            fire = 1;
        }
    } else if (any_key_down(m)) {
        via_set_ca2(via, 0);  // auto-scan toggles CA2 as it sweeps
        fire = 1;
    }
    via->peripheral_a = periph;
    via_update_bus_a(via);
    via_set_ca2(via, fire);
    sound_bus_changed(m, 0);
}

static void sysvia_port_b(struct machine *m)
{
    struct via *via = &m->sysvia;
    uint8_t bus = via_port_b(via);
    uint8_t bit = 1 << (bus & 7);
    if (bus & 0x08) {
        m->ic32 |= bit;
    } else {
        m->ic32 &= ~bit;
    }
    {
        uint8_t we = !(m->ic32 & 1);
        if (we != m->sound_we) {
            m->sound_we = we;
            sound_bus_changed(m, 1);
        }
    }
    // IC32 bits 4,5: hardware scroll wrap size (beebjit's table).
    {
        static const uint16_t wrap[4] = {0x4000, 0x2000, 0x5000, 0x2800};
        m->crtc.screen_wrap_add = wrap[(m->ic32 >> 4) & 3];
    }

    // CMOS/RTC control: PB6 enable, PB7 address strobe, IC32 bit 1 read,
    // bit 2 data (beebjit's cmos.c).
    m->cmos_enabled = (bus >> 6) & 1;
    if (m->cmos_enabled) {
        int strobe = (bus >> 7) & 1;
        int data = (m->ic32 >> 2) & 1;
        int read = (m->ic32 >> 1) & 1;
        if (!strobe && m->cmos_strobe) {
            m->cmos_addr = via->bus_a & 0x3f;
        }
        m->cmos_strobe = strobe;
        m->cmos_data = data;
        m->cmos_read = read;
        // Writes (data high to low in write mode) are ignored for now.
    }
    sysvia_port_a(m);
}

static void sysvia_port_cb(struct via *via, void *ctx, int which)
{
    (void)via;
    struct machine *m = ctx;
    if (which == 1) {
        sysvia_port_b(m);
    } else {
        sysvia_port_a(m);  // port A, or a CA2 flag clear with a key down
    }
}

// ---- per-cycle hook ---------------------------------------------------------

INLINE uint8_t machine_irq_level(const struct machine *m)
{
    return m->sysvia.irq | m->usrvia.irq;
}

// ---- display memory ---------------------------------------------------------

INLINE struct machine *crtc_owner(struct crtc *c)
{
    return (struct machine *)((char *)c - offsetof(struct machine, crtc));
}

INLINE void machine_vsync(struct machine *m, int level)
{
#ifdef MACHINE_VSYNC_LOG
    MACHINE_VSYNC_LOG(m, level);
#endif
    via_set_ca1(&m->sysvia, level);
    if (m->vs) {
        vs_put(m->vs, VS_EV(VS_VSYNC, level));
    }
}

// The RAM address the CRTC fetches for MA/RA (beebjit's video_read_data_byte
// address part). Shared by the fetch and the write-window estimate.
INLINE uint32_t machine_display_addr(const struct machine *m, uint16_t ma, uint8_t ra)
{
    uint32_t addr;
    if (ma & 0x2000) {
        addr = 0x7c00 | (ma & 0x3ff);
    } else {
        addr = (uint32_t)ma * 8 + (ra & 7);
        if (ma & 0x1000) {
            addr -= m->crtc.screen_wrap_add;
        }
        addr &= 0x7fff;
    }
    return addr;
}

// beebjit's video_read_data_byte for the Master: MODE 7 style addressing
// when MA13 is set, else MA*8 + RA with the IC32 wrap; display comes from
// LYNNE when ACCCON D xor X, with ANDY and HAZEL visible below 0x3000 when
// paged in and D set.
INLINE uint8_t machine_display_fetch(struct machine *m, uint16_t ma, uint8_t ra, int odd)
{
    uint32_t addr;
    if (ma & 0x2000) {
        addr = 0x7c00 | (ma & 0x3ff);
        if (!m->crtc.ula_teletext && m->crtc.ula_clock_fast && !odd) {
            addr ^= 64;  // 2 MHz bitmap fetch through the MODE 7 path
        }
    } else {
        addr = (uint32_t)ma * 8 + (ra & 7);
        if (ma & 0x1000) {
            addr -= m->crtc.screen_wrap_add;
        }
        addr &= 0x7fff;
    }
    if (addr >= 0x3000) {
        return m->display_base[addr];
    }
    if (m->acccon & 1) {
        if (addr < 0x1000) {
            if (m->romsel & 0x80) {
                return m->andy[addr];
            }
        } else if (m->acccon & 0x08) {
            return m->hazel[addr - 0x1000];
        }
    }
    return m->ram[addr];
}

// A linear run of display fetches from MA: the byte for MA, the step to the
// next character's byte and how many characters (at least 1) the run stays
// linear for: up to the next MA wrap boundary (the IC32 wrap term changes
// at multiples of 0x1000) and the end of the RAM region the address falls
// in. A 2 MHz bitmap fetch through MODE 7 addressing alternates between
// two bytes, so it runs one character at a time.
INLINE void machine_display_run(const struct machine *m, uint16_t ma, uint8_t ra, uint32_t tick,
                                const uint8_t **p, uint32_t *stride, uint32_t *chars)
{
    if (ma & 0x2000) {
        uint32_t addr = 0x7c00 | (ma & 0x3ff);
        if (!m->crtc.ula_teletext && m->crtc.ula_clock_fast) {
            if (!(tick & 1)) {
                addr ^= 64;
            }
            *p = m->display_base + addr;
            *stride = 0;
            *chars = 1;
            return;
        }
        *p = m->display_base + addr;
        *stride = 1;
        *chars = 0x400 - (ma & 0x3ff);
        return;
    }
    uint32_t addr = (uint32_t)ma * 8 + (ra & 7);
    if (ma & 0x1000) {
        addr -= m->crtc.screen_wrap_add;
    }
    addr &= 0x7fff;
    const uint8_t *base;
    uint32_t end;
    if (addr >= 0x3000) {
        base = m->display_base;
        end = 0x8000;
    } else if ((m->acccon & 1) && addr < 0x1000 && (m->romsel & 0x80)) {
        base = m->andy;
        end = 0x1000;
    } else if ((m->acccon & 1) && addr >= 0x1000 && (m->acccon & 0x08)) {
        base = m->hazel - 0x1000;
        end = 0x3000;
    } else {
        base = m->ram;
        end = ((m->acccon & 1) && addr < 0x1000) ? 0x1000 : 0x3000;
    }
    *p = base + addr;
    *stride = 8;
    uint32_t n = (end - addr + 7) / 8;
    uint32_t to_wrap = 0x1000 - (ma & 0xfff);
    *chars = n < to_wrap ? n : to_wrap;
}

// Advances the peripherals from m->synced to `cyc` in chunks delimited by
// events (VIA timer underflow or reload, ADC completion, a CRTC vsync
// change), so the VIAs and ADC step by subtraction and only the CRTC loops
// per tick. Returns one IRQ-level bit per cycle (newest in bit 0), at most
// the last 16, plus the cycle count in bits 16 and up.
//
// VIA ticks land on odd cycle counts; for a chunk ending at count e the
// VIA tick count is odd(e) - odd(s) where odd(x) is the number of odd
// integers up to x.
static MACHINE_NOINLINE uint32_t machine_advance_ex(struct machine *m, uint32_t cyc, int want_due, int with_crtc);
static MACHINE_NOINLINE void machine_schedule(struct machine *m);
static MACHINE_NOINLINE uint32_t machine_advance(struct machine *m, uint32_t cyc)
{
    return machine_advance_ex(m, cyc, 1, 1);
}

// Runs the CRTC up to the rest of the machine after an access left it
// behind. No vsync change can fall in that range (the access checked),
// so the IRQ history already recorded for it holds.
static MACHINE_NOINLINE void machine_crtc_catch_up(struct machine *m)
{
    while (m->crtc_synced != m->synced) {
        STAT_CLK(m->stat_clk_crtc, m->crtc_synced += crtc_run(&m->crtc, m->crtc_synced, m->synced - m->crtc_synced));
        if (m->crtc.vsync_changed) {
            STAT(m->stat_crtc_deferred_vsync);
        }
    }
}

// When must the core come back? The earliest VIA event, ADC completion
// or CRTC vsync opportunity; also bound by the CRTC's render lag. Sets
// m->sync_due and the display write window for the core. Called at the
// end of a catch-up, and after a register access that was not followed by
// one (the access may have changed what the CRTC or the VIAs do next).
static MACHINE_NOINLINE void machine_schedule(struct machine *m)
{
    uint8_t skip = machine_profile_skip;
    (void)skip;
    uint64_t due0 = MACHINE_CYCLES();
    (void)due0;
    uint32_t s = m->synced;
    uint32_t due = s + 4096;  // never let the peripherals lag far
    int src = 0;
    (void)src;
    if (!(skip & 2)) {
        uint32_t e = via_ticks_to_event(&m->sysvia);
        uint32_t e2 = via_ticks_to_event(&m->usrvia);
        if (e2 < e) {
            e = e2;
        }
        uint32_t first_odd = s + 1 + ((s + 1) & 1 ? 0 : 1);
        uint32_t v = first_odd + 2 * (e - 1);
        if ((int32_t)(v - due) < 0) {
            due = v;
            src = 1;
        }
    }
    if (m->adc_busy && (int32_t)(s + m->adc_countdown - due) < 0) {
        due = s + m->adc_countdown;
        src = 2;
    }
    if (m->key_head < m->key_count && (int32_t)(m->key_events[m->key_head].cycle - due) < 0) {
        due = m->key_events[m->key_head].cycle;
        src = 3;
    }
    if (!(skip & 1)) {
        uint32_t v = m->crtc_synced + crtc_ticks_to_check(&m->crtc, m->crtc_synced);
        if ((int32_t)(v - due) < 0) {
            due = v;
            src = 4;
        }
    }
    {
        uint32_t e = fdc_ticks_to_event(&m->fdc);
        if (e != 0xffffffffu && (int32_t)(s + e - due) < 0) {
            due = s + e;
            src = 5;
        }
        // The quiet VIA path steps neither the 1770 nor the ADC.
        m->quiet_ok = (e == 0xffffffffu) && !m->adc_busy;
    }
    if (m->irq_late) {
        m->irq_late = 0;
        due = s + 1;   // (every other term is at least this)
        src = 3;
    }
    if (m->nmi_edge) {
        due = s;   // the core must take the edge at its next boundary
        src = 6;
    }
    STAT(m->stat_due_src[src]);
    m->sync_due = due;
    m->win_due = due;

    // Display addresses the CRTC will fetch before `due`: the core only
    // catches up for writes inside this window. MA climbs within a line,
    // but a line end reloads it with the row's start and a frame start
    // with R12/R13, so past a line end those starts are in too. (A demo
    // that runs one-line frames fetches the same bytes every line.)
    // Conservative when the MA range crosses an addressing boundary.
    {
        const struct crtc *c = &m->crtc;
        uint32_t chars = due - m->crtc_synced;
        if (!c->ula_clock_fast) {
            chars = chars / 2 + 1;
        }
        uint32_t ma0 = c->address_counter;
        uint32_t ma1 = ma0 + chars + 1;
        uint32_t hc = c->horiz_counter, r0 = c->reg[CRTC_R0_HTOTAL];
        uint32_t to_eol = (hc <= r0) ? (r0 - hc + 1) : (256 - hc + r0 + 1);
        if (chars >= to_eol) {
            uint32_t lines = (chars - to_eol) / (r0 + 1) + 1;   // line ends inside
            uint32_t starts[2];
            int ns = 0;
            starts[ns++] = c->address_counter_saved;
            if (c->is_end_of_main_latched || c->is_end_of_vert_adjust_latched || c->is_end_of_frame_latched ||
                c->is_in_vert_adjust || c->is_vert_adjust_pending || c->in_dummy_raster ||
                ((uint32_t)((c->reg[CRTC_R4_VTOTAL] - c->vert_counter) & 0x7f) < lines)) {
                starts[ns++] = ((c->reg[CRTC_R12_MAH] << 8) | c->reg[CRTC_R13_MAL]) & 0x3fff;
            }
            for (int i = 0; i < ns; i++) {
                if (starts[i] < ma0) {
                    ma0 = starts[i];
                }
                if (starts[i] + chars + 1 > ma1) {
                    ma1 = starts[i] + chars + 1;
                }
            }
        }
        uint32_t lo = machine_display_addr(m, ma0, 0);
        uint32_t hi = machine_display_addr(m, ma1 & 0x3fff, 7);
        if (ma1 > 0x3fff || ((ma0 ^ ma1) & 0x3000) || hi < lo || (skip & 1)) {
            lo = 0;
            hi = 0x7fff;
        }
        m->wlo = lo;
        m->whi = hi;
    }
#ifdef MACHINE_STATS
    {
        uint32_t d = due - s, b = 0;
        while (d > 1 && b < 16) { d >>= 1; b++; }
        m->stat_due_hist[b]++;
    }
    m->stat_clk_due += MACHINE_CYCLES() - due0;
#endif

}

// `want_due` 0 skips the scheduling at the end (the next sync point and
// the display write window) when the caller advances again at once.
// `with_crtc` 0 leaves the CRTC where it is (crtc_synced): a register
// access to another device, when the scheduler's bound says no vsync
// change can fall in the window; the CRTC catches up in one run later.
static MACHINE_NOINLINE uint32_t machine_advance_ex(struct machine *m, uint32_t cyc, int want_due, int with_crtc)
{
    uint32_t mask = 0;
    uint32_t total = cyc - m->synced;
    uint8_t skip = machine_profile_skip;
    if (skip & 1) {
        with_crtc = 0;
        m->crtc_synced = cyc;   // measurement: no CRTC at all
    } else if (with_crtc && m->crtc_synced != m->synced) {
        machine_crtc_catch_up(m);
    } else if (!with_crtc) {
        STAT(m->stat_crtc_deferred);
    }

    STAT(m->stat_calls);
    if (total == 0) {
        STAT(m->stat_zero_calls);
    }
    uint64_t adv0 = MACHINE_CYCLES();
    (void)adv0;
    while (m->synced != cyc) {
        uint32_t s = m->synced;
        STAT(m->stat_chunks);
        uint32_t w = cyc - s;
        uint32_t level = machine_irq_level(m);

        // Earliest event in the window.
        uint32_t via_e = 0xffffffffu;
        if (!(skip & 2)) {
            uint32_t e = via_ticks_to_event(&m->sysvia);
            uint32_t e2 = via_ticks_to_event(&m->usrvia);
            if (e2 < e) {
                e = e2;
            }
            uint32_t first_odd = s + 1 + ((s + 1) & 1 ? 0 : 1);
            via_e = first_odd + 2 * (e - 1) - s;
        }
        if (via_e < w) {
            w = via_e;
        }
        if (m->adc_busy && m->adc_countdown < w) {
            w = m->adc_countdown;
        }
        // Scheduled key events: stop at the event cycle, apply after it.
        uint32_t key_e = 0xffffffffu;
        if (m->key_head < m->key_count) {
            key_e = m->key_events[m->key_head].cycle - s;
            if (key_e < w) {
                w = key_e;
            }
        }
        // The disc drive's next position, or a 1770 timer.
        uint32_t fdc_e = fdc_ticks_to_event(&m->fdc);
        if (fdc_e < w) {
            w = fdc_e;
        }

        // The CRTC may cut the chunk short at a vsync change.
        uint32_t done = w;
        if (with_crtc) {
            STAT_CLK(m->stat_clk_crtc, done = crtc_run(&m->crtc, s, w));
        }

        // VIAs over `done` cycles; the event tick, if this chunk ends on
        // it, is stepped exactly.
        if (!(skip & 2)) {
            // 1 MHz ticks (odd cycles) in s+1 .. s+done, without shifting the
            // absolute count: that broke when it wrapped at 2^32 (35.8 minutes)
            uint32_t k = (done + ((s + 1) & 1)) >> 1;
            uint64_t v0 = MACHINE_CYCLES();
            (void)v0;
            if (done == via_e) {
                via_advance(&m->sysvia, k - 1);
                via_advance(&m->usrvia, k - 1);
                via_tick(&m->sysvia);
                via_tick(&m->usrvia);
            } else if (k) {
                via_advance(&m->sysvia, k);
                via_advance(&m->usrvia, k);
            }
#ifdef MACHINE_STATS
            m->stat_clk_via += MACHINE_CYCLES() - v0;
#endif
        }
        if (m->adc_busy) {
            m->adc_countdown -= done;
            if (m->adc_countdown == 0) {
                m->adc_busy = 0;
                m->adc_ready = 1;
                via_set_cb1(&m->sysvia, 0);
            }
        }
        m->synced = s + done;
        if (with_crtc) {
            m->crtc_synced = m->synced;
        }
        if (fdc_e != 0xffffffffu) {
            STAT_CLK(m->stat_clk_fdc, fdc_advance(&m->fdc, done, m->synced));
        }

        // History: the old level for all but the last cycle of the chunk,
        // then the level after its events.
        if (done > 1) {
            uint32_t run = done - 1;
            if (run >= 16) {
                mask = level ? 0xffff : 0;
            } else {
                mask = (mask << run) | (level ? ((1u << run) - 1) : 0);
            }
        }
        mask = ((mask << 1) | machine_irq_level(m)) & 0xffff;

        // Key events land after the cycle's level is recorded, as beebjit's
        // wakeup timer does relative to the interrupt poll of that cycle.
        while (m->key_head < m->key_count && m->key_events[m->key_head].cycle == m->synced) {
            struct machine_key_event *e = &m->key_events[m->key_head++];
            uint8_t before = machine_irq_level(m);
            machine_set_key(m, e->row, e->col, e->down);
            if (machine_irq_level(m) != before) {
                m->irq_late = 1;   // the core carries the old level: bring it back next cycle
            }
        }
    }
    if (want_due) {
        machine_schedule(m);
    }
#ifdef MACHINE_STATS
    m->stat_clk_advance += MACHINE_CYCLES() - adv0;
#endif
    // The history is 16 bits: a longer window is reported as 16 so the
    // core's shift clears the old bits (a shift by 32 or more would not).
    return mask | ((total > 16 ? 16 : total) << 16);
}

// ---- small peripherals ---------------------------------------------------------

static uint8_t acia_status(const struct machine *m)
{
    int cts = (m->serial_ula & 0x40) != 0;  // RS423 selected, no device
    uint8_t st = cts ? 0x08 : 0x02;
    // Transmit interrupt (TCB = 01) fires when TDRE shows and CTS is low.
    if ((m->acia_ctrl & 0x60) == 0x20 && !cts) {
        st |= 0x80;
    }
    return st;
}

// A 1 MHz read happens here one cycle before beebjit's (mid cycle, where
// beebjit reads at the end of the stretch), and beebjit fires a timer due
// on its read's tick before the read: a conversion ending on the next
// cycle has ended as far as the read is concerned. (Twisted Brain polls
// the status exactly 20000 cycles after starting a conversion.)
static uint8_t adc_status(const struct machine *m)
{
    int busy = m->adc_busy && m->adc_countdown > 1;
    uint8_t st = m->adc_ctrl & 0x0f;  // channel, input flag, 12-bit mode
    st |= 0x20;                         // MSB of the 0x8000 centre value
    if (!busy) {
        st |= 0x40;
    }
    if (!m->adc_ready && busy) {
        st |= 0x80;
    }
    return st;
}

static void adc_write(struct machine *m, uint8_t val)
{
    m->adc_ctrl = val;
    m->adc_busy = 1;
    m->adc_ready = 0;
    // 10 ms (12 bit) or 4 ms in 2 MHz cycles, counted from the end of the
    // stretched write as beebjit does; the +1 is the cycle that completes
    // the write after this call.
    m->adc_countdown = (val & 0x08) ? 20001 : 8001;
    via_set_cb1(&m->sysvia, 1);
}

// ---- I/O ----------------------------------------------------------------------

// beebjit's Master rule, from Tom Seddon: everything in FC00-FEFF is 1 MHz
// except FE20-FE27, FE2C-FE3F and FEA1-FEFF.
INLINE int is_1mhz(uint16_t addr)
{
    if ((addr & 0xff00) != 0xfe00) {
        return 1;
    }
    if (addr > 0xfea0) {
        return 0;
    }
    if (addr >= 0xfe20 && addr < 0xfe28) {
        return 0;
    }
    if (addr >= 0xfe2c && addr < 0xfe40) {
        return 0;
    }
    return 1;
}


static MACHINE_NOINLINE uint8_t sheila_read(struct machine *m, uint16_t addr)
{
    uint8_t ret = 0xfe;
    switch (addr & 0xfffc) {
    case 0xfe00: case 0xfe04:
        ret = crtc_read(&m->crtc, addr & 1);
        break;
    case 0xfe08: case 0xfe0c:
        ret = (addr & 1) ? 0 : acia_status(m);
        break;
    case 0xfe10: case 0xfe14:
        // The serial ULA has no read line: a read writes the high address
        // byte, 0xFE, into it (beebjit, from a real machine).
        m->serial_ula = 0xfe;
        ret = 0;
        break;
    case 0xfe18:
        switch (addr & 3) {
        case 0: ret = adc_status(m); break;
        case 1: ret = 0x80; break;  // high byte of the 0x8000 centre value
        case 2: ret = 0x00; break;
        default: ret = 0; break;
        }
        break;
    case 0xfe1c:
        ret = 0;
        break;
    case 0xfe24:
    case 0xfe2c:
        ret = 0;   // the control register does not read back (beebjit: 0)
        break;
    case 0xfe28:
        ret = fdc_read(&m->fdc, 4 + (addr & 3));
        break;
    case 0xfe30:
        ret = m->romsel;
        break;
    case 0xfe34:
        ret = m->acccon;
        break;
    case 0xfe40: case 0xfe44: case 0xfe48: case 0xfe4c:
    case 0xfe50: case 0xfe54: case 0xfe58: case 0xfe5c:
        ret = via_read(&m->sysvia, addr & 0xf);
        break;
    case 0xfe60: case 0xfe64: case 0xfe68: case 0xfe6c:
    case 0xfe70: case 0xfe74: case 0xfe78: case 0xfe7c:
        ret = via_read(&m->usrvia, addr & 0xf);
        break;
    default:
        break;
    }
    return ret;
}

static MACHINE_NOINLINE void sheila_write(struct machine *m, uint16_t addr, uint8_t val)
{
    switch (addr & 0xfffc) {
    case 0xfe00: case 0xfe04:
        if ((addr & 1) && m->vs && (m->crtc.addr_reg & 0x1f) <= 9) {
            vs_put(m->vs, VS_EV(VS_FRAMING, 0));
        }
#ifdef MACHINE_DEBUG_CRTC
        if (addr & 1) {
            fprintf(stderr, "crtc R%u=%02x at %u" "\n", m->crtc.addr_reg, val, m->synced);
        }
#endif
        crtc_write(&m->crtc, addr & 1, val);
        break;
    case 0xfe08: case 0xfe0c:
        if (!(addr & 1)) {
            m->acia_ctrl = ((val & 3) == 3) ? 0 : val;  // 3 = master reset
        }
        break;
    case 0xfe10: case 0xfe14:
        m->serial_ula = val;
        break;
    case 0xfe18:
        if ((addr & 3) == 0) {
            adc_write(m, val);
        }
        break;
    case 0xfe20:
        if (addr & 1) {
            m->ula_pal[val >> 4] = val & 0xf;
            if (m->vs) {
                vs_put(m->vs, VS_EV(VS_ULAPAL, val));
            }
        } else {
            uint8_t old = m->ula_ctrl;
            m->ula_ctrl = val;
            crtc_set_ula(&m->crtc, val);
            if (m->vs) {
                if ((old ^ val) & 0x12) {
                    vs_put(m->vs, VS_EV(VS_FRAMING, 0));
                }
                vs_put(m->vs, VS_EV(VS_ULACTRL, val));
            }
        }
        break;
    case 0xfe24:
#ifdef MACHINE_DEBUG_FDC
        fprintf(stderr, "fdc control %02x at %u\n", val, m->synced);
#endif
        fdc_write(&m->fdc, 0, val);
        break;
    case 0xfe28:
#ifdef MACHINE_DEBUG_FDC
        fprintf(stderr, "fdc write %04x=%02x at %u (cur %d status %02x)\n", addr, val, m->synced, m->fdc.cur != NULL, m->fdc.status);
#endif
        fdc_write(&m->fdc, 4 + (addr & 3), val);
        break;
    case 0xfe30:
        if (val != m->romsel) {
            m->romsel = val;
            remap_sideways(m);
        }
        break;
    case 0xfe34: {
        uint8_t old = m->acccon;
        m->acccon = val;
        if ((old ^ val) & 0x0f) {   // D, E, X, Y: the 0x3000 window and the display
            remap_shadow(m);
        }
        if ((old ^ val) & 0x08) {   // Y: HAZEL
            remap_hazel(m);
        }
        break;
    }
    case 0xfe40: case 0xfe44: case 0xfe48: case 0xfe4c:
    case 0xfe50: case 0xfe54: case 0xfe58: case 0xfe5c:
        via_write(&m->sysvia, addr & 0xf, val);
        break;
    case 0xfe60: case 0xfe64: case 0xfe68: case 0xfe6c:
    case 0xfe70: case 0xfe74: case 0xfe78: case 0xfe7c:
        via_write(&m->usrvia, addr & 0xf, val);
        break;
    default:
        break;
    }
}

// FRED and JIM (0xFC00-0xFDFF): nothing there on this machine, reads 0xFF.
INLINE uint8_t fred_jim_read(struct machine *m, uint16_t addr)
{
    (void)m;
    (void)addr;
    return 0xff;
}

#define machine_sync(m, cyc) machine_advance(m, cyc)

// Whether an access at `cyc` needs the CRTC caught up first. The CRTC
// and ULA registers always (the stream must have the ticks before the
// write, and the ULA decides the fetch), and paging (the fetch's view of
// ANDY and HAZEL); any other device only if a vsync change could fall in
// the window, which the scheduler's own bound on the CRTC answers.
INLINE int io_needs_crtc(const struct machine *m, uint16_t addr, uint32_t cyc)
{
    if ((addr & 0xff00) == 0xfe00 && (addr < 0xfe24 || (addr >= 0xfe30 && addr < 0xfe38))) {
        return 1;
    }
    return (cyc - m->crtc_synced) >= crtc_ticks_to_check(&m->crtc, m->crtc_synced);
}

// A VIA access with nothing else going on: the demos' vsync polling loops
// read the system VIA's IFR every eight cycles, and a sample player
// writes it as often. The scheduler's sync point is already the earliest
// event of every kind (VIA timers, ADC, keys, a possible vsync change,
// the 1770, a late IRQ or NMI edge), and nothing but a VIA access has
// happened since it was computed; so if the access cycle falls before it,
// with the 1770 and ADC idle (they are not stepped here) and no key
// queued since, the VIAs are stepped by subtraction, the IRQ history is
// the level held and the CRTC is left behind. The access cycle itself
// must be before the sync point: an event tick on it has to be stepped
// exactly. A read cannot move a timer event, so the sync point stands; a
// write reschedules from the VIA's next event against the sync point the
// display write window was computed for (so that window stays valid).
// Anything else falls through to the general path.
// Inline: a call level fewer under the core, whose register windows are
// what an access costs most (the callee chain overflows them).
INLINE int io_quiet_window(struct machine *m, uint32_t end, uint32_t *mask, uint32_t *n)
{
    if (!m->quiet_ok || (int32_t)(end - m->sync_due) >= 0 || m->key_head < m->key_count) {
        return 0;
    }
    uint32_t s = m->synced;
    uint32_t w = end - s;
    uint32_t k = (w + ((s + 1) & 1)) >> 1;   // wrap-safe, as in machine_advance_ex
    via_advance(&m->sysvia, k);
    via_advance(&m->usrvia, k);
    m->synced = end;
    if (w > 16) {
        w = 16;
    }
    *mask = machine_irq_level(m) ? ((1u << w) - 1) : 0;
    *n = w;
    STAT(m->stat_via_fast);
    return 1;
}

INLINE void io_schedule_quiet(struct machine *m)
{
    uint32_t s = m->synced;
    uint32_t e = via_ticks_to_event(&m->sysvia);
    uint32_t e2 = via_ticks_to_event(&m->usrvia);
    if (e2 < e) {
        e = e2;
    }
    uint32_t first_odd = s + 1 + ((s + 1) & 1 ? 0 : 1);
    uint32_t due = first_odd + 2 * (e - 1);
    if ((int32_t)(s + 4096 - due) < 0) {
        due = s + 4096;
    }
    if ((int32_t)(m->win_due - due) < 0) {
        due = m->win_due;
    }
    m->sync_due = due;
}

// Hardware access slow paths, out of line, with no access to the core's
// state. They catch up first, then spend the stretched cycles themselves
// and return like machine_sync() with the data byte in bits 24-31. A 1 MHz
// access catches up and spends its idle cycle or two in one call. The
// cycle after the access is left to the next catch-up: the core counts
// it (the stretch) and carries the IRQ level it last saw, so if the access
// moved that level, or raised an NMI edge, that cycle is advanced here
// after all and recorded; otherwise only the scheduling is redone, since
// the access may have changed what the CRTC or the VIAs do next. A
// register access is two catch-up calls at most, not three.
#define IO_SYNC() do { \
        uint32_t r_ = machine_sync(m, cyc); \
        mask = r_ & 0xffff; \
        n = r_ >> 16; \
    } while (0)
#define IO_SYNC_IDLE() do { \
        uint32_t t_ = ((cyc + 1) & 1) ? 1 : 2; \
        cyc += t_; \
        k += t_; \
        STAT(m->stat_io_ticks); \
        uint32_t r_ = machine_advance_ex(m, cyc, 0, io_needs_crtc(m, addr, cyc)); \
        mask = r_ & 0xffff; \
        n = r_ >> 16; \
    } while (0)
#define IO_TICK() do { cyc++; n++; k++; STAT(m->stat_io_ticks); mask = (mask << 1) | (machine_advance(m, cyc) & 1); } while (0)
#define IO_LEVEL_MOVED() (m->nmi_edge || machine_irq_level(m) != (mask & 1))
// The cycle after an access (1 MHz devices, the video ULA).
#define IO_AFTER_TICK() do { \
        if (IO_LEVEL_MOVED()) { \
            IO_TICK(); \
        } else { \
            k++; \
            machine_schedule(m); \
        } \
    } while (0)
// An access that ended its cycle (2 MHz devices: the cycle was advanced
// before it, with its scheduling): only a moved level needs the core
// back next cycle.
#define IO_AFTER_ACCESS() do { \
        if (IO_LEVEL_MOVED()) { \
            m->irq_late = 1; \
            machine_schedule(m); \
        } \
    } while (0)
#define IO_RESULT(v) ((mask & 0xfff) | (k << 12) | (n << 16) | ((uint32_t)(v) << 24))
// The cycle after an access that cannot move the schedule.
#define IO_AFTER_TICK_KEEP(keep) do { \
        if (IO_LEVEL_MOVED()) { \
            IO_TICK(); \
        } else { \
            k++; \
            if (!(keep)) { \
                machine_schedule(m); \
            } \
        } \
    } while (0)

// Writes that change nothing the scheduler looks at: a palette entry, and
// the CRTC's start address (R12, R13: latched at the frame start, which
// is a CRTC check point anyway). Not the CRTC's timing registers, the
// ULA's clock rate or paging, which move the CRTC's next check or the
// display write window. Twisted Brain writes the palette four times and
// R12/R13 once each per scanline; each wrote the schedule (twice for the
// palette) at about 360 clocks.
INLINE int io_write_keeps_schedule(const struct machine *m, uint16_t addr)
{
    if ((addr & 0xfffc) == 0xfe20) {
        return addr & 1;
    }
    if ((addr & 0xfff9) == 0xfe01) {
        uint8_t r = m->crtc.addr_reg & 0x1f;
        return r == 12 || r == 13;
    }
    return 0;
}

static MACHINE_NOINLINE uint32_t machine_io_rd_slow(struct machine *m, uint16_t addr, uint32_t cyc)
{
    STAT(m->stat_sync_io);
    uint64_t io0 = MACHINE_CYCLES();
    (void)io0;
    uint32_t mask, n, k = 0;
    uint8_t v;
    if ((addr & 0xffc0) == 0xfe40) {
        uint32_t t_ = ((cyc + 1) & 1) ? 1 : 2;
        if (io_quiet_window(m, cyc + t_, &mask, &n)) {
            cyc += t_;
            k += t_;
            v = via_read((addr & 0x20) ? &m->usrvia : &m->sysvia, addr & 0xf);
            if (IO_LEVEL_MOVED()) {
                IO_TICK();
            } else {
                k++;   // a read moves no timer event: the sync point stands
            }
#ifdef MACHINE_STATS
            m->stat_clk_quiet += MACHINE_CYCLES() - io0;
#endif
            return IO_RESULT(v);
        }
    }
    if ((addr & 0xff00) != 0xfe00) {
        IO_SYNC_IDLE();
        v = fred_jim_read(m, addr);
        IO_AFTER_TICK();
    } else if (is_1mhz(addr)) {
        IO_SYNC_IDLE();
        STAT(m->stat_io_page[(addr >> 4) & 7]);
        STAT_CLK(m->stat_clk_sheila[(addr >> 4) & 7], v = sheila_read(m, addr));  // at the odd count, mid 1 MHz cycle
        IO_AFTER_TICK();
    } else {
        IO_SYNC();
        IO_TICK();
        STAT(m->stat_io_page[(addr >> 4) & 7]);
        STAT_CLK(m->stat_clk_sheila[(addr >> 4) & 7], v = sheila_read(m, addr));
        IO_AFTER_ACCESS();
    }
#ifdef MACHINE_STATS
    m->stat_clk_io += MACHINE_CYCLES() - io0;
#endif
    return IO_RESULT(v);
}

static MACHINE_NOINLINE uint32_t machine_io_wr_slow(struct machine *m, uint16_t addr, uint32_t cyc, uint8_t val)
{
    uint64_t io0 = MACHINE_CYCLES();
    (void)io0;
    uint32_t mask, n, k = 0;
    // The VIAs, and the CRTC's address register (FE00, even): selecting a
    // register touches nothing until the data access, which catches up.
    int crtc_select = (addr & 0xfff9) == 0xfe00;
    if ((addr & 0xffc0) == 0xfe40 || crtc_select) {
        uint32_t t_ = ((cyc + 1) & 1) ? 1 : 2;
        if (io_quiet_window(m, cyc + t_, &mask, &n)) {
            cyc += t_;
            k += t_;
            if (crtc_select) {
                crtc_write(&m->crtc, 0, val);
            } else {
                via_write((addr & 0x20) ? &m->usrvia : &m->sysvia, addr & 0xf, val);
            }
            if (IO_LEVEL_MOVED()) {
                IO_TICK();
            } else {
                k++;
                if (!crtc_select) {
                    io_schedule_quiet(m);
                }
            }
#ifdef MACHINE_STATS
            m->stat_clk_quiet += MACHINE_CYCLES() - io0;
#endif
            return IO_RESULT(0);
        }
    }
    if ((addr & 0xff00) != 0xfe00) {
        IO_SYNC_IDLE();
        IO_AFTER_TICK();  // FRED, JIM: 1 MHz, nothing listening
    } else if ((addr & 0xfff9) == 0xfe01) {
        // A CRTC register: beebjit ticks to the end of the stretched cycle
        // and then writes ("required for CRTC"), one tick later than the
        // other 1 MHz devices here, which matters when the write lands on
        // a counter hit (Twisted Brain rewrites R8 on a line end).
        int keep = io_write_keeps_schedule(m, addr);
        uint32_t t_ = (((cyc + 1) & 1) ? 1 : 2) + 1;
        if (keep && io_quiet_window(m, cyc + t_, &mask, &n)) {
            machine_crtc_catch_up(m);
            cyc += t_;
            k += t_;
        } else {
            IO_SYNC_IDLE();
            IO_TICK();
            keep = 0;   // IO_TICK scheduled; the write may move the schedule
        }
        STAT(m->stat_io_page[(addr >> 4) & 7]);
        STAT_CLK(m->stat_clk_sheila[(addr >> 4) & 7], sheila_write(m, addr, val));
        if (IO_LEVEL_MOVED()) {
            m->irq_late = 1;   // R7/R8 can move vsync at once
            machine_schedule(m);
        } else if (!keep) {
            machine_schedule(m);
        }
    } else if (is_1mhz(addr)) {
        int keep = io_write_keeps_schedule(m, addr);
        uint32_t t_ = ((cyc + 1) & 1) ? 1 : 2;
        if (keep && io_quiet_window(m, cyc + t_, &mask, &n)) {
            // Nothing falls due before the access: the VIAs by
            // subtraction and the CRTC run up to it, without the
            // general catch-up's event bookkeeping.
            machine_crtc_catch_up(m);
            cyc += t_;
            k += t_;
        } else {
            IO_SYNC_IDLE();
            keep = keep && (int32_t)(cyc - m->sync_due) < 0;
        }
        STAT(m->stat_io_page[(addr >> 4) & 7]);
        STAT_CLK(m->stat_clk_sheila[(addr >> 4) & 7], sheila_write(m, addr, val));
        IO_AFTER_TICK_KEEP(keep);
    } else if (addr >= 0xfe20 && addr < 0xfe24) {
        // Before the sync point nothing has fallen due, so a write that
        // cannot move the schedule leaves it as it was.
        int keep = io_write_keeps_schedule(m, addr);
        if (keep && io_quiet_window(m, cyc, &mask, &n)) {
            machine_crtc_catch_up(m);   // as above
        } else {
            keep = keep && (int32_t)(cyc - m->sync_due) < 0;
            uint32_t r_ = machine_advance_ex(m, cyc, !keep, 1);
            mask = r_ & 0xffff;
            n = r_ >> 16;
        }
        STAT(m->stat_io_page[(addr >> 4) & 7]);
        STAT_CLK(m->stat_clk_sheila[(addr >> 4) & 7], sheila_write(m, addr, val));  // video ULA: access then tick
        IO_AFTER_TICK_KEEP(keep);
    } else {
        IO_SYNC();
        IO_TICK();
        STAT(m->stat_io_page[(addr >> 4) & 7]);
        STAT_CLK(m->stat_clk_sheila[(addr >> 4) & 7], sheila_write(m, addr, val));
        IO_AFTER_ACCESS();
    }
#ifdef MACHINE_STATS
    m->stat_clk_io += MACHINE_CYCLES() - io0;
#endif
    return IO_RESULT(0);
}

static void machine_cpu_sync(struct cpu6502 *cpu);
static inline __attribute__((always_inline)) uint8_t machine_io_rd(struct cpu6502 *cpu, uint16_t addr);
static inline __attribute__((always_inline)) void machine_io_wr(struct cpu6502 *cpu, uint16_t addr, uint8_t val);

#define CPU6502_SYNC(cpu) machine_cpu_sync(cpu)
static void machine_cpu_sync_write(struct cpu6502 *cpu);
#define CPU6502_SYNC_WRITE(cpu) machine_cpu_sync_write(cpu)
#define CPU6502_IO_RD(cpu, addr) machine_io_rd(cpu, addr)
#define CPU6502_IO_WR(cpu, addr, v) machine_io_wr(cpu, addr, v)
#define CPU6502_CODE_ATTR MACHINE_CODE_ATTR
#define CPU6502_CMOS_ONLY 1   // the Master's 65C12
#include "cpu6502.c"

// Out of line: it runs at events, not per instruction, and the core's
// loop head (which the compiler copies into every opcode's tail) must stay
// small. The NMI edge from any source is handed to the core here.
static MACHINE_NOINLINE void machine_cpu_sync_to(struct cpu6502 *cpu, uint32_t to);
static MACHINE_NOINLINE void machine_cpu_sync(struct cpu6502 *cpu)
{
    machine_cpu_sync_to(cpu, cpu->cyc);
}

// A write to display RAM the CRTC may fetch (see wr_cycle in cpu6502.c):
// the machine up to the end of the last storing instruction, or, if it is
// already past that, the CRTC alone if it was left behind; then the store.
static MACHINE_NOINLINE void machine_cpu_sync_write(struct cpu6502 *cpu)
{
    struct machine *m = cpu->m;
    STAT(m->stat_sync_write);
    uint32_t to = cpu->last_wr;
    if ((int32_t)(to - m->synced) > 0) {
        machine_cpu_sync_to(cpu, to);
    } else if ((int32_t)(to - m->crtc_synced) > 0) {
        while (m->crtc_synced != to) {
            m->crtc_synced += crtc_run(&m->crtc, m->crtc_synced, to - m->crtc_synced);
        }
    }
}

// Brings the machine up to `to` (at most the core's count) and hands the
// core its IRQ level, NMI edge and next sync point.
static MACHINE_NOINLINE void machine_cpu_sync_to(struct cpu6502 *cpu, uint32_t to)
{
    struct machine *m = cpu->m;
    STAT(m->stat_sync_boundary);
#ifdef MACHINE_STATS
    uint64_t s0_ = MACHINE_CYCLES();
#endif
    if (machine_profile_skip & 4) {
        cpu6502_set_sync_due(cpu, cpu->cyc + 1000000);  // measurement: core alone
        return;
    }
    uint32_t r = machine_sync(m, to);
    cpu->irq_hist = (cpu->irq_hist << (r >> 16)) | (r & 0xffff);
    cpu->irq_line = r & 1;
    if (m->nmi_edge) {
        m->nmi_edge = 0;
        cpu->nmi_pending = 1;
        cpu->nmi_cycle = m->nmi_cycle;
    }
    cpu6502_set_sync_due(cpu, m->sync_due);
    cpu->last_sync = to;
    cpu->wlo = m->wlo;
    cpu->whi = m->whi;
#ifdef MACHINE_STATS
    m->stat_clk_sync_outer += MACHINE_CYCLES() - s0_;
#endif
}

// Slow path results: bits 0-11 history, 12-15 stretch cycles to add to the
// core's count, 16-23 history length, 24-31 data.
// Inlined at every I/O access site in the core, which the compiler
// replicates per opcode: keep it small. An NMI edge raised during the
// access is handed over by the sync hook, which the slow path forces.
INLINE void machine_io_apply(struct cpu6502 *cpu, uint32_t r)
{
    struct machine *m = cpu->m;
    uint32_t k = (r >> 12) & 0xf;
    cpu->cyc += k;
    // The slow path counts the access cycle itself; the stretch beyond a
    // 2 MHz access is one less (see the NMI poll in cpu6502.c).
    cpu->stretch += k - 1;
    cpu->irq_hist = (cpu->irq_hist << ((r >> 16) & 0xff)) | (r & 0xfff);
    cpu->irq_line = r & 1;
    cpu6502_set_sync_due(cpu, m->sync_due);   // an NMI edge makes this the next boundary
    cpu->last_sync = cpu->cyc;
    cpu->wlo = m->wlo;
    cpu->whi = m->whi;
}

INLINE uint8_t machine_io_rd(struct cpu6502 *cpu, uint16_t addr)
{
    uint32_t r = machine_io_rd_slow((struct machine *)cpu->m, addr, cpu->cyc);
    machine_io_apply(cpu, r);
    return (uint8_t)(r >> 24);
}

INLINE void machine_io_wr(struct cpu6502 *cpu, uint16_t addr, uint8_t val)
{
#ifdef MACHINE_STATS
    uint64_t c0_ = MACHINE_CYCLES();
#endif
    uint32_t r = machine_io_wr_slow((struct machine *)cpu->m, addr, cpu->cyc, val);
    machine_io_apply(cpu, r);
#ifdef MACHINE_STATS
    ((struct machine *)cpu->m)->stat_clk_wr_outer += MACHINE_CYCLES() - c0_;
#endif
}

// ---- lifecycle --------------------------------------------------------------

// jsbeeb's CMOS defaults, via beebjit: MODE 7 boot... as a real unconfigured
// Master would look after *CONFIGURE defaults.
static const uint8_t cmos_defaults[64] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xc9, 0xff, 0xff, 0x12, 0x00,
    0x17, 0xca, 0x1e, 0x05, 0x00, 0x35, 0xa6, 0x00,
};

void machine_init(struct machine *m)
{
    memset(m, 0, sizeof *m);
    memcpy(m->cmos_ram, cmos_defaults, sizeof cmos_defaults);
    m->cpu.cmos = 1;
    m->cpu.m = m;
    m->cpu.rd_tabs[0] = m->rd_tab[0];
    m->cpu.rd_tabs[1] = m->rd_tab[1];
    m->cpu.wr_tabs[0] = m->wr_tab[0];
    m->cpu.wr_tabs[1] = m->wr_tab[1];
    m->sysvia.port_cb = sysvia_port_cb;
    m->sysvia.ctx = m;
    fdc_init(&m->fdc, machine_fdc_nmi, m);
}

void machine_set_disc(struct machine *m, int drive, struct disc *d)
{
    m->fdc.drive[drive & 1].disc = d;
}

void machine_autoboot(struct machine *m)
{
    machine_set_key(m, 0, 0, 1);
    machine_queue_key(m, 3000000, 0, 0, 0);
}

void machine_reset(struct machine *m)
{
    // Power-on RAM contents as beebjit models them (and as many real BBCs
    // behave): main RAM 0xFF except zero page, the Master's extra RAM zero.
    memset(m->ram, 0xff, sizeof m->ram);
    memset(m->ram, 0x00, 0x100);
    memset(m->lynne, 0, sizeof m->lynne);
    memset(m->andy, 0, sizeof m->andy);
    memset(m->hazel, 0, sizeof m->hazel);
    for (int i = 0; i < MACHINE_SWRAM_SLOTS; i++) {
        if (m->swram[i]) {
            memset(m->swram[i], 0, 0x4000);
        }
    }
    m->romsel = 0;
    m->acccon = 0;
    m->ic32 = 0;
    remap_all(m);
    via_reset(&m->sysvia);
    via_reset(&m->usrvia);
    m->ula_ctrl = 0x02;  // teletext, 1 MHz, as beebjit's power-on ULA
    crtc_set_ula(&m->crtc, m->ula_ctrl);
    m->crtc.screen_wrap_add = 0;
    crtc_reset(&m->crtc);
    m->cpu.cycles = 0;
    m->cpu.cyc = 0;
    m->synced = 0;
    m->crtc_synced = 0;
    cpu6502_reset(&m->cpu);
    fdc_power_on_reset(&m->fdc);
}

uint64_t machine_run(struct machine *m, uint64_t until)
{
    return cpu6502_run(&m->cpu, until);
}

int machine_queue_key(struct machine *m, uint32_t cycle, int row, int col, int down)
{
    if (m->key_head == m->key_count) {
        m->key_head = m->key_count = 0;   // drained: start again (live input)
    }
    if (m->key_count >= sizeof m->key_events / sizeof m->key_events[0]) {
        return 0;
    }
    // In cycle order, after any at the same cycle: a key scheduled ahead
    // (a disc's boot key) must not hold back live input queued after it.
    uint32_t i = m->key_count++;
    while (i > m->key_head && (int32_t)(m->key_events[i - 1].cycle - cycle) > 0) {
        m->key_events[i] = m->key_events[i - 1];
        i--;
    }
    struct machine_key_event *e = &m->key_events[i];
    e->cycle = cycle;
    e->row = (uint8_t)row;
    e->col = (uint8_t)col;
    e->down = (uint8_t)down;
    return 1;
}

void machine_set_key(struct machine *m, int row, int col, int down)
{
    uint8_t bit = 1 << (row & 7);
    if (down) {
        m->keys[col & 15] |= bit;
    } else {
        m->keys[col & 15] &= ~bit;
    }
    sysvia_port_a(m);
}
