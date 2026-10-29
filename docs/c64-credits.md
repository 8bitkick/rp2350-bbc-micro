# C64 integration credits and provenance

The Presto integration builds on the work of the following authors. Copyright remains with the respective owners; this integration does not claim authorship of the emulator core, ROMs or Elite.

- **FRANK C64:** Mikhail Matveev (rh1tech) and contributors, https://github.com/rh1tech/frank-c64. Original RP2350 C64 port, incorporating Frodo. The supplied archive identifies revision `97a3592a02f015419c2114c13ffbd8af1e226fe3`. Original licence files and source notices are retained in the fetched dependency.
- **Frodo:** Christian Bauer and contributors. Original 6510, VIC, CIA, SID and disk-emulation work retains the copyright and GPL-2.0-or-later notices in the FRANK source files.
- **Commodore ROMs:** the BASIC, KERNAL and character-ROM data distributed by FRANK retain their separate original notices. They are not newly authored by this integration.
- **Elite:** Ian Bell and David Braben; Commodore 64 Elite is copyright © D. Braben and I. Bell 1985. All rights remain with the respective copyright owners, including other credited contributors to the game.
- **Elite preservation and documentation:** Mark Moxon, https://github.com/markmoxon/elite-source-code-commodore-64 and https://elite.bbcelite.com. His documentation/commentary is separately credited to him. The GMA86 PAL disk is downloaded from that project and verified by SHA-256; it is not committed to this repository. See that project's copyright notice; attribution is not a new licence grant.
- **BBC platform:** the existing 8bitkick/rp2350-bbc-micro project and its credited BBC emulator authors/contributors remain credited by the repository's existing documentation and source headers.

`third_party/c64-sources.json` records the configured FRANK fork, original repository/revision, expected adapted-file hashes and Elite source/hash. `third_party/frank-c64-presto.patch` contains only the four adapted core files: platform glue, consistent preferences layout, and two pointer-arithmetic corrections. Remaining Presto-specific display/audio/disk/web glue lives in `src/machines/c64` and the existing shared board/UI files.

A GitHub fork retains the relationship to rh1tech/frank-c64. The original source headers and LICENCE/LICENSE files must remain with redistributed sources. Downloads and generated firmware do not change ownership of their embedded components.
