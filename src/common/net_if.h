#ifndef NET_IF_H
#define NET_IF_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A board's network, if it has one (BOARD_HAS_NET; the Presto's: WiFi, its HTTP server for
// /api/<name> and the web page, src/boards/presto/presto_net.c). The common code calls these on
// core 0; a board without a network builds without them.

void net_init(void);        // once the machine is set up: start when it's running
void net_poll(void);        // whenever core 0 has a moment: move packets
void net_frame(void);       // between frames: act on requests
// /api/status's network part: ',"wifi":"...","fetch":{...}' (a JSON fragment, leading comma)
void net_status_json(char *out, size_t size);
// "!net": the "NET ..." line
void net_print_status(void);
// save a network to join (false: couldn't); then net_restart joins it
bool net_wifi_save(const char *ssid, const char *pass);
// restart to join the network saved: now, or once the HTTP reply in hand has gone
void net_restart(bool after_reply);

#ifdef __cplusplus
}
#endif

#endif
