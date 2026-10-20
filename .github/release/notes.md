The BBC Master 128 and the BBC Model B in one firmware for the [Pimoroni Presto](https://shop.pimoroni.com/products/presto). It boots the Master into Mark Moxon's flicker-free Elite, and [elite-agent v0.2](https://github.com/8bitkick/elite-agent/releases/tag/v0.2) starts playing it a few seconds later.

**Install:** plug the Presto in over USB-C, hold **BOOT**, tap **RESET**, and drag `rp2350-bbc-micro-presto-v0.2.uf2` onto the **RP2350** drive that appears. It restarts by itself.

- **BBC Master 128** (Kieran Connell's emulator) with Mark Moxon's flicker-free Elite (SNG47), the default.
- **BBC Model B** (beebjit, with a JIT for the RP2350) with Mark Moxon's flicker-free Elite (disc version). Switch machines on the web panel.
- **elite-agent v0.2** in the plugin slot: curious mode, playing on the Master as Commander JAMESON, already Deadly, and carrying on from its last save after a power-off. Remove or replace it from the web panel's Plugin section.
- **Web panel** at http://beeb.local/ once the Presto is on your WiFi (`tools/presto.py wifi secrets.py`, see the README): the machine's keyboard, discs from the SD card or the Stairway to Hell archive, the plugin's panel.
- Games keep their saves in flash through a power-off. Flashing this release replaces the Master's saves with the Deadly JAMESON; the Model B's saves are kept.

Elite is © Ian Bell and David Braben. This release includes [Mark Moxon's flicker-free builds](https://elite.bbcelite.com/), unmodified, from his [Master](https://github.com/markmoxon/elite-source-code-bbc-master) and [disc](https://github.com/markmoxon/elite-source-code-bbc-micro-disc) source projects. The repository itself doesn't contain them: `setup.sh` downloads them when you build.
