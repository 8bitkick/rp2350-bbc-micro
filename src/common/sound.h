#ifndef PRESTO_AUDIO_H
#define PRESTO_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The BBC's sound on the Presto's piezo: core 0 notes each byte the SN76489 takes (with the
// emulated time, from beebjit's sound.c), core 1 makes the samples and DMA plays them.

// on core 0, before the machine starts: the ring for the bytes, presto_audio_ring_size() bytes
// (in PSRAM will do)
void presto_audio_set_ring(void *p);
size_t presto_audio_ring_size(void);
// on core 1, once: the PWM, the DMA and its interrupt (which then runs on core 1)
void presto_audio_init(void);
// from sound.c (BEEBJIT_SOUND_WRITE_HOOK), on core 0: a byte written to the SN76489
void presto_audio_sn_write(uint64_t ticks, uint8_t value);
// each emulated frame, on core 0: the emulated time now (2 MHz ticks); mute in turbo
void presto_audio_frame(uint64_t ticks, bool mute);
// "!audio [vol <percent>]": the state of play ("AUDIO ..."), or set the volume
void presto_audio_command(const char *args);

// "!beep ...": test sounds, holding the machine still meanwhile (the BBC's sound stops).
//   beep tone <hz> [ms]   a square wave at that pitch, full volume (as Pimoroni's Buzzer)
//   beep sweep            square waves 100 Hz - 8 kHz
//   beep dac [percent]    the PWM as a DAC: a sine sweep, a square-wave chord, noise
void presto_audio_test(const char *args);

#endif
