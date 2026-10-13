#ifndef PRESTO_AMBIENT_H
#define PRESTO_AMBIENT_H

#include <stdbool.h>

// The seven LEDs on the Presto's back, lit with what's on screen: each the average of its part
// of the picture (a 3x3 grid over it, less the middle and the bottom middle, where there's no LED).

// on core 0 at boot, before anything else loads a PIO0 program: PIO0 to GPIO 16-47 (the LEDs
// are on 33), the WS2812 program, the LEDs off
void presto_ambient_init(void);
// each emulated frame, on core 0: wakes core 1 for an update every other frame
void presto_ambient_frame(void);
// on core 1, when it's idle: true if an update is due (then call presto_ambient_update)
bool presto_ambient_due(void);
void presto_ambient_update(void);
// "!leds [off|screen|test|bright <percent>]": the state ("LEDS ..."), or change it
void presto_ambient_command(const char *args);

#endif
