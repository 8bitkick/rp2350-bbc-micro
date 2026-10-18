// esp32-beeb: the SN76489 sound chip, beebjit's model restated (sound.c).
//
// Plain C, no ESP headers. The chip is clocked at 4 MHz and divides by 16,
// so its tone counters step at 250 kHz: one step every 8 system (2 MHz)
// cycles. sn_render() produces one sample per step; the platform averages
// eight into a 31250 Hz output sample (main/sound_esp.c) or writes them
// to a file (host/sound.c).
//
// Writes reach the chip through the system VIA's port A with IC32 bit 0 as
// the active-low write enable. This model takes the bytes as the machine
// sees them land (beebjit's asynchronous mode: a byte is accepted when the
// write enable goes low with it on the bus, or when the bus changes while
// it is low); the 8 us write cycle of the real chip is not modelled yet.
#ifndef SN76489_H
#define SN76489_H

#include <stdint.h>

struct sn76489 {
    uint8_t latched_address;   // bits 4-6 of the last byte with bit 7 set
    uint8_t volume[4];         // 0 loudest, 15 silent
    uint16_t period[4];        // 10 bits; [3] the noise channel's derived period
    uint16_t counter[4];
    uint8_t output[4];
    uint8_t noise_frequency;   // 0-3 (3: from channel 2)
    uint8_t noise_type;        // 0 periodic, 1 white
    uint16_t noise_rng;
    int16_t volume_outputs[16];   // sample value per volume register value
};

void sn_reset(struct sn76489 *sn);
void sn_write(struct sn76489 *sn, uint8_t value);
// n samples at 250 kHz, the four channels summed.
void sn_render(struct sn76489 *sn, int16_t *out, uint32_t n);
// The sum of the next eight 250 kHz samples (one 31250 Hz output sample
// times eight); the same as eight sn_render samples added, but a channel
// whose counter does not expire within the eight costs one step.
int32_t sn_render8(struct sn76489 *sn);

#endif
