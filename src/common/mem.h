#ifndef MEM_H
#define MEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The flash the firmware keeps things in, by region: where each is comes from the board's one
// flash layout table (BOARD_FLASH_LAYOUT, board_config.h), never from an address in the code.

enum mem_flash_region {
    MEM_FLASH_SAVE_HEAD,    // the save disc's header (a sector): which built-in disc it's a copy of
    MEM_FLASH_SAVE_DATA,    // the save disc: a whole single sided disc, 80 tracks of 10 sectors
    MEM_FLASH_SPARE,        // (a sector, kept free)
    MEM_FLASH_WIFI,         // the WiFi network to join (a sector; a board with a network)
    MEM_FLASH_REGIONS
};

// a region's offset in the flash and its size (false: the board has none)
bool mem_flash_region(int region, uint32_t *offset, uint32_t *size);
// what's at an offset, read through the XIP window
const uint8_t *mem_flash_ptr(uint32_t offset);
// erase the sector at offset (a sector boundary) and program len bytes of data into it (NULL:
// leave it erased). Core 1 is parked meanwhile (its loop must have called
// flash_safe_execute_core_init), and the slower flash timing the firmware runs with is kept.
bool mem_flash_write_sector(uint32_t offset, const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
