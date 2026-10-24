// The Presto's panel for the core's renderer (render_unit.c).
#ifndef PANEL_H
#define PANEL_H

#include "vstream.h"

// Sets up the panel (presto_lcd.c) on its framebuffer: 256 rows, one per
// BBC scanline, stretched to the panel's 480.
void panel_lcd_init(void);
// The renderer's power-on state.
void panel_init(void);
// a program's loading screen (render_unit.c): its picture (false: it has none), then what's loading
// and how far (of 1000), as often as that changes
#include <stdbool.h>
bool panel_splash_picture(void);
void panel_splash_status(const char *what, int permille);
// the framebuffer black (a program being installed: core 1 idle, the machine paused)
void panel_splash_blank(void);
// the framebuffer: its rows (machine_framebuffer)
#include <stdint.h>
uint32_t *panel_framebuffer(int *rows);
// Core 1: renders what is in the stream now (not what arrives meanwhile).
void panel_consume(struct vstream *vs);
// Core 0, right after a machine reset: the renderer starts again from its
// power-on state at this point in the stream.
void panel_machine_reset(struct vstream *vs);

#endif
