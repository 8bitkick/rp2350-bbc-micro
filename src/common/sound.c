/*
 * The BBC's sound on the board's audio out, a PWM pin (board_config.h: BOARD_AUDIO_PWM_PIN; the
 * Presto's piezo), or on another (-DSOUND_PIN; on the Presto, 40 is the Qw/ST socket's SDA, for
 * an amplifier and a speaker: docs/presto/speaker.md).
 *
 * Core 0 runs the 6502: each byte the SN76489 takes goes into a ring here with its emulated time
 * (beebjit's sound.c calls audio_sn_write), and each frame says what the time is now.
 * Core 1 makes the samples: an SN76489 as beebjit's (sound.c: the same counters, noise and
 * volumes, at its 250 kHz), averaged in pairs to 125 kHz, then through a low-pass at 8 kHz (two
 * one-pole filters in a row). Each byte lands on the SN tick it was written at, so sample
 * playback through the volume registers comes out. Through a speaker, 31.25 kHz (eights, as at
 * first) sounded harsh: the square waves' overtones fold back into the audible band; unfiltered
 * at 125 kHz it sounded tinny. !audio rate / lp / dac change these while playing, to compare.
 *
 * The ring (in PSRAM: SRAM is short, and the XIP cache that both cores read it through is one)
 * holds each byte with the low 24 bits of its time.
 *
 * The PWM is a 10 bit DAC at 244 kHz (250 MHz / 1024). Two DMA channels, chained, feed its
 * level from two buffers (16 bit writes to CC land in both halves, so either channel's pin
 * works), paced by a DMA timer at the sample rate; as each buffer finishes, the DMA interrupt
 * (on core 1) fills it. 8 bits at 977 kHz (!audio dac 8) sounded robotic.
 *
 * The sound runs LATENCY behind the emulated time. The emulation runs in bursts and sleeps, so a
 * frame's time is a poor clock: each frame gives the offset of the emulated time from the real
 * one, smoothed, and the rate it's running at (from that offset's drift). The sound follows that
 * rate, steered towards LATENCY: running slow, the bytes come further apart and the music slows
 * with the machine, at its own pitch. Off by more than SLACK (a stall, a reset), it jumps back.
 * Turbo, or no frame for a while (a disc loading), is quiet.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "sound.h"
#include "board.h"

#define SOUND_PIN       BOARD_AUDIO_PWM_PIN     // the board's audio out (board_config.h, -DSOUND_PIN)
#define DAC_TOP         1023
#define SAMPLE_HZ       31250               // the test sounds' rate
#define SAMPLE_US       32
#define SN_HZ           250000              // the SN76489's ticks, 8 of the 2 MHz ticks
#define BUFFER_SAMPLES  256                 // 2 ms at 125 kHz
#define LP_HZ           8000                // the low-pass's corner
#define LATENCY         100000              // ticks (50 ms) behind the emulated time
#define SLACK           90000               // off by this much, jump back to LATENCY
#define RING            1024                // bytes written to the SN76489, in flight

// ---------------------------------------------------------------------------------------
// The ring: core 0 adds, core 1 takes

static uint32_t *s_ring;                    // tick << 8 | value
static volatile uint32_t s_ring_head;       // written by core 0
static volatile uint32_t s_ring_tail;       // written by core 1
static volatile uint32_t s_offset;          // emulated ticks - 2 x time_us_32(), smoothed
static volatile uint32_t s_rate_q16 = 65536;  // emulated time / real time
static volatile uint32_t s_frames;
static volatile bool s_mute;
static volatile bool s_have_now;
static uint32_t s_dropped;

void __not_in_flash_func(audio_sn_write)(uint64_t ticks, uint8_t value) {
    uint32_t head = s_ring_head;
    if ((s_ring == NULL) || ((head - s_ring_tail) >= RING)) {
        s_dropped++;
        return;
    }
    s_ring[head % RING] = (((uint32_t) ticks) << 8) | value;
    __dmb();
    s_ring_head = head + 1;
}

void audio_set_ring(void *p) {
    s_ring = (uint32_t *) p;
}

size_t audio_ring_size(void) {
    return RING * sizeof(uint32_t);
}

void audio_frame(uint64_t ticks, bool mute) {
    // the smoothed offset at each of the last HISTORY frames, for the rate
    enum { HISTORY = 32 };
    static uint32_t s_hist_offset[HISTORY], s_hist_us[HISTORY];
    static uint32_t s_hist_n;
    uint32_t us = time_us_32();
    uint32_t offset = (uint32_t) ticks - (2 * us);
    int32_t d = (int32_t) (offset - s_offset);
    if (!s_have_now || (d > 200000) || (d < -200000)) {
        // the start, a stall, a reset, turbo: start again
        s_offset = offset;
        s_hist_n = 0;
        s_rate_q16 = 65536;
    } else {
        s_offset += (uint32_t) (d / 16);
    }
    uint32_t i = s_hist_n % HISTORY;
    if (s_hist_n >= HISTORY) {
        // the offset drifting by 2 ticks a microsecond is the machine at twice the speed
        int32_t drift = (int32_t) (s_offset - s_hist_offset[i]);
        uint32_t elapsed = us - s_hist_us[i];
        int64_t rate = 65536 + (((int64_t) drift << 15) / (int64_t) elapsed);
        if (rate < 16384) rate = 16384;
        if (rate > 131072) rate = 131072;
        s_rate_q16 = (uint32_t) rate;
    }
    s_hist_offset[i] = s_offset;
    s_hist_us[i] = us;
    s_hist_n++;
    s_mute = mute;
    s_have_now = true;
    __dmb();
    s_frames++;
}

// ---------------------------------------------------------------------------------------
// The SN76489, as sound.c's (see the notes there)

static uint16_t s_counter[4], s_period[4];
static uint8_t s_output[4], s_volume[4];
static uint16_t s_noise_rng;
static uint8_t s_noise_type, s_noise_frequency, s_latched;
static uint16_t s_volume_out[16];           // a channel's level, 0 .. 8191
static uint32_t s_gain = 256;               // 256 is 4 channels at full volume = full scale
// !audio rate / lp / dac: SN ticks a sample (2: 125 kHz, 4: 62.5, 8: 31.25), a two pole low-pass
// (Q16 coefficient, 0 off), and the PWM's wrap (1023: 10 bits at 244 kHz, 255: 8 bits at 977 kHz)
static volatile uint32_t s_sn_ticks = 2;
static volatile uint32_t s_lp_a;
static volatile uint32_t s_lp_hz;
static volatile uint32_t s_dac_top = DAC_TOP;
static int32_t s_lp_y1, s_lp_y2;            // the filter's state, levels << 16

static void sn_reset(void) {
    for (int i = 0; i < 4; i++) {
        s_volume[i] = 0;
        s_period[i] = 0;
        s_counter[i] = 0;
        s_output[i] = 0;
    }
    s_noise_frequency = 2;
    s_period[3] = 0x40;
    s_noise_type = 0;
    s_latched = 0;
    s_noise_rng = 0;
    for (int i = 0; i < 15; i++) {
        s_volume_out[i] = (uint16_t) lroundf(8191.0f * powf(10.0f, -0.1f * i));
    }
    s_volume_out[15] = 0;
}

static void __not_in_flash_func(sn_write)(uint8_t value) {
    if (value & 0x80) {
        s_latched = (value & 0x70);
    }
    uint8_t command = (uint8_t) (s_latched | (value & 0x80));
    uint8_t channel = ((command >> 5) & 0x03);
    int32_t new_period = -1;
    if (command & 0x10) {
        s_volume[channel] = (value & 0x0f);
    } else if (channel == 3) {
        s_noise_frequency = (value & 0x03);
        new_period = (s_noise_frequency == 0) ? 0x10 :
                     (s_noise_frequency == 1) ? 0x20 :
                     (s_noise_frequency == 2) ? 0x40 : s_period[2];
        s_noise_type = ((value & 0x04) >> 2);
        s_noise_rng = (1 << 14);
    } else if (command & 0x80) {
        new_period = ((value & 0x0f) | (s_period[channel] & 0x3f0));
    } else {
        new_period = (((value & 0x3f) << 4) | (s_period[channel] & 0x0f));
    }
    if (new_period != -1) {
        s_period[channel] = (uint16_t) new_period;
        if ((channel == 2) && (s_noise_frequency == 3)) {
            s_period[3] = (uint16_t) new_period;
        }
    }
}

// one SN tick: the sum of the four channels' levels
static inline uint32_t __not_in_flash_func(sn_tick)(void) {
    uint32_t sum = 0;
    for (int c = 0; c < 4; c++) {
        uint16_t counter = (uint16_t) ((s_counter[c] - 1) & 0x3ff);
        uint8_t output = s_output[c];
        if (counter == 0) {
            counter = s_period[c];
            output = !output;
            s_output[c] = output;
            if ((c == 3) && output) {
                if (s_noise_type == 0) {
                    s_noise_rng >>= 1;
                    if (s_noise_rng == 0) s_noise_rng = (1 << 14);
                } else {
                    int bit = ((s_noise_rng & 1) ^ ((s_noise_rng & 2) >> 1));
                    s_noise_rng = (uint16_t) ((s_noise_rng >> 1) | (bit << 14));
                }
            }
        }
        s_counter[c] = counter;
        if (c == 3) output = (s_noise_rng & 1);
        if (output) sum += s_volume_out[s_volume[c]];
    }
    return sum;
}

// ---------------------------------------------------------------------------------------
// Playing

static uint16_t s_buffer[2][BUFFER_SAMPLES];        // the PWM's level
static int s_dma[2] = { -1, -1 };
static int s_dma_timer = -1;
static uint32_t s_cursor;                           // the emulated time of the next sample
static uint32_t s_cursor_frac;                      // and 256ths of a tick
static bool s_running;                              // s_cursor means something
// for !audio
static volatile uint32_t s_jumps, s_quiet_buffers, s_buffers, s_events;
static volatile uint32_t s_jumps_ahead, s_jumps_behind;
static volatile int32_t s_last_lag, s_min_lag = INT32_MAX, s_max_lag = INT32_MIN;
static volatile uint32_t s_last_step;

static void __not_in_flash_func(fill)(uint16_t *buf) {
    static uint32_t s_last_frames, s_still_buffers;
    uint32_t frames = s_frames;
    uint32_t sn_ticks = s_sn_ticks;
    uint32_t ticks_per_sample = 8 * sn_ticks;           // 2 MHz ticks
    // no frame for 65 ms: the machine is held (a disc loading, a test sound)
    s_still_buffers = (frames == s_last_frames) ? (s_still_buffers + 1) : 0;
    s_last_frames = frames;
    bool quiet = s_mute || !s_have_now || (s_still_buffers >= 64 / sn_ticks);
    // ticks a sample, in 256ths: the machine's rate, steered towards LATENCY
    uint32_t step = (ticks_per_sample << 8);
    if (!quiet) {
        uint32_t now = (2 * time_us_32()) + s_offset;
        int32_t lag = (int32_t) (now - s_cursor);
        if (lag < s_min_lag) s_min_lag = lag;
        if (lag > s_max_lag) s_max_lag = lag;
        if (!s_running || (lag > (LATENCY + SLACK)) || (lag < (LATENCY - SLACK))) {
            if (s_running) {
                if (lag > LATENCY) s_jumps_behind++; else s_jumps_ahead++;
            }
            s_cursor = now - LATENCY;
            s_cursor_frac = 0;
            s_running = true;
            s_jumps++;
            lag = LATENCY;
        }
        s_last_lag = lag;
        // 10 ms (20000 ticks) behind runs 1% fast to catch up
        int32_t steer = (int32_t) (((lag - LATENCY) / 122) * (int32_t) sn_ticks) / 8;
        int32_t most = (int32_t) (256 * sn_ticks);
        if (steer > most) steer = most;
        if (steer < -most) steer = -most;
        step = (uint32_t) ((int32_t) ((s_rate_q16 * ticks_per_sample) >> 8) + steer);
        if (step < (ticks_per_sample << 6)) step = (ticks_per_sample << 6);
        if (step > (ticks_per_sample << 9)) step = (ticks_per_sample << 9);
    } else {
        s_running = false;
    }
    s_last_step = step;
    s_buffers++;
    if (quiet) {
        // keep up with the chip's state, quietly
        uint32_t head = s_ring_head;
        while (s_ring_tail != head) {
            sn_write((uint8_t) s_ring[s_ring_tail % RING]);
            s_ring_tail++;
        }
        for (int i = 0; i < BUFFER_SAMPLES; i++) buf[i] = 0;
        s_lp_y1 = s_lp_y2 = 0;
        s_quiet_buffers++;
        return;
    }
    uint32_t cursor = s_cursor;
    uint32_t frac = s_cursor_frac;
    uint32_t head = s_ring_head;
    __dmb();
    uint32_t tail = s_ring_tail;
    // sum: up to sn_ticks x 4 channels x 8191; gain 256 makes that full scale (DAC_TOP)
    uint32_t gain = (s_gain * 8) / sn_ticks;
    uint32_t top = s_dac_top;
    uint32_t shift = (top == DAC_TOP) ? 16 : 18;
    int32_t lp_a = (int32_t) s_lp_a;
    int32_t y1 = s_lp_y1, y2 = s_lp_y2;
    for (int i = 0; i < BUFFER_SAMPLES; i++) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < sn_ticks; k++) {
            // the emulated time this SN tick ends at
            uint32_t t = cursor + ((frac + (((k + 1) * step) / sn_ticks)) >> 8);
            // the bytes written before it (the time's low 24 bits, against t's)
            while ((tail != head) &&
                   ((int32_t) ((s_ring[tail % RING] & ~0xffu) - (t << 8)) < 0)) {
                sn_write((uint8_t) s_ring[tail % RING]);
                tail++;
                s_events++;
            }
            sum += sn_tick();
        }
        uint32_t level = (uint32_t) (((uint64_t) sum * gain) >> shift);
        if (lp_a) {
            // two one-pole low-passes in a row, in levels << 16
            y1 += (int32_t) (((int64_t) (((int32_t) level << 16) - y1) * lp_a) >> 16);
            y2 += (int32_t) (((int64_t) (y1 - y2) * lp_a) >> 16);
            level = (uint32_t) ((y2 + 0x8000) >> 16);
        }
        if (level > top) level = top;
        buf[i] = (uint16_t) level;
        frac += step;
        cursor += (frac >> 8);
        frac &= 0xff;
    }
    s_lp_y1 = y1;
    s_lp_y2 = y2;
    s_ring_tail = tail;
    s_cursor = cursor;
    s_cursor_frac = frac;
}

static void __not_in_flash_func(dma_isr)(void) {
    for (int b = 0; b < 2; b++) {
        if (dma_irqn_get_channel_status(1, s_dma[b])) {
            dma_irqn_acknowledge_channel(1, s_dma[b]);
            dma_channel_set_read_addr(s_dma[b], s_buffer[b], false);
            fill(s_buffer[b]);
        }
    }
}

static void pwm_dac(void) {
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, (uint16_t) s_dac_top);
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
    pwm_init(pwm_gpio_to_slice_num(SOUND_PIN), &cfg, true);
    pwm_set_gpio_level(SOUND_PIN, 0);
}

// a channel's CHAIN_TO, through the alias that doesn't trigger it (to itself: no chain)
static void chain(int ch, int to) {
    uint32_t ctrl = dma_hw->ch[ch].al1_ctrl;
    dma_hw->ch[ch].al1_ctrl = (ctrl & ~DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) |
                              ((uint32_t) to << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
}

static void start(void) {
    pwm_dac();
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < BUFFER_SAMPLES; i++) s_buffer[b][i] = 0;
        dma_channel_set_read_addr(s_dma[b], s_buffer[b], false);
        chain(s_dma[b], s_dma[b ^ 1]);
    }
    s_running = false;
    dma_channel_start(s_dma[0]);
}

static void stop(void) {
    // unchained first: aborting a channel can fire its chain, restarting the other
    uint32_t mask = (1u << s_dma[0]) | (1u << s_dma[1]);
    for (int b = 0; b < 2; b++) {
        dma_irqn_set_channel_enabled(1, s_dma[b], false);
        chain(s_dma[b], s_dma[b]);
    }
    dma_hw->abort = mask;
    while (dma_hw->abort & mask) tight_loop_contents();
    for (int b = 0; b < 2; b++) {
        dma_irqn_acknowledge_channel(1, s_dma[b]);
        dma_irqn_set_channel_enabled(1, s_dma[b], true);
    }
    pwm_set_gpio_level(SOUND_PIN, 0);
}

// the DMA timer at the sample rate: 250 MHz x 1 / 2000 = 125 kHz (2 SN ticks a sample)
static void set_rate(void) {
    dma_timer_set_fraction((uint) s_dma_timer, 1,
                           (uint16_t) (clock_get_hz(clk_sys) / (SN_HZ / s_sn_ticks)));
}

static void set_lp(uint32_t hz) {
    uint32_t rate = SN_HZ / s_sn_ticks;
    s_lp_hz = hz;
    s_lp_a = (hz == 0) ? 0 : (uint32_t) lroundf(65536.0f *
                                                (1.0f - expf(-6.2831853f * (float) hz / (float) rate)));
}

void audio_init(void) {
    uint slice = pwm_gpio_to_slice_num(SOUND_PIN);
    if (s_ring == NULL) return;
    sn_reset();
    s_dma_timer = dma_claim_unused_timer(true);
    set_rate();
    set_lp(LP_HZ);
    s_dma[0] = dma_claim_unused_channel(true);
    s_dma[1] = dma_claim_unused_channel(true);
    for (int b = 0; b < 2; b++) {
        dma_channel_config c = dma_channel_get_default_config(s_dma[b]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, dma_get_timer_dreq((uint) s_dma_timer));
        channel_config_set_chain_to(&c, s_dma[b ^ 1]);
        dma_channel_configure(s_dma[b], &c, &pwm_hw->slice[slice].cc, s_buffer[b],
                              BUFFER_SAMPLES, false);
        dma_irqn_set_channel_enabled(1, s_dma[b], true);
    }
    irq_set_exclusive_handler(DMA_IRQ_1, dma_isr);
    irq_set_enabled(DMA_IRQ_1, true);
    start();
}

void audio_command(const char *args) {
    unsigned percent, n;
    while (*args == ' ') args++;
    if (sscanf(args, "vol %u", &percent) == 1) {
        if (percent > 1600) percent = 1600;
        s_gain = (percent * 256) / 100;
    } else if ((sscanf(args, "rate %u", &n) == 1) && (s_dma_timer >= 0)) {
        // kHz: 31, 62 or 125
        s_sn_ticks = (n >= 100) ? 2 : (n >= 50) ? 4 : 8;
        set_rate();
        set_lp(s_lp_hz);
    } else if (sscanf(args, "lp %u", &n) == 1) {
        if (n >= (SN_HZ / s_sn_ticks) / 2) n = 0;     // below the Nyquist frequency
        set_lp(n);
    } else if ((sscanf(args, "dac %u", &n) == 1) && (s_dma[0] >= 0)) {
        s_dac_top = (n == 8) ? 255 : DAC_TOP;
        pwm_set_wrap(pwm_gpio_to_slice_num(SOUND_PIN), (uint16_t) s_dac_top);
    }
    // the lag's range is since the last !audio
    printf("AUDIO vol=%u%% hz=%lu lp=%lu dac=%u writes=%lu dropped=%lu buffers=%lu quiet=%lu "
           "jumps=%lu (behind %lu, ahead %lu) lag_ms=%ld (%ld..%ld) rate=%lu.%lu%% step=%lu.%02lu%s\n",
           (unsigned) ((s_gain * 100) / 256), (unsigned long) (SN_HZ / s_sn_ticks),
           (unsigned long) s_lp_hz, (s_dac_top == DAC_TOP) ? 10u : 8u, (unsigned long) s_events,
           (unsigned long) s_dropped, (unsigned long) s_buffers,
           (unsigned long) s_quiet_buffers, (unsigned long) s_jumps,
           (unsigned long) s_jumps_behind, (unsigned long) s_jumps_ahead,
           (long) (s_last_lag / 2000), (long) (s_min_lag / 2000), (long) (s_max_lag / 2000),
           (unsigned long) ((s_rate_q16 * 100) >> 16),
           (unsigned long) (((s_rate_q16 * 1000) >> 16) % 10),
           (unsigned long) (s_last_step >> 8), (unsigned long) (((s_last_step & 0xff) * 100) >> 8),
           (s_dma[0] < 0) ? " (not started)" : "");
    s_min_lag = INT32_MAX;
    s_max_lag = INT32_MIN;
}

// ---------------------------------------------------------------------------------------
// Test sounds: the machine stands still meanwhile, and the BBC's sound stops

// a square wave at hz, half duty
static void square(float hz) {
    uint32_t sys = clock_get_hz(clk_sys);
    float div = (float) sys / (hz * 65536.0f);
    if (div < 1.0f) div = 1.0f;
    uint32_t top = (uint32_t) ((float) sys / (div * hz)) - 1;
    if (top > 65535) top = 65535;
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, div);
    pwm_config_set_wrap(&cfg, (uint16_t) top);
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
    pwm_init(pwm_gpio_to_slice_num(SOUND_PIN), &cfg, true);
    pwm_set_gpio_level(SOUND_PIN, (uint16_t) ((top + 1) / 2));
}

static void wait_ms(uint32_t ms) {
    while (ms > 0) {
        uint32_t n = (ms > 50) ? 50 : ms;
        sleep_ms(n);
        ms -= n;
        watchdog_update();
    }
}

// play n samples from fn (each -1.0 .. 1.0), scaled by gain, one every SAMPLE_US
static void dac_play(uint32_t n, float (*fn)(uint32_t i, void *ctx), void *ctx, float gain) {
    uint64_t t = time_us_64();
    for (uint32_t i = 0; i < n; i++) {
        float v = fn(i, ctx) * gain;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int level = (int) ((v + 1.0f) * 0.5f * (float) s_dac_top + 0.5f);
        t += SAMPLE_US;
        while (time_us_64() < t) {
        }
        pwm_set_gpio_level(SOUND_PIN, (uint16_t) level);
        if ((i & 1023) == 0) watchdog_update();
    }
}

static float s_phase;

// a sine sweeping exponentially from 100 Hz to 8 kHz over n samples
static float sine_sweep(uint32_t i, void *ctx) {
    uint32_t n = *(uint32_t *) ctx;
    float hz = 100.0f * powf(80.0f, (float) i / (float) n);
    s_phase += hz / SAMPLE_HZ;
    if (s_phase >= 1.0f) s_phase -= 1.0f;
    return sinf(s_phase * 6.2831853f);
}

// three square waves (a C major chord), each a third of full scale, as the SN76489 mixes them
static float square_chord(uint32_t i, void *ctx) {
    static const float hz[3] = { 261.63f, 329.63f, 392.00f };
    float v = 0.0f;
    (void) ctx;
    for (int c = 0; c < 3; c++) {
        float p = fmodf((float) i * hz[c] / SAMPLE_HZ, 1.0f);
        v += (p < 0.5f) ? (1.0f / 3.0f) : (-1.0f / 3.0f);
    }
    return v;
}

// the SN76489's white noise: a 15-bit LFSR, shifted at 250 kHz / 16 (its highest noise rate)
static float noise(uint32_t i, void *ctx) {
    static uint16_t lfsr = 1 << 14;
    (void) ctx;
    if (i & 1) {
        int bit = (lfsr & 1) ^ ((lfsr >> 1) & 1);
        lfsr = (uint16_t) ((lfsr >> 1) | (bit << 14));
    }
    return (lfsr & 1) ? 1.0f : -1.0f;
}

void audio_test(const char *args) {
    bool playing = (s_dma[0] >= 0);
    while (*args == ' ') args++;
    if (strncmp(args, "tone", 4) && strcmp(args, "sweep") && strncmp(args, "dac", 3)) {
        printf("BEEP usage: beep tone <hz> [ms] | sweep | dac [percent]\n");
        return;
    }
    if (playing) stop();
    if (!strncmp(args, "tone", 4)) {
        float hz = 1000.0f;
        unsigned ms = 1000;
        (void) sscanf(args + 4, "%f %u", &hz, &ms);
        if (hz < 20.0f) hz = 20.0f;
        if (ms > 10000) ms = 10000;
        printf("BEEP tone %u Hz %u ms\n", (unsigned) hz, ms);
        fflush(stdout);
        square(hz);
        wait_ms(ms);
    } else if (!strcmp(args, "sweep")) {
        printf("BEEP sweep 100 Hz - 8 kHz, square\n");
        fflush(stdout);
        for (int step = 0; step <= 48; step++) {
            square(100.0f * powf(80.0f, (float) step / 48.0f));
            wait_ms(80);
        }
    } else {
        unsigned percent = 100;
        (void) sscanf(args + 3, "%u", &percent);
        float gain = (float) percent / 100.0f;
        uint32_t n = 4 * SAMPLE_HZ;
        printf("BEEP dac %u%%: sine sweep, square chord, noise\n", percent);
        fflush(stdout);
        pwm_dac();
        s_phase = 0.0f;
        dac_play(n, sine_sweep, &n, gain);
        dac_play(2 * SAMPLE_HZ, square_chord, NULL, gain);
        dac_play(SAMPLE_HZ, noise, NULL, gain);
    }
    if (playing) {
        start();
    } else {
        // driven low, as board_init leaves it (a floating piezo picks up the WiFi and buzzes)
        pwm_set_enabled(pwm_gpio_to_slice_num(SOUND_PIN), false);
        gpio_init(SOUND_PIN);
        gpio_set_dir(SOUND_PIN, GPIO_OUT);
        gpio_put(SOUND_PIN, 0);
    }
    printf("BEEP done\n");
}
