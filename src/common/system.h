#ifndef SYSTEM_H
#define SYSTEM_H

#define C64_DIAG_MAGIC 0x64360000u
#define C64_RETURN_MAGIC 0x64370000u


#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

void system_capture_c64_failure(void);
void system_failure_json(char *out, uint32_t size);

// The system: both machines in one flash, each in a slot of its own, and the launcher at the
// bottom of the flash that boots the one chosen (docs/dual-boot.md). The choice lives in the
// board's settings sector (mem.h: MEM_FLASH_SYSTEM), outside every image, read by the launcher
// (src/launcher) and written by either machine. A machine built on its own runs just the same:
// it isn't launched, so it has no other machine to offer.

// the slots: partitions of the table in src/launcher/partitions.json, in this order
#define SYSTEM_SLOT_MASTER 0
#define SYSTEM_SLOT_BEEB   1
#define SYSTEM_SLOT_C64    3
#define SYSTEM_SLOT_APP    2        // the app (app_api.h)
// (the board's flash regions, WiFi, settings and save discs, are above them all, unpartitioned)

// what the settings sector holds
#define SYSTEM_MAGIC 0x4e48434du    // "MCHN"
#define SYSTEM_VERSION 1
typedef struct {
    uint32_t magic, version;
    char machine[16];               // "master" or "beeb": the machine to boot
    // (the launcher reads only the above: what follows came later, and is erased flash, all
    // ones, in a sector written before)
    uint32_t app_off;               // an app that died at boot (app.c): its id, so it isn't run
} system_settings_t;

// the machine booted when nothing (or nothing sensible) is chosen
#define SYSTEM_DEFAULT_MACHINE "master"

// the slot a machine's name says (-1: no such machine)
static inline int system_slot(const char *machine) {
    return !strcmp(machine, "master") ? SYSTEM_SLOT_MASTER : !strcmp(machine, "beeb") ? SYSTEM_SLOT_BEEB : !strcmp(machine, "c64") ? SYSTEM_SLOT_C64 : -1;
}

// the machine a settings sector chooses: SYSTEM_DEFAULT_MACHINE if it's blank, garbage, or a
// machine there's no slot for
static inline const char *system_settings_machine(const system_settings_t *s) {
    if (s->magic != SYSTEM_MAGIC || s->version != SYSTEM_VERSION || !memchr(s->machine, 0, sizeof s->machine) ||
        system_slot(s->machine) < 0) return SYSTEM_DEFAULT_MACHINE;
    return s->machine;
}

// ---- In a machine's firmware (system.c)

// running from a slot, booted by the launcher (false: a machine built on its own)
bool system_launched(void);
// the machine chosen to boot (SYSTEM_DEFAULT_MACHINE if none is)
const char *system_choice(void);
// choose a machine for the next boot: false if it's none, or the setting couldn't be written
bool system_choose(const char *machine);
// the app turned off after it died at boot (its id, app.c; 0: none), and turning one off (0: on
// again): false if the setting couldn't be written
uint32_t system_app_off(void);
bool system_set_app_off(uint32_t id);
// "master" and "beeb", space separated: the machines there are to choose from ("" unless launched)
const char *system_machines(void);
// /api/status's "system" member (",\"system\":{...}"), or "" unless launched
void system_status_json(char *out, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
