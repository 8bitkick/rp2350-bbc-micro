// stack_guard.c: core 0's stack limit (the Master), and what it caught at the last boot
#ifndef STACK_GUARD_H
#define STACK_GUARD_H

#include <stdint.h>

// first thing in main: core 0's stack may use all of SCRATCH_Y (4 KB) and no more (MSPLIM); reports, once, a
// fault of the last boot
void stack_guard_init(void);
// the app (its id) that was running when core 0's stack overflowed at the last boot; 0: none
uint32_t stack_guard_app_overflow(void);
// (the fault handler's, from a hard fault handler that has a stack again: hang_trap.c's)
void stack_guard_fault(uint32_t *frame);

#endif
