// Core 0's stack guard (the Master). Core 0's stack is SCRATCH_Y, 4 KB under __StackTop; under it is SCRATCH_X, core
// 1's stack. Nothing stopped core 0 running on into it: a plugin's deep call chain did (elite-agent, 3.4 KB under its
// frame) and the board froze with nothing to say why. So the Cortex-M33's own stack limit, MSPLIM, at the bottom of
// SCRATCH_Y: a push below it faults (UsageFault STKOF, taken as a HardFault), the stack untouched below the limit.
//
// The handler has no stack (the fault leaves SP at the limit), so it first lets go of the limit and moves SP to a
// small one of its own, then records the fault in RAM that survives a reset and resets the board. The next boot
// prints it ("FAULT ..."), and a plugin that was running when core 0's stack overflowed is turned off at once
// (app.c: stack_guard_app_overflow), where a plain hang takes the app guard two boots.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#include "boot_guard.h"
#include "stack_guard.h"

#define SG_MAGIC 0x5347f417u
#define CFSR_STKOF (1u << 20)       // (UFSR bit 4: a stack limit violation)

typedef struct { uint32_t magic, cfsr, hfsr, pc, lr, core, app; } sg_rec_t;
static volatile sg_rec_t rec __attribute__((section(".uninitialized_data")));
static uint32_t s_app_overflow;
uint32_t sg_stack[64];              // (the handler's own stack, 256 bytes)

void stack_guard_init(void)
{
    extern uint32_t __scratch_y_start__;
    if (rec.magic == SG_MAGIC) {
        int ovf = (rec.cfsr & CFSR_STKOF) != 0;
        printf("FAULT at the last boot: core %u %s, cfsr %08x hfsr %08x pc %08x lr %08x%s\n", (unsigned) rec.core,
               ovf ? "stack overflow (core 0's stack, 4 KB, used up)" : "hard fault", (unsigned) rec.cfsr, (unsigned) rec.hfsr,
               (unsigned) rec.pc, (unsigned) rec.lr, rec.app ? ", in the app" : "");
        if (ovf && rec.core == 0) s_app_overflow = rec.app;
    }
    rec.magic = 0;
    __asm volatile ("msr msplim, %0" : : "r" (&__scratch_y_start__));
}

uint32_t stack_guard_app_overflow(void) { return s_app_overflow; }

// what faulted: its stacked registers (r0-r3, r12, lr, pc, xpsr), but not when the stack itself overflowed: then the
// fault's own stacking stopped at the limit and the frame isn't there
void __used __not_in_flash_func(stack_guard_fault)(uint32_t *frame)
{
    uint32_t cfsr = scb_hw->cfsr;
    rec.cfsr = cfsr, rec.hfsr = scb_hw->hfsr, rec.core = get_core_num();
    rec.pc = rec.lr = 0;
    if (!(cfsr & CFSR_STKOF)) rec.pc = frame[6], rec.lr = frame[5];
    rec.app = boot_guard_app_running();
    rec.magic = SG_MAGIC;
    watchdog_reboot(0, 0, 1);
    for (;;) ;
}

#if !HANG_TRAP
// (in RAM, as hang_trap.c's: a flash write stops the flash)
void __attribute__((naked, section(".time_critical.sg_hardfault"))) isr_hardfault(void)
{
    __asm volatile(
        "tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n"
        "movs r1, #0\n msr msplim, r1\n"            // (a stack again: no limit, and its own)
        "ldr r1, =sg_stack + 256\n mov sp, r1\n"
        "b stack_guard_fault\n");
}
#endif
