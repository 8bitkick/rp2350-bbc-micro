/*
 * Inside presto_beebjit: between presto_beebjit.c (the board: the panel,
 * pacing, statistics) and presto_machine.c (what the Presto's front end asks
 * of the machine: presto_machine.h and presto_sd.h, the USB channel and BBC
 * Micro Bot snapshots).
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef PRESTO_BEEBJIT_H
#define PRESTO_BEEBJIT_H

#include <stdint.h>

struct bbc_struct;

/* After the machine is made, before it runs. builtin_disc: the disc built
 * into the firmware (its romfs name), or NULL.
 */
void presto_machine_init(struct bbc_struct* p_bbc, const char* p_builtin_disc);
/* Each frame (the emulated CRT's vsync), in the emulator's thread. */
void presto_machine_frame(void);
/* Often, whenever core 0 waits: the USB channel and the network. In the
 * emulator's thread, between its own work.
 */
void presto_machine_poll(void);

/* presto_beebjit.c: the "!stats" line. */
void presto_beebjit_print_stats(void);
void presto_beebjit_lock(const char* p_arg);   /* "!lock [lead us]": the frame lock's state */
/* With PRESTO_BEEBJIT_PROF: "!prof" (sample again from now) and "!profdump"
 * (the samples, for tools/prof.py).
 */
void presto_prof_restart(void);
void presto_prof_dump(void);

#endif
