# A real speaker for the Presto

The BBC's sound (the SN76489, on either machine) already plays on the Presto's built-in piezo,
with the build option `-DSOUND=ON`. It works, but the piezo makes it sound bad. This plan puts the same audio through a small amplifier and a proper speaker, connected to the
Qw/ST (I2C) socket.

## The idea: use the socket's pins as plain GPIOs

The Qw/ST socket carries 3.3 V, ground, SDA (GPIO 40) and SCL (GPIO 41).

I2C itself is a poor way to carry audio:

- An I2C DAC such as the MCP4725 manages only about 10–30 kHz of sample writes, and it would keep
  the bus busy all the time.
- The usual I2S amplifier boards, such as the MAX98357A, need three signal pins. The socket has two.

So instead, the sound code's PWM output moves from the piezo's pin to GPIO 40 (SDA). That signal
goes through a simple filter into a small class-D amplifier board, which drives the speaker.

## Hardware

```
Qw/ST SDA (GPIO 40) ──1k──┬──── amp IN+
                          10nF
GND ──────────────────────┴──── amp IN−, amp GND
3V3 ─────────────────────────── amp VIN
                                amp OUT± ── 4–8 Ω speaker
```

| Part | Notes |
|---|---|
| Class-D amplifier | An Adafruit PAM8302 or similar: mono, about 2.5 W, runs from 2–5.5 V. It has its own input capacitors and a volume trimmer. |
| 1 kΩ resistor, 10 nF capacitor | Take out the PWM carrier. Without them the amp may hiss or whine. |
| Speaker | 4–8 Ω. Anything from a 28–40 mm speaker up to a scrap PC speaker. |
| Qw/ST cable | A JST-SH 4-pin lead with bare ends on the other side, or a Qw/ST breakout board. |

### Easiest route: the Adafruit STEMMA Speaker

The [Adafruit STEMMA Speaker](https://thepihut.com/products/adafruit-stemma-speaker-plug-and-play-audio-amplifier-ada3885)
(Adafruit 3885, about £5.70) is a class-D amplifier and a 1 W, 8 Ω speaker on one board, so
there's nothing to solder. **Tested on the board (2026-10-18): this works, with no filter.**

- **Electrically it matches.** It takes power, ground and one signal, which go to the socket's
  3V3, GND and SDA (GPIO 40). It runs from 3–5 V and accepts a signal up to its supply voltage, so
  a 3.3 V PWM output is fine. It has its own input AC coupling and a screwdriver volume control.
- **The connector doesn't match.** It uses the older 3-pin STEMMA connector (JST PH, 2 mm pitch),
  not the Presto's 4-pin Qw/ST (JST SH, 1 mm pitch). Use a Qw/ST-to-jumper-wire lead (Pimoroni and
  Adafruit sell them) and connect it either:
  - to the board's alligator pads, with clips, or
  - to its socket, with a JST PH 3-pin to jumper lead (red power, black ground, white signal).

  On the Qw/ST lead the usual colours are red 3V3, black GND, blue SDA and yellow SCL (not used).
  Check them against your lead before powering on.
- **Filter:** not needed. Wired straight from SDA, there was no whine or hiss. For another
  amplifier board that does whine, add the 1 kΩ / 10 nF filter inline on the signal wire.
- **Speaker:** about 30 mm across. It's not hi-fi, but it's a big step up from the piezo.

**Power:** running the amp from the socket's 3.3 V gives a few hundred milliwatts, which is plenty
on a desk. For more volume, take the amp's power from 5 V (USB) instead; it still accepts a 3.3 V
input signal.

## Firmware

Build with the sound on and its pin moved to the socket, for either machine:

```sh
cmake -S . -B build-sound -G Ninja ... -DMACHINE=master -DSOUND=ON -DSOUND_PIN=40
ninja -C build-sound
```

With no `SOUND_PIN`, the sound goes to the board's audio out (`BOARD_AUDIO_PWM_PIN` in
`src/boards/presto/board_config.h`: 43, the piezo). The piezo's pin is held low either way, so it
doesn't buzz. The sound code is `src/common/sound.c`.

- **PWM slice:** GPIO 40 is on PWM slice 8, which nothing else uses. The piezo (GPIO 43) uses slice
  9 and the backlight (GPIO 45) slice 10.
- **Channel:** pin 40 is channel A of its slice, where the piezo's pin is channel B. The code
  already writes the level into both halves of the slice's compare register, so that doesn't
  matter.

The controls over USB: `!audio` (state), `!audio vol N`, and `!beep tone|sweep|dac` for test
sounds. The sound is made at 125 kHz and passed through an 8 kHz low-pass filter. Through this
speaker that beat the first version's 31.25 kHz with no filter, which sounded harsh and crackly.
`!audio rate 125|62|31`, `!audio lp <hz>` (0 off) and `!audio dac 10|8` change these while the
music plays, to compare by ear.

## Trade-offs

- While the socket is used for sound, it can't also run I2C devices.
- If the board has pull-up resistors on SDA, the PWM output has to drive against them. That's a
  small load and harmless.

## Other options, not planned

- **Sound over USB:** the Presto would show up as a sound card on the PC. TinyUSB supports this
  (UAC2). It needs no extra hardware, but the sound comes out of the computer, not the Presto.
- **Bluetooth:** the WiFi chip (CYW43439) can also do Bluetooth A2DP to a speaker or headphones.
  That's a lot more firmware work.
