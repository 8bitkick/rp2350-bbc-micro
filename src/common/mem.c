// The flash, by region (mem.h).
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/structs/qmi.h"

#include "mem.h"
#include "board_config.h"
#include "system.h"
#include "machine_if.h"

typedef struct {
    uint32_t offset, size;
} region_t;

static const region_t s_layout[MEM_FLASH_REGIONS] = BOARD_FLASH_LAYOUT;

bool mem_flash_region(int region, uint32_t *offset, uint32_t *size) {
    if (region < 0 || region >= MEM_FLASH_REGIONS || !s_layout[region].size) return false;
    *offset = s_layout[region].offset;
    *size = s_layout[region].size;
    // both machines in one flash (system.h): each its own save disc, the Model B's lower down
    if ((region == MEM_FLASH_SAVE_HEAD || region == MEM_FLASH_SAVE_DATA) && !strcmp(machine_name(), "beeb") &&
        system_launched()) *offset -= BOARD_FLASH_SAVE_SLOT;
    return true;
}

const uint8_t *mem_flash_ptr(uint32_t offset) {
    return (const uint8_t *) (XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + offset);
}

static struct {
    uint32_t offset, len;
    const uint8_t *data;
} s_w;
static uint8_t s_tail[FLASH_PAGE_SIZE];     // a last part page, padded

static void __no_inline_not_in_flash_func(write_sector)(void *arg) {
    (void) arg;
    // the SDK goes back to XIP through boot2, which sets the flash's own timing, too fast at
    // our clock: put back the slower timing the firmware runs with before touching the flash
    uint32_t timing = qmi_hw->m[0].timing;
    flash_range_erase(s_w.offset, FLASH_SECTOR_SIZE);
    uint32_t whole = s_w.len & ~(FLASH_PAGE_SIZE - 1);
    if (whole) flash_range_program(s_w.offset, s_w.data, whole);
    if (s_w.len > whole) {
        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) s_tail[i] = whole + i < s_w.len ? s_w.data[whole + i] : 0xff;
        flash_range_program(s_w.offset + whole, s_tail, FLASH_PAGE_SIZE);
    }
    qmi_hw->m[0].timing = timing;
}

static uint32_t s_writes;

uint32_t mem_flash_writes(void) {
    return s_writes;
}

bool mem_flash_write_sector(uint32_t offset, const void *data, uint32_t len) {
    if (offset % FLASH_SECTOR_SIZE || len > FLASH_SECTOR_SIZE) return false;
    s_writes++;
    s_w.offset = offset;
    s_w.data = data;
    s_w.len = data ? len : 0;
    // (core 1 is parked and core 0's interrupts are off while the flash is busy: the machine
    // pauses for a moment; the panel's rows are fed by DMA alone, so its picture stays)
    if (!multicore_lockout_victim_is_initialized(get_core_num() ^ 1)) {
        // the other core not launched yet (at boot: the app guard turning an app off, the boot
        // guard's "!machine"): it's waiting in the boot ROM, nothing to park, but the SDK refuses
        // (both machines' core 1 sets up its lockout first thing, so this is the only time)
        uint32_t irq = save_and_disable_interrupts();
        write_sector(NULL);
        restore_interrupts(irq);
        return true;
    }
    return flash_safe_execute(write_sector, NULL, 1000) == PICO_OK;
}
