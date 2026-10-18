# Optional BBC audio on Pimoroni Presto

This source release integrates the existing beebjit SN76489 emulator with the
Presto's built-in GPIO43 piezo. No extra hardware is required. The implementation
has compiled successfully against Pico SDK 2.2.0 with Arm GNU 14.2.rel1, with audio
both ON and OFF. Physical playback, timing and CPU load still require board tests.

## Build and flash

```sh
scripts/setup.sh --audio
scripts/setup.sh --audio --gxr --flash
```

The second command also fetches the GXR ROM and flashes a Presto in BOOTSEL mode.
Existing --apt, --demos, --no-build and --flash options remain available.
--audio --no-build downloads dependencies but does not configure or build; repeat
--audio when you later run setup to build.

Audio defaults OFF. setup.sh always passes PRESTO_BEEBJIT_AUDIO=ON or OFF, so
running setup without --audio disables an earlier audio-enabled configuration.
A direct incremental build keeps the cached setting.

For manual configuration after `. deps/env.sh`:

```sh
cmake -S . -B build -DPRESTO_BEEBJIT_AUDIO=ON
cmake --build build --target presto_beebjit
```

Set OFF to disable it. CMake generates the build files; no handwritten Makefile
is needed. No Pico Extras dependency was added. PRESTO_BEEBJIT_FAST=ON is rejected
with audio because that build mode bypasses normal synchronous sound servicing.
Runtime turbo remains supported, with audio muted.

## Integrated changes

- presto_audio.c/.h implement os_sound.h using a copied PCM ring buffer and a
  repeating timer. GPIO43 PWM uses wrap 7999, divider 1; at 250 MHz its carrier
  is 31.25 kHz. Samples are updated every 32 microseconds (31,250 Hz).
- sound.c adds sound_tick_nonblocking() and sound_discard_pending(). Existing
  SN register handling, tone/noise generation and the desktop tick remain.
- bbc.c submits sound without blocking and retains bbc_do_sleep(), so LCD frame
  lock and USB/network polling remain the pacing path.
- os_embedded.c includes the null backend only when audio is disabled.
- run_beebjit() initializes/attaches the backend before emulation begins. Driver
  initialization failure is logged and emulation continues silently.
- Turbo changes and resets clear old output; screenshots temporarily mute and
  clear output on completion. The emulator's pending sample data is also cleared.
- core1_main() calls flash_safe_execute_core_init(), with pico/flash.h included.
  This fixes coordination of Wi-Fi credential flash writes with the active core.
  It is unconditional within core1_main(), independent of the audio build option.

The audio path disables the desktop double-precision IIR with
sound:filter_cutoff=0 to reduce CPU work on the RP2350. Existing averaging
resampling remains; there is no claim of desktop-equivalent filtered fidelity.

## Buffering and clock drift

The hardware queue holds 4096 samples (8 KB). Playback begins after a 512-sample
prefill (~16.4 ms). The emulator's default output buffer is 1024 int16 samples
and its internal SN buffer is 8192 int16 samples (~18 KB together, allocated
only when a driver is attached).

A smoothed queue-occupancy controller varies the fractional read rate by up to
1%, with linear interpolation, to accommodate small clock differences while
leaving the display in charge of emulator pacing. This avoids relying on an
unbounded queue. Underflow returns to silence/rebuffering; overflow drops newest
samples instead of stalling emulation. The fixed-size queue and counters make
these failures observable.

All backend APIs must run on core 0, using its default SDK alarm pool. The
renderer stays on core 1; no audio thread, extra PIO state machine or DMA channel
is required. Default gain is now 8192/256 (32x). The backend removes DC before
amplification. presto_audio_set_gain(0..8192) accepts up to 32x and clears queued
audio so the new setting takes effect promptly.

## Diagnostics and validation

```sh
python3 tools/presto.py raw audio
python3 tools/presto.py stats
python3 tools/presto.py raw lock
```

AUDIO v=3 reports initialization/mute/playback state, timer ticks, submitted
sample count, the latest PCM block minimum/maximum, queue depth, underrun
events, dropped samples, resampling correction, gain, clipping and delivered PWM
range. An audio-disabled build reports
that it needs rebuilding with --audio.

Portable host checks (C compiler plus Python 3; no SDK or network required):

```sh
python3 tests/audio/run.py
python3 tests/audio/setup_test.py
```

Tests cover the actual backend with mocked peripherals, initialization failure,
queue bounds, mute/flush, underrun recovery and 60-second simulations with +/-0.2%
producer clock mismatch. The actual sound generator is tested for tone output,
period submission, sample count and reset timing. Setup tests check help, invalid
options, combined flags, explicit ON/OFF and --no-build behaviour offline.

Release checks: full audio-enabled and audio-disabled firmware builds succeeded
using Pico SDK 2.2.0 and Arm GNU 14.2.rel1. These validation builds did not fetch
optional GXR/disc assets and are not included in the source ZIP. setup.sh retains
its normal Elite download/build behaviour. Existing presto_net.c format-truncation
and host picotool duplicate-library warnings remain; neither blocked linking.

On the board, test the startup sound, BASIC `SOUND 1,-15,100,20`, simultaneous
channels, noise and games; check CPU load, LCD lock and audio counters. Exercise
turbo, resets, screenshots and saving Wi-Fi credentials. The piezo has limited
bass. The timer interrupt can be delayed by flash operations or long critical
sections, so short audio interruptions are possible. Hardware playback and
performance have not been measured for this release. DMA output is a possible
future improvement if timer overhead or jitter proves excessive.

## Audio v2 diagnostic revision

The original 244 kHz carrier was selected without accounting for the speaker's
transistor switch and passive pull-up (R32, 1 kohm, across the piezo). This is not
a push-pull analogue output. The high carrier can limit recharge during PWM low
time and attenuate the audible signal. The v2 revision reduces the carrier to
31.25 kHz. This is a hardware-informed change, not a confirmed diagnosis of any
particular board's silence. Schematic: https://cdn.shopify.com/s/files/1/0174/1800/files/pico_presto_schematic.pdf?v=1734528559

After extracting this revision, BUILD AND FLASH it (building alone does not
change the running board):

```sh
scripts/setup.sh --audio --gxr --flash
python3 tools/presto.py raw "audio test"
python3 tools/presto.py raw audio
```

The direct hardware test should produce a 1 kHz beep followed by a 2 kHz beep,
300 ms each. It bypasses the emulator's samples and the sample timer, briefly
pauses emulation, and restores the PCM PWM setup and mute state afterwards.
It also works if sample-driver initialization failed. The AUDIO test=sent reply
means the PWM settings were applied, not that audible output was measured.

Run `raw audio` twice a few seconds apart. `ticks` should increase by about
31,250 per second; `submitted` should increase at roughly the same rate while
running at normal speed. `pcm_min` and `pcm_max` should differ during a sound.
A constant zero block is normal when the BBC is not playing anything. In BASIC,
try `SOUND 1,-15,150,100` for a sustained tone. If muted=1 and stats says turbo,
toggle turbo off using the existing turbo command before testing BBC audio.

If neither direct beep is audible, investigate GPIO/speaker output rather than
SN emulation. If direct beeps work but BBC sound does not, report two AUDIO v=2
lines and STATS while a sustained tone is playing. v=2 identifies this revision;
older output means the previous firmware is still running. The USB helper now
waits for an AUDIO reply, filtering unrelated periodic status messages.

Host regression tests include the revised PWM scale, timer/sample diagnostics,
and direct-test restoration both with and without an initialized sample driver.
The build is still subject to physical-board validation; no claim of verified
sound output is made.

## Audio v3 volume correction

Hardware testing of v2 confirmed that the direct test beeps worked but the BASIC
SOUND loop was almost inaudible. The reported PCM block ranged from 0 to 1634,
while the old backend applied 0.5x gain. This left very little PWM duty variation.

The v3 backend removes the DC component using a fixed-point ~19 Hz high-pass,
then applies adjustable gain with saturation before PWM scaling. The initial v3
default was 8x; the current default is 32x after listening tests on the Presto.
The high-pass allows positive and negative waveform swing instead of amplifying the unipolar mix's DC offset.
No change is made to the SN volume registers or channel mixing.

Build and flash this source, then run the continuous BASIC sound loop:

```basic
10 SOUND 1,-15,150,20
20 GOTO 10
RUN
```

Adjust gain without rebuilding:

```sh
python3 tools/presto.py raw "audio gain 8"
python3 tools/presto.py raw "audio gain 16"
python3 tools/presto.py raw audio
```

The command accepts integers 0..32; 0 silences PCM. The value is not saved to
flash, so reboot restores 32x. For loud multi-channel material, reduce the gain
if it sounds distorted. `clipped` is a cumulative count of saturated samples;
compare two readings to see if it is increasing. `pwm_min`/`pwm_max` show delivered
PWM counts since the last flush/gain change (silence midpoint 4000, range 0..7999).
`gain_q8=8192` means 32x, `gain_q8=4096` means 16x and `gain_q8=2048` means 8x. Raw `pcm_min`/`pcm_max` still
show the unamplified samples, so they will not rise when gain is increased.

The AUDIO line now starts with v=3. The hardware test and flash-safe modification
are retained. The gain, DC rejection, clipping and PWM bounds are covered by
host tests, including a 0..1634 waveform matching the user's measurement. Full
SDK builds are checked with audio on and off. Audible loudness of this revision
still needs the user's hardware check. Cumulative underruns are a separate
measurement; a single total does not establish their current rate.

## Default gain update

Listening tests found 8x very quiet, 16x still quiet, and 32x usable in a quiet
room. The startup and initialization default is therefore now 32x (gain_q8=8192).
The runtime command still accepts 0..32. All other v3 audio processing and the
flash-safe modification are retained. The AUDIO diagnostic remains v=3; inspect
gain_q8 to verify the new default after restarting the board.
