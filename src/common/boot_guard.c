#if BOOT_GUARD
/*
 * Boot guard (dev aid, BOOT_GUARD): finds out where a build that never gets going dies.
 *
 * Each boot marks a record in RAM that survives a reset (it's never zeroed) as "starting", and arms
 * the watchdog. Reaching the running emulator (boot_guard_ok) disarms both. If instead the build
 * hangs, the watchdog resets it; if it faults, the fault handler saves where and resets it. Either
 * way the next boot finds the record still "starting", and instead of running the emulator it
 * brings up just USB and reports, over and over: the last step reached (boot_guard_step) and any
 * fault's PC, LR and fault status. "!bootsel" then drops it into the USB bootloader for the next
 * flash, so a broken build never needs the buttons; with both machines in the flash, "!machine
 * <name>" boots the other one instead (system.h).
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"
#include "boot_guard.h"
#include "system.h"

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
        // wait a second for "!bootsel", or "!machine <name>" (the launcher boots that one next)
        absolute_time_t until = make_timeout_time_ms(1000);
        static char line[24];
        static int len = -1;
        while (!time_reached(until)) {
            int c = getchar_timeout_us(10000);
            if (c == PICO_ERROR_TIMEOUT) continue;
            if (c == '!') {
                len = 0;
            } else if (len >= 0 && (c == '\n' || c == '\r')) {
                line[len] = 0;
                len = -1;
                if (!strcmp(line, "bootsel")) reset_usb_boot(0, 0);
                if (!strncmp(line, "machine ", 8) && system_choose(line + 8)) {
                    printf("MACHINE %s: restarting\n", line + 8);
                    sleep_ms(100);
                    watchdog_reboot(0, 0, 10);
                }
            } else if (len >= 0 && len < (int) sizeof line - 1) {
                line[len++] = (char) c;
            }
        }
    }
}

static volatile struct { uint32_t state, id, last, times; } app_record __attribute__((section(".uninitialized_data")));
#define APP_IN 0xa99c0de5u

void boot_guard_begin(void) {
    // the app died, not the boot: the app guard takes it (app.c runs the machine without it)
    if (app_record.state == APP_IN && record.state != 0) record.state = 0;
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

void boot_guard_step(uint32_t step) {
    record.step = step;
}

void boot_guard_note(uint32_t a, uint32_t b) {
    record.cfsr = a;
    record.bfar = b;
}

void boot_guard_ok(void) {
#if BOOT_GUARD_KEEP
    return;     // diagnosis: stay armed, so a fault or hang at any time is reported at the next boot
#endif
    if (record.state == BOOT_STARTING) {
        watchdog_disable();
        record.state = 0;
    }
}

// the stacked registers of whatever faulted: r0-r3, r12, lr, pc, xpsr
void __used boot_guard_fault(uint32_t *frame) {
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
void __attribute__((noreturn)) boot_guard_panic(const char *fmt, ...) {
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
        "b boot_guard_fault\n");
}
#endif

// ---- The app guard (boot_guard.h): in every build
#include "boot_guard.h"

#if !BOOT_GUARD
static volatile struct { uint32_t state, id, last, times; } app_record __attribute__((section(".uninitialized_data")));
#define APP_IN 0xa99c0de5u
#endif

void boot_guard_app_enter(uint32_t id) {
    app_record.id = id;
    app_record.state = APP_IN;
}

void boot_guard_app_leave(void) {
    app_record.state = 0;
}

uint32_t boot_guard_app_running(void) {
    return app_record.state == APP_IN ? app_record.id : 0;
}

uint32_t boot_guard_app_died(void) {
    // Whatever reset the board (the watchdog; a fault, then the watchdog; RESET), it came while
    // the app was running. (Not the watchdog's reason register: through the launcher it doesn't
    // say.) A RESET the moment the app was called would look the same, so it takes two boots in
    // a row ended in the same app.
    if (app_record.state != APP_IN) app_record.times = 0;
    else if (app_record.last == (app_record.id ^ APP_IN)) app_record.times++;
    else {
        app_record.last = app_record.id ^ APP_IN;
        app_record.times = 1;
    }
    app_record.state = 0;
    return app_record.times >= 2 ? app_record.id : 0;
}
