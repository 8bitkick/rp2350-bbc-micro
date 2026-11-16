# Presto startup status screen

On power-on, the BBC Model B and Master show the supplied owl graphic, source build
ID, build date/time, DHCP progress and IP address, joystick and keyboard status,
and **WEB: BEEB.LOCAL**. Open `http://beeb.local/` in a browser on the same network
once the emulator starts; use the displayed IP address if mDNS is unavailable.
This is the local web interface, not an Internet service.

The banner remains for at least three seconds. With saved Wi-Fi credentials it
waits up to 30 seconds for an address, or two seconds after the address appears.
No network, a failed join or an allocation failure prevents the emulator from
starting. Watchdog restarts (including web machine switches) skip the banner.

Bluetooth is optional and supplied by the separate Classic Bluetooth PR. Without
that build option both input lines say **DISABLED IN BUILD**. With it, they show
live connection progress. The banner polls Bluetooth without writing pairing
flash: core 1 may not yet be ready for flash lockout. Connection retries continue
normally after the emulator starts.

The two temporary packed-pixel images reuse the idle disc-upload staging area.
HTTP uploads start only after the banner releases it. Status updates draw into
the inactive buffer, wait for two display frames before reusing the previous
one, and restore the original display table before releasing the pointer tables.
The power-on banner does not change CPU emulation, the render pipeline, JIT or
save-disc layout. Runtime BOOT controls are described below.

The owl in `assets/boot-owl.png` is user-supplied artwork. Its aspect ratio is
preserved. `tools/embed_boot_logo.py` regenerates the checked-in header with
Pillow; normal firmware builds do not need Pillow.

Validation:

- `python3 tests/startup/test_banner.py` exercises the actual banner with simulated
  panel/network services, with and without Bluetooth hooks, under address and
  undefined-behaviour sanitizers: reset skip, offline timeout, DHCP, row end
  markers and original display restoration.
- Both Model B and Master firmware must build, alone and with the Bluetooth PR.
- Hardware acceptance: cold boot with/without Wi-Fi, readable stable banner,
  correct IP, browser access after boot, keyboard/Nimbus reconnection, normal
  gameplay and web machine switching. Host tests cannot verify panel tearing.

## BOOT button: SD chooser and status

With `BOOT_FILE_SELECTOR=ON` (default), press the Presto's BOOT button while
running to pause emulation and mute sound temporarily. Wi-Fi, the web interface
and Bluetooth continue to work.

If an enabled joystick or a Classic Bluetooth keyboard is connected and the SD
card root contains usable titles, the first press opens the file chooser:

- Joystick Up/Down or keyboard Up/Down selects a title, with hold-to-repeat.
- Nimbus A, mapped Fire, Return or keypad Enter loads it and resumes emulation.
- A second BOOT press shows status; a third returns to the current emulator.
- Without a connected input device or usable titles, BOOT opens status directly;
  the next press returns. A failed load shows an error and leaves BOOT available.

The keyboard's cursor/Enter actions use its physical HID keys, independent of
saved emulator mappings. Menu input is captured, and held controls must be
released before they reach the emulator again. Keyboard and joystick can be used
together. The keyboard alone is sufficient; a joystick need not be configured.

Only files accepted by the existing BBC SD loader are listed, sorted by name,
up to 240 nonempty files no larger than its image limit. Subdirectories are not
browsed. Titles use the existing loader and machine-safe reset mechanism.

The menu reserves 256 KiB at the top of PSRAM, outside both machine allocators,
and allocates approximately 50 KiB of SRAM on first use for immutable display
rows. Allocation failure leaves the emulator running. Display table changes are
published during vertical blank; emulation resumes with its pacing clock
adjusted for the pause. The original sound settings are retained.

`-DBOOT_FILE_SELECTOR=OFF` disables runtime BOOT handling and its PSRAM reservation;
the power-on banner remains available. With Bluetooth disabled the runtime
status screen still works, but there is no keyboard/joystick file selection.
