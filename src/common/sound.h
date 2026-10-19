#ifndef PRESTO_AUDIO_H
#define PRESTO_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The BBC's sound on the board's audio out (the Presto: its piezo): core 0 notes each byte the SN76489 takes (with the
// emulated time, from beebjit's sound.c), core 1 makes the samples and DMA plays them.

// on core 0, before the machine starts: the ring for the bytes, audio_ring_size() bytes
// (in PSRAM will do)
void audio_set_ring(void *p);
size_t audio_ring_size(void);
// on core 1, once: the PWM, the DMA and its interrupt (which then runs on core 1)
void audio_init(void);
// from sound.c (BEEBJIT_SOUND_WRITE_HOOK), on core 0: a byte written to the SN76489
void audio_sn_write(uint64_t ticks, uint8_t value);
// each emulated frame, on core 0: the emulated time now (2 MHz ticks); mute in turbo
void audio_frame(uint64_t ticks, bool mute);
// "!audio": the state of play ("AUDIO ..."); or, while playing, set
//   audio vol <percent>    the volume (100: four channels at full volume are full scale)
//   audio rate 125|62|31   the sample rate, kHz (125)
//   audio lp <hz>          the low-pass's corner, 0 off (8000)
//   audio dac 10|8         the PWM: 10 bits at 244 kHz, or 8 bits at 977 kHz (10)
void audio_command(const char *args);

// the user's settings (the web panel's Sound, "!sound"): on (1) / off (0, as it starts), and the volume in percent
// (100: full scale); -1 leaves either as it is
void audio_set(int on, int percent);
void audio_get(bool *on, unsigned *percent);

// "!beep ...": test sounds, holding the machine still meanwhile (the BBC's sound stops).
//   beep tone <hz> [ms]   a square wave at that pitch, full volume (as Pimoroni's Buzzer)
//   beep sweep            square waves 100 Hz - 8 kHz
//   beep dac [percent]    the PWM as a DAC: a sine sweep, a square-wave chord, noise
void audio_test(const char *args);

#endif
