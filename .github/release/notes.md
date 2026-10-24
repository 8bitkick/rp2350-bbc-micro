The BBC Master 128 and the BBC Model B in one firmware for the [Pimoroni Presto](https://shop.pimoroni.com/products/presto). It boots the Master into Mark Moxon's flicker-free Elite, and [elite-agent v0.4](https://github.com/8bitkick/elite-agent/releases/tag/v0.4) starts playing it a few seconds later.

**Install:** plug the Presto in over USB-C, hold **BOOT**, tap **RESET**, and drag `rp2350-bbc-micro-presto-v0.5.uf2` onto the **RP2350** drive that appears. It restarts by itself.

**New in v0.5: WiFi setup on the Presto, with a phone.** No computer or config file needed:

1. **Set up WiFi:** the Presto shows a QR code. Scan it with your phone to join the Presto's own setup network.
2. **Now finish on your phone:** the screen moves on by itself as your phone joins. Pick your WiFi and give its password on the page that opens.
3. **Joining**, with progress. If it can't (a wrong password, say), it says why in red, and you can try again.
4. **Connected!** A QR code for the web panel, its `presto-xxxx.local` name and IP.

At each power-on the Presto joins the same way (**Joining**, then **Connected!** for 20 seconds: **SKIP**, or **WAIT** to keep it up) before the machine starts. Hold a finger in the top-left corner for 3 seconds to see the address again. **Skip** in setup carries on without WiFi. Setup runs in memory the emulator isn't using yet, so it costs the machine nothing.

- **BBC Master 128** (Kieran Connell's emulator) with Mark Moxon's flicker-free Elite (SNG47), the default.
- **BBC Model B** (beebjit, with a JIT for the RP2350) with Mark Moxon's flicker-free Elite (disc version). Switch machines on the web panel.
- **elite-agent v0.4** in the plugin slot: smoother, faster docking (efficient mode reaches Elite in about 24 game hours, a new record) and half the RAM of v0.3; curious mode, playing on the Master as Commander JAMESON, already Deadly, and carrying on from its last save after a power-off. Remove or replace it from the web panel's Plugin section. This firmware takes plugins for app API 2 only: elite-agent v0.2 and older are refused.
- **Stack guard:** a plugin that overflows core 0's stack can no longer freeze the board. It faults, the Presto restarts by itself, and the plugin is turned off with the reason on the web panel.
- **Web panel** at http://presto-xxxx.local/ (the name's on the Presto's screen) once the Presto is on your WiFi: the machine's keyboard, discs from the SD card or the Stairway to Hell archive, the plugin's panel (folded until you open it).
- Games keep their saves in flash through a power-off (written at most once an hour, for the flash's sake: a power cut loses at most the last hour). Flashing this release replaces the Master's saves with the Deadly JAMESON; the Model B's saves are kept.

Elite is © Ian Bell and David Braben. This release includes [Mark Moxon's flicker-free builds](https://elite.bbcelite.com/), unmodified, from his [Master](https://github.com/markmoxon/elite-source-code-bbc-master) and [disc](https://github.com/markmoxon/elite-source-code-bbc-micro-disc) source projects. The repository itself doesn't contain them: `setup.sh` downloads them when you build.
