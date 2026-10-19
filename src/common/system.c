// The system (system.h): which machine the launcher boots, from either machine.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/structs/qmi.h"

#include "system.h"
#include "mem.h"

bool system_launched(void) {
    // the boot ROM chained into a slot maps it to 0x10000000 (the QMI's address translation):
    // ATRANS0 points somewhere other than the flash's start. Out of reset it's 1:1.
    return (qmi_hw->atrans[0] & QMI_ATRANS0_BASE_BITS) != 0;
}

static const system_settings_t *settings(void) {
    uint32_t off, size;
    if (!mem_flash_region(MEM_FLASH_SYSTEM, &off, &size)) return NULL;
    return (const system_settings_t *) mem_flash_ptr(off);
}

const char *system_choice(void) {
    const system_settings_t *s = settings();
    return s ? system_settings_machine(s) : SYSTEM_DEFAULT_MACHINE;
}

uint32_t system_app_off(void) {
    const system_settings_t *s = settings();
    if (!s || s->magic != SYSTEM_MAGIC || s->version != SYSTEM_VERSION || s->app_off == 0xffffffffu) return 0;
    return s->app_off;
}

// the settings with the machine and the app turned off as given, written if they changed
static bool write_settings(const char *machine, uint32_t app_off) {
    uint32_t off, size;
    if (!mem_flash_region(MEM_FLASH_SYSTEM, &off, &size)) return false;
    if (!strcmp(system_choice(), machine) && system_app_off() == app_off) return true;
    system_settings_t s = {.magic = SYSTEM_MAGIC, .version = SYSTEM_VERSION, .app_off = app_off};
    snprintf(s.machine, sizeof s.machine, "%s", machine);
    return mem_flash_write_sector(off, &s, sizeof s);
}

bool system_choose(const char *machine) {
    return system_slot(machine) >= 0 && write_settings(machine, system_app_off());
}

bool system_set_app_off(uint32_t id) {
    return write_settings(system_choice(), id);
}

const char *system_machines(void) {
    return system_launched() ? "master beeb" : "";
}

void system_status_json(char *out, uint32_t size) {
    if (!system_launched()) {
        if (size) out[0] = 0;
        return;
    }
    snprintf(out, size, ",\"system\":{\"machines\":[\"master\",\"beeb\"],\"choice\":\"%s\"}", system_choice());
}
