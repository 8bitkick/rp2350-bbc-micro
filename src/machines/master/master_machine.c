// What the web panel, the SD card and the USB channel ask of the machine
// (machine_if.h, presto_sd.h), for Kieran Connell's Master 128. All of
// it runs on core 0, between machine runs (main.c), so nothing here races
// the emulation.
//
// Drive 0 (machine_if.h; src/common/drive.c decides what goes in it, and keeps
// the built-in disc's saved games in flash): an image is expanded into drive
// 0's track data (PSRAM: two sides' worth, for .dsd images) as it goes in, so
// the disc RAM where images arrive is free again straight away. Writes go to
// the track data; the core marks the tracks written (disc.h), and
// disc_read_sector reads a sector back out of them for the save disc.
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/adc.h"

#include "machine_if.h"
#include "frontend.h"
#include "presto_psram.h"
#include "presto_sd.h"
#include "master_main.h"
#include "drive.h"

#include "machine.h"
#include "disc.h"

static struct machine *s_m;
static struct disc s_disc;
static uint32_t *s_pulses;            // 2 sides of track data
static void reset(bool boot);

// ---- Set-up ----

void presto_disc_progress(void (*fn)(int done, int total)) { s_disc.progress = fn; }

void presto_machine_init(struct machine *m, uint32_t *pulses, uint8_t *disc_ram,
                         const uint8_t *builtin, uint32_t builtin_size)
{
    s_m = m;
    s_pulses = pulses;
    drive0_init(builtin, builtin_size, disc_ram);   // the built-in disc (or its save disc) in
    machine_set_disc(m, 0, &s_disc);
    frontend_init();
}

// ---- Resets ----

// A power-on reset (the machine's cycle count starts again: main.c
// re-paces), with SHIFT held for a second and a half if it should boot the
// disc. Keys held or queued are let go.
static void reset(bool boot)
{
    struct machine *m = s_m;
    memset(m->keys, 0, sizeof m->keys);
    m->key_head = m->key_count = 0;
    machine_reset(m);
    if (boot) {
        machine_autoboot(m);
    }
    main_machine_was_reset();
}

static bool s_boot_next;

// BREAK: a power-on reset, which boots the disc if SHIFT is held (from the
// keyboard, or machine_boot_shift). The core has no warm reset, so a BASIC
// program in memory does not survive BREAK as on a real Master.
void machine_break(void)
{
    bool shift = (s_m->keys[0] & 1) != 0;   // SHIFT: row 0, column 0
    reset(s_boot_next || shift);
    s_boot_next = false;
}

// The next machine_break boots (SHIFT+BREAK).
void machine_boot_shift(void)
{
    s_boot_next = true;
}

// ---- Keys ----

void machine_key(int vkey, bool down)
{
    struct machine *m = s_m;
    machine_queue_key(m, m->synced + 2000, (vkey >> 4) & 7, vkey & 15, down);
}

// ---- The keyboard's LEDs ----

unsigned machine_leds(void)
{
    // CAPS LOCK and SHIFT LOCK are lit when their IC32 latch bits (6, 7) are low; no cassette motor
    return ((s_m->ic32 & 0x40) ? 0 : 1) | ((s_m->ic32 & 0x80) ? 0 : 2);
}

// ---- Drive 0 ----

void machine_drive_load(const uint8_t *image, uint32_t size, bool dsd, bool boot)
{
    disc_init(&s_disc, s_pulses, dsd ? 2 : 1, 1);
    if (image && size) disc_load_ssd(&s_disc, image, size);
    if (boot) reset(true);
}

uint32_t machine_drive_writes(void)
{
    return s_disc.writes;
}

bool machine_drive_sector(int track, int sector, uint8_t *out)
{
    if (track < 0 || track >= DISC_TRACKS || !s_disc.dirty[0][track]) return false;
    return disc_read_sector(&s_disc, 0, track, sector, out) != 0;
}

void machine_drive_clean(void)
{
    memset(s_disc.dirty, 0, sizeof s_disc.dirty);
}

// ---- Each frame and in between ----

void presto_machine_frame(void)
{
    frontend_frame();
}

void presto_machine_poll(void)
{
    frontend_poll();
}
