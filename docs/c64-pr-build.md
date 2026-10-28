# Building the r11 C64 integration against the updated BBC tree

This is an integration of experimental r11 behaviour with the supplied newer BBC source archive, not a byte-identical reconstruction of the earlier r11 UF2. Newer streamed SD listings, allocation handling and Master stack/app changes are retained.

## Dependencies

1. Use the shared public fork https://github.com/andriodious/frank-c64. Other builders do not need to create their own FRANK fork. Original upstream is https://github.com/rh1tech/frank-c64; the BBC repository carries the four-file Presto adaptation patch.
2. Keep `frank_repository` and `frank_revision` in `third_party/c64-sources.json` unchanged. The pinned revision has been downloaded from the shared fork and verified. Updates to the fork's main branch do not change builds. Only a deliberate configuration/patch/checksum update selects a different source revision.
3. Run `python3 scripts/fetch-c64.py`, or configure/build C64: its CMake setup invokes that script before listing sources. The script downloads the fork at the pinned revision, applies the patch in a temporary directory, verifies every required adapted core file, then installs it into ignored `deps/frank-c64`. It downloads and verifies the Elite PAL image into ignored `discs/`.
4. Existing downloaded files are reused only after verification. Changed local dependency files or a different disk cause an error rather than an overwrite. Move them aside if you intentionally want a fresh fetch.

The dependency source and disk URLs/credits are in `docs/c64-credits.md`. Neither the game disk nor a second copy of FRANK source belongs in the BBC commit.

## Build on Debian / Raspberry Pi

From a clean checkout, using the existing shared FRANK fork:

```sh
scripts/setup.sh --no-build --roms
. deps/env.sh
python3 scripts/fetch-c64.py
NINJA_JOBS=2 scripts/build-dual-boot.sh build-r11
```

Output: `build-r11/presto_dual_boot.uf2`, containing launcher, Master, Model B and C64. Use this combined image for the web machine selector. The historical dual-boot filename is retained.

`WEBDIAG` defaults OFF. For a new diagnostic build:

```sh
OS_CMAKE_ARGS='-DSOUND=ON -DAMBIENT=ON -DWEBDIAG=ON' NINJA_JOBS=2 scripts/build-dual-boot.sh build-r11-diag
```

Existing build directories retain their CMake options; reconfigure their individual master/beeb/c64 directories to change them. See `docs/c64-r11.md`.

Offline validation can use `C64_FRANK_ARCHIVE=/absolute/path/frank-c64-main.zip` and `C64_ELITE_FILE=/absolute/path/elite-commodore-64-gma86-pal.d64` when running the fetch script. Both still undergo checksum validation. These are optional testing inputs, not needed by a normal network build.

## Behaviour and limitations

Master is the power-on default. C64 selection automatically submits LOAD and RUN for the bundled disk. Choose **N** for Elite's fastloader: this integration uses DOS-level IEC access and not full 1541 CPU emulation. D64/D81 loading, C64 keyboard, Turbo frame-conversion skipping, ambient lighting and optional web diagnostics are included. C64 writes remain session-only.

Piezo sound remains faint; gain can clip and audio underruns occur when emulation runs below PAL speed. This is an experimental integration, not a claim that these issues are solved. User hardware results from the earlier r11 development are not hardware validation of this newer merged tree.
