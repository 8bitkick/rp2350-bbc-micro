// What the web panel, the SD card and the USB channel ask of the machine
// (presto_machine.h, presto_sd.h), for Kieran Connell's Master 128. All of
// it runs on core 0, between machine runs (main.c), so nothing here races
// the emulation.
//
// Discs: drive 0 holds either the disc built into the firmware or one from
// the card or the web. An image arrives at the start of the disc RAM (in
// PSRAM, PRESTO_DISC_MAX bytes) and is expanded into drive 0's track data
// (also PSRAM: two sides' worth, for .dsd images) as it goes in, so the disc
// RAM is free again straight away. Inserting one resets the machine with
// SHIFT held, so it boots.
//
// The built-in disc keeps what games save to it: the golden image in the
// firmware stays as it is, and a save disc (a whole copy, 80 tracks) lives in
// flash. At boot the save disc goes into the drive if it's a copy of this
// golden disc, else the golden one. Writes go to the track data; a second
// after the drive goes quiet the tracks written are read back and the flash
// blocks that changed are rewritten (an Elite save: one or two). "!disc
// restore" (or POST /api/disc/restore) puts the golden disc back. Discs from
// the card or the web keep their writes for the session only.
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/adc.h"

#include "presto_machine.h"
#include "presto_psram.h"
#include "presto_sd.h"
#include "presto_net.h"
#include "presto_main.h"

#include "machine.h"
#include "disc.h"

static struct machine *s_m;
static struct disc s_disc;
static uint32_t *s_pulses;            // 2 sides of track data
static uint8_t *s_disc_ram;           // PRESTO_DISC_MAX bytes
static const uint8_t *s_builtin;
static uint32_t s_builtin_size;
static char s_disc_name[72];          // "" for the built-in disc
static void reset(bool boot);

// ---- the save disc ----
#define SAVE_SIZE (80 * 10 * 256)                                   // a whole single sided disc
#define SAVE_DATA_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE - SAVE_SIZE)   // below the WiFi settings (and a spare sector)
#define SAVE_HEAD_OFFSET (SAVE_DATA_OFFSET - FLASH_SECTOR_SIZE)
#define SAVE_MAGIC 0x53415645u                                      // "SAVE"
typedef struct { uint32_t magic, golden_hash, golden_size, saves; } save_head_t;
static bool s_persist;              // the built-in disc is in: keep its writes
static uint32_t s_golden_hash, s_last_writes, s_quiet;

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static const save_head_t *save_head(void)
{
    const save_head_t *h = (const save_head_t *) (XIP_BASE + SAVE_HEAD_OFFSET);
    return h->magic == SAVE_MAGIC && h->golden_hash == s_golden_hash && h->golden_size == s_builtin_size ? h : NULL;
}
static const uint8_t *save_data(void) { return (const uint8_t *) (XIP_BASE + SAVE_DATA_OFFSET); }

// the built-in disc into the drive: the save disc if there is one for it, else the golden one
static void load_builtin(void)
{
    disc_init(&s_disc, s_pulses, 1, 1);
    s_persist = true;
    if (save_head()) disc_load_ssd(&s_disc, save_data(), SAVE_SIZE);
    else disc_load_ssd(&s_disc, s_builtin, s_builtin_size);
    s_last_writes = 0;
}

static struct { uint32_t offset; const uint8_t *data; uint32_t len; } s_fw;
static void flash_write(void *arg)
{
    (void) arg;
    flash_range_erase(s_fw.offset, FLASH_SECTOR_SIZE);
    if (s_fw.data) flash_range_program(s_fw.offset, s_fw.data, s_fw.len);
}
static bool flash_block(uint32_t offset, const uint8_t *data, uint32_t len)
{
    s_fw.offset = offset, s_fw.data = data, s_fw.len = len;
    // (core 1 is parked, the panel freezes for a moment, while the flash is busy)
    return flash_safe_execute(flash_write, NULL, 1000) == PICO_OK;
}

// the tracks written since the last time, back into the save disc: only the flash blocks that changed
static void save_disc_flush(void)
{
    static uint8_t blk[FLASH_SECTOR_SIZE];
    const save_head_t *h = save_head();
    int blocks = 0;
    for (uint32_t b = 0; b < SAVE_SIZE / FLASH_SECTOR_SIZE; b++) {
        uint32_t off = b * FLASH_SECTOR_SIZE;
        bool any = false;
        for (uint32_t s = off / 256; s < (off + FLASH_SECTOR_SIZE) / 256; s++) any |= s_disc.dirty[0][s / 10] != 0;
        if (!any && h) continue;                       // (the first save copies the whole disc)
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            uint32_t a = off + i;
            blk[i] = h ? save_data()[a] : a < s_builtin_size ? s_builtin[a] : 0;
        }
        for (uint32_t s = off / 256; s < (off + FLASH_SECTOR_SIZE) / 256; s++)
            if (s_disc.dirty[0][s / 10]) disc_read_sector(&s_disc, 0, (int) (s / 10), (int) (s % 10), blk + (s * 256 - off));
        if (h && !memcmp(blk, save_data() + off, FLASH_SECTOR_SIZE)) continue;
        if (!flash_block(SAVE_DATA_OFFSET + off, blk, FLASH_SECTOR_SIZE)) { printf("DISC save failed\n"); return; }
        blocks++;
    }
    memset(s_disc.dirty, 0, sizeof s_disc.dirty);
    save_head_t nh = {SAVE_MAGIC, s_golden_hash, s_builtin_size, h ? h->saves + 1 : 1};
    if (!h || blocks) {
        memset(blk, 0xff, FLASH_PAGE_SIZE);
        memcpy(blk, &nh, sizeof nh);
        flash_block(SAVE_HEAD_OFFSET, blk, FLASH_PAGE_SIZE);   // (last: a save disc half copied isn't one)
    }
    printf("DISC saved (%d blocks)\n", blocks);
}

// what's in the drive, for the loading screen
const char *presto_disc_source(void)
{
    static char s[40];
    const save_head_t *h = save_head();
    if (!s_persist) snprintf(s, sizeof s, "%s", s_disc_name);
    else if (h) snprintf(s, sizeof s, "SAVE DISC (%lu)", (unsigned long) h->saves);
    else snprintf(s, sizeof s, "ORIGINAL");
    return s;
}

// the golden disc back: the save disc forgotten, and the machine booted from the golden one
void presto_disc_restore(void)
{
    flash_block(SAVE_HEAD_OFFSET, NULL, 0);
    load_builtin();
    s_disc_name[0] = 0;
    reset(true);
    printf("DISC golden disc restored\n");
}

uint8_t IC32;

// ---- Set-up ----

void presto_disc_progress(void (*fn)(int done, int total)) { s_disc.progress = fn; }

void presto_machine_init(struct machine *m, uint32_t *pulses, uint8_t *disc_ram,
                         const uint8_t *builtin, uint32_t builtin_size)
{
    s_m = m;
    s_pulses = pulses;
    s_disc_ram = disc_ram;
    s_builtin = builtin;
    s_builtin_size = builtin_size;
    s_golden_hash = fnv(s_builtin, s_builtin_size);
    load_builtin();
    machine_set_disc(m, 0, &s_disc);
    presto_net_init();
}

uint8_t *presto_disc_ram(void)
{
    return s_disc_ram;
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
// keyboard, or presto_boot_shift). The core has no warm reset, so a BASIC
// program in memory does not survive BREAK as on a real Master.
void presto_break(void)
{
    bool shift = (s_m->keys[0] & 1) != 0;   // SHIFT: row 0, column 0
    reset(s_boot_next || shift);
    s_boot_next = false;
}

// The next presto_break boots (SHIFT+BREAK).
void presto_boot_shift(void)
{
    s_boot_next = true;
}

// ---- Keys ----

void key_bbc(int vkey, bool down)
{
    struct machine *m = s_m;
    machine_queue_key(m, m->synced + 2000, (vkey >> 4) & 7, vkey & 15, down);
}

// ---- The panel's LEDs and the temperature ----

uint8_t serial_get_reg(void)
{
    return 0;   // no cassette motor
}

// In hundredths of a degree (RP2350 datasheet: 27C at 0.706V, -1.721mV/C).
int presto_temp_centi(void)
{
    static bool s_adc_ready;
    if (!s_adc_ready) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        s_adc_ready = true;
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 64; i++) {
        sum += adc_read();
    }
    int32_t uv = (int32_t)(((uint64_t)sum * 3300000u) / (64 * 4096));
    return 2700 - (int)(((int64_t)(uv - 706000) * 100) / 1721);
}

// ---- Discs ----

const char *presto_disc_name(void)
{
    return s_disc_name[0] ? s_disc_name : NULL;
}

bool presto_disc_insert(const char *name, uint32_t size)
{
    uint8_t *ram = s_disc_ram;
    if (!ram || size == 0 || size > PRESTO_DISC_MAX) {
        return false;
    }
    // DFS single sided (.ssd) or interleaved double sided (.dsd). The name
    // isn't always right (double sided images get called .ssd), so ask the
    // disc too: its catalogue (bytes 6 and 7 of sector 1) says how many
    // sectors a side holds, so an image bigger than one side is two.
    const char *dot = strrchr(name, '.');
    bool dsd = dot && !strcasecmp(dot, ".dsd");
    uint32_t side_sectors = size >= 0x108 ? (((ram[0x106] & 3u) << 8) | ram[0x107]) : 0;
    if ((side_sectors == 400 || side_sectors == 800) && size > side_sectors * 256) {
        dsd = true;
    }
    if (!dsd && size > PRESTO_DISC_MAX / 2) {
        return false;
    }
    uint32_t padded = (size + 255) & ~255u;   // whole sectors
    memset(ram + size, 0, padded - size);
    disc_init(&s_disc, s_pulses, dsd ? 2 : 1, 1);   // (writes kept for the session)
    disc_load_ssd(&s_disc, ram, padded);
    s_persist = false;
    snprintf(s_disc_name, sizeof s_disc_name, "%s", name);
    reset(true);
    return true;
}

bool presto_sd_reinsert_disc(void)
{
    return s_disc_name[0] != 0;   // the drive keeps its disc through resets
}

void presto_sd_eject_disc(void)
{
    load_builtin();
    s_disc_name[0] = 0;
    reset(true);
    printf("DISC built-in\n");
}

// ---- Each frame and in between ----

void presto_machine_frame(void)
{
    IC32 = s_m->ic32;
    // the save disc: a second after the drive's last write
    if (s_persist) {
        if (s_disc.writes != s_last_writes) s_last_writes = s_disc.writes, s_quiet = 0;
        else if (s_quiet < 50 && ++s_quiet == 50) save_disc_flush();
    }
    presto_net_frame();
}

void presto_machine_poll(void)
{
    presto_net_poll();
}
