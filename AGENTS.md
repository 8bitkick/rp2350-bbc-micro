# AGENTS.md

Notes for anyone, human or coding agent, working on this repository: how to build and measure it,
where things are, what has already been tried, and the traps that cost time.

## What this is

[beebjit](https://github.com/scarybeasts/beebjit), Chris Evans' cycle-exact BBC Micro emulator, on
the Pimoroni Presto (RP2350, two Cortex-M33 cores at 250 MHz, 520 KB SRAM, 8 MB PSRAM, 480×480
ST7701 RGB panel), with a new Thumb-2 JIT backend. The firmware target is `presto_beebjit`.

## Build, flash, run

```sh
scripts/setup.sh            # first time: toolchain and Pico SDK into deps/, Elite, build
. deps/env.sh && ninja -C build presto_beebjit   # rebuild -> build/src/pico/beebjit/presto_beebjit.uf2
```

Flash by copying the `.uf2` to the RP2350 drive. To get the drive: hold BOOT and tap RESET, send
`!bootsel` over USB, or open the USB serial port at 1200 baud. `PRESTO_BOOT_GUARD` (on by default)
catches a build that hangs or faults at boot, and reports where it died over USB, still accepting
`!bootsel`, instead of needing the buttons.

Check a change on the board with:
- `tools/presto.py stats`: per-core load and emulation speed (`speed=100%` is real time). The
  firmware also prints `#stats` and `#xip` (XIP cache hits and misses) lines every 250 frames.
- `tools/presto.py raw turbo`: unthrottled, to measure top speed.
- `tools/presto.py shot out.png`: the framebuffer, read back. That's what the firmware means to
  show, not what reaches the glass: after any display-driver or PIO change, look at the panel.
- `tools/presto.py raw clk`: clk_sys as measured by the frequency counter.

Off the board, the test harness runs the same Cortex-M33 code natively on a Raspberry Pi 5 (AArch32
mode) or in QEMU (mps2-an505), and checks the panel image and every frame's checksum against
`src/pico/beebjit/harness/golden.txt`, for the interpreter, the JIT and the two-core render wire:

```sh
src/pico/beebjit/harness/build.sh a32                       # build-a32/harness
WIRE=1 src/pico/beebjit/harness/build.sh a32 build-a32-wire
src/pico/beebjit/harness/tests.sh a32; JIT=1 src/pico/beebjit/harness/tests.sh a32
JIT=1 src/pico/beebjit/harness/tests.sh a32-wire
src/pico/beebjit/harness/bench.sh -jit                      # QEMU: instructions per frame
```

A change to video, render or the JIT isn't done until those are frame-exact. For the web panel,
`tools/webdev.py <presto address>` serves `src/pico/presto/web/index.html` from disk and passes
`/api/` through to the board.

## Where things are

| Path | What |
|---|---|
| `src/beebjit/` | beebjit's emulator core (see `UPSTREAM` for the commit and local changes) |
| `src/beebjit/asm/thumb2/` | The Thumb-2 JIT backend and inturbo (one handler per opcode, for self-modifying code) |
| `src/pico/beebjit/presto/presto_beebjit.c` | Firmware main: clocks, panel, PIO 640→480 scaler, pacing and turbo, statistics, profiler |
| `src/pico/beebjit/presto/presto_machine.c` | The front end's machine hooks: keys, BREAK, discs, ROM sockets, snapshots, USB `!` commands |
| `src/pico/beebjit/beebjit_wire.c` | The render wire: core 0's video sends render calls to core 1 |
| `src/pico/beebjit/os_embedded.c`, `beebjit_embedded.c` | beebjit's OS layer and setup for a host with no OS; the SRAM / PSRAM allocator |
| `src/pico/beebjit/harness/` | The off-board test harness, golden checksums, benchmarks |
| `src/pico/presto/presto_lcd.c`, `st7701_*.pio` | Panel driver: ST7701 init, PIO2 timing, PIO1 scanout of a 4-bit framebuffer, row-table DMA |
| `src/pico/presto/presto_net.c`, `web/` | WiFi, the JSON API and the web control panel |
| `src/pico/presto/presto_sd.c`, `presto_psram.c` | SD card (FatFs) and PSRAM |
| `tools/beebjit_hillclimb.py` | The optimization graph, with each step's measurements |

Memory: hot code (interpreter, video, render, JIT compiler...) is copied to SRAM at boot
(`PRESTO_BEEBJIT_RAM_OBJECTS`), since the XIP cache is only 16 KB. The PSRAM holds the JIT's code
(the bottom 4 MB, 64 bytes per 6502 address), beebjit's big cold tables and disc tracks, and at the
top the area where disc images and snapshots arrive.

## What has been done (and measured)

On Elite's title screen, from the JIT's first run to now: turbo 1.45× → 7.6× real time; at real
speed, core 0 69% → 13%, core 1 11%; 35 KB of SRAM free with WiFi up. `tools/beebjit_hillclimb.py`
has each step. The big ones: video emulated in runs and whole scanlines, then whole character rows;
drawing moved to core 1; core 1 skipping black and the 640→480 scaling on PIO; hot code in SRAM; no
steady-state recompiles.

## Optimization avenues not yet taken

Measured in turbo (both cores saturated): core 0 is ~29% JIT code, ~27% video (most of it encoding
each line for core 1), ~21% the interpreter handling hardware register accesses; core 1 is mostly
per-line overhead, with ~13% rebuilding colour tables as Elite rewrites its palette mid-frame.

- XIP cache misses on the JIT's code in PSRAM cost roughly a fifth of core 0 (slowing the PSRAM
  clock by a third costs 7%); flash misses cost nothing measurable.
- Hardware register accesses from JIT code (beebjit's "encoded callbacks" on x64) instead of
  falling back to the interpreter. Register-heavy programs (raster demos such as Bitshifters' Bar
  Beeb) run below real time today.
- Tried and dropped: drawing ordinary lines straight into the panel row on core 1 (no gain:
  the per-line costs moved rather than went).
- The clock stays at 250 MHz: speed comes from code, PIO and DMA, not overclocking further.

## Traps

- The harness's SRAM heap is unlimited: something that fits there can run out of SRAM on the board
  (the boot guard then reports a panic at step 4). Watch `#memory` and `!mem` after any change.
- beebjit's power-on reset fills RAM with &FF, so Elite's title-screen dashboard shows its escape
  pod colours (Elite reads the flag before setting it).
- PIO0 holds the 640→480 scaler, PIO1 the scanout, PIO2 the panel timing; the WiFi chip's SPI
  takes a PIO state machine too.
- Writing flash (saving WiFi details) needs core 1 locked out (`multicore_lockout`).
- The flash interface is slowed (QMI timing) before the clock goes to 250 MHz.
- Anything that changes the running CPU's view (ROM sockets, a snapshot's memory and registers) is
  done in the reset hook (`bbc_request_reset()`), at the CPU driver's safe point, not from a timer
  callback.
