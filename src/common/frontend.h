#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The front end every machine shares, on core 0: the USB channel (its "!" commands, through the
// command layer, api.h, and its keys), the board's network if it has one, and the keys queued
// for the machine. A machine calls frontend_poll whenever core 0 has a moment and frontend_frame
// between frames (where requests may press keys, change the disc or reset the machine).

void frontend_init(void);       // once the machine is set up
void frontend_poll(void);
void frontend_frame(void);

// the chip's temperature, in hundredths of a degree (the RP2350's sensor)
int frontend_temp_centi(void);

#ifdef __cplusplus
}
#endif

#endif
