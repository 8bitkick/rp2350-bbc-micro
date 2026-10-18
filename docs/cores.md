# Which core does what, and what it costs

The RP2350 has two Cortex-M33 cores (250 MHz here). Each machine gives each one fixed work. A
feature joins one of them, so before adding one, check that core has room on the hardest disc
the machine runs. A change that moves any figure here should say so in its commit, measured
the same way.

## The Model B (beebjit)

| Core 0 | Core 1 |
|---|---|
| beebjit: the 6502 (its JIT, or the interpreter), the video's timing, the VIAs, the disc | drawing: the render wire's calls (`beebjit_wire.c`) into the display's rows |
| pacing to real time, locked to the panel's refresh | the sound (`-DSOUND`): the SN76489's samples, made in the DMA interrupt (`src/common/sound.c`) |
| the front end: the USB channel, the command layer, the network (polled), drive 0 and the save disc | the LEDs (`-DAMBIENT`), when it's idle (`presto_ambient.c`) |
| a program built in alongside (`app_frame`), between frames | |

## The Master 128 (kc-beeb)

| Core 0 | Core 1 |
|---|---|
| kc-beeb: the 65C12, the VIAs, the CRTC, the 1770, paging (`machine_unit.c`, from SRAM) | the renderer: the video stream into the display's rows (`render_unit.c`, from SRAM) |
| pacing to real time | the sound (`-DSOUND`), as the Model B's |
| the front end, as the Model B's | the LEDs (`-DAMBIENT`), when it's idle |
| a program built in alongside (`app_frame`), between frames | |

## Measured

On the Presto at 250 MHz, `tools/presto.py stats` (each core's busy share, and the speed against
the real machine) about 45 s after flashing, on Elite's title screen:

| Build | Speed | Core 0 | Core 1 |
|---|---|---|---|
| Model B | 100% | 10% | 11% |
| Model B, SOUND + AMBIENT | 100% | 10-11% | 13-14% |
| Master | 100.0% | 44-45% | 32-33% |
| Master, SOUND + AMBIENT | 100.0% | 45% | 36% |
| Master, the Elite agent built in | 100.0% | 44% | 32% |

The sound and the LEDs cost core 1 about 3 points on either machine, and nothing on core 0. The
Master's harder discs leave much less room. These are from docs/master/README.md, before sound
and the LEDs:

| Disc | Speed | Core 0 / core 1 |
|---|---|---|
| Stunt Car Racer, racing | 100% | 47% / 32% |
| Prince of Persia | 100% | 50% / 33% |
| Twisted Brain (demo) | 100% | 54% / 34% |
| Arcadians | 100% | 43% / 45% |
| Evil Influences (demo), late part | **60%** | 98% / 49% |

So on the Master, core 0 is where the room runs out. Anything per frame that a feature adds there
(the front end, a built-in program) comes out of the machine's own time. Core 1 has room for
the sound and the LEDs on every disc but Evil Influences, which is held by core 0 anyway.

SRAM is the other budget. Free SRAM is `free` in `/api/status`: 73-75 KB on the Model B and
98-100 KB on the Master as built here, 44 KB on the Master with the Elite agent. The SRAM
placement (what runs from SRAM rather than through the 16 KB XIP cache) is each machine's own
linker script fragment, in `src/machines/<machine>/machine.cmake`.
