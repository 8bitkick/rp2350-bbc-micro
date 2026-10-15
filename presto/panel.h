// The Presto's panel for the core's renderer (render_unit.c).
#ifndef PANEL_H
#define PANEL_H

#include "vstream.h"

// Sets up the panel (presto_lcd.c) on its framebuffer: 256 rows, one per
// BBC scanline, stretched to the panel's 480.
void panel_lcd_init(void);
// The renderer's power-on state.
void panel_init(void);
// Core 1: renders what is in the stream now (not what arrives meanwhile).
void panel_consume(struct vstream *vs);
// Core 0: writes "SHOT 480 480" and the panel's rows as RGB565 to stdout (as
// they are, so a row core 1 is drawing may be half new).
void panel_shot(void);

#endif
