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
No CPU emulation, render pipeline, JIT, save-disc layout or runtime BOOT-button
behaviour is changed.

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
