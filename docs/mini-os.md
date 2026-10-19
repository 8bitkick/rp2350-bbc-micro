# The mini OS: two machines and, later, an app

One firmware with both machines in it, the BBC Master 128 (kc-beeb) and the BBC Model B
(beebjit), and the choice between them on the web panel. A small launcher at the bottom of the
flash picks one at power-on; the machine you chose last is the one that comes up. Later, a third
slot holds an RP2350 application (elite-agent, say) loaded alongside the machine.

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
0xE00000   1 MB  2 "app"         reserved for the app (the later phase): empty for now
0xF00000   1 MB  3 "data"        shared, outside every image: the board's flash regions
                                 (BOARD_FLASH_LAYOUT), the same physical offsets as ever
```

The data partition, from the top down (`src/boards/presto/board_config.h`):

| Where | What | Who |
|---|---|---|
| last sector | the WiFi network (`MEM_FLASH_WIFI`) | both machines: one network |
| the sector below | the system settings (`MEM_FLASH_SYSTEM`, once the spare sector): which machine boots | the launcher reads it, either machine writes it |
| 204 KB + a header sector below that | the Master's save disc (`MEM_FLASH_SAVE_*`) | the Master, where it always was |
| 256 KB lower | the Model B's save disc | the Model B, in the combined firmware |

The slots are 4 MB because both images are well under 2 MB (Master 1.1 MB with elite-agent built
in, Model B 0.8 MB) and the boot ROM's address translation maps a slot to 0x10000000 in 4 MB
windows. The partition table is `src/launcher/partitions.json`; the launcher and
`tools/os_uf2.py` both take the slots from it.

The partitions accept made-up UF2 families (none of them `rp2350-arm-s`), so a plain machine
`.uf2` dropped on the drive never lands in a slot by accident. The unpartitioned space accepts
`absolute` (the combined `.uf2`) and `rp2350-arm-s`.

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
  `MACHINE beeb: restarting` and restarts. `tools/presto.py machine [beeb|master]` does that, then
  waits for the board to come back and says which machine answered and how long it took.
- The panel: a Machine switch in the header (BBC Master / BBC Model B), shown only when the status
  has `"system"`. Choosing the other asks first, then the page waits for the board to come back
  and rebuilds itself for that machine's keyboard.

The panel is heading for three boxes: **System** at the top (WiFi, sound and lights, the save
disc, the firmware version, the flash/PSRAM/RAM bars), **Emulator** (the machine, its keyboard and
disc), and **RP2350 Application** (the app, collapsed when there's none). This phase adds only
the Machine switch to the existing header.

## Later: the app slot

An app is built on its own, linked at the app slot's address (XIP 0x10E00000, run untranslated),
and flashed into partition 2 with its own UF2 family. It starts with a header:

```c
struct app_header {
    uint32_t magic;            // "BAPP"
    uint32_t api_version;      // APP_API_VERSION it was built against
    uint32_t machine;          // enum machine_model it needs (0: either)
    char disc[32];             // the disc it needs in drive 0 ("" none), checked by name and hash
    uint32_t disc_hash;
    uint32_t ram_size;         // how much of the app RAM area it needs (its .data + .bss)
    uint32_t data_lma;         // its initialised data, to copy into the app RAM area
    // entry points, all called on core 0 between frames
    bool (*init)(const struct app_api *api);
    void (*frame)(const struct machine_hook_io *io);
    bool (*command)(const char *cmd, char *reply, uint32_t reply_size);
    const char *(*panel_json)(void);
};
```

The machine checks the header and hands the app an `app_api`: a table of services that extends
today's `machine_hook_io` (the machine's RAM, keys, analogue) with `log(line)` (to USB and the
panel's log), `time_us()`, `turbo_get()` / `turbo_set()`, and the app RAM area's address and size.
The rules:

- an app touches no hardware and calls nothing of the Pico SDK: everything it does goes through
  `app_api`, so the machine's firmware can change underneath it;
- it brings its own libm and string functions (statically linked, `-ffreestanding`), since it
  can't call the firmware's;
- the versions are checked both ways: the machine refuses an app whose `api_version`, machine or
  disc doesn't match, and says why on the panel; the app refuses an `app_api` older than it needs;
- `boot_guard` covers it: a fault or hang in the app's `init` or `frame` marks the app slot bad
  (a flag in the settings sector), and the next boot runs the machine without it.

The RAM is the hard part. The Master with elite-agent built in uses about 498 of the RP2350's 520
KB of SRAM, so a fixed app area needs SRAM freed first. What could move to the 8 MB PSRAM (slower,
and shared with nothing timing-critical on core 1):

- lwIP's buffers and pools and the HTTP server's (about 30 KB): the network is not real-time;
- the app's own big tables (elite-agent's planner and caches, a few tens of KB);
- code copied to RAM (`.data` is 157 KB, much of it `__not_in_flash` code): what isn't on the
  emulation's hot path can go back to running from flash.

What can't: the BBC's RAM (`s_machine`, 73 KB), the framebuffer (62 KB, scanned out by DMA), the
render rows, the sound buffers and the cores' stacks.

## Failure modes and getting back

| What goes wrong | What happens |
|---|---|
| no settings sector, or garbage in it | the launcher boots the Master |
| the chosen slot is empty or its image is bad | the launcher boots the other machine; its status shows `"choice"` differing from `"machine"` |
| both slots bad | the launcher enters BOOTSEL: drop the combined `.uf2` |
| the chosen machine crashes at boot | the boot guard reports over USB on the next boot and takes `!bootsel` or `!machine <name>` there |
| a plain machine `.uf2` dropped on the drive | it goes to the start of the flash, over the launcher and its partition table: the flash is a single-machine one again (WiFi and save discs kept). The combined `.uf2` puts the launcher back |
| no USB, no panel | hold BOOT and tap RESET: BOOTSEL, whatever the flash holds |

The settings and the save discs live outside every image, so flashing either kind of `.uf2`
never touches them; `!restore` still forgets the running machine's save disc.

## Building it

```sh
scripts/build-os.sh [build root]     # -> <root>/presto_os.uf2
```

builds the launcher, a Master and a Model B (each `-DSOUND=ON -DAMBIENT=ON`, override with
`OS_CMAKE_ARGS`) and joins them with `tools/os_uf2.py`, which places each image's blocks at its
slot (from `partitions.json`) and writes one UF2 of the `absolute` family.
