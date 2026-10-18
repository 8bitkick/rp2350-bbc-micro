/*
 * Disc images from the Presto's micro SD card: a small SPI driver for the card, under ChaN's
 * FatFs, loading a .ssd / .dsd into the PSRAM for the emulator to run from, and receiving disc images
 * over USB (!put) to write to the card.
 * The card is on SPI0: SCK 34, MOSI 35, MISO 36, CS 39 (as in Pimoroni's examples).
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "diskio.h"
#include "board.h"
#include "drive.h"

#define SD_SPI      spi0
#define SD_SCK      34
#define SD_MOSI     35
#define SD_MISO     36
#define SD_CS       39

static bool card_ready, card_block_addressed;
static FATFS fs;
static bool mounted;

static inline uint8_t xfer(uint8_t out) {
    uint8_t in;
    spi_write_read_blocking(SD_SPI, &out, &in, 1);
    return in;
}

static void select_card(bool on) {
    gpio_put(SD_CS, !on);
    xfer(0xff);     // a clock with CS changed, so the card lets go of MISO
}

// send a command (card selected), returning its R1 response (0xff: no answer)
static uint8_t command(uint8_t cmd, uint32_t arg) {
    for (int i = 0; i < 1000 && xfer(0xff) != 0xff; i++) {}    // wait while busy
    uint8_t frame[6] = {(uint8_t) (0x40 | cmd), (uint8_t) (arg >> 24), (uint8_t) (arg >> 16), (uint8_t) (arg >> 8),
                        (uint8_t) arg, cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01};   // CRC only matters for these
    spi_write_blocking(SD_SPI, frame, sizeof(frame));
    uint8_t r = 0xff;
    for (int i = 0; i < 10 && ((r = xfer(0xff)) & 0x80); i++) {}
    return r;
}

static bool card_init(void) {
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO);
    gpio_init(SD_CS);
    gpio_set_dir(SD_CS, GPIO_OUT);
    gpio_put(SD_CS, 1);
    for (int i = 0; i < 10; i++) xfer(0xff);    // 80 clocks with the card deselected

    uint8_t r = 0xff;
    for (int i = 0; i < 10 && r != 1; i++) {    // CMD0: go idle, into SPI mode
        select_card(true);
        r = command(0, 0);
        select_card(false);
    }
    if (r != 1) return false;

    select_card(true);                          // CMD8: v2 card? (needed for SDHC)
    bool v2 = command(8, 0x1aa) == 1;
    uint8_t r7[4];
    for (int i = 0; i < 4; i++) r7[i] = xfer(0xff);
    select_card(false);
    if (v2 && (r7[3] != 0xaa)) return false;

    absolute_time_t give_up = make_timeout_time_ms(1000);
    do {                                        // ACMD41 until it leaves the idle state
        select_card(true);
        command(55, 0);
        r = command(41, v2 ? 0x40000000 : 0);
        select_card(false);
    } while (r == 1 && !time_reached(give_up));
    if (r) return false;

    card_block_addressed = false;
    if (v2) {                                   // CMD58: is it addressed by block (SDHC/SDXC)?
        select_card(true);
        if (command(58, 0) == 0) {
            uint8_t ocr = xfer(0xff);
            for (int i = 0; i < 3; i++) xfer(0xff);
            card_block_addressed = (ocr & 0x40) != 0;
        }
        select_card(false);
    }
    if (!card_block_addressed) {
        select_card(true);
        command(16, 512);                       // 512 byte blocks
        select_card(false);
    }
    spi_set_baudrate(SD_SPI, 25 * 1000 * 1000);
    return true;
}

static bool read_block(uint32_t lba, uint8_t *buf) {
    select_card(true);
    bool ok = false;
    if (command(17, card_block_addressed ? lba : lba * 512) == 0) {
        absolute_time_t give_up = make_timeout_time_ms(200);
        uint8_t token;
        while ((token = xfer(0xff)) == 0xff && !time_reached(give_up)) {}
        if (token == 0xfe) {
            spi_read_blocking(SD_SPI, 0xff, buf, 512);
            xfer(0xff);     // CRC
            xfer(0xff);
            ok = true;
        }
    }
    select_card(false);
    return ok;
}

// wait until the card has finished programming (it holds MISO low while busy)
static bool wait_ready(uint32_t ms) {
    absolute_time_t give_up = make_timeout_time_ms(ms);
    while (xfer(0xff) != 0xff) if (time_reached(give_up)) return false;
    return true;
}

static bool write_block(uint32_t lba, const uint8_t *buf) {
    select_card(true);
    bool ok = false;
    if (command(24, card_block_addressed ? lba : lba * 512) == 0) {
        xfer(0xff);
        xfer(0xfe);                                 // start token
        spi_write_blocking(SD_SPI, buf, 512);
        xfer(0xff);                                 // CRC (ignored in SPI mode)
        xfer(0xff);
        ok = (xfer(0xff) & 0x1f) == 0x05 && wait_ready(500);   // data accepted, then written
    }
    select_card(false);
    return ok;
}

// FatFs's disk interface (one drive)
DSTATUS disk_status(BYTE pdrv) {
    return pdrv || !card_ready ? STA_NOINIT : 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv) return STA_NOINIT;
    card_ready = card_init();
    return card_ready ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !card_ready) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (!read_block((uint32_t) sector + i, buff + i * 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !card_ready) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (!write_block((uint32_t) sector + i, buff + i * 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv) return RES_PARERR;
    if (cmd == CTRL_SYNC) {
        select_card(true);
        bool ok = wait_ready(500);
        select_card(false);
        return ok ? RES_OK : RES_ERROR;
    }
    return RES_OK;
}

static bool mount(void) {
    // (re)mount each time: the card may have been swapped
    mounted = f_mount(&fs, "", 1) == FR_OK;
    return mounted;
}

static bool is_disc_name(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".ssd") || !strcasecmp(dot, ".dsd"));
}

int board_sd_each_disc(void (*fn)(const char *name, uint32_t size, void *ctx), void *ctx) {
    if (!mount()) return -1;
    DIR dir;
    FILINFO info;
    int n = 0;
    if (f_opendir(&dir, "") == FR_OK) {
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if (info.fattrib & AM_DIR || !is_disc_name(info.fname)) continue;
            fn(info.fname, (uint32_t) info.fsize, ctx);
            n++;
        }
        f_closedir(&dir);
    }
    return n;
}

static void print_disc(const char *name, uint32_t size, void *ctx) {
    printf("DISC %s %u\n", name, (uint) size);
}

void board_sd_list_discs(void) {
    int n = board_sd_each_disc(print_disc, NULL);
    if (n < 0) printf("DISCS no card\n");
    else printf("DISCS %d\n", n);
}

// find a disc by name, ignoring case, with or without its extension
static bool find_disc(const char *want, char *found, size_t found_size) {
    DIR dir;
    FILINFO info;
    bool ok = false;
    if (f_opendir(&dir, "") != FR_OK) return false;
    size_t wl = strlen(want);
    while (!ok && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        if (info.fattrib & AM_DIR || !is_disc_name(info.fname)) continue;
        size_t fl = strlen(info.fname);
        if (!strcasecmp(info.fname, want) || (fl == wl + 4 && !strncasecmp(info.fname, want, wl))) {
            snprintf(found, found_size, "%s", info.fname);
            ok = true;
        }
    }
    f_closedir(&dir);
    return ok;
}

static bool safe_disc_name(const char *name);

bool board_sd_load_disc(const char *want) {
    uint8_t *ram = drive0_staging();
    if (!ram) {
        printf("DISC no PSRAM\n");
        return false;
    }
    if (!mount()) {
        printf("DISC no card\n");
        return false;
    }
    char name[FF_MAX_LFN + 1];
    if (!find_disc(want, name, sizeof(name))) {
        printf("DISC not found: %s\n", want);
        return false;
    }
    FIL f;
    if (f_open(&f, name, FA_READ) != FR_OK) {
        printf("DISC can't open %s\n", name);
        return false;
    }
    uint32_t size = (uint32_t) f_size(&f);
    if (size > DRIVE_IMAGE_MAX) {
        printf("DISC too big: %s\n", name);
        f_close(&f);
        return false;
    }
    UINT got = 0;
    FRESULT fr = f_read(&f, ram, size, &got);
    f_close(&f);
    if (fr != FR_OK || got != size || !drive0_insert(name, size)) {
        printf("DISC read failed: %s\n", name);
        return false;
    }
    printf("DISC loaded %s (%u bytes)\n", name, (uint) size);
    return true;
}

bool board_sd_save(const char *name, const uint8_t *data, uint32_t size) {
    if (!safe_disc_name(name) || !mount()) return false;
    FIL f;
    if (f_open(&f, name, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT written = 0;
    bool ok = f_write(&f, data, size, &written) == FR_OK && written == size;
    if (f_close(&f) != FR_OK) ok = false;
    if (!ok) f_unlink(name);
    return ok;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

// only plain names of disc images, in the card's root
static bool safe_disc_name(const char *name) {
    if (!*name || strlen(name) > 64 || !is_disc_name(name)) return false;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char) *p) && !strchr(" ._-()", *p)) return false;
    }
    return true;
}

void board_sd_put(const char *args) {
    // "<name> <size>", then exactly <size> bytes of the file (the name may contain spaces)
    char name[72];
    const char *sp = strrchr(args, ' ');
    unsigned long size = sp ? strtoul(sp + 1, NULL, 10) : 0;
    size_t nl = sp ? (size_t) (sp - args) : 0;
    if (!sp || nl >= sizeof(name) || !size || size > 800 * 1024) {
        printf("PUT error: usage put <name.ssd> <size>\n");
        return;
    }
    memcpy(name, args, nl);
    name[nl] = 0;
    if (!safe_disc_name(name)) {
        printf("PUT error: bad name\n");
        return;
    }
    if (!mount()) {
        printf("PUT error: no card\n");
        return;
    }
    FIL f;
    if (f_open(&f, name, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("PUT error: can't create %s\n", name);
        return;
    }
    printf("PUT ready\n");
    fflush(stdout);
    static uint8_t buf[512];
    uint32_t crc = 0, done = 0;
    bool ok = true;
    while (ok && done < size) {
        uint n = 0, want = size - done < sizeof(buf) ? size - done : sizeof(buf);
        while (n < want) {
            int c = getchar_timeout_us(2000000);
            if (c == PICO_ERROR_TIMEOUT) break;
            buf[n++] = (uint8_t) c;
        }
        UINT written = 0;
        if (n < want || f_write(&f, buf, n, &written) != FR_OK || written != n) ok = false;
        crc = crc32_update(crc, buf, n);
        done += n;
        watchdog_update();      // (a build with a running watchdog: this can take a while)
    }
    if (f_close(&f) != FR_OK) ok = false;
    if (!ok) {
        f_unlink(name);
        printf("PUT error: failed after %u bytes\n", (uint) done);
        return;
    }
    printf("PUT ok %s %u %08x\n", name, (uint) done, (uint) crc);
}
