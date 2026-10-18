#ifndef PRESTO_AMBIENT_H
#define PRESTO_AMBIENT_H

#include <stdbool.h>
#include <stdint.h>

// The seven LEDs on the Presto's back, lit with what's on screen: each the average of its part
// of the picture (a 3x3 grid over it, less the middle and the bottom middle, where there's no LED).

// on core 0 at boot, before anything else loads a PIO0 program: PIO0 to GPIO 16-47 (the LEDs
// are on 33), the WS2812 program, the LEDs off
void presto_ambient_init(void);
// each emulated frame, on core 0: the keyboard's lock LEDs (bit 0 CAPS LOCK, 1 SHIFT LOCK lit);
// wakes core 1 for an update every other frame
void presto_ambient_frame(uint8_t locks);
// on core 1, when it's idle: true if an update is due (then call presto_ambient_update)
bool presto_ambient_due(void);
void presto_ambient_update(void);
// whether the LEDs started (presto_ambient_init)
bool presto_ambient_ready(void);
// the settings: the screen's colours on, the brightness (percent), CAPS LOCK / SHIFT LOCK shown
void presto_ambient_get(bool *on, unsigned *bright, bool *locks);
// change them; -1 leaves one as it is
void presto_ambient_set(int on, int bright, int locks);
// "!leds [off|screen|test|bright <percent>|locks on|locks off]": the state ("LEDS ..."), or
// change it
void presto_ambient_command(const char *args);

#endif
