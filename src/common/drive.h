#ifndef DRIVE_H
#define DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The disc in drive 0, for any machine: the disc built into the firmware, or one from the SD card
// or the web; and the built-in disc's saved games, kept in flash.
//
// The built-in disc (the golden image, in the firmware) stays as it is; a full copy of it, the
// save disc, lives in flash (mem.h: MEM_FLASH_SAVE_*). At boot the save disc goes into the drive
// if it's a copy of this golden disc, else the golden one. A game's writes go to the machine's
// drive; a second after the drive goes quiet the sectors written are read back and only the
// flash sectors that changed are rewritten (an Elite save: one or two). drive0_restore forgets
// the save disc. Discs from the card or the web keep their writes for the session only. A save
// disc that's a copy of another disc (another build's, another machine's) is never written over:
// that disc's saves stay, and this one's last the session, until drive0_restore.
//
// The machine does the drive itself (machine_if.h: machine_drive_load, _writes, _sector,
// _clean); this decides what goes in it. All on core 0, between frames.

// the size the disc images arriving (from the card, the web) may be: an 80 track double sided .dsd
#define DRIVE_IMAGE_MAX (80 * 10 * 256 * 2)

// at boot, before the machine runs: the built-in disc (NULL: none) into the drive, the save disc
// if there's one for it; staging: DRIVE_IMAGE_MAX bytes where images arrive (drive0_staging)
void drive0_init(const uint8_t *golden, uint32_t golden_size, uint8_t *staging);
uint8_t *drive0_staging(void);
// the image at the start of the staging area into drive 0 as <name> (.ssd/.dsd says which kind,
// and so does the disc's catalogue), and a reset boots it; false if it's no size for a disc
bool drive0_insert(const char *name, uint32_t size);
// the name of the disc in drive 0, or NULL for the built-in one
const char *drive0_name(void);
// the built-in disc's title, from its catalogue ("" if it has none, or there's no built-in disc)
const char *drive0_builtin_title(void);
// back to the built-in disc (its save disc, if any), booted
void drive0_eject(void);
// the built-in disc as it came (what games saved to it forgotten), booted
void drive0_restore(void);
// the save disc's saves so far (0: none yet), or -1 with no built-in disc to save to
int drive0_saves(void);
// what's in the drive: "ORIGINAL", "SAVE DISC (n)" or a disc's name (a loading screen)
const char *drive0_source(void);
// each frame: a second after the drive's last write to the built-in disc, keep what changed
void drive0_frame(void);
// keep what's been written to the built-in disc now, without waiting (before a restart)
void drive0_sync(void);

#ifdef __cplusplus
}
#endif

#endif
