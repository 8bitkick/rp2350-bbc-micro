/*
 * The Presto's 8MB PSRAM (an APS6404 on QMI chip select 1, GPIO 47), mapped at 0x11000000.
 * Used to hold disc images loaded from the SD card. The setup follows the usual RP2350 recipe:
 * talk to the chip directly to put it into quad (QPI) mode, then give the QMI its read and write
 * formats, and allow writes through the XIP window.
 */
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "presto_psram.h"

#define PSRAM_CS_PIN 47
#define PSRAM_BASE ((volatile uint8_t *) 0x11000000)
#ifndef PSRAM_MAX_HZ
#define PSRAM_MAX_HZ 133000000
#endif
#define PSRAM_BYTES (8u << 20)

static size_t psram_bytes;

// one command, with the chip selected only while it's sent; quad for when it may be in QPI mode
static void __no_inline_not_in_flash_func(direct_command)(uint8_t cmd, bool quad) {
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | cmd |
                        (quad ? QMI_DIRECT_TX_OE_BITS | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) : 0);
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
}

// runs from RAM with interrupts off: while the QMI is in direct mode, nothing can run from flash
// (not even a library call or a 64 bit divide: the timing is worked out beforehand, by the caller)
static void __no_inline_not_in_flash_func(psram_setup)(uint32_t timing) {
    uint32_t irq = save_and_disable_interrupts();
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    direct_command(0xf5, true);     // leave QPI mode, in case a previous run left it there
    direct_command(0x66, false);    // reset enable
    direct_command(0x99, false);    // reset
    busy_wait_at_least_cycles(50 * 250);   // >= 50us at up to 250MHz (tRST)
    direct_command(0x35, false);    // enter QPI mode

    qmi_hw->m[1].timing = timing;
    // quad read 0xEB with 6 wait cycles (24 bits of dummy at quad width), quad write 0x38
    qmi_hw->m[1].rfmt = QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
                        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
                        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
                        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
                        QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB |
                        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
                        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
                        QMI_M0_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_RFMT_SUFFIX_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xeb << QMI_M0_RCMD_PREFIX_LSB;
    qmi_hw->m[1].wfmt = QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
                        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
                        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
                        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
                        QMI_M0_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M0_WFMT_DUMMY_LEN_LSB |
                        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
                        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB |
                        QMI_M0_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_WFMT_SUFFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38 << QMI_M0_WCMD_PREFIX_LSB;
    qmi_hw->direct_csr = 0;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    restore_interrupts(irq);
}

static uint32_t psram_timing(void) {
    // at most 133MHz, chip select low for at most 8us (refresh), high for at least 18ns
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint divisor = (sys_hz + PSRAM_MAX_HZ - 1) / PSRAM_MAX_HZ;
    if (divisor == 1 && sys_hz > 100000000) divisor = 2;
    uint rxdelay = divisor;
    if (sys_hz / divisor > 100000000) rxdelay++;
    uint32_t period_fs = (uint32_t) (1000000000000000ull / sys_hz);
    uint max_select = (125 * 1000000) / period_fs;      // in units of 64 cycles: 8us
    uint min_deselect = (18 * 1000000 + period_fs - 1) / period_fs - (divisor + 1) / 2;
    return 1 << QMI_M1_TIMING_COOLDOWN_LSB |
           QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
           max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
           min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
           rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
           divisor << QMI_M1_TIMING_CLKDIV_LSB;
}

size_t presto_psram_init(void) {
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    psram_setup(psram_timing());
    // check it's there: patterns at a few places in the 8MB, through the uncached alias
    volatile uint32_t *nocache = (volatile uint32_t *) 0x15000000;
    static const uint32_t at[] = {0, 1, 0x1234, (4u << 20) / 4 - 1, (8u << 20) / 4 - 1};
    for (uint i = 0; i < count_of(at); i++) nocache[at[i]] = 0x5a17c0deu ^ (at[i] * 2654435761u);
    size_t size = PSRAM_BYTES;
    for (uint i = 0; i < count_of(at); i++) if (nocache[at[i]] != (0x5a17c0deu ^ (at[i] * 2654435761u))) size = 0;
    psram_bytes = size;
    return size;
}

size_t presto_psram_size(void) {
    return psram_bytes;
}

uint8_t *presto_psram(void) {
    return psram_bytes ? (uint8_t *) PSRAM_BASE : NULL;
}
