# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

AGENTS.md (imported above) is the main guide: build, flash, measure, layout, history and traps.
What follows only adds to it.

## Running the harness

The scripts are POSIX `sh`: run them from the Bash tool, not PowerShell. The `a32` target needs an
Arm64 host with AArch32 (a Raspberry Pi 5); on other hosts use `qemu` (`build.sh qemu`, then
`tests.sh qemu`), or `native` (host compiler, for profiling only, not the M33 code paths).

- `tests.sh` runs a fixed set of scenarios (boot to BASIC, BASIC over RS423 in MODE 2, teletext,
  NMOS decimal mode, and Elite if `discs/elite-disc-flicker-free-sth.ssd` exists, from
  `scripts/fetch-elite.sh`). There is no per-test selector: to run one scenario, run the harness
  binary directly with that scenario's arguments from `tests.sh`, e.g.
  `build-a32/harness -cycles 6000000 -mode7 -frame-crc -ppm out.ppm` (add `-jit` for the JIT).
- JIT runs are checked against the `-nosync` golden entries, because the JIT turns off beebjit's
  video memory sync. `NOSYNC=1 tests.sh` runs the interpreter the same way.
- `GOLDEN=update tests.sh` (with `NOSYNC=1` for the `-nosync` entries) rewrites `golden.txt`. Only
  do this when a pixel change is intended; a performance change must match the existing goldens.
- `PACKED4=0` at build time renders ARGB instead of the Presto's packed 4-bit codes; `WIRE=1`
  routes render calls through the render wire (drained on the same core).
- Other harness options worth knowing (see `harness_main.c`): `-disc`, `-autoboot`, `-type`,
  `-keys`, `-profile`, `-frame-stats`, `-frame-trace`, `-no-render`, `-opt <beebjit options>`.
