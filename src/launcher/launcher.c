// The launcher (docs/dual-boot.md): what the boot ROM runs first when both machines are in the
// flash. It reads which machine was chosen (the settings sector, system.h) and has the boot ROM
// chain into that machine's slot, mapped to 0x10000000 as if it were the only firmware there.
// No choice, or a choice of nothing there is: the Master. A slot with nothing bootable in it:
// the other one. Neither: the USB bootloader, ready for a .uf2.
//
// The partition table (partitions.json) is in this image's block loop, so the boot ROM has it
// loaded already; it says where each slot is. Nothing else is set up: no clocks beyond the
// SDK's, no USB, no stdio, so the machine starts from much the state it would without us.
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"

#include "board_config.h"
#include "system.h"

// the boot ROM's work area for searching and checking an image (3264 bytes, it says)
static uint8_t s_work[4096] __aligned(4);

// boot the image in a slot: only returns if there's no bootable image there
static void chain(int slot) {
    uint32_t info[3];
    int n = rom_get_partition_table_info(info, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION |
                                                  ((uint32_t) slot << 24));
    if (n != 3) return;
    uint32_t first = (info[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (info[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    rom_chain_image(s_work, sizeof s_work, XIP_BASE + first * FLASH_SECTOR_SIZE, (last + 1 - first) * FLASH_SECTOR_SIZE);
}

int main(void) {
    rom_load_partition_table(s_work, sizeof s_work, false);
    const system_settings_t *s = (const system_settings_t *) (XIP_NOCACHE_NOALLOC_BASE + BOARD_FLASH_SYSTEM_OFFSET);
    int slot = system_slot(system_settings_machine(s));
    chain(slot);
    chain(slot == SYSTEM_SLOT_MASTER ? SYSTEM_SLOT_BEEB : SYSTEM_SLOT_MASTER);
    reset_usb_boot(0, 0);
}
