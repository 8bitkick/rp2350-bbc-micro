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
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
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

uint8_t IC32;

// ---- Set-up ----

void presto_machine_init(struct machine *m, uint32_t *pulses, uint8_t *disc_ram,
                         const uint8_t *builtin, uint32_t builtin_size)
{
    s_m = m;
    s_pulses = pulses;
    s_disc_ram = disc_ram;
    s_builtin = builtin;
    s_builtin_size = builtin_size;
    disc_init(&s_disc, s_pulses, 1, 0);
    disc_load_ssd(&s_disc, s_builtin, s_builtin_size);
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
    disc_init(&s_disc, s_pulses, dsd ? 2 : 1, 0);
    disc_load_ssd(&s_disc, ram, padded);
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
    disc_init(&s_disc, s_pulses, 1, 0);
    disc_load_ssd(&s_disc, s_builtin, s_builtin_size);
    s_disc_name[0] = 0;
    reset(true);
    printf("DISC built-in\n");
}

// ---- Each frame and in between ----

void presto_machine_frame(void)
{
    IC32 = s_m->ic32;
    presto_net_frame();
}

void presto_machine_poll(void)
{
    presto_net_poll();
}
