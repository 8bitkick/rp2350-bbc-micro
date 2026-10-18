/*
 * ST7701 480x480 RGB panel driver for the Pimoroni Presto.
 *
 * Adapted from Pimoroni's MIT licensed driver (pimoroni/presto drivers/st7701),
 * Copyright (c) 2024 Pimoroni Ltd. Stripped down to RGB565 and given a per-row
 * source mapping so that a short framebuffer can be stretched vertically for free.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "board.h"

#ifndef PRESTO_LCD_TIMING_DMA
#define PRESTO_LCD_TIMING_DMA 1
#endif

#include "st7701_parallel.pio.h"
#include "st7701_timing.pio.h"

#define LCD_SPI         spi1
#define LCD_CS          28
#define LCD_CLK         26
#define LCD_DAT         27
#define LCD_BACKLIGHT   45
#define LCD_D0          1
#define LCD_HSYNC       19
#define LCD_VSYNC       20
#define LCD_DE          21
#define LCD_DOT_CLK     22

#define BACKLIGHT_PWM_TOP 6200

// The vertical front porch sets the refresh rate: PRESTO_LCD_V_FRONT_PORCH lines of 530 dot
// clocks at 15.625 MHz (clk_sys 250 MHz / 16). 5 lines gave 59.2 Hz; 96 give 50.05 Hz, the
// BBC's frame rate (312 lines of 64 us: 50.08 Hz), so an emulator locked to the frame
// interrupt (board_display_frame_time_us) shows each of its frames once, whole.
#ifndef PRESTO_LCD_V_FRONT_PORCH
#define PRESTO_LCD_V_FRONT_PORCH 96
#endif
#define TIMING_V_PULSE   8
#define TIMING_V_BACK    (5 + TIMING_V_PULSE)
#define TIMING_V_DISPLAY (BOARD_DISPLAY_HEIGHT + TIMING_V_BACK)
#define TIMING_V_FRONT   (PRESTO_LCD_V_FRONT_PORCH + TIMING_V_DISPLAY)
#define TIMING_H_FRONT   4
#define TIMING_H_PULSE   16
#define TIMING_H_BACK    30
#define TIMING_H_DISPLAY BOARD_DISPLAY_WIDTH
#ifndef TIMING_H_DATA_LEAD
#define TIMING_H_DATA_LEAD 1    // dots the data SM's irq leads the panel's first pixel
#endif

// The pixel programs have PIO1's 32 instructions to themselves; the timing program (which also
// clocks the panel) runs in PIO2 and raises PIO1's IRQ flags with "prev" index mode
static PIO lcd_pio = pio1;
static PIO timing_pio = pio2;
#define IRQ_PREV 0x08u   // irq instruction index mode 01: the next lower numbered PIO block
static uint parallel_sm, timing_sm;
static bool rgbe_ok;    // false if the pixel program couldn't be loaded (see !lcd)
static uint lcd_dma, lcd_dma2;

// The DMA walks the row table itself, one address per line, so there is no interrupt per line at
// all, only one per frame to rewind it. The table ends with a NULL, and a zero written to a DMA
// trigger register doesn't start the channel.
static const uint16_t *const *volatile row_table;
static volatile uint32_t frame_count;
static volatile uint32_t frame_time_us;     // when the current frame's display area began
static uint32_t display_offset_us;          // from the frame interrupt (the front porch's start) to it
static uint32_t line_ns;                    // a line's time

// the timing SM's instruction word for one quarter (front porch, sync, back porch, display)
// of one line; see st7701_timing.pio
static uint32_t __not_in_flash_func(timing_word)(uint row, uint phase) {
    uint32_t instr;
    switch (phase) {
        case 0:
            instr = 0x4000B042u;  // HSYNC high, NOP
            instr |= (TIMING_H_FRONT - 3) << 16;
            break;
        case 1:
            instr = 0x0000B042u;  // HSYNC low, NOP
            instr |= (TIMING_H_PULSE - 3) << 16;
            break;
        case 2:
            instr = 0x40000000u;  // HSYNC high
            if (row >= TIMING_V_BACK && row < TIMING_V_DISPLAY) instr |= 0xD004u | IRQ_PREV;  // irq 4 -> data SM
            else instr |= 0xB042u;
            // the data SM's first pixel reaches the pins a dot after its irq: raise it a dot early,
            // so pixel 0 is the panel's first (the panel, in HV mode, starts 0x2E dots after HSYNC)
            instr |= (TIMING_H_BACK - 3 - TIMING_H_DATA_LEAD) << 16;
            break;
        default:
            instr = 0x40000000u;  // HSYNC high
            if (row == TIMING_V_DISPLAY) instr |= 0xD001u | IRQ_PREV;  // irq 1 -> new frame
            else if (row >= TIMING_V_BACK - 1 && row < TIMING_V_DISPLAY) instr |= 0xD000u | IRQ_PREV;  // irq 0 -> new line
            else instr |= 0xB042u;
            instr |= (TIMING_H_DISPLAY - 3 + TIMING_H_DATA_LEAD) << 16;
            break;
    }
    if (row >= TIMING_V_PULSE) instr |= 0x80000000u;  // VSYNC high outside the pulse
    return instr;
}

#if PRESTO_LCD_TIMING_DMA
// The timing words repeat every frame, so rather than an ISR refilling the timing SM, a DMA
// channel streams this table into it and a second channel restarts it at the end of each frame
static uint32_t timing_table[TIMING_V_FRONT * 4];
static const uint32_t *timing_table_addr = timing_table;
static uint timing_dma, timing_dma2;
#else
static uint16_t timing_row, timing_phase;

static void __not_in_flash_func(timing_isr)(void) {
    while (!pio_sm_is_tx_fifo_full(timing_pio, timing_sm)) {
        pio_sm_put(timing_pio, timing_sm, timing_word(timing_row, timing_phase));
        if (++timing_phase == 4) {
            timing_phase = 0;
            if (++timing_row >= TIMING_V_FRONT) timing_row = 0;
        }
    }
}
#endif

// The pixel program's colour registers (see st7701_parallel.pio): red, green and blue as the pins
// take them (RGB565, bit reversed onto GP1-16); their complements are cyan, magenta and yellow.
// Loaded at each frame start, after the restart that clears them.
static uint32_t pins_for(uint16_t rgb565) {
    uint32_t r;
    __asm ("rbit %0, %1" : "=r" (r) : "r" ((uint32_t) rgb565 << 16));  // bit reversed onto the pins
    return r;
}

// An instruction written to the state machine runs on its next clock, and it runs at a quarter
// or so of the system clock: write the next one sooner and it replaces this one unrun
static void __not_in_flash_func(sm_exec)(uint instr) {
    pio_sm_exec(lcd_pio, parallel_sm, instr);
    busy_wait_at_least_cycles(32);
}

static void __not_in_flash_func(load_colours)(void) {
    static const uint8_t to[3] = {pio_x, pio_y, pio_isr};
    static const uint16_t rgb[3] = {0xf800, 0x07e0, 0x001f};
    for (int i = 0; i < 3; i++) {
        pio_sm_put(lcd_pio, parallel_sm, pins_for(rgb[i]));
        sm_exec(pio_encode_pull(false, true));
        sm_exec(pio_encode_mov((enum pio_src_dest) to[i], pio_osr));
        // empty the OSR: with autopull on, a pull does nothing while it's full, so without this
        // every register got the first colour
        sm_exec(pio_encode_out(pio_null, 32));
    }
}

static void __not_in_flash_func(frame_start_isr)(void) {
    if (lcd_pio->irq & 0x2) {
        // start of frame
        hw_clear_bits(&lcd_pio->irq, 0x2);
        frame_count++;
        frame_time_us = time_us_32() + display_offset_us;
        dma_channel_abort(lcd_dma);
        dma_channel_wait_for_finish_blocking(lcd_dma);
        pio_sm_set_enabled(lcd_pio, parallel_sm, false);
        pio_sm_clear_fifos(lcd_pio, parallel_sm);
        pio_sm_restart(lcd_pio, parallel_sm);
        load_colours();
        sm_exec(pio_encode_jmp(st7701_rgbe_offset_row_start));
        pio_sm_set_enabled(lcd_pio, parallel_sm, true);
        dma_hw->ch[lcd_dma2].read_addr = (uintptr_t) &row_table[1];
        dma_channel_set_read_addr(lcd_dma, row_table[0], true);
        __sev();
    }
}

static void lcd_command(uint8_t cmd, size_t len, const char *data) {
    uint16_t buf[20];
    gpio_put(LCD_CS, 0);
    uint16_t c = cmd;  // 9th bit (D/CX) = 0 for command
    spi_write16_blocking(LCD_SPI, &c, 1);
    if (data) {
        for (size_t i = 0; i < len; i++) buf[i] = 0x100 | (uint8_t) data[i];
        spi_write16_blocking(LCD_SPI, buf, len);
    }
    gpio_put(LCD_CS, 1);
}

#define CMD(c, s) lcd_command(c, sizeof(s) - 1, s)

static void lcd_panel_init(void) {
    CMD(0x01, "");                 // SWRESET
    sleep_ms(150);
    CMD(0xFF, "\x77\x01\x00\x00\x10");  // BK0
    CMD(0xC0, "\x3b\x00");         // 480 lines
    CMD(0xC1, "\x0d\x02");         // porch
    CMD(0xC2, "\x31\x01");
    CMD(0xCD, "\x08");
    CMD(0xB0, "\x00\x11\x18\x0e\x11\x06\x07\x08\x07\x22\x04\x12\x0f\xaa\x31\x18");
    CMD(0xB1, "\x00\x11\x19\x0e\x12\x07\x08\x08\x08\x22\x04\x11\x11\xa9\x32\x18");
    CMD(0xC3, "\x80\x2e\x0e");     // HV mode
    CMD(0xFF, "\x77\x01\x00\x00\x11");  // BK1
    CMD(0xB0, "\x60");
    CMD(0xB1, "\x32");
    CMD(0xB2, "\x07");
    CMD(0xB3, "\x80");
    CMD(0xB5, "\x49");
    CMD(0xB7, "\x85");
    CMD(0xB8, "\x21");
    CMD(0xC1, "\x78");
    CMD(0xC2, "\x78");
    // panel specific, undocumented (from Pimoroni)
    CMD(0xE0, "\x00\x1b\x02");
    CMD(0xE1, "\x08\xa0\x00\x00\x07\xa0\x00\x00\x00\x44\x44");
    CMD(0xE2, "\x11\x11\x44\x44\xed\xa0\x00\x00\xec\xa0\x00\x00");
    CMD(0xE3, "\x00\x00\x11\x11");
    CMD(0xE4, "\x44\x44");
    CMD(0xE5, "\x0a\xe9\xd8\xa0\x0c\xeb\xd8\xa0\x0e\xed\xd8\xa0\x10\xef\xd8\xa0");
    CMD(0xE6, "\x00\x00\x11\x11");
    CMD(0xE7, "\x44\x44");
    CMD(0xE8, "\x09\xe8\xd8\xa0\x0b\xea\xd8\xa0\x0d\xec\xd8\xa0\x0f\xee\xd8\xa0");
    CMD(0xEB, "\x02\x00\xe4\xe4\x88\x00\x40");
    CMD(0xEC, "\x3c\x00");
    CMD(0xED, "\xab\x89\x76\x54\x02\xff\xff\xff\xff\xff\xff\x20\x45\x67\x98\xba");
    lcd_command(0x36, 1, "\x00");
    CMD(0xFF, "\x77\x01\x00\x00\x13");  // BK3
    CMD(0xE5, "\xe4");
    CMD(0xFF, "\x77\x01\x00\x00\x00");  // banks off
    CMD(0x3A, "\x66");             // COLMOD 18bpp
    lcd_command(0x36, 1, "\x00");  // MADCTL, rotation 0
    CMD(0xFF, "\x77\x01\x00\x00\x10");
    lcd_command(0xC7, 1, "\x00");  // SDIR
    CMD(0xFF, "\x77\x01\x00\x00\x00");
    CMD(0x21, "");                 // INVON
    sleep_ms(1);
    CMD(0x11, "");                 // SLPOUT
    sleep_ms(120);
    CMD(0x29, "");                 // DISPON
    sleep_ms(50);
}

void board_display_backlight(uint8_t brightness) {
    uint16_t value;
    if (brightness == 0) value = 0;
    else if (brightness == 255) value = BACKLIGHT_PWM_TOP;
    else value = 181 + (brightness * brightness) / 85;
    pwm_set_gpio_level(LCD_BACKLIGHT, value);
}

void board_display_init(const uint16_t *const *rows) {
    row_table = rows;

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, BACKLIGHT_PWM_TOP);
    pwm_init(pwm_gpio_to_slice_num(LCD_BACKLIGHT), &cfg, true);
    gpio_set_function(LCD_BACKLIGHT, GPIO_FUNC_PWM);
    board_display_backlight(0);

    parallel_sm = pio_claim_unused_sm(lcd_pio, true);
    timing_sm = pio_claim_unused_sm(timing_pio, true);
    uint timing_offset = pio_add_program(timing_pio, &st7701_timing_program);
    // the pixel program has PIO1 to itself, at offset 0 (it jumps to absolute addresses). Never
    // panic here: this runs on core 1 while core 0 waits for it, so the board would just hang
    rgbe_ok = pio_can_add_program_at_offset(lcd_pio, &st7701_rgbe_program, 0);
    if (rgbe_ok) pio_add_program_at_offset(lcd_pio, &st7701_rgbe_program, 0);

    spi_init(LCD_SPI, 8000000);
    gpio_init(LCD_CS);
    gpio_set_dir(LCD_CS, GPIO_OUT);
    gpio_put(LCD_CS, 1);
    gpio_set_function(LCD_DAT, GPIO_FUNC_SPI);
    gpio_set_function(LCD_CLK, GPIO_FUNC_SPI);
    spi_set_format(LCD_SPI, 9, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    pio_gpio_init(timing_pio, LCD_HSYNC);
    pio_gpio_init(timing_pio, LCD_VSYNC);
    pio_gpio_init(lcd_pio, LCD_DE);
    pio_gpio_init(timing_pio, LCD_DOT_CLK);
    for (uint i = 0; i < 16; i++) pio_gpio_init(lcd_pio, LCD_D0 + i);
    // panel is 18 bit; tie the two low bits off
    for (uint i = 16; i < 18; i++) {
        gpio_init(LCD_D0 + i);
        gpio_set_dir(LCD_D0 + i, GPIO_OUT);
        gpio_put(LCD_D0 + i, false);
    }
    pio_sm_set_consecutive_pindirs(lcd_pio, parallel_sm, LCD_D0, 16, true);
    pio_sm_set_consecutive_pindirs(lcd_pio, parallel_sm, LCD_DE, 1, true);
    pio_sm_set_consecutive_pindirs(timing_pio, timing_sm, LCD_HSYNC, 2, true);
    pio_sm_set_consecutive_pindirs(timing_pio, timing_sm, LCD_DOT_CLK, 1, true);

    pio_sm_config c = st7701_rgbe_program_get_default_config(0);
    sm_config_set_out_pins(&c, LCD_D0, 16);
    sm_config_set_sideset_pins(&c, LCD_DE);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_in_shift(&c, false, false, 32);

    const uint32_t max_pio_clk = 34 * MHZ;
    uint32_t clk_div = (clock_get_hz(clk_sys) + max_pio_clk - 1) / max_pio_clk;
    if (clk_div & 1) clk_div++;
    sm_config_set_clkdiv_int_frac8(&c, clk_div >> 1, 0);
    pio_sm_init(lcd_pio, parallel_sm, st7701_rgbe_offset_row_start, &c);
    load_colours();
    pio_sm_set_enabled(lcd_pio, parallel_sm, true);

    c = st7701_timing_program_get_default_config(timing_offset);
    sm_config_set_out_pins(&c, LCD_HSYNC, 2);
    sm_config_set_sideset_pins(&c, LCD_DOT_CLK);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_clkdiv_int_frac8(&c, clk_div, 0);
    pio_sm_init(timing_pio, timing_sm, timing_offset, &c);
    pio_sm_set_enabled(timing_pio, timing_sm, true);
    // for board_display_frame_time_us / board_display_scan_row: the timing program takes two of its
    // cycles per dot; the frame interrupt comes at the start of the front porch
    line_ns = (uint32_t) (((uint64_t) (TIMING_H_FRONT + TIMING_H_PULSE + TIMING_H_BACK + TIMING_H_DISPLAY)
                           * clk_div * 2 * 1000000000ull) / clock_get_hz(clk_sys));
    display_offset_us = ((PRESTO_LCD_V_FRONT_PORCH + TIMING_V_BACK) * line_ns) / 1000;

    lcd_dma = dma_claim_unused_channel(true);
    lcd_dma2 = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(lcd_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_high_priority(&dc, true);   // the panel must never wait for other DMA
    channel_config_set_dreq(&dc, pio_get_dreq(lcd_pio, parallel_sm, true));
    channel_config_set_chain_to(&dc, lcd_dma2);
    dma_channel_configure(lcd_dma, &dc, &lcd_pio->txf[parallel_sm], NULL, BOARD_DISPLAY_ROW_WORDS, false);

    // reloads lcd_dma's read address (and retriggers it) from the row table
    dc = dma_channel_get_default_config(lcd_dma2);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_high_priority(&dc, true);   // the panel must never wait for other DMA
    channel_config_set_read_increment(&dc, false);
    // steps through the row table: each completed line loads (and starts) the next address
    channel_config_set_read_increment(&dc, true);
    dma_channel_configure(lcd_dma2, &dc, &dma_hw->ch[lcd_dma].al3_read_addr_trig, &row_table[1], 1, false);

    lcd_panel_init();

#if PRESTO_LCD_TIMING_DMA
    for (uint row = 0; row < TIMING_V_FRONT; row++)
        for (uint phase = 0; phase < 4; phase++) timing_table[row * 4 + phase] = timing_word(row, phase);
    timing_dma = dma_claim_unused_channel(true);
    timing_dma2 = dma_claim_unused_channel(true);
    dc = dma_channel_get_default_config(timing_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_high_priority(&dc, true);   // the panel must never wait for other DMA
    channel_config_set_dreq(&dc, pio_get_dreq(timing_pio, timing_sm, true));
    channel_config_set_chain_to(&dc, timing_dma2);
    dma_channel_configure(timing_dma, &dc, &timing_pio->txf[timing_sm], timing_table, count_of(timing_table), false);
    // reloads the table address into timing_dma, which retriggers it
    dc = dma_channel_get_default_config(timing_dma2);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_high_priority(&dc, true);   // the panel must never wait for other DMA
    channel_config_set_read_increment(&dc, false);
    dma_channel_configure(timing_dma2, &dc, &dma_hw->ch[timing_dma].al3_read_addr_trig, &timing_table_addr, 1, false);
    dma_channel_start(timing_dma);
#else
    hw_set_bits(&timing_pio->inte1, 0x010 << timing_sm);  // TX not full
    irq_set_exclusive_handler(pio_get_irq_num(timing_pio, 1), timing_isr);
    irq_set_priority(pio_get_irq_num(timing_pio, 1), 0x40);
    irq_set_enabled(pio_get_irq_num(timing_pio, 1), true);
#endif

    hw_set_bits(&lcd_pio->inte0, 0x200);  // PIO irq 1: start of frame
    irq_set_exclusive_handler(pio_get_irq_num(lcd_pio, 0), frame_start_isr);
    irq_set_priority(pio_get_irq_num(lcd_pio, 0), 0x40);
    irq_set_enabled(pio_get_irq_num(lcd_pio, 0), true);

    sleep_ms(50);
    board_display_backlight(255);
}

void board_display_set_rows(const uint16_t *const *rows) {
    row_table = rows;
}

// a pixel code -> RGB565
static uint16_t rgb_for(uint code) {
    return (uint16_t) ((code & 4 ? 0xf800 : 0) | (code & 2 ? 0x07e0 : 0) | (code & 1 ? 0x001f : 0));
}

const uint16_t *board_display_row_rgb565(int row, uint16_t *buf) {
    // expand a row to what the panel shows, as byte swapped RGB565
    const uint32_t *src = (const uint32_t *) row_table[row];
    for (int x = 0; x < BOARD_DISPLAY_WIDTH; x++)
        buf[x] = __builtin_bswap16(rgb_for((src[x >> 3] >> ((x & 7) * 4)) & 7));
    return buf;
}

const uint16_t *__not_in_flash_func(board_display_row)(int row) {
    return row_table[row];
}

uint32_t board_display_frames(void) {
    return frame_count;
}

uint32_t board_display_frame_time_us(void) {
    return frame_time_us;
}

int32_t board_display_scan_row(uint32_t now_us) {
    int32_t dt_us = (int32_t) (now_us - frame_time_us);
    if (dt_us < 0) return -(int32_t) (((uint32_t) -dt_us * 1000u) / line_ns) - 1;
    return (int32_t) (((uint32_t) dt_us * 1000u) / line_ns);
}

uint32_t board_display_frame_lines(void) {
    return TIMING_V_FRONT;
}

uint32_t board_display_frame_period_us(void) {
    // dot clocks a frame over the dot clock (clk_sys / clk_div / 2: the timing program takes
    // two of its cycles per dot)
    const uint32_t max_pio_clk = 34 * MHZ;
    uint32_t clk_div = (clock_get_hz(clk_sys) + max_pio_clk - 1) / max_pio_clk;
    if (clk_div & 1) clk_div++;
    uint64_t dots = (uint64_t) TIMING_V_FRONT * (TIMING_H_FRONT + TIMING_H_PULSE + TIMING_H_BACK + TIMING_H_DISPLAY);
    return (uint32_t) ((dots * clk_div * 2 * 1000000ull) / clock_get_hz(clk_sys));
}

void board_display_debug(void) {
    printf("LCD rgbe_ok=%d sm_pc=%u txlevel=%u stalled=%d dma_ctrl=%08x count=%08x read=%08x busy=%d frames=%u\n",
           rgbe_ok, pio_sm_get_pc(lcd_pio, parallel_sm),
           pio_sm_get_tx_fifo_level(lcd_pio, parallel_sm),
           !!(lcd_pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + parallel_sm))),
           (uint) dma_hw->ch[lcd_dma].ctrl_trig, (uint) dma_hw->ch[lcd_dma].transfer_count,
           (uint) dma_hw->ch[lcd_dma].read_addr, dma_channel_is_busy(lcd_dma), (uint) frame_count);
    hw_set_bits(&lcd_pio->fdebug, 1u << (PIO_FDEBUG_TXSTALL_LSB + parallel_sm));
}
