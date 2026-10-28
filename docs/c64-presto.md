> For this PR fileset, use [c64-pr-build.md](c64-pr-build.md): FRANK is fetched into `deps/frank-c64` instead of vendored. This port overview is supplemented by r11 and PR validation notes.

# Experimental Presto C64 integration

This source adds Commodore 64 to BBC Master and BBC Model B. It is an experimental port. Earlier builds were tested on a Presto by the contributor; this newer merged tree still needs hardware validation. Keep your known-good UF2 for rollback.

## What is connected

- FRANK/Frodo PAL CPU, VIC, CIA, IEC and SID cores, from the supplied FRANK archive (see `third_party/c64-sources.json`).
- Presto ST7701 LCD: all 16 C64 colours, using a separate RGB565 PIO output path. The original BBC display path is unchanged.
- Presto PWM sound output (GPIO 43 by default), driven by SID samples at approximately 44.1 kHz. Enable Sound in the web page. `SOUND_PIN` can select an external PWM output; this port does not implement an I2S DAC driver.
- Existing Wi-Fi/web API and SD driver. C64 mode lists `.d64` and `.d81` images. Uploads and ZIP extraction accept these formats too.
- Web machine selector, C64 on-screen keyboard, captured PC keyboard, and USB serial scancode input. This is not a USB-host or PS/2 hardware keyboard driver.
- Drive 8 uses the core's DOS-level IEC emulation. Custom 1541 fastloaders, copy protection and cycle-exact drive software are not supported by this integration.

## Using it

Choose **Commodore 64** in the web machine selector. Copy D64/D81 files to the SD card in the same location used for BBC disk images. Select an image in the SD list, or upload one through the web page. The port resets the C64 and queues `LOAD"*",8,1` followed by `RUN`. Some disks need a particular filename or a manual `SYS` command instead; use the keyboard for those.

The web Boot button repeats this sequence. F12 sends RESTORE; Escape sends RUN/STOP. Shift plus cursor keys is handled for left/up. F1–F8 map to the C64 function keys. With keyboard capture on, numeric keypad 8/2/4/6/0 controls joystick port 2 (up/down/left/right/fire). On-screen joystick buttons send brief presses.

**Disk writes last only for the current mounted session.** Resetting the emulated CPU keeps the mounted disk, but replacing/ejecting the disk, switching machine or restarting the Presto discards its changes. The SD original is not overwritten. There is no C64 flash-save or disk-export feature in this version.

## Build

Use the original setup instructions to install Pico SDK and the Arm toolchain. The C64 target requires C++20 (tested toolchain will be recorded in the validation report).

```
cmake -S . -B build-c64 -G Ninja -DMACHINE=c64 -DSOUND=ON -DCMAKE_BUILD_TYPE=Release
ninja -C build-c64
```

The existing combined-build script now builds **all three** machines plus the launcher:

```
scripts/build-dual-boot.sh build-multimachine
```

The historical output filename remains `build-multimachine/presto_dual_boot.uf2`. It now contains three machines. Use this combined image to obtain the machine selector. Standalone C64 builds do not contain the BBC machines or selector.

The packer requires `--c64 build-c64/presto_c64.uf2` with this three-machine partition table. The C64 slot is partition index 3, at 8224 KiB, with a 4096 KiB capacity. Master and Model B slots and the app slot remain at their existing addresses. Wi-Fi/settings/save regions are outside these slots. Do not use the modified launcher with an older two-image packaging command: use the supplied three-machine build script.

## Memory and timing

The port retains the BBC build's 250 MHz CPU setting. The original FRANK board's 378–504 MHz settings are not assumed safe or necessary for Presto. Real-time speed, Wi-Fi responsiveness under load, display timing and sound quality must be measured on the device.

PSRAM layout: first 1 MiB is upload staging; next 1 MiB is the mounted disk; remaining PSRAM is the C++ heap and VIC framebuffer. C64 RAM stays in SRAM. A separate 272-row RGB565 LCD framebuffer uses 262208 bytes of SRAM. Core 0 has a reserved 16 KiB stack, accommodating the disk helpers rather than relying on the SDK's 2 KiB default. Audio uses a bounded producer/consumer ring; overflow drops samples rather than writing beyond it. DMA wraps inside each audio buffer while flash operations defer interrupts. The renderer never waits for audio samples.

SD filenames use the newer upstream streamed listing implementation in all three machines; the list is not limited to one response-buffer-sized batch. HTTP responses use deferred connection cleanup so streaming callbacks do not free a live connection.

C64 ambient LEDs use a separate RGB565 sampler. BBC modes retain their existing sampler. BBC app plugins are not initialized in C64 mode.

## Changes to imported code

`rp2350/Prefs_rp2350.cpp` uses the canonical `Prefs.h` definition. Its original simplified header omitted map members present in the core's definition, causing incompatible object layouts. `C64_rp2350.cpp` creates the existing tape stub, since CPU port writes unconditionally call its motor method. A null tape pointer would fault even without loading a tape. All other platform adaptation lives in `src/machines/c64`, common integration or the Presto board driver.

The FRANK dependency retains its ROM arrays and source licences. The build fetches the pinned FRANK fork and the checksum-verified Elite disk as described in c64-pr-build.md.

## Hardware acceptance checks still required

1. Boot each of the three machines; switch between them and power-cycle with C64 selected.
2. Confirm the C64 BASIC screen, all 16 colours and stable LCD timing.
3. Load known standard-loader D64 and D81 images from SD and browser upload; test an invalid/truncated image.
4. Check keyboard, RESTORE, joystick port 2 and SID audio.
5. Keep C64 running overnight, checking the web API, speed and memory figures periodically.
6. Recheck BBC Master and Model B sound, disks and saved games.

Compilation and host-side tests cannot establish these device behaviours.
