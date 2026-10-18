/*
 * ST7701 480x480 RGB panel driver for the Pimoroni Presto
 */
#ifndef PRESTO_LCD_H
#define PRESTO_LCD_H

#include "pico.h"

#define PRESTO_LCD_WIDTH 480
#define PRESTO_LCD_HEIGHT 480
// A row is 480 4 bit pixels, then an end of row code: PRESTO_LCD_ROW_WORDS words. Each nibble,
// from the low end of each word: bit 0 blue, 1 green, 2 red (the BBC's 8 colours), 3 E: end of
// row (see st7701_parallel.pio). The last word of a row is PRESTO_LCD_ROW_END.
#define PRESTO_LCD_ROW_WORDS (PRESTO_LCD_WIDTH / 8 + 1)
#define PRESTO_LCD_ROW_END 0x8u

#ifdef __cplusplus
extern "C" {
#endif

// rows[i] is the source for panel row i (PRESTO_LCD_ROW_WORDS words); the DMA reads the table
// directly, so it must stay valid, live in RAM, and have a NULL at rows[PRESTO_LCD_HEIGHT].
// Entries may be changed at any time (pointer writes are atomic).
void presto_lcd_init(const uint16_t *const *rows);
void presto_lcd_set_rows(const uint16_t *const *rows);

void presto_lcd_debug(void);

// the pixels of a panel row, as byte swapped RGB565 (for screenshots); buf holds PRESTO_LCD_WIDTH
const uint16_t *presto_lcd_row(int row, uint16_t *buf);
// the 4 bit pixels the panel shows for a row, as above (PRESTO_LCD_ROW_WORDS words)
const uint16_t *presto_lcd_row_raw(int row);
void presto_lcd_set_backlight(uint8_t brightness);
uint32_t presto_lcd_frame_count(void);
uint32_t presto_lcd_frame_time_us(void);     // time_us_32() when the current frame's display area began (the first row's scan)
uint32_t presto_lcd_frame_period_us(void);   // the refresh period
uint32_t presto_lcd_frame_lines(void);       // lines a frame, blanking included
int32_t presto_lcd_scan_row(uint32_t now_us);  // the panel row being scanned at that time, in lines from this frame's display area (< 0: before it; >= height: after it)

#ifdef __cplusplus
}
#endif

#endif
