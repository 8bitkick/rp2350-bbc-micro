// hang_trap.c: a diagnosis build's record of where the firmware froze (HANG_TRAP)
#pragma once
#include <stdint.h>
extern volatile uint32_t ht_beat0, ht_beat1;
void ht_init_core0(void);
void ht_init_core1(void);
void ht_report(void);
