# The dual-boot launcher: two machines and a plugin slot

One firmware with both machines in it, the BBC Master 128 (kc-beeb) and the BBC Model B
(beebjit), and the choice between them on the web panel. A small launcher at the bottom of the
flash picks one at power-on; the machine you chose last is the one that comes up. A third slot
holds an RP2350 application (elite-agent, say), built on its own and run alongside the machine
(below: the plugin).

What stays as it was: `-DMACHINE=beeb|master` still builds one machine on its own, its `.uf2`
flashed the usual way. The combined firmware is made from those same two builds: an image doesn't
know or care which slot it's in.

## The flash (16 MB)

```
0x000000  32 KB  launcher        unpartitioned space: the launcher, with the partition table
                                 embedded in its block loop
0x008000   4 MB  0 "master"      the BBC Master 128 image (a plain -DMACHINE=master build)
0x408000   4 MB  1 "beeb"        the BBC Model B image (a plain -DMACHINE=beeb build)
0x808000         (free)          room for either to grow
0xE00000   1 MB  2 "app"         the plugin (src/common/app_api.h), installed by the running firmware
0xF00000   1 MB  (shared)        unpartitioned, outside every image: the board's flash regions
                                 (BOARD_FLASH_LAYOUT), at the same physical offsets as ever
```

The shared area isn't a partition: picotool's RP2350-E10 workaround needs the last page of the
flash (0x10ffff00, where a UF2's "ignore" block points) in unpartitioned space that takes the
`absolute` family, and the WiFi sector is the last one. The shared area, from the top down (`src/boards/presto/board_config.h`):

| Where | What | Who |
|---|---|---|
| last sector | the WiFi network (`MEM_FLASH_WIFI`) | both machines: one network |
| the sector below | the system settings (`MEM_FLASH_SYSTEM`, once the spare sector): which machine boots | the launcher reads it, either machine writes it |
| 204 KB + a header sector below that | the Master's save disc (`MEM_FLASH_SAVE_*`) | the Master, where it always was |
| 256 KB lower | the Model B's save disc | the Model B, in the combined firmware |

The slots are 4 MB because both images are well under 2 MB (Master 0.8 MB, 1.1 MB with
elite-agent built in; Model B 0.9 MB) and the boot ROM's address translation maps a slot to 0x10000000 in 4 MB
windows. The partition table is `src/launcher/partitions.json`; the launcher and
`tools/dual_boot_uf2.py` both take the slots from it.

The machine partitions accept made-up UF2 families (none of them `rp2350-arm-s`), so a plain
machine `.uf2` dropped on the drive never lands in a slot by accident; the plugin partition accepts
none: a plugin isn't a `.uf2` at all, it's installed by the running firmware (below). The unpartitioned space accepts
`absolute` (the combined `.uf2`) and `rp2350-arm-s`, but on the Presto the boot ROM still
ignores a plain `rp2350-arm-s` `.uf2` once the table is there (it stays in BOOTSEL, nothing
written): to go back to one machine, flash the plain build made `absolute` with
`tools/dual_boot_uf2.py --plain`.

## How it boots

1. The boot ROM finds the launcher's block loop at the bottom of the flash. The partition table
   is in it, so the table is loaded, and the IMAGE_DEF beside it is booted: the launcher. The
   partitions are marked `ignored_during_arm_boot` / `_riscv_boot`, so the boot ROM never tries a
   slot on its own.
2. The launcher (`src/launcher/launcher.c`) reads the settings sector straight from the flash: a
   magic number and a machine name. Anything missing or unknown means the Master.
3. It looks the slot up in the partition table (`rom_get_partition_table_info`) and calls
   `rom_chain_image(workarea, size, XIP_BASE + start, slot size)`. The boot ROM checks the image's
   IMAGE_DEF in the slot's first 4 KB, sets the QMI's address translation (ATRANS) so the slot
   appears at 0x10000000, and enters it. The image was linked at 0x10000000 like any other build,
   so the same `.uf2` content works in a slot or on its own.
4. If the chain returns (no valid image there), the launcher tries the other machine's slot, and
   if that fails too it drops into BOOTSEL, ready for a `.uf2`.

Why this way: it is the boot ROM's own mechanism for bootloaders (the SDK's encrypted bootloader
example does the same), so the images need no special link address, no copied vector tables and
no jump code of ours; translation means one image build serves both uses. The alternatives were
worse: A/B partitions (`rom_pick_ab_partition`) pick by version, not by a setting, and are meant
for updates; a reboot with a "flash update" window only nudges that same A/B choice; linking each
machine at its own flash address would make two builds of each and break the single-machine
`.uf2`.

picotool does the partition table: `picotool partition create` turns the JSON into the binary
table, and the SDK's `pico_embed_pt_in_binary` puts it in the launcher's block loop. It doesn't
make one UF2 out of several images at several places (`picotool uf2 convert` takes one ELF or
BIN, one offset, one family), so a short script does that part.

What changes in the images (it's the same in a single-machine build, where it's a no-op):

- **Reading the shared area.** Under translation `XIP_BASE + offset` is the slot, not the flash.
  `mem_flash_ptr` reads through the untranslated, uncached alias
  (`XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE`, 0x1c000000) instead. Erasing and programming were
  always by physical offset (the SDK's `flash_range_*` talk to the flash directly), so writes need
  nothing.
- **Knowing it's launched.** An image is in a slot when ATRANS0 doesn't map 0x10000000 to the
  flash's start (`system_launched()`). Only then does it offer the other machine, and only then
  does the Model B use its own save disc.
- **The launcher's RAM.** The boot guard keeps its record in RAM that survives a reset, near the
  bottom of SRAM. The launcher links its RAM at the top (`memmap_launcher.ld`) so a watchdog reset
  that goes through the launcher keeps the record.

## The shared settings

The WiFi network and the save discs were already in the board's flash regions at fixed physical
offsets near the top of the flash, so in the combined firmware both machines find them where
they always were: nothing moves. Saving the WiFi network on one machine is saving it for both.

The machine choice takes the spare sector (`MEM_FLASH_SYSTEM`):
`{magic "MCHN", version 1, char machine[16]}`. `src/common/system.c` reads and writes it; the
launcher has the same struct from `src/common/system.h`.

The save disc is per machine: its header names the built-in disc it is a copy of, and the two
machines' built-in discs differ, so one shared save disc would be thrown away each time the
other machine saved. The Master keeps the slot it always had (so a save disc made on the
single-machine Master build carries over); the Model B's sits 256 KB lower
(`BOARD_FLASH_SAVE_SLOT`), only when launched. A single-machine Model B build is unchanged.

And a save never lands on another disc's save disc: if the region holds a save disc whose
header names another built-in disc (another build's, or the other machine's in a single-machine
build, where both share one region), the save is refused (`DISC not saved: the save disc is
another disc's`) and the game's writes last only the session. `!restore` gives the region to the
disc in use. (Before this, the first save of a different built-in disc silently replaced the
save disc there.)

Before restarting into the other machine the firmware commits any save still pending
(`drive0_sync`), so a game saved a moment before a switch isn't lost.

## The panel, the API and USB

- `GET /api/status` keeps `"machine"` (the one running) and, in the combined firmware, adds
  `"system": {"machines": ["master", "beeb"], "choice": "master"}`. A build without the launcher
  has no `"system"`.
- `POST /api/machine` with `master` or `beeb` as the body: saves the choice and restarts into it
  once the reply has gone. 404 without the launcher, 400 for an unknown name. An empty body
  replies with the same `"system"` object.
- USB: `!machine` prints `MACHINE master (choice master; master beeb)`; `!machine beeb` prints
  `MACHINE beeb: restarting (asked by USB)` and restarts (a switch from the panel logs the
  client's address there instead). `tools/presto.py machine [beeb|master]` does that, then waits
  for the board to come back and says which machine answered and how long it took.
- The panel: a Machine switch in the header (BBC Master / BBC Model B), shown only when the status
  has `"system"`. Choosing the other asks first, then the page waits for the board to come back
  and rebuilds itself for that machine's keyboard.

The panel is heading for three boxes: **System** at the top (WiFi, sound and lights, the save
disc, the firmware version, the flash/PSRAM/RAM bars), **Emulator** (the machine, its keyboard and
disc), and **Plugin**. So far there's the Machine switch in the header and the
Application section (below).

## The plugin

A plugin is an RP2350 program run alongside the machine: it reads the BBC's RAM and presses its
keys, like the programs built in before (`machine_hook.h`), but it's built on its own, against
`src/common/app_api.h` alone, and installed into the plugin slot by the running firmware. The first
is elite-agent (its `presto/app/build.sh`).

### What a plugin is

A raw image for the slot (`elite-agent.bin`, 404 KB): a header, the code and constants, the
first values of its `.data`, and, if it carries one, a disc. The header (`struct app_header`, 152
bytes, `APP_API_VERSION` 2):

| | |
|---|---|
| `magic`, `api_version`, `header_size` | `"BAPP"`, the API it was built for, the header's size then |
| `machine` | the `enum machine_model` it runs on (0: either) |
| `name`, `version` | shown as they are: "elite-agent", "076af7c" |
| `image_size`, `crc` | the image's bytes from the header on, and their CRC-32 (zlib's, the field taken as 0): what an install and every boot check |
| `data_load`, `data_size`, `bss_size` | its RAM: `.data` copied from `data_load`, `.bss` zeroed, at `ram_base` |
| `stack_size` | the stack it needs (it gets `APP_STACK_SIZE`, 3 KB, of core 0's) |
| `disc`, `disc_hash`, `disc_load`, `disc_size` | the disc it needs in drive 0: a name, a hash (FNV-1a, as `drive.c`'s), and the disc itself if it carries it |
| `boot_image`, `init`, `frame`, `command`, `program_json` | the entry points, any of them NULL |
| `ram_base` | where its RAM starts: `.data` then `.bss`, up to `APP_RAM_TOP` |

The firmware hands `init` a `struct app_api`: `version` and `size` (it only ever grows at the
end), `io` (the `machine_hook_io`: the machine's model, its RAM, keys and analogue inputs), `log`
(a line to USB and the panel), `time_us`, `turbo_get` / `turbo_set`, and the plugin's RAM,
`ram` and `ram_size`.

The rules:

- it is linked at the slot's address, `0x10E00000`, and runs there, from the flash through the
  XIP cache. The machine runs in its own slot through the boot ROM's address translation, which
  maps a slot to `0x10000000` in 4 MB windows; the loader puts the top window (`ATRANS3`) back
  onto the flash's own top 4 MB, its reset value, so the plugin is where it was linked, cached. (The
  untranslated alias at `0x1C000000` is uncached: too slow to run code from.)
- its `.data` and `.bss` are linked at the top of the SRAM's main 512 KB, ending at `APP_RAM_TOP`
  (`0x20080000`), and take only what they need, at most `APP_RAM_MAX` (72 KB): its build links it
  twice, the second time at `APP_RAM_TOP` less its size, and puts that in `ram_base`. The Master's
  heap stops below it (`app.c`'s `_sbrk`: the first allocation reads the slot's header); with no
  plugin, or one refused, the heap has it all. The Model B has none: its JIT wants the SRAM. A
  plugin that needs RAM is refused there.
- it touches no hardware and calls nothing of the firmware's but `app_api`. It brings its own C
  library: newlib's string functions and libm (static), and its own printf and the few other
  things newlib would need its reentrancy structure and malloc for (elite-agent's
  `presto/app/app_libc.c`). Its maths are the Pico SDK's double and float routines, compiled into
  it from the SDK's source as the firmware has them (`--wrap`): they use the core's FPU and its
  double coprocessor (DCP), which the firmware has on for core 0. (libgcc's soft doubles instead:
  elite-agent's frame took six times as long.)
- the entry points are called on core 0 between frames, never at once: `boot_image` at power on,
  before `init` (constant data only); `init` at the first frame (false: it won't run, having
  logged why); `frame` each frame; `command` for a command the firmware doesn't know (`!<cmd>`
  over USB, `POST /api/command`); `program_json` for its panel (`GET /api/program`).
- the versions are checked both ways: the loader refuses a plugin built for another API version,
  another machine, or a disc it hasn't got; the plugin refuses an `app_api` older than it needs.

A program built into the firmware (`-DAPP_SOURCES=...`) is a plugin too: one whose header is
compiled in (the same `APP_HEADER(...)` macro: elite-agent's `presto/build.sh` still builds it
that way). The loader takes it before the slot's, with the same calls. A program with
`machine_hook.h`'s functions (`machine_frame_hook` and the rest) still works: the loader makes it
a header of those.

### Loading it

`app_load` (`src/common/app.c`), at power on, before the boot image and the disc: maps the slot,
finds the plugin (built in, or the slot's) and checks it: the magic, the API version, the header and
image sizes, the CRC over the whole image, the machine, that the plugin guard hasn't turned it off,
the RAM and stack it needs, and its disc. Then it copies `.data` in and zeroes `.bss`, and a plugin
that carries a disc has it booted as the built-in disc (its saved games in the save disc, as
any built-in disc's: the same disc, the same save disc, whichever firmware it came in). Anything
wrong: the machine runs without it, and `/api/app`, `!app` and the panel say why:

```
APP refused: built for the BBC Master 128, this is the BBC Model B
APP refused: built for app API 2, this firmware has 1
APP refused: the image is damaged (CRC 1a2b3c4d, its header says 05b1b2b4)
APP refused: the app slot holds no app (no header: aeb2a0bd)
APP off: it stopped the machine at the last boot ("!app on" to try again)
```

The plugin guard (`boot_guard.c`, in every build): each call into the plugin is marked in RAM that
survives a reset, and for the plugin's first minute the watchdog is armed (on the Master, which has
no watchdog of its own; the Model B's is always on). A reset that comes while the plugin is running
(it hung; or it faulted, and the fault handler's lockup lasted till the watchdog), at two boots
in a row (a RESET pressed the moment the plugin was called looks the same once), turns it off, by
its CRC in the settings sector (`app_off`): the machine runs without it
until another plugin is installed, or `!app on` (`POST /api/app/on`). On the Model B the boot guard's
own report gives way to it.

### Installing it

The running firmware installs a plugin, three ways, one code path (`app_install_*`):

- `POST /api/app` with the image as the body (`curl --data-binary @elite-agent.bin
  http://presto-xxxx.local/api/app`);
- the panel's Plugin section: **Install plugin…**, or a `.bin` dropped on it, with the
  upload's progress, then the result; a plugin for the other machine offers the switch;
- `tools/presto.py app install elite-agent.bin`: over USB (`!app install <size>` then the
  bytes), or over WiFi with `--host <address>`.

It goes into the slot as it arrives, a sector at a time (the machine pauses a moment for each;
the panel's picture stays, its rows fed by DMA). The header's sector goes first, erased, and is
written last, once the whole image has been checked against it: its size, its CRC, the API
version and a machine there is. An upload that's damaged, stops halfway or isn't a plugin leaves no
plugin (`"the last install failed: ..."`), never a broken one. Then the board restarts into it (400
KB: about 7 s over USB, 6 s over WiFi; it answers again 11-13 s after it began). `DELETE /api/app` (`POST /api/app/remove`, `!app
remove`, the panel's **Remove**) erases the header. `GET /api/app` (`!app`, `presto.py app`):

```json
{"state": "running", "why": null, "name": "elite-agent", "version": "076af7c", "machine": "master",
 "from": "slot", "disc": "deadly.ssd", "ram": [72344, 73728], "flash": [413728, 1048576],
 "log": ["PLAYER time avg 120 us max 830 us, cruise turbo 0 frames", ...]}
```

(`state`: none, loaded, running, refused, off, stopped, installing, installed); `/api/status`
has the first six as `"app"`. A machine `.uf2` never touches the slot, and the slot can't be
written by dropping a `.uf2`; `tools/dual_boot_uf2.py --app` puts a plugin in a combined (factory)
`.uf2`.

### The panel

The **Plugin** section: the plugin's state (a dot: running, refused, none), name,
version, the machine it's for and where it came from, and why it isn't running if it isn't; the
Install and Remove buttons; the lines it has logged lately; and its own panel (`program_json`)
under that. It folds away (the page remembers).

### The RAM and the time

The SRAM: the Master gives the plugin the top of the SRAM, as much as its header says it needs, up
to 72 KB; the heap stops below it (`app.c`'s `_sbrk`), and has it all with no plugin or one
refused. Not in a build with a plugin compiled in (`APP_SOURCES`: that plugin's RAM is the
firmware's own). elite-agent built for the slot (without fair mode, which stays in the simulator
and the lab: `presto/app/build.sh --fair` puts it back) takes 55,872 bytes: its `.bss` (49 KB: the
planner's tables, the galaxy) and `.data` (5.5 KB, of which 5 KB is libgcc, libm and the string
functions, run from RAM as the firmware runs its own: `app.ld`). That leaves the Master about 27 KB
free at run time with it (`"free"` in `/api/status`), 83 KB with no plugin. Nothing had to move to
the PSRAM.

The flash: elite-agent's image is 404 KB of the slot's 1 MB: 203 KB of code and constants (61 KB
of them its loading screen) and its 200 KB disc. The Master's own image is 787 KB without it,
1,149 KB with it compiled in.

The time: elite-agent's frame (`PLAYER time`, a minute's average and maximum) run from the slot,
against the same agent compiled into the same firmware, each from power on through the same
course (dock at Lave, Leesti, Diso): 120-376 us a minute (median 300) from the slot, 159-974 us
(median 278) compiled in; the maxima 0.8-1.1 ms both, but for the save and launch frames (13-32
ms, both). The machine at 100.0% either way, core 0 at 46-57%. With libgcc's soft doubles and
newlib's double maths instead of the SDK's it was 760-1,100 us.

## Failure modes and getting back

| What goes wrong | What happens |
|---|---|
| no settings sector, or garbage in it | the launcher boots the Master |
| the chosen slot is empty or its image is bad | the launcher boots the other machine; its status shows `"choice"` differing from `"machine"` |
| both slots bad | the launcher enters BOOTSEL: drop the combined `.uf2` |
| the chosen machine crashes at boot | the Model B's boot guard (the Master has none yet) reports over USB on the next boot and takes `!bootsel` or `!machine <name>` there; otherwise BOOT and RESET, below |
| a plain machine `.uf2` dropped on the drive | ignored: the board stays in BOOTSEL. Its `dual_boot_uf2.py --plain` version (family `absolute`) goes to the start of the flash, over the launcher and its partition table: the flash is a single-machine one again (WiFi, settings and save discs kept). The combined `.uf2` puts the launcher back |
| the plugin is for the other machine, another API version, or damaged | refused, the reason on the panel and in `/api/app`; the machine runs without it |
| the plugin hangs or faults in its first minute | the watchdog resets the board; the second time, the plugin guard turns the plugin off: the machine runs without it (`!app on` to try it again) |
| a plugin upload stops halfway or arrives damaged | refused: the slot is left empty (the header goes last, after the check) |
| no USB, no panel | hold BOOT and tap RESET: BOOTSEL, whatever the flash holds |

The settings and the save discs live outside every image, so flashing any `.uf2` never touches
them (nor the plugin slot, unless the `.uf2` has a plugin in it); `!restore` still forgets the running machine's save disc.

## Building it

```sh
scripts/build-dual-boot.sh [build root]     # -> <root>/presto_dual_boot.uf2
```

builds the launcher, a Master and a Model B (each `-DSOUND=ON -DAMBIENT=ON`, override with
`OS_CMAKE_ARGS`) and joins them with `tools/dual_boot_uf2.py`, which places each image's blocks at its
slot (from `partitions.json`) and writes one UF2 of the `absolute` family. `APP=<app.bin>` puts an
plugin in its slot too (`dual_boot_uf2.py --app`): a factory image with everything in it.

What each `.uf2` does dropped on the drive (BOOTSEL):

| `.uf2` | what it writes |
|---|---|
| the combined one (`presto_dual_boot.uf2`) | the launcher and its partition table, both machines' slots and, made with `APP=`, the plugin's: the rest of the flash (the shared area) is left as it is |
| a machine's own, made `absolute` (`dual_boot_uf2.py --plain`) | that machine at the bottom of the flash, over the launcher: a single-machine flash again |
| a machine's own, as built (`rp2350-arm-s`) | nothing, with the partition table there: ignored |

A plugin is never a `.uf2` of its own: it's installed by the running firmware (above).

The plugin is built in its own project: elite-agent's

```sh
EMU=<rp2350-bbc-micro> PICO_SDK_PATH=<sdk> presto/app/build.sh --disc=<deadly.ssd>   # -> presto/app/build/elite-agent.bin
```
