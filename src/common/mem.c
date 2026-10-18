// The flash, by region (mem.h).
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/structs/qmi.h"

#include "mem.h"
#include "board_config.h"

typedef struct {
    uint32_t offset, size;
} region_t;

static const region_t s_layout[MEM_FLASH_REGIONS] = BOARD_FLASH_LAYOUT;

bool mem_flash_region(int region, uint32_t *offset, uint32_t *size) {
    if (region < 0 || region >= MEM_FLASH_REGIONS || !s_layout[region].size) return false;
    *offset = s_layout[region].offset;
    *size = s_layout[region].size;
    return true;
}

const uint8_t *mem_flash_ptr(uint32_t offset) {
    return (const uint8_t *) (XIP_BASE + offset);
}

static struct {
    uint32_t offset, len;
    const uint8_t *data;
} s_w;

static void __no_inline_not_in_flash_func(write_sector)(void *arg) {
    (void) arg;
    // the SDK goes back to XIP through boot2, which sets the flash's own timing, too fast at
    // our clock: put back the slower timing the firmware runs with before touching the flash
    uint32_t timing = qmi_hw->m[0].timing;
    flash_range_erase(s_w.offset, FLASH_SECTOR_SIZE);
    if (s_w.data && s_w.len) flash_range_program(s_w.offset, s_w.data, s_w.len);
    qmi_hw->m[0].timing = timing;
}

bool mem_flash_write_sector(uint32_t offset, const void *data, uint32_t len) {
    static uint8_t page_buf[FLASH_SECTOR_SIZE];
    if (offset % FLASH_SECTOR_SIZE || len > FLASH_SECTOR_SIZE) return false;
    // whole pages, from SRAM (data may be in flash or PSRAM, both behind the XIP window the
    // write takes away)
    uint32_t n = (len + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    if (data && len) {
        memset(page_buf, 0xff, n);
        memcpy(page_buf, data, len);
    }
    s_w.offset = offset;
    s_w.len = (data && len) ? n : 0;
    s_w.data = page_buf;
    // (core 1 is parked, the panel freezes for a moment, while the flash is busy)
    return flash_safe_execute(write_sector, NULL, 1000) == PICO_OK;
}
