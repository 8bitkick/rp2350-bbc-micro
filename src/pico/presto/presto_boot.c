#if PRESTO_BOOT_GUARD
/*
 * Boot guard (dev aid, PRESTO_BOOT_GUARD): finds out where a build that never gets going dies.
 *
 * Each boot marks a record in RAM that survives a reset (it's never zeroed) as "starting", and arms
 * the watchdog. Reaching the running emulator (presto_boot_ok) disarms both. If instead the build
 * hangs, the watchdog resets it; if it faults, the fault handler saves where and resets it. Either
 * way the next boot finds the record still "starting", and instead of running the emulator it
 * brings up just USB and reports, over and over: the last step reached (presto_boot_step) and any
 * fault's PC, LR and fault status. "!bootsel" then drops it into the USB bootloader for the next
 * flash, so a broken build never needs the buttons.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"
#include "presto_boot.h"

#define BOOT_STARTING 0xb00757a7u
#define BOOT_FAULTED  0xb007fa17u
#define BOOT_PANICKED 0xb007a71cu

typedef struct {
    uint32_t state, step, pc, lr, cfsr, bfar, core, xpsr;
} boot_record_t;

static volatile boot_record_t record __attribute__((section(".uninitialized_data")));

static void report_forever(boot_record_t r) {
    // and where the profiler last saw each core (presto_prof.c's breadcrumbs)
    uint32_t last[4];
    for (int i = 0; i < 4; i++) last[i] = watchdog_hw->scratch[i];
    stdio_init_all();
    while (true) {
        printf("LASTPC core0 pc=%08x lr=%08x core1 pc=%08x lr=%08x\n", (uint) last[0], (uint) last[1], (uint) last[2], (uint) last[3]);
        printf("BOOTFAIL %s step=%u pc=%08x lr=%08x cfsr=%08x bfar=%08x core=%u xpsr=%08x\n",
               r.state == BOOT_FAULTED ? "fault" : r.state == BOOT_PANICKED ? "panic" : "hang",
               (uint) r.step, (uint) r.pc, (uint) r.lr,
               (uint) r.cfsr, (uint) r.bfar, (uint) r.core, (uint) r.xpsr);
        // wait a second for "!bootsel"
        absolute_time_t until = make_timeout_time_ms(1000);
        static int matched;
        while (!time_reached(until)) {
            int c = getchar_timeout_us(10000);
            if (c == PICO_ERROR_TIMEOUT) continue;
            static const char want[] = "!bootsel";
            matched = c == want[matched] ? matched + 1 : c == '!';
            if (!want[matched]) reset_usb_boot(0, 0);
        }
    }
}

void presto_boot_begin(void) {
    if (record.state == BOOT_STARTING || record.state == BOOT_FAULTED || record.state == BOOT_PANICKED) {
        boot_record_t r = record;
        record.state = 0;           // next boot runs normally
        report_forever(r);
    }
    record.state = BOOT_STARTING;
    record.step = 0;
    record.pc = record.lr = record.cfsr = record.bfar = record.core = record.xpsr = 0;
    watchdog_enable(8000, true);
}

void presto_boot_step(uint32_t step) {
    record.step = step;
}

void presto_boot_note(uint32_t a, uint32_t b) {
    record.cfsr = a;
    record.bfar = b;
}

void presto_boot_ok(void) {
#if PRESTO_BOOT_GUARD_KEEP
    return;     // diagnosis: stay armed, so a fault or hang at any time is reported at the next boot
#endif
    if (record.state == BOOT_STARTING) {
        watchdog_disable();
        record.state = 0;
    }
}

// the stacked registers of whatever faulted: r0-r3, r12, lr, pc, xpsr
void __used presto_boot_fault(uint32_t *frame) {
    if (record.state == BOOT_STARTING) {
        record.pc = frame[6];
        record.lr = frame[5];
        record.xpsr = frame[7];
        record.cfsr = scb_hw->cfsr;
        record.bfar = scb_hw->bfar;
        record.core = get_core_num();
        record.state = BOOT_FAULTED;
        watchdog_reboot(0, 0, 1);
    }
    while (true) __breakpoint();
}

// an SDK panic (PICO_PANIC_FUNCTION): "panic" with pc = the message's address (a string in the
// ELF: arm-none-eabi-objdump -s --start-address=<pc> ...) and lr = who panicked
void __attribute__((noreturn)) presto_boot_panic(const char *fmt, ...) {
    if (record.state == BOOT_STARTING) {
        record.pc = (uint32_t) fmt;
        record.lr = (uint32_t) __builtin_return_address(0);
        record.core = get_core_num();
        record.state = BOOT_PANICKED;
        sleep_ms(300);      // lets what was printed before the panic reach the USB console
        watchdog_reboot(0, 0, 1);
    }
    while (true) __breakpoint();
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b presto_boot_fault\n");
}
#endif
