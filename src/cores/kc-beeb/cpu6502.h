// esp32-beeb: cycle-stepped 6502 / 65C12 core.
//
// Plain C, no ESP headers: this file and cpu6502.c compile on a desktop
// compiler for trace diffs against beebjit.
//
// Every bus access is one emulated cycle and goes through one inline read or
// write, which first runs the per-cycle hook (CPU6502_TICK). The machine
// hangs the CRTC/ULA/VIA step off that hook.
//
// cpu->cmos = 1 gives the 65C12 in the Master 128 (full opcode map, CMOS bus
// quirks); 0 gives the NMOS 6502 with the documented opcodes only.
//
// Memory is a table of 256 pages of 256 bytes, one table for reads and one
// for writes. A NULL page sends the access to the CPU6502_IO_RD / _WR hooks,
// which the machine defines and which do their own cycle ticking (the 1 MHz
// bus stretch lives there). There are two table sets: set 1 is used while
// the current opcode was fetched from 0xC000-0xDFFF (the Master's VDU-driver
// shadow rule), set 0 otherwise. A machine without that rule points both at
// the same tables. ROM pages have a write table entry pointing at a scratch
// page so stores to ROM vanish without a branch.

#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>

struct cpu6502 {
    uint16_t pc;
    // Kept 32 bits wide (always 0-255): byte fields cost a bit-field insert
    // at every write on the M33.
    uint32_t a, x, y, s;
    // Flags kept unpacked. n holds bit 7 only (0 or 0x80), the rest are 0/1.
    uint32_t n, z, c, v, i, d;
    uint32_t cmos;  // 1: 65C12 (Master), 0: NMOS 6502 (Model B)
    // Interrupts. irq_hist holds one bit per cycle, newest in bit 0, of the
    // IRQ line level after that cycle; the machine appends to it when it
    // catches up (CPU6502_SYNC). nmi_pending is set by the machine on the
    // NMI edge. An IRQ is taken after an instruction if the line was
    // asserted two cycles before its end (the chip polls before the
    // penultimate cycle) and I was clear at the instruction start (RTI: as
    // pulled); i_poll is that I value (the one-cycle NOPs set it, as they
    // are not polled).
    uint32_t irq_line;
    uint32_t nmi_pending;
    uint32_t nmi_cycle;   // cycle the NMI edge was latched on
    uint32_t stretch;     // 1 MHz stretch cycles in the current instruction
    uint32_t irq_hist;    // as of last_sync: the line has been irq_line since
    uint32_t i_poll;
    // Cycle count at which the machine next has an event that can change
    // the IRQ line or needs exact ordering; until then the instruction
    // boundary needs no catch-up and the history is just the current level
    // repeated for the cycles since the last catch-up (last_sync). stop is
    // the earlier of sync_due and the end of the run, the loop's one test;
    // whoever moves sync_due sets it through cpu6502_set_sync_due().
    uint32_t sync_due, last_sync;
    uint32_t stop, run_end;
    // Writes to addresses in [wlo, whi] need a catch-up first: that is the
    // display memory the CRTC will fetch before sync_due. The machine sets
    // it; 0..0xFFFF means every RAM write syncs.
    uint16_t wlo, whi;
    // The cycle count at the end of the last instruction that stored to
    // memory (a store or read-modify-write: the write is its last cycle;
    // not the stack). beebjit brings its video up to date after each such
    // instruction, so a display write is visible to every fetch since.
    uint32_t last_wr;
    uint32_t cyc;      // running cycle counter used by the hot loop, wraps
    uint64_t cycles;   // total, updated when cpu6502_run returns
    // Memory page tables, see above. rd_tab/wr_tab are the set in use.
    uint8_t *const *rd_tab, *const *wr_tab;
    uint8_t *const *rd_tabs[2], *const *wr_tabs[2];
    // The owning machine, for the hooks.
    void *m;
    // Scratch for the per-cycle hook. Lives in the struct so that it is part
    // of the local copy cpu6502_run works on (see the aliasing note there).
    uint32_t hook[4];
};


// cpu6502.c is compiled through a wrapper that may define these first:
//   CPU6502_TICK(cpu)      per-cycle hook, default nothing; the machine
//                          leaves it empty and catches up in CPU6502_SYNC
//   CPU6502_SYNC(cpu)      called at every instruction start and before a
//                          RAM write: the machine steps up to cpu->cyc and
//                          appends one IRQ level bit per cycle to irq_hist
//   CPU6502_IO_RD(cpu, a)  read of a NULL page; must count the data cycle
//   CPU6502_IO_WR(cpu, a, v) write of a NULL page; must count the data cycle
//   CPU6502_TRACE(cpu)     called at every opcode fetch, default nothing
//   CPU6502_TRACE_IRQ(cpu, nmi) called when an interrupt is taken
//   CPU6502_CODE_ATTR      attribute for the hot functions (IRAM_ATTR)
//   CPU6502_TABLE_ATTR     attribute for the dispatch table (DRAM_ATTR)
//   CPU6502_USE_SWITCH     dispatch with a switch instead of computed goto
//   CPU6502_FN(name)       renames the public entry points per variant
#ifndef CPU6502_FN
#define CPU6502_FN(name) name
#endif

void CPU6502_FN(cpu6502_reset)(struct cpu6502 *cpu);
// Fills a 256-entry table so that every page maps straight into a flat
// 64 KB array, and points both table sets at it. For the test harnesses.
void CPU6502_FN(cpu6502_map_flat)(struct cpu6502 *cpu, uint8_t *mem, uint8_t **table);
// Runs whole instructions until cpu->cycles >= until. Returns cycles run.
uint64_t CPU6502_FN(cpu6502_run)(struct cpu6502 *cpu, uint64_t until);
// Packs/unpacks the flags to the P register layout (bit 5 set, B as given).
uint8_t CPU6502_FN(cpu6502_get_p)(const struct cpu6502 *cpu, int b);
void CPU6502_FN(cpu6502_set_p)(struct cpu6502 *cpu, uint8_t p);

#endif
