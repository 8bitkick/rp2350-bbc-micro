#ifndef PRESTO_PSRAM_H
#define PRESTO_PSRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// set up the PSRAM; returns its size in bytes (0 if it isn't working)
size_t presto_psram_init(void);
size_t presto_psram_size(void);
uint8_t *presto_psram(void);    // its start, or NULL
// where disc images from the card or the web arrive (PRESTO_DISC_MAX bytes; presto_machine.c)
uint8_t *presto_disc_ram(void);


#ifdef __cplusplus
}
#endif

#endif
