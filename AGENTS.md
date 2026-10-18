# AGENTS.md

Notes for anyone, human or coding agent, working on this repository: how to build and measure it,
where things are, what has already been tried, and the traps that cost time.

## What this is

[beebjit](https://github.com/scarybeasts/beebjit), Chris Evans' cycle-exact BBC Micro emulator, on
the Pimoroni Presto (RP2350, two Cortex-M33 cores at 250 MHz, 520 KB SRAM, 8 MB PSRAM, 480×480
ST7701 RGB panel), with a new Thumb-2 JIT backend. The firmware target is `presto_beebjit`.

The same tree builds a BBC Master 128 instead with `-DMACHINE=master` (target
`presto_master`, `build-master/presto_master.uf2`): Kieran Connell's cycle-stepped emulator in
`src/cores/kc-beeb/` (kept as upstream, with the changes listed in docs/master/README.md), its own Presto
port in `src/machines/master/` and `src/machines/master/machine.cmake`. For now the Master has its own copy
of the Presto layer (panel, PSRAM, SD, net, web panel) and no sound or ambient LEDs.

## Build, flash, run

```sh
scripts/setup.sh            # first time: toolchain and Pico SDK into deps/, Elite, build
. deps/env.sh && ninja -C build presto_beebjit   # rebuild -> build/presto_beebjit.uf2
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
`src/machines/beeb/harness/golden.txt`, for the interpreter, the JIT and the two-core render wire:

```sh
src/machines/beeb/harness/build.sh a32                       # build-a32/harness
WIRE=1 src/machines/beeb/harness/build.sh a32 build-a32-wire
src/machines/beeb/harness/tests.sh a32; JIT=1 src/machines/beeb/harness/tests.sh a32
JIT=1 src/machines/beeb/harness/tests.sh a32-wire
src/machines/beeb/harness/bench.sh -jit                      # QEMU: instructions per frame
```

A change to video, render or the JIT isn't done until those are frame-exact. For the web panel,
`tools/webdev.py <presto address>` serves `src/common/api/web/index.html` from disk and passes
`/api/` through to the board.

## Where things are

| Path | What |
|---|---|
| `src/cores/beebjit/` | beebjit, ported to the RP2350: Thumb-2 JIT backend and memory/render changes, on upstream 2ff70a0 (listed in `UPSTREAM`) |
| `src/cores/beebjit/asm/thumb2/` | The Thumb-2 JIT backend and inturbo (one handler per opcode, for self-modifying code) |
| `src/machines/beeb/beeb_main.c` | Firmware main: clocks, panel, PIO 640→480 scaler, pacing and turbo, statistics, profiler |
| `src/machines/beeb/beeb_machine.c` | The front end's machine hooks: keys, BREAK, discs, ROM sockets, snapshots, USB `!` commands |
| `src/machines/beeb/beebjit_wire.c` | The render wire: core 0's video sends render calls to core 1 |
| `src/machines/beeb/os_embedded.c`, `beebjit_embedded.c` | beebjit's OS layer and setup for a host with no OS; the SRAM / PSRAM allocator |
| `src/machines/beeb/harness/` | The off-board test harness, golden checksums, benchmarks |
| `src/boards/presto/presto_lcd.c`, `st7701_*.pio` | Panel driver: ST7701 init, PIO2 timing, PIO1 scanout of a 4-bit framebuffer, row-table DMA |
| `src/boards/presto/presto_net.c`, `web/` | WiFi, the JSON API and the web control panel |
| `src/boards/presto/presto_sd.c`, `presto_psram.c` | SD card (FatFs) and PSRAM |
| `src/cores/kc-beeb/` | The Master 128: Kieran Connell's emulator core (`machine_hook.h`: the interface for a program built in alongside) |
| `src/machines/master/` | The Master's Presto port: `main.c` (pacing, USB, stats), `machine_unit.c` / `render_unit.c` (run from SRAM), its own panel, PSRAM, SD, net and web panel |
| `tools/beebjit_hillclimb.py` | The optimization graph, with each step's measurements |

Memory: hot code (interpreter, video, render, JIT compiler...) is copied to SRAM at boot
(`PRESTO_BEEBJIT_RAM_OBJECTS`), since the XIP cache is only 16 KB. The PSRAM holds the JIT's code
(the bottom 4 MB, 64 bytes per 6502 address), beebjit's big cold tables and disc tracks, and at the
top the area where disc images and snapshots arrive.

## What has been done (and measured)

On Elite's title screen, from the JIT's first run to now: turbo 1.45× → ~9× real time; at real
speed, core 0 69% → 10%, core 1 10%; ~87 KB of SRAM free with WiFi up. `tools/beebjit_hillclimb.py`
has each step. The big ones: video emulated in runs and whole scanlines, then whole character rows;
drawing moved to core 1; core 1 skipping black and the 640→480 scaling on PIO; hot code in SRAM; no
steady-state recompiles; hardware register accesses compiled into JIT code (encoded callbacks);
smaller JIT code for the XIP cache (`subs`/`bpl` countdown checks, JSR / RTS / JMP (ind) through
stubs); MODE 7 as one wire command a line, drawn through a cache of teletext character runs; the
panel DMA reading the renderer's row directly; newlib's `mem*` functions in RAM.

Since step 18 the panel runs at 50 Hz with the BBC's clock locked to it (`lock_frame()`), and a
frame is emulated in 64-scanline slices so its rows reach core 1 at the beam's pace. That cost ~8%
of turbo, for smooth motion; `presto.py raw lock` (and `LOCK=1 tools/scenes.py`) reports rows drawn
after the scan passed them, which should be 0. Moving MODE 7 scenes (the scroll, the color
scroller, Teletext Elite) are the most demanding on core 1, so measure them first.

## Optimization avenues not yet taken

Measured in turbo at v0.1 (both cores saturated): core 0 was ~29% JIT code, ~27% video (most of
it encoding each line for core 1), ~21% the interpreter handling hardware register accesses (since
compiled into JIT code); core 1 mostly per-line overhead. Profile again before relying on these
shares: build with `-DPRESTO_BEEBJIT_PROF=ON` and run `tools/prof.py 0` (or 1).

- XIP cache misses on the JIT's code in PSRAM cost roughly a fifth of core 0 (slowing the PSRAM
  clock by a third costs 7%); flash misses cost nothing measurable.
- Bar Beeb's core 0 (~60% at real speed): its raster loop bounces out of the JIT at every
  scanline's video timer and the interpreter runs on to the next block start.
- SRAM: ~87 KB free. The sideways ROM banks live in PSRAM (a bank switch copies 16 KB from
  PSRAM, ~0.5 ms; nothing measured paid for it), the renderer's pixel indices are one 2 KB table
  for every mode, and the stacks are sized to at least 2.5x their high-water marks (`!mem` prints
  `STACKS`). The teletext run cache could now survive the frame if it were bigger.
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
