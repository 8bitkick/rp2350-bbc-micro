#ifndef PRESTO_SD_H
#define PRESTO_SD_H

#include <stdbool.h>
#include <stdint.h>

// print the .ssd / .dsd files on the SD card ("DISC <name> <bytes>" lines, then "DISCS <count>")
void presto_sd_list_discs(void);
// load a disc from the card into drive 0 (drive.h), and reset to boot it
bool presto_sd_load_disc(const char *name);
// call fn for each .ssd / .dsd on the card; the count, or -1 with no card
int presto_sd_each_disc(void (*fn)(const char *name, uint32_t size, void *ctx), void *ctx);
// write an image to the card's root (a plain .ssd/.dsd name)
bool presto_sd_save(const char *name, const uint8_t *data, uint32_t size);
// "!put <name> <size>": receive a disc image over USB and write it to the card
void presto_sd_put(const char *args);

#endif
