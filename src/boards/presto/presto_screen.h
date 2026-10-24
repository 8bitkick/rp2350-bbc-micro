// The Presto's own screens, shown on the panel in place of the machine's picture (presto_screen.c):
// the loading screen while WiFi joins at power on, and the WiFi setup screen, the address card and
// the "Join this network?" question (presto_net.c). All on core 0.
#ifndef PRESTO_SCREEN_H
#define PRESTO_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// colours: the panel's codes (bit 0 blue, 1 green, 2 red)
enum {
    SCREEN_BLACK, SCREEN_BLUE, SCREEN_GREEN, SCREEN_CYAN, SCREEN_RED, SCREEN_MAGENTA, SCREEN_YELLOW, SCREEN_WHITE
};

// ---- Memory, for the screens and the network while the machine hasn't started: the machine's
// framebuffer, lent (machine_framebuffer), so they cost it nothing; malloc's heap otherwise. A
// block is cleared when it's given back (it may have held a password).
void screen_lend(uint32_t *rows, int n);    // the machine's framebuffer, n rows of the panel's format
bool screen_give_back(void);    // false while any of it is still in use; then the framebuffer goes
                                // back blank, and everything after comes from the heap
void *presto_alloc(size_t n);
void *presto_calloc(size_t n);
void presto_free(void *p);

// ---- The loading screen: the app's picture (app_boot_image), if it has one, with what's going on
// and how far (of 1000) under it, drawn in the lent framebuffer; permille < 0: over
void screen_loading(const char *what, int permille);

// ---- An overlay: boxes, text and QR codes. overlay_open clears it (allocating it the first time:
// it uses no memory while closed); what's added shows from the next overlay_frame_done.
bool overlay_open(int background);
void overlay_close(void);
bool overlay_is_open(void);
void overlay_private(void);     // it shows a password: not in screenshots (!shot)
void overlay_box(int x, int y, int w, int h, int colour);
void overlay_text(int centre_x, int y, int scale, int colour, const char *text);   // 8x8 glyphs, scaled
// a QR code on a white box, as large as fits max_px across (and down): the box's size, or 0
int overlay_qr(int centre_x, int y, int max_px, const char *text);
void overlay_frame_done(void);  // once a frame: draws what's changed; frees a closed overlay
bool screen_private(void);      // what the panel shows is private (presto_lcd.c's screenshots)

// presto_lcd.c: the row table the panel is scanning out from now
const uint16_t *const *presto_lcd_rows(void);

#endif
