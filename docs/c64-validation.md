# Validation — merged experimental r11 PR fileset

Checked on 28 October 2026 against the supplied `rp2350-bbc-micro-main 2.zip` baseline. This is r11 functionality merged with newer BBC changes, not the original r11 binary.

## Completed

- Launcher, Master, Model B and C64 compile and link with Arm GNU 14.2.Rel1 (GCC 14.2.1), Pico SDK 2.2.0, Release, sound and ambient LEDs enabled, web diagnostics disabled by default.
- The combined build script produces `presto_dual_boot.uf2`. All 10,146 payload blocks match their relocated input images, with no overlaps. Master, Model B and C64 fit their 4 MiB slots (approximately 795, 875 and 859 KiB respectively).
- Packer tests cover slot placement, missing C64 input, overflow and overlap rejection.
- AddressSanitizer/UndefinedBehaviorSanitizer host checks pass for disk bounds, preferences layout, app metadata flash access, HTTP connection lifetime and the merged streamed directory listing. The listing test exercises 75 entries across multiple output buffers.
- Audio host tests and launcher watchdog-fallback checks pass.
- The native emulator harness passes 1,000 BASIC frames and 10,000 Elite frames under address/undefined-behaviour sanitizers. Leak detection is disabled for that harness; this is not a leak test or a Presto simulation.
- Headless browser checks pass for generated WEBDIAG OFF and ON pages using a simulated API.
- FRANK extraction, four-file patch application, adapted-source checksums, dependency reuse and protection of modified dependency files pass inside a Git checkout using the supplied original archive.
- The Elite network download matches the SHA-256 of the user's tested GMA86 PAL disk.
- The generated BBC patch is applied to a clean copy of the supplied archive and checked against every packaged source file. Git whitespace checks pass.

## Integration adjustment found by the build

The newer BBC tree adds `machine_install_screen`. C64 now provides that callback and pauses emulation during app installation while keeping the last RGB565 picture and servicing the frontend/watchdog. Abort releases the pause. This path still needs device testing.

## Memory and firmware identity

C64 static SRAM ends at `0x20068860`; the ordinary heap limit is `0x2007c000`, leaving 79,776 bytes before runtime allocations. A separate 16 KiB core-0 stack is reserved. These are link-time figures, not runtime free-memory or leak measurements.

Local combined verification build SHA-256:
`186170aa3dbd2669d0f84436de808206fd56b7675c8ae9b58f6b3a9a5c5621f1`

Firmware is not included in the PR fileset. Builds may differ with other toolchains or downloaded BBC ROM inputs. Compiler warnings remain, including imported-core signedness/unused-code warnings, volatile increment deprecation and newlib's unsupported unlink warning; there are no final compile/link errors.

## Not yet verified

- No hardware test of this newly merged tree, overnight run, timing/audio measurement, or BBC frame-exact harness run was performed here. Earlier user observations belong to earlier firmware.
- The pinned archive has now been downloaded successfully from the public `andriodious/frank-c64` fork and passed patch application, source checksums, reuse and modified-file protection checks. The earlier firmware build used the uploaded archive with the same verified source contents; no firmware code changed for this follow-up. Future builds require network access unless the documented offline inputs are supplied.
- Full 1541 CPU/fastloader emulation is absent: answer **N** to Elite's fastloader prompt. Disk writes remain session-only.
- Piezo sound is faint and can clip. Audio underruns and below-real-time performance remain unresolved. Turbo and LEDs need testing on the merged firmware.
- Keep the PR in draft until the merged firmware is tested on Presto and GitHub checks pass. Follow the acceptance checklist in START-HERE.md (package) or c64-pr-build.md (repository).
