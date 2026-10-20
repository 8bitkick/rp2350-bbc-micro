// The disc in drive 0 and the built-in disc's save disc (drive.h).
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"

#include "drive.h"
#include "machine_if.h"
#include "mem.h"

static const uint8_t *s_golden;     // the built-in disc (NULL: none)
static uint32_t s_golden_size;
static uint8_t *s_staging;
static char s_name[72];             // "" for the built-in disc
static bool s_persist;              // the built-in disc is in: keep its writes
static bool s_save_ok;              // there's a save disc for it (the region, and it fits)
static uint32_t s_golden_hash, s_last_writes, s_quiet;
static uint32_t s_head_off, s_data_off, s_save_size;

#define SAVE_MAGIC 0x53415645u      // "SAVE"
typedef struct {
    uint32_t magic, golden_hash, golden_size, saves;
} save_head_t;

static uint32_t fnv(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

// the save disc's header, if it's a copy of this golden disc
static const save_head_t *save_head(void) {
    if (!s_save_ok) return NULL;
    const save_head_t *h = (const save_head_t *) mem_flash_ptr(s_head_off);
    return h->magic == SAVE_MAGIC && h->golden_hash == s_golden_hash && h->golden_size == s_golden_size ? h : NULL;
}

// another disc's save disc is there (another build's built-in disc, or the other machine's): it
// isn't this one's to write over. !restore (drive0_restore) gives the region to this disc.
static bool save_taken(void) {
    if (!s_save_ok || save_head()) return false;
    return ((const save_head_t *) mem_flash_ptr(s_head_off))->magic == SAVE_MAGIC;
}

static const uint8_t *save_data(void) {
    return mem_flash_ptr(s_data_off);
}

// the built-in disc into the drive: the save disc if there is one for it, else the golden one
static void load_builtin(bool boot) {
    s_name[0] = 0;
    s_persist = s_save_ok;
    if (save_head()) machine_drive_load(save_data(), s_save_size, false, boot);
    else machine_drive_load(s_golden, s_golden_size, false, boot);
    s_last_writes = machine_drive_writes();
    s_quiet = 50;       // nothing to keep until the drive is written to
}

void drive0_init(const uint8_t *golden, uint32_t golden_size, uint8_t *staging) {
    s_golden = golden;
    s_golden_size = golden ? golden_size : 0;
    s_staging = staging;
    uint32_t head_size;
    s_save_ok = golden && golden_size &&
                mem_flash_region(MEM_FLASH_SAVE_HEAD, &s_head_off, &head_size) &&
                mem_flash_region(MEM_FLASH_SAVE_DATA, &s_data_off, &s_save_size) &&
                golden_size <= s_save_size;
    if (s_save_ok) s_golden_hash = fnv(golden, golden_size);
    if (save_taken()) printf("DISC the save disc is another disc's: saves to this one aren't kept (restore to start one)\n");
    load_builtin(false);
}

uint8_t *drive0_staging(void) {
    return s_staging;
}

const char *drive0_name(void) {
    return s_name[0] ? s_name : NULL;
}

const char *drive0_builtin_title(void) {
    // DFS: the title's first 8 characters at the start of sector 0, the last 4 at sector 1's
    static char t[13];
    uint32_t n = 0;
    if (s_golden && s_golden_size >= 512)
        for (int i = 0; i < 12; i++) {
            uint8_t c = s_golden[i < 8 ? i : 256 + i - 8];
            if (c == 0) break;
            t[n++] = c >= 32 && c < 127 ? (char) c : ' ';
        }
    while (n && t[n - 1] == ' ') n--;
    t[n] = 0;
    return t;
}

int drive0_saves(void) {
    if (!s_save_ok) return -1;
    const save_head_t *h = save_head();
    return h ? (int) h->saves : 0;
}

const char *drive0_source(void) {
    static char s[40];
    const save_head_t *h = save_head();
    if (!s_persist) snprintf(s, sizeof s, "%s", s_name[0] ? s_name : "NONE");
    else if (h) snprintf(s, sizeof s, "SAVE DISC (%lu)", (unsigned long) h->saves);
    else snprintf(s, sizeof s, "ORIGINAL");
    return s;
}

bool drive0_insert(const char *name, uint32_t size) {
    uint8_t *ram = s_staging;
    if (!ram || size == 0 || size > DRIVE_IMAGE_MAX) return false;
    // DFS single sided (.ssd) or interleaved double sided (.dsd). The name isn't always right
    // (double sided images get called .ssd), so ask the disc too: its catalogue (bytes 6 and 7 of
    // sector 1) says how many sectors a side holds, so an image bigger than one side is two.
    const char *dot = strrchr(name, '.');
    bool dsd = dot && !strcasecmp(dot, ".dsd");
    uint32_t side_sectors = size >= 0x108 ? (((ram[0x106] & 3u) << 8) | ram[0x107]) : 0;
    if ((side_sectors == 400 || side_sectors == 800) && size > side_sectors * 256) dsd = true;
    if (!dsd && size > DRIVE_IMAGE_MAX / 2) return false;
    uint32_t padded = (size + 255) & ~255u;     // whole sectors
    memset(ram + size, 0, padded - size);
    s_persist = false;                          // (its writes kept for the session)
    machine_drive_load(ram, padded, dsd, true);
    snprintf(s_name, sizeof s_name, "%s", name);
    s_last_writes = machine_drive_writes();
    return true;
}

void drive0_eject(void) {
    load_builtin(true);
    printf("DISC built-in\n");
}

void drive0_restore(void) {
    if (!s_save_ok) return;
    mem_flash_write_sector(s_head_off, NULL, 0);
    load_builtin(true);
    printf("DISC golden disc restored\n");
}

bool drive0_import(uint32_t size, const char **why) {
    static uint8_t blk[FLASH_SECTOR_SIZE];
    const uint8_t *img = s_staging;
    if (!s_save_ok) { *why = "this build keeps no save disc"; return false; }
    if (!img || size < 512 || size > s_save_size) { *why = "not a single sided disc the save disc can hold"; return false; }
    // the same disc: its title (sector 0's first 8 bytes, sector 1's first 4) as the built-in one's
    if (memcmp(img, s_golden, 8) || memcmp(img + 256, s_golden + 256, 4)) { *why = "not a copy of the built-in disc (another title)"; return false; }
    const save_head_t *h = save_head();
    uint32_t saves = h ? h->saves : 0;
    for (uint32_t off = 0; off < s_save_size; off += FLASH_SECTOR_SIZE) {
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) blk[i] = off + i < size ? img[off + i] : 0;
        if (!mem_flash_write_sector(s_data_off + off, blk, FLASH_SECTOR_SIZE)) { *why = "the flash write failed"; return false; }
    }
    // last: a save disc half written isn't one
    save_head_t nh = {SAVE_MAGIC, s_golden_hash, s_golden_size, saves + 1};
    mem_flash_write_sector(s_head_off, &nh, sizeof nh);
    load_builtin(true);
    printf("DISC imported as the save disc (%lu bytes)\n", (unsigned long) size);
    return true;
}

// the sectors written since the last time, into the save disc: only the flash sectors that changed
static void commit(void) {
    static uint8_t blk[FLASH_SECTOR_SIZE];
    const save_head_t *h = save_head();
    if (!h && save_taken()) {
        printf("DISC not saved: the save disc is another disc's (restore to start one for this disc)\n");
        return;
    }
    int blocks = 0;
    for (uint32_t off = 0; off < s_save_size; off += FLASH_SECTOR_SIZE) {
        // what's there now (the first save copies the whole disc), then the sectors written
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            uint32_t a = off + i;
            blk[i] = h ? save_data()[a] : a < s_golden_size ? s_golden[a] : 0;
        }
        bool any = false;
        for (uint32_t s = off / 256; s < (off + FLASH_SECTOR_SIZE) / 256; s++) {
            any |= machine_drive_sector((int) (s / 10), (int) (s % 10), blk + (s * 256 - off));
        }
        if (h && (!any || !memcmp(blk, save_data() + off, FLASH_SECTOR_SIZE))) continue;
        if (!mem_flash_write_sector(s_data_off + off, blk, FLASH_SECTOR_SIZE)) {
            printf("DISC save failed\n");
            return;
        }
        blocks++;
    }
    machine_drive_clean();
    if (!h || blocks) {
        // last: a save disc half copied isn't one
        save_head_t nh = {SAVE_MAGIC, s_golden_hash, s_golden_size, h ? h->saves + 1 : 1};
        mem_flash_write_sector(s_head_off, &nh, sizeof nh);
    }
    printf("DISC saved (%d blocks)\n", blocks);
}

void drive0_frame(void) {
    if (!s_persist) return;
    uint32_t w = machine_drive_writes();
    if (w != s_last_writes) {
        s_last_writes = w;
        s_quiet = 0;
    } else if (s_quiet < 50 && ++s_quiet == 50) {
        commit();
    }
}

void drive0_sync(void) {
    if (!s_persist) return;
    if (machine_drive_writes() != s_last_writes || s_quiet < 50) {
        s_last_writes = machine_drive_writes();
        s_quiet = 50;
        commit();
    }
}
