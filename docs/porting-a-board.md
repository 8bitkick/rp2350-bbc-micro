# Porting to another RP2350 board

The firmware is split by role:

| Directory | What | Knows the board? |
|---|---|---|
| `src/cores/beebjit/`, `src/cores/kc-beeb/` | the emulators | no |
| `src/machines/beeb/`, `src/machines/master/` | each machine's glue: a core on an RP2350 (pacing, rendering into the display's format, its drive, its own USB commands) | only through `src/common/board.h` |
| `src/common/` | any RP2350 board: the command layer and web page (`api/`), the USB channel (`frontend.c`), drive 0 and the save disc (`drive.c`), the flash by region (`mem.c`), the sound synthesis (`sound.c`), the boot guard, the program hook (`app.c`) | only through `board.h` |
| `src/boards/<board>/` | the board's hardware | yes |

A build picks one of each: `-DMACHINE=beeb|master -DBOARD=presto`. The Presto is the only board
so far. A new board is a new `src/boards/<board>/` directory with three things in it, and
nothing in `src/common` or `src/machines` should need to change.

## 1. `board.cmake`

Included by the top-level `CMakeLists.txt` before the SDK is set up. It sets `PICO_BOARD` and
`PICO_PLATFORM` (and adds its directory to `PICO_BOARD_HEADER_DIRS` if it brings its own SDK
board header), and defines `board_setup(TARGET)`, which a machine calls to add the board's
sources (its network and web page among them, if it has one), PIO programs, include
directories and libraries to its firmware. `src/boards/presto/board.cmake` is the example.

## 2. `board_config.h`

The values the common code and the machines build with:

- `BOARD_NAME`: what `/api/status` reports (and the page shows).
- `BOARD_HAS_NET`, `BOARD_HAS_SD`, `BOARD_HAS_LEDS`: 0 or 1.
- The display: `BOARD_DISPLAY_FORMAT`, `BOARD_DISPLAY_WIDTH`, `BOARD_DISPLAY_HEIGHT`,
  `BOARD_DISPLAY_ROW_WORDS`, `BOARD_DISPLAY_ROW_END`. One format exists,
  `BOARD_DISPLAY_PACKED4_ROWS` (`board.h` describes it): a table of rows of 4-bit pixels, scanned
  out by the board. Both machines draw in it, 640-pixel BBC lines packed into 480 pixels, and
  refuse to build (`#error`) for another format or width. A board with another display either
  converts from this format (a 480-wide row table) or brings a new format, with a renderer for it
  in each machine.
- `BOARD_AUDIO_PWM_PIN`: the pin `src/common/sound.c` drives as a PWM DAC (with `-DSOUND=ON`).
- The flash layout, `BOARD_FLASH_LAYOUT`: the one table of where things live in the flash, by
  `mem.h`'s regions (the save disc's header and data, a spare sector, the WiFi network). Nothing
  else in the code has a flash address in it.

## 3. The functions in `src/common/board.h`

| Group | Functions | The Presto's |
|---|---|---|
| start-up | `board_init` | `presto_board.c` (the piezo pin driven off) |
| display | `board_display_init`, `_set_rows`, `_backlight`, `_row_rgb565` (screenshots), `_row`, `_frames`, `_frame_time_us`, `_frame_period_us`, `_frame_lines`, `_scan_row` (the Model B locks its frames to the refresh), `_debug` | `presto_lcd.c`: ST7701, PIO and DMA |
| big memory | `board_psram_init`, `board_psram_size`, `board_psram` | `presto_psram.c`: 8 MB PSRAM |
| storage | `board_sd_list_discs`, `_load_disc`, `_each_disc`, `_save`, `_put` (with `BOARD_HAS_SD`) | `presto_sd.c`, FatFs |
| LEDs | `board_leds_init`, `_frame`, `_due`, `_update`, `_ready`, `_get`, `_set`, `_command` (with `BOARD_HAS_LEDS`, `-DAMBIENT=ON`) | `presto_ambient.c`: 7 WS2812s |
| network | `net_init`, `net_poll`, `net_frame`, `net_status_json`, `net_print_status`, `net_wifi_save`, `net_restart` (`net_if.h`, with `BOARD_HAS_NET`) | `presto_net.c`: CYW43 WiFi, lwIP, mDNS, the HTTP server for `/api/<name>` and the page |

The network's HTTP server maps `/api/<name>` onto the command layer (`api_find_http`,
`api_run`), so the web page and `tools/presto.py` work on any board with one. A board without a
network leaves `BOARD_HAS_NET` at 0 and builds without the page; the USB channel has every
command.

## What still assumes this kind of board

- **PSRAM.** Both machines want the big memory: the Model B for its JIT's code (the bottom 4 MB)
  and beebjit's big tables, the Master for its sideways RAM, ROMs and disc track data. A board
  with no PSRAM would need the machines cut down (the Model B's interpreter only, fewer ROMs).
- **The clock.** Both machines run the RP2350 at 250 MHz (1.25 V) and set the flash's timing
  for it; their speed figures (docs/cores.md) are at that clock.
- **The display's format and width**, as above.
- **Core 1.** Both machines render on core 1 and make the sound and the LEDs there; a board's
  drivers must leave it free (the Presto's run on DMA and PIO).
