#ifndef PRESTO_NET_H
#define PRESTO_NET_H

#include <stdbool.h>
#include "pico/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// The web control panel: joins the WiFi network saved with "!wifi", and serves a page (and a small
// JSON API) at http://beeb.local/ for the emulator's controls, a keyboard and loading discs.
//
// Nothing here runs in an interrupt or on core 1: the network is polled by core 0 while it waits
// for core 1 anyway (presto_net_poll), and anything that touches the emulated machine waits for
// the gap between frames (presto_net_frame), so the emulator's timing is never disturbed.

void presto_net_init(void);
void presto_net_poll(void);         // move packets; cheap when there's nothing to do
void presto_net_frame(void);        // between frames: carry out what requests asked for
// "!wifi <ssid> <password>" (a quoted ssid may contain spaces): save to flash, then reboot
void presto_net_set_wifi(const char *args);
void presto_net_print_status(void);

#ifdef __cplusplus
}
#endif

#endif
