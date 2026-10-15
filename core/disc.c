// esp32-beeb: disc images as pulses. See disc.h. Restated from beebjit's
// disc.c, disc_ssd.c and ibm_disc_format.c (GPL, see decisions.md).

#include <string.h>

#include "disc.h"

#ifndef DISC_API
#define DISC_API
#endif

DISC_API uint16_t disc_crc_init(int is_mfm)
{
    return is_mfm ? 0xcdb4 : 0xffff;   // MFM starts after the 3 x A1 syncs
}

DISC_API uint16_t disc_crc_add(uint16_t crc, uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        int bit = byte & 0x80;
        int test = (crc & 0x8000) ^ (bit << 8);
        crc <<= 1;
        if (test) {
            crc ^= 0x1021;
        }
        byte <<= 1;
    }
    return crc;
}

DISC_API uint32_t disc_fm_to_pulses(uint8_t clocks, uint8_t data)
{
    uint32_t ret = 0;
    for (int i = 0; i < 8; i++) {
        ret <<= 4;
        if (clocks & 0x80) {
            ret |= 0x04;
        }
        if (data & 0x80) {
            ret |= 0x01;
        }
        clocks <<= 1;
        data <<= 1;
    }
    return ret;
}

DISC_API void disc_pulses_to_fm(uint8_t *p_clocks, uint8_t *p_data, int *p_iffy, uint32_t pulses)
{
    uint8_t clocks = 0, data = 0;
    int iffy = 0;
    // 2 us pulses down to 4 us FM cells, clocks and data apart.
    for (int i = 0; i < 8; i++) {
        clocks <<= 1;
        data <<= 1;
        if (pulses & 0x80000000u) {
            clocks |= 1;
        }
        if (pulses & 0x20000000u) {
            data |= 1;
        }
        if (pulses & 0x50000000u) {
            iffy = 1;
        }
        pulses <<= 4;
    }
    *p_clocks = clocks;
    *p_data = data;
    *p_iffy = iffy;
}

DISC_API uint16_t disc_mfm_to_pulses(int *p_last_bit, uint8_t byte)
{
    uint16_t pulses = 0;
    int last = *p_last_bit;
    for (int i = 0; i < 8; i++) {
        int bit = (byte & 0x80) != 0;
        pulses <<= 2;
        byte <<= 1;
        if (bit) {
            pulses |= 0x01;
        } else if (!last) {
            pulses |= 0x02;
        }
        last = bit;
    }
    *p_last_bit = last;
    return pulses;
}

DISC_API uint8_t disc_pulses_to_mfm(uint16_t pulses)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte <<= 1;
        if ((pulses & 0xc000) == 0x4000) {
            byte |= 1;
        }
        pulses <<= 2;
    }
    return byte;
}

void disc_init(struct disc *d, uint32_t *pulses, int sides, int writeable)
{
    memset(d, 0, sizeof *d);
    d->pulses = pulses;
    d->sides = sides;
    d->writeable = writeable;
    memset(pulses, 0, (size_t)sides * DISC_TRACKS * DISC_BYTES_PER_TRACK * sizeof(uint32_t));
    for (int s = 0; s < 2; s++) {
        for (int t = 0; t < DISC_TRACKS; t++) {
            d->length[s][t] = DISC_BYTES_PER_TRACK;
        }
    }
}

// ---- track building (FM) ----

static void build_track(struct disc *d, int side, uint32_t track)
{
    d->build = disc_track(d, side, track);
    d->build_index = 0;
    d->length[side][track] = DISC_BYTES_PER_TRACK;
}

static void put_fm(struct disc *d, uint8_t data, uint8_t clocks)
{
    if (d->build_index < DISC_BYTES_PER_TRACK) {
        d->build[d->build_index++] = disc_fm_to_pulses(clocks, data);
    }
    d->crc = disc_crc_add(d->crc, data);
}

static void put_fm_repeat(struct disc *d, uint8_t data, uint32_t n)
{
    while (n--) {
        put_fm(d, data, 0xff);
    }
}

static void put_crc(struct disc *d)
{
    uint16_t crc = d->crc;
    put_fm(d, (uint8_t)(crc >> 8), 0xff);
    put_fm(d, (uint8_t)crc, 0xff);
}

void disc_load_ssd(struct disc *d, const uint8_t *image, uint32_t size)
{
    // The standard DFS single density layout: 10 sectors of 256 bytes a
    // track, 80 tracks, the gaps of a freshly formatted disc.
    enum { SECTOR = 256, SECTORS = 10, TRACKS = 80 };
    enum { GAP1_FFS = 16, SYNC_00S = 6, GAP2_FFS = 11, GAP3_FFS = 21 };
    uint32_t pos = 0;
    for (uint32_t track = 0; track < TRACKS; track++) {
        for (int side = 0; side < d->sides; side++) {
            build_track(d, side, track);
            put_fm_repeat(d, 0xff, GAP1_FFS);   // GAP 5 after the index
            put_fm_repeat(d, 0x00, SYNC_00S);
            for (uint32_t sector = 0; sector < SECTORS; sector++) {
                d->crc = disc_crc_init(0);
                put_fm(d, DISC_ID_MARK, DISC_MARK_CLOCKS);
                put_fm(d, (uint8_t)track, 0xff);
                put_fm(d, 0, 0xff);
                put_fm(d, (uint8_t)sector, 0xff);
                put_fm(d, 1, 0xff);   // 256 byte sectors
                put_crc(d);
                put_fm_repeat(d, 0xff, GAP2_FFS);
                put_fm_repeat(d, 0x00, SYNC_00S);
                d->crc = disc_crc_init(0);
                put_fm(d, DISC_DATA_MARK, DISC_MARK_CLOCKS);
                for (uint32_t i = 0; i < SECTOR; i++) {
                    uint8_t b = (pos + i < size) ? image[pos + i] : 0;
                    put_fm(d, b, 0xff);
                }
                pos += SECTOR;
                put_crc(d);
                if (sector != SECTORS - 1) {
                    put_fm_repeat(d, 0xff, GAP3_FFS);
                    put_fm_repeat(d, 0x00, SYNC_00S);
                }
            }
            put_fm_repeat(d, 0xff, DISC_BYTES_PER_TRACK - d->build_index);   // GAP 4
        }
    }
}
