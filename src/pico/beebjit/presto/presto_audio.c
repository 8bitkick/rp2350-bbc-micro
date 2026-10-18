/* SPDX-License-Identifier: GPL-3.0-or-later
 * Initial Pimoroni Presto PCM output backend: GPIO43 PWM, 31250 Hz samples.
 * Owns GPIO43 and its PWM slice. No PIO state machines or DMA channels used.
 * A repeating timer updates PWM every 32 us; profile IRQ cost on real hardware.
 * All public calls must run on core 0, where the default alarm pool runs.
 */
#include "presto_audio.h"
#include "os_sound.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include <stddef.h>
#include <string.h>

#define AUDIO_PIN 43u
#define SAMPLE_RATE 31250u
#define SAMPLE_US 32
#define PWM_TOP 7999u
#define PWM_MID 4000u
#define RING_SIZE 4096u
#define RING_MASK (RING_SIZE - 1u)
#define PREFILL 512u
#define DEFAULT_BUFFER 1024u
#define DEFAULT_PERIODS 4u
#define DEFAULT_GAIN_Q8 8192u /* 32x, selected from listening tests on the piezo. */

struct os_sound_struct {
    uint32_t buffer_size;
    uint32_t period_size;
    bool allocated;
    bool initialized;
};
static struct os_sound_struct driver;
static struct repeating_timer timer;
static uint16_t samples[RING_SIZE];
static volatile uint32_t head, tail;
static volatile uint32_t underruns, dropped;
static volatile bool playing, muted;
static volatile bool testing;
static volatile uint32_t timer_ticks, submitted_samples;
static int16_t pcm_min, pcm_max;
/* One PSG voice uses at most a quarter of the mix range. The on-board piezo
 * needs more gain than a line output. DC removal makes that gain usable. */
static uint32_t gain = DEFAULT_GAIN_Q8;
static int32_t dc_estimate;
static uint32_t clipped_samples;
static volatile uint16_t pwm_min = PWM_MID, pwm_max = PWM_MID;
/* Fractional read position and low-pass queue occupancy. Keep the LCD clock
 * master: gently resample by at most 1% instead of accumulating clock drift. */
static uint32_t read_fraction;
static int32_t average_fill = (PREFILL << 8);
static volatile int32_t rate_correction;

static bool __not_in_flash_func(audio_timer)(struct repeating_timer *unused) {
    (void) unused;
    ++timer_ticks;
    if (testing) return true;
    uint32_t available = head - tail;
    if (muted) {
        pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
        return true;
    }
    if (!playing) {
        if (available < PREFILL) {
            pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
            return true;
        }
        playing = true;
    }
    if (available == 0u) {
        ++underruns;
        playing = false;
        read_fraction = 0;
        average_fill = (PREFILL << 8);
        rate_correction = 0;
        pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
        return true;
    }
    int32_t value = samples[tail & RING_MASK];
    if (available > 1u) {
        int32_t next = samples[(tail + 1u) & RING_MASK];
        value += ((next - value) * (int32_t) read_fraction) / 65536;
    }
    pwm_set_gpio_level(AUDIO_PIN, (uint16_t) value);
    if (value < pwm_min) pwm_min = (uint16_t) value;
    if (value > pwm_max) pwm_max = (uint16_t) value;
    average_fill += (((int32_t) available * 256) - average_fill) / 1024;
    int32_t correction = ((average_fill / 256) - (int32_t) PREFILL) * 4;
    if (correction > 655) correction = 655;
    if (correction < -655) correction = -655;
    rate_correction = correction;
    read_fraction += (uint32_t) (65536 + correction);
    uint32_t advance = read_fraction >> 16;
    read_fraction &= 65535u;
    if (advance > available) advance = available;
    tail += advance;
    return true;
}

uint32_t os_sound_get_default_sample_rate(void) { return SAMPLE_RATE; }
uint32_t os_sound_get_default_buffer_size(void) { return DEFAULT_BUFFER; }
uint32_t os_sound_get_default_num_periods(void) { return DEFAULT_PERIODS; }

struct os_sound_struct *os_sound_create(char *name, uint32_t rate,
                                        uint32_t buffer_size,
                                        uint32_t num_periods) {
    (void) name;
    if (driver.allocated || rate != SAMPLE_RATE || num_periods == 0u ||
        buffer_size == 0u || buffer_size > RING_SIZE ||
        buffer_size % num_periods != 0u) {
        return NULL;
    }
    memset(&driver, 0, sizeof(driver));
    driver.buffer_size = buffer_size;
    driver.period_size = buffer_size / num_periods;
    driver.allocated = true;
    return &driver;
}

int os_sound_init(struct os_sound_struct *d) {
    if (d != &driver || !driver.allocated) return -1;
    if (driver.initialized) return 0;
    /* 31.25 kHz carrier at the project's 250 MHz system clock. The piezo's
     * switched transistor/passive pull-up circuit needs time to recharge;
     * the original 244 kHz carrier can severely attenuate the audio. */
    if (clock_get_hz(clk_sys) != 250000000u) return -1;
    head = tail = underruns = dropped = 0u;
    playing = muted = false;
    read_fraction = 0;
    average_fill = (PREFILL << 8);
    rate_correction = 0;
    gain = DEFAULT_GAIN_Q8;
    dc_estimate = 0;
    clipped_samples = 0;
    pwm_min = pwm_max = PWM_MID;
    testing = false;
    timer_ticks = submitted_samples = 0;
    pcm_min = pcm_max = 0;
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, PWM_TOP);
    uint slice = pwm_gpio_to_slice_num(AUDIO_PIN);
    pwm_init(slice, &cfg, false);
    pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    pwm_set_enabled(slice, true);
    /* Negative interval requests start-to-start scheduling. */
    if (!add_repeating_timer_us(-SAMPLE_US, audio_timer, NULL, &timer)) {
        pwm_set_enabled(slice, false);
        gpio_init(AUDIO_PIN);
        gpio_set_dir(AUDIO_PIN, GPIO_OUT);
        gpio_put(AUDIO_PIN, 0);
        return -1;
    }
    driver.initialized = true;
    return 0;
}

void presto_audio_flush(void) {
    uint32_t irq = save_and_disable_interrupts();
    head = tail = 0u;
    playing = false;
    dc_estimate = 0;
    pwm_min = pwm_max = PWM_MID;
    read_fraction = 0;
    average_fill = (PREFILL << 8);
    rate_correction = 0;
    if (driver.initialized) pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
    restore_interrupts(irq);
}

void presto_audio_set_muted(bool value) {
    uint32_t irq = save_and_disable_interrupts();
    muted = value;
    head = tail = 0u;
    playing = false;
    dc_estimate = 0;
    pwm_min = pwm_max = PWM_MID;
    read_fraction = 0;
    average_fill = (PREFILL << 8);
    rate_correction = 0;
    if (driver.initialized) pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
    restore_interrupts(irq);
}

void presto_audio_set_gain(uint32_t value) {
    gain = value > 8192u ? 8192u : value;
    presto_audio_flush();
}

void presto_audio_get_stats(struct presto_audio_stats *out) {
    if (out == NULL) return;
    uint32_t irq = save_and_disable_interrupts();
    out->queued_samples = head - tail;
    out->underruns = underruns;
    out->dropped_samples = dropped;
    out->timer_ticks = timer_ticks;
    out->submitted_samples = submitted_samples;
    out->pcm_min = pcm_min;
    out->pcm_max = pcm_max;
    out->initialized = driver.initialized;
    out->muted = muted;
    out->playing = playing;
    out->gain_q8 = gain;
    out->clipped_samples = clipped_samples;
    out->pwm_min = pwm_min;
    out->pwm_max = pwm_max;
    int32_t correction = rate_correction;
    restore_interrupts(irq);
    out->rate_adjust_ppm = (int32_t) (((int64_t) correction * 1000000) / 65536);
}

void os_sound_write(struct os_sound_struct *d, int16_t *frames, uint32_t count) {
    if (d != &driver || !driver.initialized || frames == NULL || muted) return;
    if (count != 0u) pcm_min = pcm_max = frames[0];
    submitted_samples += count;
    for (uint32_t i = 0; i < count; ++i) {
        if (frames[i] < pcm_min) pcm_min = frames[i];
        if (frames[i] > pcm_max) pcm_max = frames[i];
        /* Remove DC with an ~19 Hz high-pass before amplifying. The PSG mix
         * is normally unipolar; otherwise extra gain just biases/clips it.
         * Q8 state retains precision without floating point. */
        dc_estimate += (((int32_t) frames[i] * 256) - dc_estimate) / 256;
        int32_t centered = (int32_t) frames[i] - dc_estimate / 256;
        int32_t value = (centered * (int32_t) gain) / 256;
        /* Clamp before PWM scaling: prevents overflow at high gain. */
        if (value > 32767) {
            value = 32767;
            ++clipped_samples;
        } else if (value < -32768) {
            value = -32768;
            ++clipped_samples;
        }
        value = ((value + 32768) * (int32_t)(PWM_TOP + 1u)) / 65536;
        if (value < 0) value = 0;
        if (value > (int32_t) PWM_TOP) value = PWM_TOP;
        /* Keep the critical section short: conversion is outside it. */
        uint32_t irq = save_and_disable_interrupts();
        if (head - tail == RING_SIZE) {
            ++dropped; /* Drop newest; never stall emulation. */
        } else {
            samples[head & RING_MASK] = (uint16_t) value;
            ++head;
        }
        restore_interrupts(irq);
    }
}

bool presto_audio_test(void) {
    uint32_t clock = clock_get_hz(clk_sys);
    /* A divider of four gives 1 kHz with wrap 62499 at 250 MHz. */
    if (clock < 4000u || clock / 4000u > 65536u) return false;
    uint slice = pwm_gpio_to_slice_num(AUDIO_PIN);
    uint32_t irq = save_and_disable_interrupts();
    testing = true;
    bool was_muted = muted;
    restore_interrupts(irq);
    presto_audio_flush();
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_config_set_wrap(&cfg, (uint16_t)(clock / 4000u - 1u));
    pwm_init(slice, &cfg, false);
    pwm_set_gpio_level(AUDIO_PIN, (uint16_t)(clock / 8000u));
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    pwm_set_enabled(slice, true);
    sleep_ms(300);
    pwm_set_wrap(slice, (uint16_t)(clock / 8000u - 1u));
    pwm_set_gpio_level(AUDIO_PIN, (uint16_t)(clock / 16000u));
    sleep_ms(300);
    pwm_set_enabled(slice, false);
    if (driver.initialized) {
        cfg = pwm_get_default_config();
        pwm_config_set_clkdiv(&cfg, 1.0f);
        pwm_config_set_wrap(&cfg, PWM_TOP);
        pwm_init(slice, &cfg, false);
        pwm_set_gpio_level(AUDIO_PIN, PWM_MID);
        pwm_set_enabled(slice, true);
    } else {
        gpio_init(AUDIO_PIN);
        gpio_set_dir(AUDIO_PIN, GPIO_OUT);
        gpio_put(AUDIO_PIN, 0);
    }
    presto_audio_set_muted(was_muted);
    irq = save_and_disable_interrupts();
    testing = false;
    restore_interrupts(irq);
    return true;
}

uint32_t os_sound_get_sample_rate(struct os_sound_struct *d) {
    return (d == &driver && driver.allocated) ? SAMPLE_RATE : 0u;
}
uint32_t os_sound_get_buffer_size(struct os_sound_struct *d) {
    return (d == &driver && driver.allocated) ? driver.buffer_size : 0u;
}
uint32_t os_sound_get_period_size(struct os_sound_struct *d) {
    return (d == &driver && driver.allocated) ? driver.period_size : 0u;
}
void os_sound_destroy(struct os_sound_struct *d) {
    if (d != &driver || !driver.allocated) return;
    if (driver.initialized) {
        cancel_repeating_timer(&timer);
        pwm_set_enabled(pwm_gpio_to_slice_num(AUDIO_PIN), false);
        gpio_init(AUDIO_PIN);
        gpio_set_dir(AUDIO_PIN, GPIO_OUT);
        gpio_put(AUDIO_PIN, 0);
    }
    memset(&driver, 0, sizeof(driver));
    head = tail = 0u;
    playing = muted = false;
}
