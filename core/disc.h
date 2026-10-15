// esp32-beeb: a disc as the drive sees it.
//
// beebjit's representation (disc.c there): every track is 3125 positions
// of 32 two-microsecond pulse slots, 64 us a position, 200 ms a
// revolution. An FM byte is one position (8 clock and 8 data pulses, each
// in a 4 us cell); the 1770 recovers clocks, data and markers from the
// pulses, so sector headers, gaps, CRCs and missing-clock marks are all
// just pulses here, and what the controller sees is what a real one sees.
// .ssd/.dsd images are expanded into this at load, the standard DFS
// layout with the standard gaps (disc_ssd.c there). Writes go into the
// pulses and stay for the session; nothing is written back to the image.
//
// Memory: 12.5 KB a track, 1 MB a side; the board keeps it in PSRAM, which
// the drive reads one word per 64 us.

#ifndef DISC_H
#define DISC_H

#include <stdint.h>

#define DISC_BYTES_PER_TRACK 3125
#define DISC_TRACKS 84            // physical head positions
#define DISC_TICKS_PER_REV 400000 // 2 MHz cycles: 300 rpm

#define DISC_MARK_CLOCKS 0xc7
#define DISC_ID_MARK 0xfe
#define DISC_DATA_MARK 0xfb
#define DISC_DELETED_DATA_MARK 0xf8
#define DISC_MFM_A1_SYNC 0x4489
#define DISC_MFM_C2_SYNC 0x5224

struct disc {
    uint32_t *pulses;      // [sides][DISC_TRACKS][DISC_BYTES_PER_TRACK]
    uint32_t length[2][DISC_TRACKS];
    int sides;             // 1 or 2
    int writeable;
    // build state
    uint32_t *build;
    uint32_t build_index;
    uint16_t crc;
};

// pulses must hold sides * DISC_TRACKS * DISC_BYTES_PER_TRACK words.
void disc_init(struct disc *d, uint32_t *pulses, int sides, int writeable);
// Expands an .ssd (sides 1) or .dsd (sides 2) image of `size` bytes.
void disc_load_ssd(struct disc *d, const uint8_t *image, uint32_t size);

static inline uint32_t *disc_track(struct disc *d, int side, uint32_t track)
{
    return d->pulses + ((uint32_t)side * DISC_TRACKS + track) * DISC_BYTES_PER_TRACK;
}

// CRC-CCITT as the 1770 and the disc format use it.
uint16_t disc_crc_init(int is_mfm);
uint16_t disc_crc_add(uint16_t crc, uint8_t byte);
// Pulse conversions (ibm_disc_format.c in beebjit).
uint32_t disc_fm_to_pulses(uint8_t clocks, uint8_t data);
void disc_pulses_to_fm(uint8_t *clocks, uint8_t *data, int *iffy, uint32_t pulses);
uint16_t disc_mfm_to_pulses(int *last_bit, uint8_t byte);
uint8_t disc_pulses_to_mfm(uint16_t pulses);

#endif
