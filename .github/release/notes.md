The BBC Master 128 and the BBC Model B in one firmware for the [Pimoroni Presto](https://shop.pimoroni.com/products/presto). It boots the Master into Mark Moxon's flicker-free Elite, and [elite-agent v0.5](https://github.com/8bitkick/elite-agent/releases/tag/v0.5) starts playing it a few seconds later.

**Install:** plug the Presto in over USB-C, hold **BOOT**, tap **RESET**, and drag `rp2350-bbc-micro-presto-v0.5.uf2` onto the **RP2350** drive that appears. It restarts by itself.

- **BBC Master 128** (Kieran Connell's emulator) with Mark Moxon's flicker-free Elite (SNG47), the default.
- **BBC Model B** (beebjit, with a JIT for the RP2350) with Mark Moxon's flicker-free Elite (disc version). Switch machines on the web panel.
- **elite-agent v0.5** in the plugin slot, playing curious mode on the Master as Commander JAMESON. **The modes' AI pilots now fly on the Presto**: tiny neural networks, under 30 KB each, written directly by an LLM with no gradient training ([how](https://8bitkick.cc/elite-agent/blog.html)).
- **Tap anywhere on the panel** to swap between the game and a live view of the network flying, or the agent's log when none is. The machine runs on underneath either way.
- **AI viewer** on the web panel: the web demo's diagram, live from the board (needs internet access in your browser, to load its code).
- **Fair mode** isn't in this plugin: build it with `presto/app/build.sh --fair`. It doesn't include the tap viewer, and needs this firmware or later, for its tables in the PSRAM.
- **Known issues:** the docking run can occasionally hit station traffic, and one frame soon after boot can run long, without making the emulator late.
- **Stack guard:** a plugin that overflows core 0's stack can no longer freeze the board. It faults, the Presto restarts by itself, and the plugin is turned off with the reason on the web panel.
- **Web panel** at http://beeb.local/ once the Presto is on your WiFi (`tools/presto.py wifi secrets.py`, see the README): the machine's keyboard, discs from the SD card or the Stairway to Hell archive, the plugin's panel.
- Games keep their saves in flash through a power-off (written at most once an hour, for the flash's sake: a power cut loses at most the last hour). Flashing this release replaces the Master's saves with the Deadly JAMESON; the Model B's saves are kept.

Elite is © Ian Bell and David Braben. This release includes [Mark Moxon's flicker-free builds](https://elite.bbcelite.com/), unmodified, from his [Master](https://github.com/markmoxon/elite-source-code-bbc-master) and [disc](https://github.com/markmoxon/elite-source-code-bbc-micro-disc) source projects. The repository itself doesn't contain them: `setup.sh` downloads them when you build.
