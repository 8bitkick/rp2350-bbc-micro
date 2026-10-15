#ifndef PRESTO_MACHINE_H
#define PRESTO_MACHINE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the Presto's front end (the web panel, the SD card, the USB channel) asks of the emulated
// machine: presto_machine.c and main.c. Called on core 0, between the machine's runs. Disc changes
// are in presto_sd.h (presto_disc_insert, presto_disc_name, presto_sd_eject_disc).

// a key by its place in the BBC's keyboard matrix (row << 4 | column)
void key_bbc(int vkey, bool down);
// BREAK
void presto_break(void);
// hold SHIFT for the next second, so that a reset boots the disc (as SHIFT+BREAK)
void presto_boot_shift(void);
// run as fast as possible (every frame still drawn)
bool presto_turbo_get(void);
void presto_turbo_set(bool on);
// the speed and load figures on the panel
bool presto_overlay_get(void);
void presto_overlay_set(bool on);
// each core's load and the emulation speed, in percent
void presto_perf_get(uint *core0, uint *core1, uint *speed);
// the chip's temperature, in hundredths of a degree
int presto_temp_centi(void);
// the keyboard's LEDs: the system VIA's IC32 latch (bit 6 CAPS LOCK, bit 7 SHIFT LOCK, lit when
// low) and the serial ULA's control register (bit 7 the cassette motor)
extern uint8_t IC32;
uint8_t serial_get_reg(void);

#ifdef __cplusplus
}
#endif

#endif
