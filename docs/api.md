# The HTTP API

Everything the web panel does goes through a small HTTP API on the board, so a script can do it
too. The board answers at `http://presto-xxxx.local/` (its own name, from the end of its unique ID: the
screen shows it at power on; or its address: `python3 tools/presto.py net`).
Replies are JSON; an error is `{"error": "why"}` with a 4xx or 5xx code.

Each command is also on the USB serial channel as `!<name> <args>` (`tools/presto.py` sends them),
replying in plain text instead of JSON. The code is `src/common/api/api.c` (the commands) and
`src/boards/presto/presto_net.c` (the HTTP server, and the uploads below).

```sh
curl http://presto-xxxx.local/api/status
curl -X POST -d 1 http://presto-xxxx.local/api/turbo
curl --data-binary @Repton.ssd "http://presto-xxxx.local/api/disc?name=Repton.ssd"
```

## Reading

| Request | Reply |
|---|---|
| `GET /api/status` | everything the panel shows (below) |
| `GET /api/stats` | `{"speed", "core0", "core1", "turbo"}`: the speed against the real machine and each core's load, in % |
| `GET /api/discs` | `{"discs": [{"name", "size"}...], "card": true}`: the disc images on the SD card (`"card": false` without one) |
| `GET /api/net` | `{"fetch": {...}, "wifi": "up"}`: the WiFi link, and a download in progress (`/api/fetch`) |
| `GET /api/app` | the plugin in the plugin slot ([dual-boot.md](dual-boot.md#the-plugin)), with its RAM, flash and its last log lines |
| `GET /api/program` | the plugin's own panel, as it describes it (404 without one) |

`GET /api/status`:

```json
{"machine": "beeb", "board": "presto", "turbo": false, "overlay": false, "speed": 100,
 "core0": 11, "core1": 12, "disc": null, "builtin": "E L I T E", "saves": 1, "free": 75000,
 "mhz": 250, "temp": 34, "leds": 1, "program": false,
 "fetch": {"state": "idle", "got": 0, "length": 0, "error": ""}, "wifi": "up",
 "sound": {"on": false, "vol": 100}, "ambient": {"on": true, "bright": 40, "locks": false},
 "mem": {"flash": [1107448, 16777216], "psram": [7237920, 8388608], "ram": [457496, 532480]},
 "system": {"machines": ["master", "beeb"], "choice": "beeb"},
 "app": {"state": "none", "why": "the app slot is empty", "name": null, "version": null,
         "machine": "none", "from": "slot"}}
```

| Field | |
|---|---|
| `machine` | `"beeb"` (the BBC Model B) or `"master"` (the BBC Master 128) |
| `speed`, `core0`, `core1` | the speed against the real machine, and each core's load, in % |
| `disc` | the name of a disc loaded into the drive, or `null` for the built-in disc |
| `builtin` | the built-in disc's title, from its catalogue |
| `saves` | 1 once games have saved to the built-in disc (kept in the flash, at most once an hour), 0 before, -1 in a build that keeps no save disc |
| `free` | free SRAM, in bytes |
| `mhz`, `temp` | the clock, and the chip's temperature in °C |
| `leds` | the keyboard's lights: 1 CAPS LOCK, 2 SHIFT LOCK, 4 the cassette motor |
| `overlay` | the Model B's load display on the screen (`null`: not in this build) |
| `fetch` | a download by the board (`/api/fetch`): `state` is idle, downloading, loading or error |
| `sound`, `ambient` | only in builds with sound (`-DSOUND=ON`) or the LEDs (`-DAMBIENT=ON`) |
| `mem` | flash, PSRAM and SRAM: `[used, total]` bytes |
| `system` | only in the combined firmware (both machines): which there are, and which boots |
| `app` | the plugin's state: none, loaded, running, refused, off, stopped, installing, installed |

## Doing

Each takes its argument as the request body, plain text. Those that change a setting reply with
the new `/api/status`.

| Request | Body | |
|---|---|---|
| `POST /api/keys` | `d<hex>` / `u<hex>` / `b`, space separated | keys down and up, by their place in the BBC's keyboard matrix (`row << 4 \| column`, hex); `b` is BREAK. Queued, each held for a few frames so the machine sees it |
| `POST /api/boot` | | SHIFT+BREAK: boot the disc in the drive |
| `POST /api/break` | | BREAK |
| `POST /api/turbo` | `1`, `0`, or empty to toggle | run as fast as it can |
| `POST /api/overlay` | `1`, `0`, or empty | the Model B's load display on the screen |
| `POST /api/sound` | `on=1&vol=<0-200>` (either) | the sound, in builds with it |
| `POST /api/ambient` | `on=1&bright=<5-100>&locks=1` (any) | the LEDs on the back, in builds with them |
| `POST /api/machine` | `master` or `beeb` | the machine to boot: saved, and the board restarts into it (`{"restart": true}`). Empty: which there are |
| `POST /api/command` | a command | for the plugin (`{"reply": "..."}`; 404 if it doesn't take it) |

The keys for the examples: RETURN `49`, SPACE `62`, SHIFT `00`, CTRL `01`, Z `61`, X `42`. The
panel's keyboard (`ROWS_BEEB` and `ROWS_MASTER` in `src/common/api/web/index.html`) has them all. Pressing SPACE:

```sh
curl -X POST -d "d62 u62" http://presto-xxxx.local/api/keys
```

## Discs

| Request | Body | |
|---|---|---|
| `POST /api/disc?name=<name>&boot=1&save=0` | the disc image (`.ssd`, or `.dsd` double sided; at most 400 KB) | into the drive and booted (`boot=0`: only into the drive); `save=1` also keeps it on the SD card |
| `POST /api/disc?import=1` | a copy of the built-in disc with games saved on it (a backup, another board's) | kept in the flash as the built-in disc's save disc, and booted (refused if its title isn't the built-in disc's) |
| `POST /api/fetch` | a URL, and `\nsave` to keep it on the card | the board downloads it itself (plain `http://` only) and boots it; watch `fetch` in `/api/status` |
| `POST /api/sd` | a name from `/api/discs` | that disc from the SD card, booted |
| `POST /api/eject` | | back to the built-in disc, booted |
| `POST /api/disc/restore` | | the built-in disc as it came: what games saved to it forgotten, and booted |

A `.zip` isn't taken: the panel unzips it in the browser first.

## The plugin

| Request | Body | |
|---|---|---|
| `POST /api/app` | the plugin's image (`.bin`) | installed into the plugin slot, checked, and the board restarts into it |
| `DELETE /api/app` (or `POST /api/app/remove`) | | the plugin removed |
| `POST /api/app/on` | | a plugin the plugin guard turned off runs again, from the next boot |

[dual-boot.md](dual-boot.md#the-plugin) says what a plugin is, and how it's checked.

## The network

| Request | Body | |
|---|---|---|
| `POST /api/wifi` | `<network name>\n<password>` | `{"confirm": true}`: the Presto's screen asks "Join this network?" (30 seconds); **Join** there saves it and restarts to join it. In WiFi setup (its own access point), `{"joining": true}`: joined at once, saved once it has |
