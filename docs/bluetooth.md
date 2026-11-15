# Optional Classic Bluetooth input on Presto

Build with `-DBLUETOOTH=ON` for an original SteelSeries Nimbus and a Classic HID
keyboard, independently connected to either BBC Model B or BBC Master. The default
build remains Bluetooth-free. BLE-only keyboards and Nimbus+ are not supported.
The transport and mappings were ported from [andriodious/rp2350-8-bit-retro v0.6.0](https://github.com/andriodious/rp2350-8-bit-retro/tree/v0.6.0);
this upstream adaptation still needs its own hardware validation.

## Build

```sh
scripts/setup.sh --no-build --roms
. deps/env.sh
OS_CMAKE_ARGS="-DSOUND=ON -DAMBIENT=ON -DBLUETOOTH=ON" \
  scripts/build-dual-boot.sh build-bluetooth
```

Output: `build-bluetooth/presto_dual_boot.uf2`. Single-machine CMake builds also
accept `-DBLUETOOTH=ON`. `BT_KEYBOARD_CLASSIC` and `BT_KEYBOARD_DISCOVERY` default
on when Bluetooth is enabled; either can be disabled for a controller-only build.
The internal `NIMBUS_TEST` define is retained from the port but is not the public
build switch. No plugin or save-disc overwrite is added by this command. Existing
plugin/touch-screen features, hostname (`beeb.local`) and launcher layout remain.

## Connect and configure

Configure Wi-Fi first using the existing instructions, then open `http://beeb.local/`
or the device's IP address. Disconnect input devices from other hosts before pairing.
The new web panes are hidden in builds without the corresponding support.

- **Nimbus controller:** put the original Nimbus into pairing mode, Scan for
  controllers, select it and Connect. Under Game controls, choose Default mappings
  and Apply controls. Joystick mode provides BBC analogue channels and Fire; select
  joystick controls in the game. Keyboard mode maps directions/buttons to BBC keys.
  Dead zone, stick selection, inversion and button assignments are configurable.
- **Bluetooth keyboard:** put it into pairing mode, Scan for keyboards, select its
  name/address and Connect. Follow the displayed PIN instruction if prompted. Stop
  scan ends discovery early. A sleeping keyboard may need a keypress to reconnect.
- Both devices reconnect independently after machine switches and power cycles.
  Reconnect resumes after an explicit Disconnect. Forget removes that device's bond.
  Each machine has its own controller profile and keyboard mappings.

Under keyboard **Key mappings**, Add mapping, press a physical Bluetooth key, then
click its destination on the on-screen BBC keyboard. Release the source key. Capture
suppresses game input, can be cancelled and expires after 60 seconds. Up to 32 mappings
per machine; Delete mapping restores normal behaviour. Web/USB, Nimbus and keyboard
key ownership is combined so one source releasing a key does not release another's.

## Storage and timing

Two 4 KiB records at flash offsets `0xf00000` and `0xf01000` store bonds/profiles.
They start after the existing app partition and do not overlap either save-disc slot,
Wi-Fi or machine selection. Writes use the existing safe flash writer, deferred from
Bluetooth callbacks to core-0 frame processing. The SDK flash-TLV backend is not used.
The v2 record preserves the downstream format, including reserved machine/profile
fields; this port only selects BBC Master and Model B fields and does not change
sound/LED preferences. Disabling Bluetooth leaves the records untouched.

Outgoing reconnect attempts wait for Wi-Fi/DHCP and back off when a remembered
controller is absent. Machine switching closes links and allows up to 1.5 seconds
for radio shutdown before the existing watchdog restart. Physical reset handling
is unchanged. With no configured Wi-Fi, upstream does not initialise the shared
radio, so Bluetooth also remains unavailable.

## Validation

```sh
PICO_SDK_PATH=/path/to/pico-sdk sh tests/bluetooth/run.sh
python3 tests/bluetooth/firmware_test.py build-bluetooth arm-none-eabi-nm
python3 tests/bluetooth/keyboard_map_web_fixture.py /tmp/keyboard-web.html
```

Open the last fixture in a browser; `body.dataset.testResult` becomes `PASS` after
mapping/capture/delete/cancel checks on both BBC keyboards. Radio and HTTP responses
are mocked. Host tests cover real HID parsing, report bounds/rollover, discovery,
reconnect gating, key ownership, mappings and redundant-record interrupted writes.

Before merging, test both machines on Presto with keyboard and Nimbus individually
and together, scan/Stop scan, typing, analogue/D-pad/Fire, saved maps, switches and
power cycles. Check Wi-Fi/web/audio, save discs, and the existing plugin/touch viewer.
Compilation and host tests do not replace those hardware tests.

BTstack is supplied by the Pico SDK; its licence and upstream source notices apply.
See the retained notice in `docs/licenses/BTstack.txt`.

Port validation on 14 November 2026: both machine payloads built with Bluetooth
on and off; sanitizer tests and mocked browser mapping checks passed. The combined
Bluetooth UF2 matched all 7,347 payload blocks. Static headroom was 102,012 bytes
for Master and 147,656 bytes for Model B (not live free RAM). No upstream-port
hardware results are claimed yet.
