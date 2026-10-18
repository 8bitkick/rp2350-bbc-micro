// Between main.c (the machine's pacing, on core 0) and presto_machine.c (the
// web panel, the SD card and the USB commands' view of the machine).
#ifndef PRESTO_MAIN_H
#define PRESTO_MAIN_H

#include <stdbool.h>
#include <stdint.h>

struct machine;

// presto_machine.c: put the built-in disc in drive 0 and start the network.
// pulses: two sides of track data (2 x DISC_TRACKS x DISC_BYTES_PER_TRACK
// words); disc_ram: PRESTO_DISC_MAX bytes, where images arrive.
void presto_machine_init(struct machine *m, uint32_t *pulses, uint8_t *disc_ram,
                         const uint8_t *builtin, uint32_t builtin_size);
// Between frames: what web requests asked for (keys, discs, resets).
void presto_machine_frame(void);
// Whenever core 0 has a moment: move network packets.
void presto_machine_poll(void);

// main.c: the machine was reset (its cycle count is 0 again): re-pace.
void main_machine_was_reset(void);

#endif
