// Hang trap (-DHANG_TRAP=ON, the Master: off by default): where the firmware stops when it freezes.
//
// Each core's SysTick samples, 1000 times a second, where that core was interrupted (the stacked PC and LR) into a
// record in RAM that survives a reset. Core 0's tick also feeds the watchdog, but only while both cores show
// progress (ht_beat0 from the machine's frames, ht_beat1 from the renderer's loop). When either stalls for 2 s the
// record is marked "hang" and the feeding stops: the watchdog resets the board 8 s later, the samples still coming
// from wherever the cores are stuck. A hard fault saves its stacked registers and resets at once. The next boot
// prints the record every 5 s (ht_report) until the next freeze.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/scb.h"
#include "hang_trap.h"

#define HT_MAGIC 0x4a4e4748u     // "HGNJ"
enum { HT_RUN = 1, HT_HANG, HT_FAULT };
#define HT_N 16

typedef struct {
    uint32_t magic, state, boots, at_ms, stalled, cfsr, bfar, fcore, fpc, flr, fxpsr, beat0, beat1;
    uint32_t n[2], pc[2][HT_N], lr[2][HT_N];
} ht_rec_t;
static volatile ht_rec_t rec __attribute__((section(".uninitialized_data")));
static ht_rec_t last;            // the previous boot's, copied at start
static int have_last;
volatile uint32_t ht_beat0, ht_beat1;

static void systick_start(void) {
    systick_hw->csr = 0;
    systick_hw->rvr = 250000 - 1;           // (1 ms at 250 MHz)
    systick_hw->cvr = 0;
    systick_hw->csr = 0x7;                   // processor clock, interrupt, enable
}

void ht_init_core0(void) {
    if (rec.magic == HT_MAGIC) { memcpy(&last, (const void *) &rec, sizeof last); have_last = 1; }
    uint32_t boots = rec.magic == HT_MAGIC ? rec.boots + 1 : 1;
    memset((void *) &rec, 0, sizeof rec);
    rec.magic = HT_MAGIC, rec.state = HT_RUN, rec.boots = boots;
    watchdog_enable(8000, true);
    systick_start();
}
void ht_init_core1(void) { systick_start(); }

void __used __not_in_flash_func(ht_tick)(uint32_t *frame) {
    uint c = get_core_num();
    uint32_t i = rec.n[c]++ % HT_N;
    rec.pc[c][i] = frame[6], rec.lr[c][i] = frame[5];
    if (c) return;
    // each core on its own: a stall of either for 2 s stops the feeding (core 1 rendering on while core 0 was stuck
    // kept a "both" test feeding it, and held off the firmware's own watchdog_reboot after an app install)
    static uint32_t b0, b1, same0, same1;
    if (ht_beat0 != b0) b0 = ht_beat0, same0 = 0; else same0++;
    if (ht_beat1 != b1) b1 = ht_beat1, same1 = 0; else same1++;
    if (rec.state == HT_RUN) {
        // (and not when the firmware has asked for a reboot of its own, watchdog_reboot's few ms: feeding it held that
        // off until the stall check let go, 2 s later)
        if (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS && watchdog_get_time_remaining_ms() < 500) ;
        else if (same0 < 2000 && same1 < 2000) {
            if (!(watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS)) watchdog_enable(8000, true);     // (the app guard's probation turns it off)
            watchdog_update();
        } else {
            rec.state = HT_HANG, rec.at_ms = to_ms_since_boot(get_absolute_time()), rec.beat0 = ht_beat0, rec.beat1 = ht_beat1;
            rec.stalled = (same0 >= 2000) | (same1 >= 2000) << 1;
            // (and it must go off: the app guard turns the watchdog off when an app stops in its first minute)
            if (!(watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS)) watchdog_enable(8000, true);
        }
    }
}

// (the handlers in RAM: a flash write stops the flash, and a tick on a core with interrupts on would fault)
void __attribute__((naked, section(".time_critical.ht_systick"))) isr_systick(void) {
    __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b ht_tick\n");
}

void __used __not_in_flash_func(ht_fault)(uint32_t *frame) {
    rec.cfsr = scb_hw->cfsr;
    if (!(rec.cfsr & (1u << 20))) rec.fpc = frame[6], rec.flr = frame[5], rec.fxpsr = frame[7];     // (no frame after a stack overflow)
    rec.cfsr = scb_hw->cfsr, rec.bfar = scb_hw->bfar, rec.fcore = get_core_num();
    rec.at_ms = to_ms_since_boot(get_absolute_time());
    rec.state = HT_FAULT;
    watchdog_reboot(0, 0, 1);
    for (;;) ;
}
void __attribute__((naked, section(".time_critical.ht_hardfault"))) isr_hardfault(void) {
    // (core 0's stack limit, stack_guard.c: a stack overflow leaves no room, so a stack of its own first)
    __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n movs r1, #0\n msr msplim, r1\n"
                   "ldr r1, =sg_stack + 256\n mov sp, r1\n b ht_fault\n");
}

void ht_report(void) {
    if (!have_last) { printf("HANGTRAP none (first boot since power-on)\n"); return; }
    const ht_rec_t *r = &last;
    static const char *const S[] = {"?", "running (reset, power or watchdog with core 0's interrupts off)", "hang", "fault"};
    printf("HANGTRAP prev boot %u: %s at %u ms, stalled %s, beats %u/%u\n", (unsigned) r->boots, S[r->state < 4 ? r->state : 0],
           (unsigned) r->at_ms, r->stalled == 1 ? "core0" : r->stalled == 2 ? "core1" : r->stalled == 3 ? "both" : "-",
           (unsigned) r->beat0, (unsigned) r->beat1);
    if (r->state == HT_FAULT)
        printf("HANGTRAP fault core %u pc=%08x lr=%08x xpsr=%08x cfsr=%08x bfar=%08x\n", (unsigned) r->fcore,
               (unsigned) r->fpc, (unsigned) r->flr, (unsigned) r->fxpsr, (unsigned) r->cfsr, (unsigned) r->bfar);
    for (int c = 0; c < 2; c++) {
        printf("HANGTRAP core%d n=%u", c, (unsigned) r->n[c]);
        for (int k = 0; k < HT_N; k++) {
            uint32_t i = (r->n[c] + k) % HT_N;
            printf(" %08x/%08x", (unsigned) r->pc[c][i], (unsigned) r->lr[c][i]);
        }
        printf("\n");
    }
}
