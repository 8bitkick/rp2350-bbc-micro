/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PRESTO_AUDIO_H
#define PRESTO_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* Call these and all os_sound_* functions from core 0 only.
 * os_sound_init() must also run on core 0, using its default alarm pool.
 * This initial driver supports 31250 Hz mono only. */
struct presto_audio_stats {
    uint32_t queued_samples;
    uint32_t underruns;
    uint32_t dropped_samples;
    int32_t rate_adjust_ppm;
    uint32_t timer_ticks;
    uint32_t submitted_samples;
    int16_t pcm_min;
    int16_t pcm_max;
    bool initialized;
    bool muted;
    bool playing;
    uint32_t gain_q8;
    uint32_t clipped_samples;
    uint16_t pwm_min;
    uint16_t pwm_max;
};

/* Flush queued audio, then wait for a fresh prefill before playing. */
void presto_audio_flush(void);
/* Mute also flushes. Writes while muted are discarded (for turbo mode). */
void presto_audio_set_muted(bool muted);
/* 0..8192, where 256 is unity gain. Default is 8192 (32x).
 * Changing gain flushes previously queued audio. */
void presto_audio_set_gain(uint32_t gain);
void presto_audio_get_stats(struct presto_audio_stats *stats);
/* Two direct hardware PWM tones, independent of the emulator and sample timer.
 * Pauses the caller for 600 ms, flushes playback, and restores mute state. */
bool presto_audio_test(void);

#endif
