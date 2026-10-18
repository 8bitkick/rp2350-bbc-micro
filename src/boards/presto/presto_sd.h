#ifndef PRESTO_SD_H
#define PRESTO_SD_H

#include <stdbool.h>
#include <stdint.h>

// the biggest disc image: an 80 track double sided .dsd
#define PRESTO_DISC_MAX (80 * 10 * 256 * 2)

// print the .ssd / .dsd files on the SD card ("DISC <name> <bytes>" lines, then "DISCS <count>")
void presto_sd_list_discs(void);
// load a disc from the card into the PSRAM as drive 0, and reset to boot it
bool presto_sd_load_disc(const char *name);
// put the disc loaded from the card back in drive 0 (after a reset); false if there isn't one
bool presto_sd_reinsert_disc(void);
// forget it, and reset to the built-in disc
void presto_sd_eject_disc(void);
// call fn for each .ssd / .dsd on the card; the count, or -1 with no card
int presto_sd_each_disc(void (*fn)(const char *name, uint32_t size, void *ctx), void *ctx);
// the image already at the start of the PSRAM goes in drive 0 as <name> (.ssd/.dsd says which
// kind), and a reset boots it; false if it's no size for a disc
bool presto_disc_insert(const char *name, uint32_t size);
// the name of the disc in drive 0, or NULL for the built-in one
const char *presto_disc_name(void);
// write an image to the card's root (a plain .ssd/.dsd name)
bool presto_sd_save(const char *name, const uint8_t *data, uint32_t size);
// "!put <name> <size>": receive a disc image over USB and write it to the card
void presto_sd_put(const char *args);

#endif
