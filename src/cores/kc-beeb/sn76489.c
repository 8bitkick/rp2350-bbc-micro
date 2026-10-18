// esp32-beeb: the SN76489 (see sn76489.h). Register semantics and the tone
// and noise generators follow beebjit's sound.c, including its choices
// where the chip's behaviour is unsettled: power-on registers as if zero
// (full volume, lowest tone) except a noise frequency of 2, the noise
// register updated every second counter expiry with half periods, and the
// counters underflowing from 0 to 0x3ff.
#include "sn76489.h"

#ifndef SN_API
#define SN_API
#endif

SN_API void sn_reset(struct sn76489 *sn)
{
    // beebjit: the full output range shared equally by the four channels,
    // 2 dB per volume step, volume 15 silent, silence at zero voltage.
    static const int16_t quarter[16] = {
        8191, 6507, 5169, 4106, 3262, 2591, 2058, 1635,
        1299, 1032, 820, 651, 517, 411, 326, 0,
    };
    for (int i = 0; i < 16; i++) {
        sn->volume_outputs[i] = quarter[i];
    }
    for (int i = 0; i < 4; i++) {
        sn->volume[i] = 0;
        sn->period[i] = 0;
        sn->counter[i] = 0;
        sn->output[i] = 0;
    }
    sn->noise_frequency = 2;
    sn->period[3] = 0x40;
    sn->noise_type = 0;
    sn->latched_address = 0;
    sn->noise_rng = 0;
}

SN_API void sn_write(struct sn76489 *sn, uint8_t value)
{
    if (value & 0x80) {
        sn->latched_address = value & 0x70;
    }
    uint8_t command = sn->latched_address | (value & 0x80);
    uint8_t channel = (command >> 5) & 3;
    int32_t new_period = -1;
    if (command & 0x10) {
        sn->volume[channel] = value & 0x0f;
    } else if (channel == 3) {
        // The noise channel only ever takes the low bits.
        uint8_t f = value & 3;
        sn->noise_frequency = f;
        new_period = f == 0 ? 0x10 : f == 1 ? 0x20 : f == 2 ? 0x40 : sn->period[2];
        sn->noise_type = (value >> 2) & 1;
        sn->noise_rng = 1 << 14;
    } else if (command & 0x80) {
        new_period = (value & 0x0f) | (sn->period[channel] & 0x3f0);   // low bits
    } else {
        new_period = ((value & 0x3f) << 4) | (sn->period[channel] & 0x0f);   // high bits
    }
    if (new_period != -1) {
        sn->period[channel] = (uint16_t)new_period;
        if (channel == 2 && sn->noise_frequency == 3) {
            sn->period[3] = (uint16_t)new_period;
        }
    }
}

SN_API int32_t sn_render8(struct sn76489 *sn)
{
    int32_t sum = 0;
    for (int ch = 0; ch < 4; ch++) {
        uint16_t counter = sn->counter[ch];
        // No expiry within the eight steps: counter - k stays non-zero for
        // k = 1..8, i.e. counter > 8 (counter 0 underflows to 0x3ff first).
        if (counter > 8 || counter == 0) {
            if (counter == 0) {
                counter = 0x3ff;
                sn->counter[ch] = (uint16_t)(counter - 7);
            } else {
                sn->counter[ch] = (uint16_t)(counter - 8);
            }
            uint8_t output = ch == 3 ? (sn->noise_rng & 1) : sn->output[ch];
            if (output) {
                sum += 8 * sn->volume_outputs[sn->volume[ch]];
            }
            continue;
        }
        // An expiry within the eight: step this channel one at a time.
        uint16_t rng = sn->noise_rng;
        for (int k = 0; k < 8; k++) {
            uint16_t c = (uint16_t)((sn->counter[ch] - 1) & 0x3ff);
            uint8_t output = sn->output[ch];
            if (c == 0) {
                c = sn->period[ch];
                output = !output;
                sn->output[ch] = output;
                if (ch == 3 && output) {
                    if (sn->noise_type == 0) {
                        rng >>= 1;
                        if (rng == 0) {
                            rng = 1 << 14;
                        }
                    } else {
                        int bit = (rng & 1) ^ ((rng >> 1) & 1);
                        rng = (uint16_t)((rng >> 1) | (bit << 14));
                    }
                }
            }
            sn->counter[ch] = c;
            if (ch == 3) {
                output = rng & 1;
            }
            if (output) {
                sum += sn->volume_outputs[sn->volume[ch]];
            }
        }
        sn->noise_rng = rng;
    }
    return sum;
}

SN_API void sn_render(struct sn76489 *sn, int16_t *out, uint32_t n)
{
    uint16_t rng = sn->noise_rng;
    for (uint32_t i = 0; i < n; i++) {
        int32_t sample = 0;
        for (int ch = 0; ch < 4; ch++) {
            uint16_t counter = (uint16_t)((sn->counter[ch] - 1) & 0x3ff);
            uint8_t output = sn->output[ch];
            if (counter == 0) {
                counter = sn->period[ch];
                output = !output;
                sn->output[ch] = output;
                if (ch == 3 && output) {
                    if (sn->noise_type == 0) {
                        rng >>= 1;
                        if (rng == 0) {
                            rng = 1 << 14;
                        }
                    } else {
                        int bit = (rng & 1) ^ ((rng >> 1) & 1);
                        rng = (uint16_t)((rng >> 1) | (bit << 14));
                    }
                }
            }
            sn->counter[ch] = counter;
            if (ch == 3) {
                output = rng & 1;
            }
            if (output) {
                sample += sn->volume_outputs[sn->volume[ch]];
            }
        }
        out[i] = (int16_t)sample;
    }
    sn->noise_rng = rng;
}
