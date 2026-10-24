// The Presto's touch screen, for its own screens (presto_touch.c)
#ifndef PRESTO_TOUCH_H
#define PRESTO_TOUCH_H
#include <stdint.h>
void touch_init(void);
void touch_poll(void);          // once a frame, on core 0
uint32_t touch_held_us(void);   // how long the screen has been touched (0: it isn't)
#endif
