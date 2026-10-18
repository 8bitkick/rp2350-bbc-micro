#include <assert.h>
#include <stdio.h>
#include "../../src/pico/beebjit/presto/presto_audio.c"

int main(void) {
    assert(os_sound_create(NULL, 44100, 1024, 4) == NULL);
    assert(os_sound_create(NULL, 31250, 1024, 0) == NULL);
    struct os_sound_struct *d = os_sound_create(NULL, 31250, 1024, 4);
    assert(d && os_sound_get_period_size(d) == 256);
    assert(os_sound_create(NULL, 31250, 1024, 4) == NULL);
    mock_timer_fail = true;
    assert(os_sound_init(d) == -1);
    mock_timer_fail = false;
    assert(os_sound_init(d) == 0);
    assert(gain == 8192);
    int16_t block[4097];
    for (uint i = 0; i < 4097; ++i) block[i] = 32767;
    presto_audio_set_gain(256);
    os_sound_write(d, block, 4097);
    struct presto_audio_stats stats;
    presto_audio_get_stats(&stats);
    assert(stats.queued_samples == 4096 && stats.dropped_samples == 1);
    assert(stats.initialized && !stats.muted && stats.submitted_samples == 4097);
    assert(stats.pcm_min == 32767 && stats.pcm_max == 32767);
    assert(mock_wrap == 7999 && mock_divider == 1.0f);
    audio_timer(NULL);
    assert(mock_level > 7900 && mock_level <= 7999);
    for (uint i = 0; i < 4200; ++i) audio_timer(NULL);
    presto_audio_get_stats(&stats);
    assert(stats.underruns == 1 && mock_level == 4000);
    presto_audio_set_muted(true);
    os_sound_write(d, block, 512);
    assert(head == tail);
    presto_audio_set_muted(false);
    for (uint i = 0; i < 512; ++i) block[i] = -32768;
    os_sound_write(d, block, 512);
    audio_timer(NULL);
    assert(mock_level < 32);
    presto_audio_flush();
    assert(head == tail && !playing && read_fraction == 0);

    /* Reproduce the user's quiet 0..1634 PCM. Gain must increase the PWM
     * swing, remove DC and remain bounded for high-amplitude input. */
    for (uint i = 0; i < 512; ++i) block[i] = (i % 32 < 16) ? 1634 : 0;
    presto_audio_set_gain(256);
    os_sound_write(d, block, 512);
    uint16_t unity_lo = 7999, unity_hi = 0;
    for (uint i = 0; i < 512; ++i) {
        if (samples[i] < unity_lo) unity_lo = samples[i];
        if (samples[i] > unity_hi) unity_hi = samples[i];
    }
    presto_audio_set_gain(4096); /* 16x */
    os_sound_write(d, block, 512);
    uint16_t loud_lo = 7999, loud_hi = 0;
    for (uint i = 0; i < 512; ++i) {
        if (samples[i] < loud_lo) loud_lo = samples[i];
        if (samples[i] > loud_hi) loud_hi = samples[i];
    }
    assert(loud_hi - loud_lo > 10 * (unity_hi - unity_lo));
    assert(loud_lo < 4000 && loud_hi > 4000);
    presto_audio_set_gain(99999); /* Clamp to 32x. */
    assert(gain == 8192);
    uint32_t old_clipped = clipped_samples;
    for (uint i = 0; i < 512; ++i) block[i] = (i & 1) ? 32767 : -32768;
    os_sound_write(d, block, 512);
    assert(clipped_samples > old_clipped);
    for (uint i = 0; i < 512; ++i) assert(samples[i] <= 7999);
    presto_audio_set_gain(0);
    os_sound_write(d, block, 512);
    for (uint i = 0; i < 512; ++i) assert(samples[i] == 4000);
    presto_audio_set_gain(256);
    for (uint i = 0; i < 4097; ++i) block[i] = 10000;
    os_sound_write(d, block, 4097);
    assert(samples[4000] >= 4000 && samples[4000] <= 4001);
    presto_audio_flush();
    assert(dc_estimate == 0);

    /* 60 seconds each with a 0.2% faster/slower producer, in 256-sample
     * blocks. Verify bounded queues without overflow or rebuffering. */
    for (int delta = -2; delta <= 2; delta += 4) {
        presto_audio_flush();
        underruns = dropped = 0;
        for (uint i = 0; i < 512; ++i) block[i] = 0;
        os_sound_write(d, block, 512);
        uint32_t accumulator = 0;
        for (uint i = 0; i < 31250u * 60u; ++i) {
            accumulator += (uint32_t) (1000 + delta);
            if (accumulator >= 256000u) {
                accumulator -= 256000u;
                os_sound_write(d, block, 256);
            }
            audio_timer(NULL);
        }
        presto_audio_get_stats(&stats);
        assert(stats.underruns == 0 && stats.dropped_samples == 0);
        assert(stats.queued_samples > 128 && stats.queued_samples < 1024);
        assert(delta < 0 ? stats.rate_adjust_ppm < 0 : stats.rate_adjust_ppm > 0);
    }
    presto_audio_set_muted(true);
    uint32_t old_ticks = timer_ticks;
    assert(presto_audio_test());
    assert(mock_slept_ms == 600);
    assert(mock_wrap == 7999 && mock_divider == 1.0f);
    assert(muted && !testing && head == tail);
    audio_timer(NULL);
    assert(timer_ticks == old_ticks + 1);
    os_sound_destroy(d);
    assert(presto_audio_test()); /* Direct output also works without the sample driver. */
    assert(mock_slept_ms == 1200);
    assert(!driver.allocated);
    assert(os_sound_create(NULL, 31250, 1024, 4) != NULL);
    puts("PASS: backend conversion, gain, DC removal, clipping, bounds, lifecycle and clock drift");
    return 0;
}
