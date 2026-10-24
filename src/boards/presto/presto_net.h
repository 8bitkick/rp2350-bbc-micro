// What the Presto's own screens ask of its network (presto_net.c), within the board
#ifndef PRESTO_NET_H
#define PRESTO_NET_H
#include <stdbool.h>
#include <stdint.h>
void net_show_address(uint32_t seconds);    // the web panel's address on screen for a while
bool net_screen_up(void);                   // one of its screens is up (taps are its own)
void net_screen_tap(int x, int y);          // touched there, in panel pixels
#endif
