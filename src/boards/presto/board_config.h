// The Pimoroni Presto, as the common code and the machines see it (docs/porting-a-board.md):
// everything here is what another board would give differently.
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/flash.h"

// ---- The flash layout (16 MB), the board's one table of it (src/common/mem.h's regions).
// The firmware from the bottom; at the top, down from the end: the WiFi network (a sector), a
// spare sector, the save disc (80 tracks x 10 sectors x 256 bytes), its header (a sector).
#define BOARD_FLASH_SAVE_DATA_SIZE  (80 * 10 * 256)
#define BOARD_FLASH_WIFI_OFFSET     (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define BOARD_FLASH_SPARE_OFFSET    (BOARD_FLASH_WIFI_OFFSET - FLASH_SECTOR_SIZE)
// (-DBOARD_FLASH_SAVE_SHIFT=<bytes>, a multiple of the sector: the save disc that much lower, to
// test saving without touching the real one)
#ifndef BOARD_FLASH_SAVE_SHIFT
#define BOARD_FLASH_SAVE_SHIFT 0
#endif
#define BOARD_FLASH_SAVE_DATA_OFFSET (BOARD_FLASH_SPARE_OFFSET - BOARD_FLASH_SAVE_DATA_SIZE - BOARD_FLASH_SAVE_SHIFT)
#define BOARD_FLASH_SAVE_HEAD_OFFSET (BOARD_FLASH_SAVE_DATA_OFFSET - FLASH_SECTOR_SIZE)
#define BOARD_FLASH_LAYOUT { \
    [MEM_FLASH_SAVE_HEAD] = {BOARD_FLASH_SAVE_HEAD_OFFSET, FLASH_SECTOR_SIZE}, \
    [MEM_FLASH_SAVE_DATA] = {BOARD_FLASH_SAVE_DATA_OFFSET, BOARD_FLASH_SAVE_DATA_SIZE}, \
    [MEM_FLASH_SPARE] = {BOARD_FLASH_SPARE_OFFSET, FLASH_SECTOR_SIZE}, \
    [MEM_FLASH_WIFI] = {BOARD_FLASH_WIFI_OFFSET, FLASH_SECTOR_SIZE}, \
}

#endif
