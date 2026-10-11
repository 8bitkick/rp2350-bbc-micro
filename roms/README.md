BBC Micro ROMs
==============

The firmware builds in three Acorn ROMs: the MOS 1.20 (`os/os12.rom`), BBC BASIC II
(`general/basic2.rom`) and DFS 0.90 (`general/dfs09.rom`).

These ROMs are NOT covered by the GPL licence of the rest of this repository. They are placed
here in "good faith" in accordance with the GPL licence [0], as other BBC Micro emulators do.
Should these terms be unacceptable to any holder of the ROMs' copyrights, they will be removed,
and users would have to source the ROMs elsewhere.

`scripts/setup.sh --gxr` also fetches Acorn's Graphics Extension ROM to `general/gxr.rom` (it isn't
part of this repository); many BBC Micro Bot snapshots need it.

[0] https://www.gnu.org/licenses/gpl-faq.en.html#GPLInProprietarySystem
