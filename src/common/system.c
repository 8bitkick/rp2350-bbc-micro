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

bool system_choose(const char *machine) {
    uint32_t off, size;
    if (system_slot(machine) < 0 || !mem_flash_region(MEM_FLASH_SYSTEM, &off, &size)) return false;
    if (!strcmp(system_choice(), machine)) return true;
    system_settings_t s = {.magic = SYSTEM_MAGIC, .version = SYSTEM_VERSION};
    snprintf(s.machine, sizeof s.machine, "%s", machine);
    return mem_flash_write_sector(off, &s, sizeof s);
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
