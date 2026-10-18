// esp32-beeb: cycle-stepped 6502 / 65C12 core. See cpu6502.h.
//
// Structure: computed-goto dispatch per opcode; every bus access is an inline
// rd() or wr() that runs the per-cycle hook first. The addressing-mode
// helpers reproduce the real chip's bus sequence, including the dummy
// accesses, so that Phase 1 can hang cycle-exact peripherals off the hook.
//
// cpu->cmos selects the 65C12 (BBC Master) or the NMOS 6502 (Model B). The
// CMOS quirks follow beebjit's interp.c, which is the oracle:
//   - read-modify-write does read, read, write (NMOS: read, write, write)
//   - abs,X / abs,Y / (zp),Y reads that cross a page spend the extra cycle on
//     a harmless re-read of the last instruction byte, not on the wrong page
//   - abs,X / abs,Y writes without a page crossing put the final address on
//     the bus for the dummy read; with a crossing the dummy is harmless
//   - ASL/LSR/ROL/ROR abs,X take 6 cycles unless the page crosses; INC/DEC
//     abs,X always take 7
//   - decimal ADC/SBC take one extra cycle
//   - JMP (abs) has no page-wrap bug and takes 6 cycles; JMP (abs,X) exists
//   - BRK and interrupts clear D
//   - the unused opcodes are NOPs of defined length and cycle count
// Two places where the 65C02 and beebjit's model of it disagree, and the
// Klaus 65C02 tests side with the chip: BIT immediate sets only Z, and
// decimal-mode N/Z come from the decimal result. The real build follows the
// chip; CPU6502_ORACLE_COMPAT (host trace harness only) follows beebjit so
// trace diffs stay clean. See decisions.md.

#include "cpu6502.h"

#ifndef CPU6502_TICK
#define CPU6502_TICK(cpu) ((void)0)
#endif
#ifndef CPU6502_CODE_ATTR
#define CPU6502_CODE_ATTR
#endif
#ifndef CPU6502_TABLE_ATTR
#define CPU6502_TABLE_ATTR
#endif
#ifndef CPU6502_SYNC
#define CPU6502_SYNC(cpu) ((void)0)
#endif
#ifndef CPU6502_SYNC_WRITE
#define CPU6502_SYNC_WRITE(cpu) CPU6502_SYNC(cpu)
#endif
#ifndef CPU6502_IO_RD
#define CPU6502_IO_RD(cpu, addr) (cycle(cpu), (uint8_t)0xff)
#endif
#ifndef CPU6502_IO_WR
#define CPU6502_IO_WR(cpu, addr, v) (cycle(cpu), (void)(v))
#endif
#ifndef CPU6502_TRACE
#define CPU6502_TRACE(cpu) ((void)0)
#endif
#ifndef CPU6502_TRACE_IRQ
#define CPU6502_TRACE_IRQ(cpu, nmi) ((void)0)
#endif

#define INLINE static inline __attribute__((always_inline))

// A machine with one CPU type defines CPU6502_CMOS_ONLY (1 or 0) and the
// type checks and the other type's paths compile away; otherwise cpu->cmos
// decides at run time (the Klaus harness tests both).
#ifdef CPU6502_CMOS_ONLY
#define IS_CMOS(cpu) (CPU6502_CMOS_ONLY)
#else
#define IS_CMOS(cpu) ((cpu)->cmos)
#endif

// ---- bus -------------------------------------------------------------------

// One bus cycle. The machine is not stepped here (see CPU6502_SYNC).
INLINE void cycle(struct cpu6502 *cpu)
{
    cpu->cyc++;
    CPU6502_TICK(cpu);
}

INLINE uint8_t rd(struct cpu6502 *cpu, uint16_t addr)
{
    uint8_t *p = cpu->rd_tab[addr >> 8];
    if (__builtin_expect(p != 0, 1)) {
        cycle(cpu);
        return p[addr & 0xff];
    }
    return CPU6502_IO_RD(cpu, addr);
}

// A write cycle. Display RAM the CRTC is about to fetch makes the machine
// catch up first, to the end of the previous storing instruction
// (CPU6502_SYNC_WRITE, which reads last_wr): beebjit's video catches up
// after each storing instruction, so the fetches since then see this byte.
INLINE void wr_cycle(struct cpu6502 *cpu, uint16_t addr, uint8_t v, int data)
{
    uint8_t *p = cpu->wr_tab[addr >> 8];
    if (__builtin_expect(p != 0, 1)) {
        cycle(cpu);
        if (addr >= cpu->wlo && addr <= cpu->whi) {
            CPU6502_SYNC_WRITE(cpu);
        }
        p[addr & 0xff] = v;
        if (data) {
            cpu->last_wr = cpu->cyc;
        }
        return;
    }
    CPU6502_IO_WR(cpu, addr, v);
    if (data) {
        cpu->last_wr = cpu->cyc;   // after the access's stretch
    }
}

// A store or read-modify-write data cycle (always the instruction's last).
INLINE void wr(struct cpu6502 *cpu, uint16_t addr, uint8_t v)
{
    wr_cycle(cpu, addr, v, 1);
}

INLINE uint8_t fetch(struct cpu6502 *cpu)
{
    return rd(cpu, cpu->pc++);
}

// Opcode fetch: selects the table set for this instruction (see cpu6502.h).
INLINE uint8_t fetch_opcode(struct cpu6502 *cpu)
{
    int sel = ((cpu->pc >> 13) == 6);
    cpu->rd_tab = cpu->rd_tabs[sel];
    cpu->wr_tab = cpu->wr_tabs[sel];
    cpu->i_poll = cpu->i;
    CPU6502_TRACE(cpu);
    return fetch(cpu);
}

// A cycle the CMOS part spends re-reading the last instruction byte: it hits
// nothing that cares.
INLINE void idle(struct cpu6502 *cpu)
{
    (void)rd(cpu, cpu->pc - 1);
}

INLINE void push(struct cpu6502 *cpu, uint8_t v)
{
    wr_cycle(cpu, 0x100 | cpu->s, v, 0);
    cpu->s = (cpu->s - 1) & 0xff;
}

INLINE uint8_t pull(struct cpu6502 *cpu)
{
    cpu->s = (cpu->s + 1) & 0xff;
    return rd(cpu, 0x100 | cpu->s);
}

// ---- flags -----------------------------------------------------------------

INLINE void set_nz(struct cpu6502 *cpu, uint8_t v)
{
    cpu->n = v & 0x80;
    cpu->z = (v == 0);
}

INLINE uint8_t pack_p(const struct cpu6502 *cpu, int b)
{
    return (cpu->n ? 0x80 : 0) | (cpu->v ? 0x40 : 0) | 0x20 | (b ? 0x10 : 0) |
           (cpu->d ? 0x08 : 0) | (cpu->i ? 0x04 : 0) | (cpu->z ? 0x02 : 0) |
           (cpu->c ? 0x01 : 0);
}

INLINE void unpack_p(struct cpu6502 *cpu, uint8_t p)
{
    cpu->n = p & 0x80;
    cpu->v = (p >> 6) & 1;
    cpu->d = (p >> 3) & 1;
    cpu->i = (p >> 2) & 1;
    cpu->z = (p >> 1) & 1;
    cpu->c = p & 1;
}

uint8_t CPU6502_FN(cpu6502_get_p)(const struct cpu6502 *cpu, int b)
{
    return pack_p(cpu, b);
}

void CPU6502_FN(cpu6502_set_p)(struct cpu6502 *cpu, uint8_t p)
{
    unpack_p(cpu, p);
}

// ---- addressing modes: each returns the effective address having performed
// exactly the bus cycles the chip does before the data cycle. --------------

INLINE uint16_t am_zp(struct cpu6502 *cpu)
{
    return fetch(cpu);
}

INLINE uint16_t am_zpi(struct cpu6502 *cpu, uint8_t idx)
{
    uint8_t zp = fetch(cpu);
    (void)rd(cpu, zp);  // dummy read of the unindexed zero page address
    return (uint8_t)(zp + idx);
}

INLINE uint16_t am_abs(struct cpu6502 *cpu)
{
    uint16_t lo = fetch(cpu);
    return lo | (fetch(cpu) << 8);
}

// Indexed absolute for reads: the extra cycle happens only on page crossing.
INLINE uint16_t am_absi_r(struct cpu6502 *cpu, uint8_t idx)
{
    uint16_t base = am_abs(cpu);
    uint16_t ea = base + idx;
    if ((ea ^ base) & 0xff00) {
        if (IS_CMOS(cpu)) {
            idle(cpu);
        } else {
            (void)rd(cpu, (base & 0xff00) | (ea & 0xff));
        }
    }
    return ea;
}

// Indexed absolute for writes: the dummy read always happens. NMOS reads the
// un-carried address; CMOS reads the final address unless the page crossed.
INLINE uint16_t am_absi_w(struct cpu6502 *cpu, uint8_t idx)
{
    uint16_t base = am_abs(cpu);
    uint16_t ea = base + idx;
    if (IS_CMOS(cpu)) {
        if ((ea ^ base) & 0xff00) {
            idle(cpu);
        } else {
            (void)rd(cpu, ea);
        }
    } else {
        (void)rd(cpu, (base & 0xff00) | (ea & 0xff));
    }
    return ea;
}

INLINE uint16_t am_izx(struct cpu6502 *cpu)
{
    uint8_t zp = fetch(cpu);
    (void)rd(cpu, zp);
    zp += cpu->x;
    uint16_t lo = rd(cpu, zp);
    return lo | (rd(cpu, (uint8_t)(zp + 1)) << 8);
}

INLINE uint16_t am_izy_r(struct cpu6502 *cpu)
{
    uint8_t zp = fetch(cpu);
    uint16_t lo = rd(cpu, zp);
    uint16_t base = lo | (rd(cpu, (uint8_t)(zp + 1)) << 8);
    uint16_t ea = base + cpu->y;
    if ((ea ^ base) & 0xff00) {
        if (IS_CMOS(cpu)) {
            idle(cpu);
        } else {
            (void)rd(cpu, (base & 0xff00) | (ea & 0xff));
        }
    }
    return ea;
}

INLINE uint16_t am_izy_w(struct cpu6502 *cpu)
{
    uint8_t zp = fetch(cpu);
    uint16_t lo = rd(cpu, zp);
    uint16_t base = lo | (rd(cpu, (uint8_t)(zp + 1)) << 8);
    uint16_t ea = base + cpu->y;
    if (IS_CMOS(cpu)) {
        idle(cpu);
    } else {
        (void)rd(cpu, (base & 0xff00) | (ea & 0xff));
    }
    return ea;
}

// CMOS only: (zp) with no index.
INLINE uint16_t am_izp(struct cpu6502 *cpu)
{
    uint8_t zp = fetch(cpu);
    uint16_t lo = rd(cpu, zp);
    return lo | (rd(cpu, (uint8_t)(zp + 1)) << 8);
}

// ---- ALU -------------------------------------------------------------------

INLINE void op_adc(struct cpu6502 *cpu, uint8_t m)
{
    uint8_t a = cpu->a;
    if (!cpu->d) {
        unsigned sum = a + m + cpu->c;
        cpu->v = ((a ^ sum) & (m ^ sum) & 0x80) != 0;
        cpu->c = sum > 0xff;
        cpu->a = (uint8_t)sum;
        set_nz(cpu, cpu->a);
    } else {
        // Decimal. V comes from the intermediate sum before the +0x60
        // adjust on both parts. NMOS: Z from the binary sum, N from the
        // intermediate. CMOS: N and Z from the final decimal result, and an
        // extra cycle. (beebjit applies the NMOS flag rules to the 65C12
        // too; the Klaus 65C02 test wants the CMOS rules, so that wins.)
        unsigned bin = a + m + cpu->c;
        int al = (a & 0x0f) + (m & 0x0f) + cpu->c;
        if (al >= 0x0a) {
            al = ((al + 0x06) & 0x0f) + 0x10;
        }
        int r = (a & 0xf0) + (m & 0xf0) + al;
        int rs = (int8_t)(a & 0xf0) + (int8_t)(m & 0xf0) + al;
        cpu->v = (rs < -128) || (rs > 127);
        cpu->n = r & 0x80;
        if (r >= 0xa0) {
            r += 0x60;
        }
        cpu->c = r >= 0x100;
        cpu->a = (uint8_t)r;
#ifdef CPU6502_ORACLE_COMPAT
        if (IS_CMOS(cpu)) {
            idle(cpu);
        }
        cpu->z = ((bin & 0xff) == 0);  // beebjit: NMOS flag rules on CMOS
#else
        if (IS_CMOS(cpu)) {
            idle(cpu);
            set_nz(cpu, cpu->a);
        } else {
            cpu->z = ((bin & 0xff) == 0);
        }
#endif
    }
}

INLINE void op_sbc(struct cpu6502 *cpu, uint8_t m)
{
    uint8_t a = cpu->a;
    unsigned borrow = cpu->c ^ 1;
    int bin = a - m - borrow;
    uint8_t res = (uint8_t)bin;
    // Flags are binary in both modes (beebjit's rule for both parts).
    cpu->v = ((a ^ m) & (a ^ res) & 0x80) != 0;
    cpu->c = bin >= 0;
    set_nz(cpu, res);
    if (!cpu->d) {
        cpu->a = res;
    } else if (IS_CMOS(cpu)) {
        // 65C02: adjust the whole byte first, then the low nibble, which
        // matters for operands that are not valid BCD. N and Z come from
        // the decimal result, and there is an extra cycle.
        int al = (a & 0x0f) - (m & 0x0f) - borrow;
        int r = bin;
        if (r < 0) {
            r -= 0x60;
        }
        if (al < 0) {
            r -= 0x06;
        }
        cpu->a = (uint8_t)r;
        idle(cpu);
#ifndef CPU6502_ORACLE_COMPAT
        set_nz(cpu, cpu->a);
#endif
    } else {
        int al = (a & 0x0f) - (m & 0x0f) - borrow;
        if (al < 0) {
            al = ((al - 0x06) & 0x0f) - 0x10;
        }
        int r = (a & 0xf0) - (m & 0xf0) + al;
        if (r < 0) {
            r -= 0x60;
        }
        cpu->a = (uint8_t)r;
    }
}

INLINE void op_cmp(struct cpu6502 *cpu, uint8_t r, uint8_t m)
{
    cpu->c = r >= m;
    set_nz(cpu, (uint8_t)(r - m));
}

INLINE void op_bit(struct cpu6502 *cpu, uint8_t m)
{
    cpu->n = m & 0x80;
    cpu->v = (m >> 6) & 1;
    cpu->z = ((cpu->a & m) == 0);
}

INLINE uint8_t op_asl(struct cpu6502 *cpu, uint8_t m)
{
    cpu->c = m >> 7;
    m <<= 1;
    set_nz(cpu, m);
    return m;
}

INLINE uint8_t op_lsr(struct cpu6502 *cpu, uint8_t m)
{
    cpu->c = m & 1;
    m >>= 1;
    set_nz(cpu, m);
    return m;
}

INLINE uint8_t op_rol(struct cpu6502 *cpu, uint8_t m)
{
    uint8_t c = cpu->c;
    cpu->c = m >> 7;
    m = (uint8_t)((m << 1) | c);
    set_nz(cpu, m);
    return m;
}

INLINE uint8_t op_ror(struct cpu6502 *cpu, uint8_t m)
{
    uint8_t c = cpu->c;
    cpu->c = m & 1;
    m = (uint8_t)((m >> 1) | (c << 7));
    set_nz(cpu, m);
    return m;
}

INLINE uint8_t op_inc(struct cpu6502 *cpu, uint8_t m)
{
    m++;
    set_nz(cpu, m);
    return m;
}

INLINE uint8_t op_dec(struct cpu6502 *cpu, uint8_t m)
{
    m--;
    set_nz(cpu, m);
    return m;
}

INLINE uint8_t op_tsb(struct cpu6502 *cpu, uint8_t m)
{
    cpu->z = ((cpu->a & m) == 0);
    return m | cpu->a;
}

INLINE uint8_t op_trb(struct cpu6502 *cpu, uint8_t m)
{
    cpu->z = ((cpu->a & m) == 0);
    return m & ~cpu->a;
}

// Read-modify-write data cycles. NMOS: read, write old, write new.
// CMOS: read, read again, write new.
INLINE void rmw_tail(struct cpu6502 *cpu, uint16_t ea, uint8_t m)
{
    if (IS_CMOS(cpu)) {
        (void)rd(cpu, ea);
    } else {
        wr(cpu, ea, m);
    }
}

// beebjit polls a read-modify-write's interrupts in its last access, not
// before its first, so a 1 MHz stretch does not move the poll: stretch 0.
#define RMW(ea, op)                              \
    do {                                         \
        uint16_t ea_ = (ea);                     \
        uint8_t m_ = rd(cpu, ea_);               \
        rmw_tail(cpu, ea_, m_);                  \
        wr(cpu, ea_, op(cpu, m_));               \
        cpu->stretch = 0;                        \
    } while (0)

// abs,X read-modify-write. The shift/rotate group on CMOS takes 6 cycles
// unless the page crosses; INC/DEC and all NMOS RMW always take 7.
INLINE uint16_t am_absx_rmw(struct cpu6502 *cpu, int six_cycle)
{
    uint16_t base = am_abs(cpu);
    uint16_t ea = base + cpu->x;
    if (!IS_CMOS(cpu)) {
        (void)rd(cpu, (base & 0xff00) | (ea & 0xff));
    } else if ((ea ^ base) & 0xff00) {
        idle(cpu);
    } else if (!six_cycle) {
        (void)rd(cpu, ea);
    }
    return ea;
}

INLINE void branch(struct cpu6502 *cpu, int taken)
{
    int8_t off = (int8_t)fetch(cpu);
    if (!taken) {
        return;
    }
    uint16_t old = cpu->pc;
    uint16_t npc = old + off;
    (void)rd(cpu, old);  // dummy read of the next opcode
    if ((npc ^ old) & 0xff00) {
        if (IS_CMOS(cpu)) {
            idle(cpu);
        } else {
            (void)rd(cpu, (old & 0xff00) | (npc & 0xff));
        }
    }
    cpu->pc = npc;
}

// Interrupt entry: BRK and hardware interrupts share this sequence.
INLINE void interrupt(struct cpu6502 *cpu, uint16_t vec, int b)
{
    push(cpu, cpu->pc >> 8);
    push(cpu, cpu->pc & 0xff);
    push(cpu, pack_p(cpu, b));
    cpu->i = 1;
    cpu->i_poll = 1;  // beebjit: no IRQ straight after BRK or an interrupt
    if (IS_CMOS(cpu)) {
        cpu->d = 0;
    }
    uint16_t lo = rd(cpu, vec);
    cpu->pc = lo | (rd(cpu, vec + 1) << 8);
}

void CPU6502_FN(cpu6502_map_flat)(struct cpu6502 *cpu, uint8_t *mem, uint8_t **table)
{
    for (int i = 0; i < 256; i++) {
        table[i] = mem + i * 256;
    }
    cpu->rd_tabs[0] = cpu->rd_tabs[1] = table;
    cpu->wr_tabs[0] = cpu->wr_tabs[1] = table;
    cpu->rd_tab = cpu->wr_tab = table;
}

void CPU6502_FN(cpu6502_reset)(struct cpu6502 *cpu)
{
    const uint8_t *vec = cpu->rd_tabs[0][0xff];
    // Power-on register values are undefined on the chip; these are
    // beebjit's, so traces match from the first instruction.
    cpu->a = 0xaa;
    cpu->x = cpu->y = 0;
    cpu->s = 0xfd;
    cpu->n = cpu->c = cpu->v = cpu->d = 0;
    cpu->z = 1;
    cpu->i = 1;
    cpu->irq_line = 0;
    cpu->nmi_pending = 0;
    cpu->nmi_cycle = 0;
    cpu->stretch = 0;
    cpu->irq_hist = 0;
    cpu->i_poll = 1;
    cpu->sync_due = 0;
    cpu->last_sync = 0;
    cpu->last_wr = 0;
    cpu->wlo = 0;
    cpu->whi = 0xffff;
    cpu->rd_tab = cpu->rd_tabs[0];
    cpu->wr_tab = cpu->wr_tabs[0];
    cpu->pc = vec[0xfc] | (vec[0xfd] << 8);
}

// The machine moved the sync point (a catch-up, an access).
INLINE void cpu6502_set_sync_due(struct cpu6502 *cpu, uint32_t due)
{
    cpu->sync_due = due;
    cpu->stop = ((int32_t)(due - cpu->run_end) < 0) ? due : cpu->run_end;
}

// The history brought up to the current cycle: the line has held irq_line
// since last_sync.
INLINE void cpu_hist_update(struct cpu6502 *cpu)
{
    uint32_t n = cpu->cyc - cpu->last_sync;
    uint32_t fill = cpu->irq_line ? 0xffffffffu : 0;
    cpu->irq_hist = (n >= 16) ? fill : ((cpu->irq_hist << n) | (fill & ((1u << n) - 1)));
    cpu->last_sync = cpu->cyc;
}

// Bit 2 of the history is the IRQ line two cycles before the instruction's
// end, and an NMI edge counts if it was latched by then: the poll sits two
// cycles into the instruction's own cycles, before a 1 MHz access's stretch
// (beebjit's interpreter polls before the access), so an edge in the
// stretch waits. An IRQ that is being taken vectors through NMI instead if
// the edge arrived after the poll (the 65C12 as beebjit models it).
// For the IRQ line, a two-cycle stretch moves the poll back two bits; a
// one-cycle stretch (the access started in step with the 1 MHz clock, on
// which the VIA timers change) leaves it at bit 2, since a timer event on
// the polled tick fires before beebjit's poll. Read-modify-writes poll in
// their last access in beebjit and clear the stretch (RMW above).
// tools/irqpoll.sh checks this for each addressing mode against beebjit.
// Returns 1 if an interrupt sequence ran.
static CPU6502_CODE_ATTR __attribute__((noinline)) int take_interrupt(struct cpu6502 *cpu)
{
    cpu_hist_update(cpu);
    int nmi_polled = cpu->nmi_pending && (int32_t)(cpu->cyc - cpu->stretch - cpu->nmi_cycle) >= 2;
    uint32_t poll_bit = (cpu->stretch == 2) ? 4 : 2;
    if (!nmi_polled && !(((cpu->irq_hist >> poll_bit) & 1) && !cpu->i_poll)) {
        return 0;
    }
    int take_nmi = cpu->nmi_pending;
    cpu->nmi_pending = 0;
    CPU6502_TRACE_IRQ(cpu, take_nmi);
    (void)rd(cpu, cpu->pc);
    (void)rd(cpu, cpu->pc);
    if (!take_nmi) {
        // beebjit looks for an NMI three cycles into the IRQ sequence and,
        // if one has been latched by then, takes the NMI vector instead:
        // one sequence, not two. The machine is brought to that cycle first
        // if a sync point (an NMI edge among them) has come due.
        push(cpu, cpu->pc >> 8);
        if ((int32_t)(cpu->cyc - cpu->sync_due) >= 0) {
            CPU6502_SYNC(cpu);
        }
        if (cpu->nmi_pending) {
            cpu->nmi_pending = 0;
            take_nmi = 1;
        }
        push(cpu, cpu->pc & 0xff);
        push(cpu, pack_p(cpu, 0));
        cpu->i = 1;
        cpu->i_poll = 1;
        if (IS_CMOS(cpu)) {
            cpu->d = 0;
        }
        uint16_t vec = take_nmi ? 0xfffa : 0xfffe;
        uint16_t lo = rd(cpu, vec);
        cpu->pc = lo | (rd(cpu, vec + 1) << 8);
        return 1;
    }
    interrupt(cpu, 0xfffa, 0);
    return 1;
}

CPU6502_CODE_ATTR
uint64_t CPU6502_FN(cpu6502_run)(struct cpu6502 *cpu_in, uint64_t until)
{
    // Work on a local copy. A byte store through cpu->mem could alias any
    // field of *cpu_in, so with the pointer the compiler reloads every
    // register after every bus write (measured: 6 clocks per cycle on the
    // ESP32-S3). A local whose address never escapes cannot be aliased by
    // mem[], so its fields stay in machine registers.
    struct cpu6502 local = *cpu_in;
    struct cpu6502 *cpu = &local;
    // The hot loop counts in 32 bits (cpu->cyc); the 64-bit total is folded
    // in on exit. A slice is never anywhere near 2^31 cycles.
    uint32_t start = cpu->cyc;
    uint32_t end = start + (uint32_t)(until - cpu->cycles);

    // Computed-goto dispatch: one load and one indirect jump per opcode. GCC
    // for Xtensa turns the equivalent switch into a compare tree, which
    // Phase 0 measured at 9 to 10 clocks per cycle slower. Tables live
    // wherever CPU6502_TABLE_ATTR says (DRAM on the ESP32-S3).
    //
    // NMOS table: documented opcodes; the undocumented ones go to op_undoc
    // until a Model B profile needs them. CMOS table: the full 65C12 map.
#define T16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    &&op_##a, &&op_##b, &&op_##c, &&op_##d, &&op_##e, &&op_##f, &&op_##g, &&op_##h, \
    &&op_##i, &&op_##j, &&op_##k, &&op_##l, &&op_##m, &&op_##n, &&op_##o, &&op_##p,
    static CPU6502_TABLE_ATTR void *const nmos_table[256] = {
        T16(00, 01, undoc, undoc, undoc, 05, 06, undoc, 08, 09, 0a, undoc, undoc, 0d, 0e, undoc)
        T16(10, 11, undoc, undoc, undoc, 15, 16, undoc, 18, 19, undoc, undoc, undoc, 1d, 1e, undoc)
        T16(20, 21, undoc, undoc, 24, 25, 26, undoc, 28, 29, 2a, undoc, 2c, 2d, 2e, undoc)
        T16(30, 31, undoc, undoc, undoc, 35, 36, undoc, 38, 39, undoc, undoc, undoc, 3d, 3e, undoc)
        T16(40, 41, undoc, undoc, undoc, 45, 46, undoc, 48, 49, 4a, undoc, 4c, 4d, 4e, undoc)
        T16(50, 51, undoc, undoc, undoc, 55, 56, undoc, 58, 59, undoc, undoc, undoc, 5d, 5e, undoc)
        T16(60, 61, undoc, undoc, undoc, 65, 66, undoc, 68, 69, 6a, undoc, 6c, 6d, 6e, undoc)
        T16(70, 71, undoc, undoc, undoc, 75, 76, undoc, 78, 79, undoc, undoc, undoc, 7d, 7e, undoc)
        T16(undoc, 81, undoc, undoc, 84, 85, 86, undoc, 88, undoc, 8a, undoc, 8c, 8d, 8e, undoc)
        T16(90, 91, undoc, undoc, 94, 95, 96, undoc, 98, 99, 9a, undoc, undoc, 9d, undoc, undoc)
        T16(a0, a1, a2, undoc, a4, a5, a6, undoc, a8, a9, aa, undoc, ac, ad, ae, undoc)
        T16(b0, b1, undoc, undoc, b4, b5, b6, undoc, b8, b9, ba, undoc, bc, bd, be, undoc)
        T16(c0, c1, undoc, undoc, c4, c5, c6, undoc, c8, c9, ca, undoc, cc, cd, ce, undoc)
        T16(d0, d1, undoc, undoc, undoc, d5, d6, undoc, d8, d9, undoc, undoc, undoc, dd, de, undoc)
        T16(e0, e1, undoc, undoc, e4, e5, e6, undoc, e8, e9, ea, undoc, ec, ed, ee, undoc)
        T16(f0, f1, undoc, undoc, undoc, f5, f6, undoc, f8, f9, undoc, undoc, undoc, fd, fe, undoc)
    };
    // nop1: 1 byte 1 cycle. nop2: 2 bytes 2 cycles. nopzp/nopzpx: 2 bytes with
    // a zero page read (3/4 cycles). nop8: 3 bytes 8 cycles. nopabs: 3 bytes
    // with an absolute read (4 cycles).
    static CPU6502_TABLE_ATTR void *const cmos_table[256] = {
        T16(00, 01, nop2, nop1, 04, 05, 06, nop1, 08, 09, 0a, nop1, 0c, 0d, 0e, nop1)
        T16(10, 11, 12, nop1, 14, 15, 16, nop1, 18, 19, 1a, nop1, 1c, 1d, 1e, nop1)
        T16(20, 21, nop2, nop1, 24, 25, 26, nop1, 28, 29, 2a, nop1, 2c, 2d, 2e, nop1)
        T16(30, 31, 32, nop1, 34, 35, 36, nop1, 38, 39, 3a, nop1, 3c, 3d, 3e, nop1)
        T16(40, 41, nop2, nop1, nopzp, 45, 46, nop1, 48, 49, 4a, nop1, 4c, 4d, 4e, nop1)
        T16(50, 51, 52, nop1, nopzpx, 55, 56, nop1, 58, 59, 5a, nop1, nop8, 5d, 5e, nop1)
        T16(60, 61, nop2, nop1, 64, 65, 66, nop1, 68, 69, 6a, nop1, 6c, 6d, 6e, nop1)
        T16(70, 71, 72, nop1, 74, 75, 76, nop1, 78, 79, 7a, nop1, 7c, 7d, 7e, nop1)
        T16(80, 81, nop2, nop1, 84, 85, 86, nop1, 88, 89, 8a, nop1, 8c, 8d, 8e, nop1)
        T16(90, 91, 92, nop1, 94, 95, 96, nop1, 98, 99, 9a, nop1, 9c, 9d, 9e, nop1)
        T16(a0, a1, a2, nop1, a4, a5, a6, nop1, a8, a9, aa, nop1, ac, ad, ae, nop1)
        T16(b0, b1, b2, nop1, b4, b5, b6, nop1, b8, b9, ba, nop1, bc, bd, be, nop1)
        T16(c0, c1, nop2, nop1, c4, c5, c6, nop1, c8, c9, ca, nop1, cc, cd, ce, nop1)
        T16(d0, d1, d2, nop1, nopzpx, d5, d6, nop1, d8, d9, da, nop1, nopabs, dd, de, nop1)
        T16(e0, e1, nop2, nop1, e4, e5, e6, nop1, e8, e9, ea, nop1, ec, ed, ee, nop1)
        T16(f0, f1, f2, nop1, nopzpx, f5, f6, nop1, f8, f9, fa, nop1, nopabs, fd, fe, nop1)
    };
#undef T16
    void *const *dispatch = IS_CMOS(cpu) ? cmos_table : nmos_table;



#define OPC(h) op_##h:
#define NEXT continue

    cpu->run_end = end;
    cpu6502_set_sync_due(cpu, cpu->sync_due);
    for (;;) {
        // One test per instruction: the end of the run or the sync point.
        if ((int32_t)(cpu->cyc - cpu->stop) >= 0) {
            if ((int32_t)(cpu->cyc - end) >= 0) {
                break;
            }
            CPU6502_SYNC(cpu);
        }
        // Interrupt decision for the instruction that just finished. The
        // test here must stay tiny: this loop head is duplicated into every
        // opcode's tail by the compiler, so the rest is out of line. The
        // history is kept lazily: the polled bit can only be set if the line is
        // high or was within the last five cycles of the history (bit 4
        // after a two-cycle stretch), and take_interrupt brings it up to
        // date before deciding.
        if ((cpu->nmi_pending || ((cpu->irq_line || (cpu->irq_hist & 0x1f)) && !cpu->i_poll)) && take_interrupt(cpu)) {
            continue;
        }

        cpu->stretch = 0;
        uint8_t op = fetch_opcode(cpu);
        uint16_t ea;
        uint8_t t;

        goto *dispatch[op];

        // ---- loads ----
        OPC(a9) set_nz(cpu, cpu->a = fetch(cpu)); NEXT;
        OPC(a5) set_nz(cpu, cpu->a = rd(cpu, am_zp(cpu))); NEXT;
        OPC(b5) set_nz(cpu, cpu->a = rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(ad) set_nz(cpu, cpu->a = rd(cpu, am_abs(cpu))); NEXT;
        OPC(bd) set_nz(cpu, cpu->a = rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(b9) set_nz(cpu, cpu->a = rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(a1) set_nz(cpu, cpu->a = rd(cpu, am_izx(cpu))); NEXT;
        OPC(b1) set_nz(cpu, cpu->a = rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(b2) set_nz(cpu, cpu->a = rd(cpu, am_izp(cpu))); NEXT;

        OPC(a2) set_nz(cpu, cpu->x = fetch(cpu)); NEXT;
        OPC(a6) set_nz(cpu, cpu->x = rd(cpu, am_zp(cpu))); NEXT;
        OPC(b6) set_nz(cpu, cpu->x = rd(cpu, am_zpi(cpu, cpu->y))); NEXT;
        OPC(ae) set_nz(cpu, cpu->x = rd(cpu, am_abs(cpu))); NEXT;
        OPC(be) set_nz(cpu, cpu->x = rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;

        OPC(a0) set_nz(cpu, cpu->y = fetch(cpu)); NEXT;
        OPC(a4) set_nz(cpu, cpu->y = rd(cpu, am_zp(cpu))); NEXT;
        OPC(b4) set_nz(cpu, cpu->y = rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(ac) set_nz(cpu, cpu->y = rd(cpu, am_abs(cpu))); NEXT;
        OPC(bc) set_nz(cpu, cpu->y = rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;

        // ---- stores ----
        OPC(85) wr(cpu, am_zp(cpu), cpu->a); NEXT;
        OPC(95) wr(cpu, am_zpi(cpu, cpu->x), cpu->a); NEXT;
        OPC(8d) wr(cpu, am_abs(cpu), cpu->a); NEXT;
        OPC(9d) wr(cpu, am_absi_w(cpu, cpu->x), cpu->a); NEXT;
        OPC(99) wr(cpu, am_absi_w(cpu, cpu->y), cpu->a); NEXT;
        OPC(81) wr(cpu, am_izx(cpu), cpu->a); NEXT;
        OPC(91) wr(cpu, am_izy_w(cpu), cpu->a); NEXT;
        OPC(92) wr(cpu, am_izp(cpu), cpu->a); NEXT;

        OPC(86) wr(cpu, am_zp(cpu), cpu->x); NEXT;
        OPC(96) wr(cpu, am_zpi(cpu, cpu->y), cpu->x); NEXT;
        OPC(8e) wr(cpu, am_abs(cpu), cpu->x); NEXT;

        OPC(84) wr(cpu, am_zp(cpu), cpu->y); NEXT;
        OPC(94) wr(cpu, am_zpi(cpu, cpu->x), cpu->y); NEXT;
        OPC(8c) wr(cpu, am_abs(cpu), cpu->y); NEXT;

        OPC(64) wr(cpu, am_zp(cpu), 0); NEXT;
        OPC(74) wr(cpu, am_zpi(cpu, cpu->x), 0); NEXT;
        OPC(9c) wr(cpu, am_abs(cpu), 0); NEXT;
        OPC(9e) wr(cpu, am_absi_w(cpu, cpu->x), 0); NEXT;

        // ---- transfers ----
        OPC(aa) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->x = cpu->a); NEXT;
        OPC(a8) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->y = cpu->a); NEXT;
        OPC(8a) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->a = cpu->x); NEXT;
        OPC(98) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->a = cpu->y); NEXT;
        OPC(ba) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->x = cpu->s); NEXT;
        OPC(9a) (void)rd(cpu, cpu->pc); cpu->s = cpu->x; NEXT;

        // ---- stack ----
        OPC(48) (void)rd(cpu, cpu->pc); push(cpu, cpu->a); NEXT;
        OPC(08) (void)rd(cpu, cpu->pc); push(cpu, pack_p(cpu, 1)); NEXT;
        OPC(da) (void)rd(cpu, cpu->pc); push(cpu, cpu->x); NEXT;
        OPC(5a) (void)rd(cpu, cpu->pc); push(cpu, cpu->y); NEXT;
        OPC(68)
            (void)rd(cpu, cpu->pc);
            (void)rd(cpu, 0x100 | cpu->s);
            set_nz(cpu, cpu->a = pull(cpu));
            NEXT;
        OPC(28)
            (void)rd(cpu, cpu->pc);
            (void)rd(cpu, 0x100 | cpu->s);
            unpack_p(cpu, pull(cpu));
            NEXT;
        OPC(fa)
            (void)rd(cpu, cpu->pc);
            (void)rd(cpu, 0x100 | cpu->s);
            set_nz(cpu, cpu->x = pull(cpu));
            NEXT;
        OPC(7a)
            (void)rd(cpu, cpu->pc);
            (void)rd(cpu, 0x100 | cpu->s);
            set_nz(cpu, cpu->y = pull(cpu));
            NEXT;

        // ---- ALU ----
        OPC(69) op_adc(cpu, fetch(cpu)); NEXT;
        OPC(65) op_adc(cpu, rd(cpu, am_zp(cpu))); NEXT;
        OPC(75) op_adc(cpu, rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(6d) op_adc(cpu, rd(cpu, am_abs(cpu))); NEXT;
        OPC(7d) op_adc(cpu, rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(79) op_adc(cpu, rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(61) op_adc(cpu, rd(cpu, am_izx(cpu))); NEXT;
        OPC(71) op_adc(cpu, rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(72) op_adc(cpu, rd(cpu, am_izp(cpu))); NEXT;

        OPC(e9) op_sbc(cpu, fetch(cpu)); NEXT;
        OPC(e5) op_sbc(cpu, rd(cpu, am_zp(cpu))); NEXT;
        OPC(f5) op_sbc(cpu, rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(ed) op_sbc(cpu, rd(cpu, am_abs(cpu))); NEXT;
        OPC(fd) op_sbc(cpu, rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(f9) op_sbc(cpu, rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(e1) op_sbc(cpu, rd(cpu, am_izx(cpu))); NEXT;
        OPC(f1) op_sbc(cpu, rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(f2) op_sbc(cpu, rd(cpu, am_izp(cpu))); NEXT;

        OPC(29) set_nz(cpu, cpu->a &= fetch(cpu)); NEXT;
        OPC(25) set_nz(cpu, cpu->a &= rd(cpu, am_zp(cpu))); NEXT;
        OPC(35) set_nz(cpu, cpu->a &= rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(2d) set_nz(cpu, cpu->a &= rd(cpu, am_abs(cpu))); NEXT;
        OPC(3d) set_nz(cpu, cpu->a &= rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(39) set_nz(cpu, cpu->a &= rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(21) set_nz(cpu, cpu->a &= rd(cpu, am_izx(cpu))); NEXT;
        OPC(31) set_nz(cpu, cpu->a &= rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(32) set_nz(cpu, cpu->a &= rd(cpu, am_izp(cpu))); NEXT;

        OPC(09) set_nz(cpu, cpu->a |= fetch(cpu)); NEXT;
        OPC(05) set_nz(cpu, cpu->a |= rd(cpu, am_zp(cpu))); NEXT;
        OPC(15) set_nz(cpu, cpu->a |= rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(0d) set_nz(cpu, cpu->a |= rd(cpu, am_abs(cpu))); NEXT;
        OPC(1d) set_nz(cpu, cpu->a |= rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(19) set_nz(cpu, cpu->a |= rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(01) set_nz(cpu, cpu->a |= rd(cpu, am_izx(cpu))); NEXT;
        OPC(11) set_nz(cpu, cpu->a |= rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(12) set_nz(cpu, cpu->a |= rd(cpu, am_izp(cpu))); NEXT;

        OPC(49) set_nz(cpu, cpu->a ^= fetch(cpu)); NEXT;
        OPC(45) set_nz(cpu, cpu->a ^= rd(cpu, am_zp(cpu))); NEXT;
        OPC(55) set_nz(cpu, cpu->a ^= rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(4d) set_nz(cpu, cpu->a ^= rd(cpu, am_abs(cpu))); NEXT;
        OPC(5d) set_nz(cpu, cpu->a ^= rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(59) set_nz(cpu, cpu->a ^= rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(41) set_nz(cpu, cpu->a ^= rd(cpu, am_izx(cpu))); NEXT;
        OPC(51) set_nz(cpu, cpu->a ^= rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(52) set_nz(cpu, cpu->a ^= rd(cpu, am_izp(cpu))); NEXT;

        OPC(c9) op_cmp(cpu, cpu->a, fetch(cpu)); NEXT;
        OPC(c5) op_cmp(cpu, cpu->a, rd(cpu, am_zp(cpu))); NEXT;
        OPC(d5) op_cmp(cpu, cpu->a, rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(cd) op_cmp(cpu, cpu->a, rd(cpu, am_abs(cpu))); NEXT;
        OPC(dd) op_cmp(cpu, cpu->a, rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
        OPC(d9) op_cmp(cpu, cpu->a, rd(cpu, am_absi_r(cpu, cpu->y))); NEXT;
        OPC(c1) op_cmp(cpu, cpu->a, rd(cpu, am_izx(cpu))); NEXT;
        OPC(d1) op_cmp(cpu, cpu->a, rd(cpu, am_izy_r(cpu))); NEXT;
        OPC(d2) op_cmp(cpu, cpu->a, rd(cpu, am_izp(cpu))); NEXT;

        OPC(e0) op_cmp(cpu, cpu->x, fetch(cpu)); NEXT;
        OPC(e4) op_cmp(cpu, cpu->x, rd(cpu, am_zp(cpu))); NEXT;
        OPC(ec) op_cmp(cpu, cpu->x, rd(cpu, am_abs(cpu))); NEXT;
        OPC(c0) op_cmp(cpu, cpu->y, fetch(cpu)); NEXT;
        OPC(c4) op_cmp(cpu, cpu->y, rd(cpu, am_zp(cpu))); NEXT;
        OPC(cc) op_cmp(cpu, cpu->y, rd(cpu, am_abs(cpu))); NEXT;

        OPC(24) op_bit(cpu, rd(cpu, am_zp(cpu))); NEXT;
        OPC(2c) op_bit(cpu, rd(cpu, am_abs(cpu))); NEXT;
        OPC(34) op_bit(cpu, rd(cpu, am_zpi(cpu, cpu->x))); NEXT;
        OPC(3c) op_bit(cpu, rd(cpu, am_absi_r(cpu, cpu->x))); NEXT;
#ifdef CPU6502_ORACLE_COMPAT
        OPC(89) op_bit(cpu, fetch(cpu)); NEXT;  // beebjit: full BIT flags
#else
        OPC(89) cpu->z = ((cpu->a & fetch(cpu)) == 0); NEXT;  // BIT imm: Z only
#endif

        OPC(04) RMW(am_zp(cpu), op_tsb); NEXT;
        OPC(0c) RMW(am_abs(cpu), op_tsb); NEXT;
        OPC(14) RMW(am_zp(cpu), op_trb); NEXT;
        OPC(1c) RMW(am_abs(cpu), op_trb); NEXT;

        // ---- shifts ----
        OPC(0a) (void)rd(cpu, cpu->pc); cpu->a = op_asl(cpu, cpu->a); NEXT;
        OPC(06) RMW(am_zp(cpu), op_asl); NEXT;
        OPC(16) RMW(am_zpi(cpu, cpu->x), op_asl); NEXT;
        OPC(0e) RMW(am_abs(cpu), op_asl); NEXT;
        OPC(1e) RMW(am_absx_rmw(cpu, 1), op_asl); NEXT;

        OPC(4a) (void)rd(cpu, cpu->pc); cpu->a = op_lsr(cpu, cpu->a); NEXT;
        OPC(46) RMW(am_zp(cpu), op_lsr); NEXT;
        OPC(56) RMW(am_zpi(cpu, cpu->x), op_lsr); NEXT;
        OPC(4e) RMW(am_abs(cpu), op_lsr); NEXT;
        OPC(5e) RMW(am_absx_rmw(cpu, 1), op_lsr); NEXT;

        OPC(2a) (void)rd(cpu, cpu->pc); cpu->a = op_rol(cpu, cpu->a); NEXT;
        OPC(26) RMW(am_zp(cpu), op_rol); NEXT;
        OPC(36) RMW(am_zpi(cpu, cpu->x), op_rol); NEXT;
        OPC(2e) RMW(am_abs(cpu), op_rol); NEXT;
        OPC(3e) RMW(am_absx_rmw(cpu, 1), op_rol); NEXT;

        OPC(6a) (void)rd(cpu, cpu->pc); cpu->a = op_ror(cpu, cpu->a); NEXT;
        OPC(66) RMW(am_zp(cpu), op_ror); NEXT;
        OPC(76) RMW(am_zpi(cpu, cpu->x), op_ror); NEXT;
        OPC(6e) RMW(am_abs(cpu), op_ror); NEXT;
        OPC(7e) RMW(am_absx_rmw(cpu, 1), op_ror); NEXT;

        // ---- inc/dec ----
        OPC(e6) RMW(am_zp(cpu), op_inc); NEXT;
        OPC(f6) RMW(am_zpi(cpu, cpu->x), op_inc); NEXT;
        OPC(ee) RMW(am_abs(cpu), op_inc); NEXT;
        OPC(fe) RMW(am_absx_rmw(cpu, 0), op_inc); NEXT;
        OPC(c6) RMW(am_zp(cpu), op_dec); NEXT;
        OPC(d6) RMW(am_zpi(cpu, cpu->x), op_dec); NEXT;
        OPC(ce) RMW(am_abs(cpu), op_dec); NEXT;
        OPC(de) RMW(am_absx_rmw(cpu, 0), op_dec); NEXT;
        OPC(e8) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->x = (cpu->x + 1) & 0xff); NEXT;
        OPC(ca) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->x = (cpu->x - 1) & 0xff); NEXT;
        OPC(c8) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->y = (cpu->y + 1) & 0xff); NEXT;
        OPC(88) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->y = (cpu->y - 1) & 0xff); NEXT;
        OPC(1a) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->a = (cpu->a + 1) & 0xff); NEXT;
        OPC(3a) (void)rd(cpu, cpu->pc); set_nz(cpu, cpu->a = (cpu->a - 1) & 0xff); NEXT;

        // ---- flags ----
        OPC(18) (void)rd(cpu, cpu->pc); cpu->c = 0; NEXT;
        OPC(38) (void)rd(cpu, cpu->pc); cpu->c = 1; NEXT;
        OPC(58) (void)rd(cpu, cpu->pc); cpu->i = 0; NEXT;
        OPC(78) (void)rd(cpu, cpu->pc); cpu->i = 1; NEXT;
        OPC(b8) (void)rd(cpu, cpu->pc); cpu->v = 0; NEXT;
        OPC(d8) (void)rd(cpu, cpu->pc); cpu->d = 0; NEXT;
        OPC(f8) (void)rd(cpu, cpu->pc); cpu->d = 1; NEXT;
        OPC(ea) (void)rd(cpu, cpu->pc); NEXT;

        // ---- branches ----
        OPC(10) branch(cpu, !cpu->n); NEXT;
        OPC(30) branch(cpu, cpu->n); NEXT;
        OPC(50) branch(cpu, !cpu->v); NEXT;
        OPC(70) branch(cpu, cpu->v); NEXT;
        OPC(90) branch(cpu, !cpu->c); NEXT;
        OPC(b0) branch(cpu, cpu->c); NEXT;
        OPC(d0) branch(cpu, !cpu->z); NEXT;
        OPC(f0) branch(cpu, cpu->z); NEXT;
        OPC(80) branch(cpu, 1); NEXT;

        // ---- jumps and calls ----
        OPC(4c) cpu->pc = am_abs(cpu); NEXT;
        OPC(6c)
            ea = am_abs(cpu);
            if (IS_CMOS(cpu)) {
                idle(cpu);
                t = rd(cpu, ea);
                cpu->pc = t | (rd(cpu, ea + 1) << 8);
            } else {
                t = rd(cpu, ea);
                // NMOS bug: the high byte read does not carry into the page.
                cpu->pc = t | (rd(cpu, (ea & 0xff00) | ((ea + 1) & 0xff)) << 8);
            }
            NEXT;
        OPC(7c)
            ea = am_abs(cpu);
            idle(cpu);
            ea += cpu->x;
            t = rd(cpu, ea);
            cpu->pc = t | (rd(cpu, ea + 1) << 8);
            NEXT;
        OPC(20)
            t = fetch(cpu);
            (void)rd(cpu, 0x100 | cpu->s);
            push(cpu, cpu->pc >> 8);
            push(cpu, cpu->pc & 0xff);
            cpu->pc = t | (fetch(cpu) << 8);
            NEXT;
        OPC(60)
            (void)rd(cpu, cpu->pc);
            (void)rd(cpu, 0x100 | cpu->s);
            t = pull(cpu);
            cpu->pc = t | (pull(cpu) << 8);
            (void)rd(cpu, cpu->pc);
            cpu->pc++;
            NEXT;
        OPC(40)
            (void)rd(cpu, cpu->pc);
            (void)rd(cpu, 0x100 | cpu->s);
            unpack_p(cpu, pull(cpu));
            cpu->i_poll = cpu->i;  // pulled I is what the poll sees
            t = pull(cpu);
            cpu->pc = t | (pull(cpu) << 8);
            NEXT;
        OPC(00)
            (void)fetch(cpu);  // padding byte
            interrupt(cpu, 0xfffe, 1);
            NEXT;

        // ---- CMOS defined NOPs ----
        OPC(nop1) cpu->i_poll = 1; NEXT;   // not polled: no IRQ after it
        OPC(nop2) (void)fetch(cpu); NEXT;
        OPC(nopzp) (void)rd(cpu, am_zp(cpu)); NEXT;
        OPC(nopzpx) (void)rd(cpu, am_zpi(cpu, cpu->x)); NEXT;
        OPC(nopabs) (void)rd(cpu, am_abs(cpu)); NEXT;
        OPC(nop8)
            (void)am_abs(cpu);
            for (t = 0; t < 5; t++) {
                idle(cpu);
            }
            NEXT;

        // Undocumented NMOS opcode: not implemented until a Model B profile
        // needs them. Treated as a one-byte, two-cycle NOP.
        OPC(undoc) (void)rd(cpu, cpu->pc); NEXT;
    }

    CPU6502_SYNC(cpu);
    uint32_t ran = cpu->cyc - start;
    cpu->cycles += ran;
    *cpu_in = local;
    return ran;
}
