// Kieran Connell's BBC Master 128 (src/cores/kc-beeb/) on the Pimoroni Presto (RP2350, 250 MHz).
//
// Core 0 runs the machine (machine_unit.c), paced to real time; it writes
// the video stream (vstream.h). Core 1 runs the renderer on the stream
// (render_unit.c), which hands each raster to the panel (panel_emit.c):
// one 4-bit row per BBC scanline, scanned out by PIO and DMA
// (presto_lcd.c).
//
// USB serial (tools/presto.py):
//   0x1a 0x00 <SDL scancode>   key down
//   0x1a 0x01 <SDL scancode>   key up
//   !stats                     STATS line: speed (to 0.1%) and load
//   !shot                      SHOT 480 256, then the panel's rows as RGB565
//   !bootsel                   reboot to the USB bootloader
//   !turbo                     toggle turbo: no waiting for real time (every
//                              frame still drawn)
//   !wifi <ssid> <password>    save the WiFi network (a quoted ssid may have
//                              spaces) and restart to join it
//   !net                       the WiFi link and address
//   !discs                     the .ssd/.dsd images on the SD card
//   !disc [name]               boot a disc from the card (no name: the
//                              built-in one)
//   !put <name> <size>         receive a disc image and write it to the card
//
// The web panel (presto_net.c) at http://beeb.local/ once on WiFi.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"

#include "presto_lcd.h"
#include "presto_psram.h"
#include "presto_machine.h"
#include "presto_main.h"
#include "presto_net.h"
#include "presto_sd.h"
#if WITH_SOUND
#include "presto_audio.h"
#endif
#if WITH_AMBIENT
#include "presto_ambient.h"
#endif

#include "machine.h"
#include "machine_hook.h"
#include "disc.h"
#include "panel.h"

// Built in (embedded.c, generated): the Master's ROMs and the disc.
extern const uint8_t rom_mos[16384], rom_dfs[16384], rom_viewsht[16384], rom_edit[16384];
extern const uint8_t rom_basic4[16384], rom_adfs[16384], rom_view[16384], rom_terminal[16384];
extern const uint8_t disc_image[];
extern const uint32_t disc_image_size;

static struct machine s_machine;
static struct vstream s_stream;

// ---- Memory: PSRAM at 0x11000000, handed out from the bottom ----

static uint8_t *s_psram_next = (uint8_t *)0x11000000;

static void *psram_alloc(size_t size)
{
    void *p = s_psram_next;
    s_psram_next += (size + 31) & ~31u;
    return p;
}

// ---- Clock ----

// Slow the flash interface before raising the clock (nothing else sets it
// for the app on RP2350). Runs from RAM.
static void __no_inline_not_in_flash_func(slow_flash_timing)(void)
{
    const uint clkdiv = 3;
    const uint rxdelay = 2;
    hw_write_masked(&qmi_hw->m[0].timing,
                    ((clkdiv << QMI_M0_TIMING_CLKDIV_LSB) & QMI_M0_TIMING_CLKDIV_BITS) |
                    ((rxdelay << QMI_M0_TIMING_RXDELAY_LSB) & QMI_M0_TIMING_RXDELAY_BITS),
                    QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
}

// ---- Core 1: the renderer ----

static volatile uint64_t s_core1_busy_us;

static void __not_in_flash_func(core1_main)(void)
{
    // Core 0 parks this core while it writes the flash (saving the WiFi).
    flash_safe_execute_core_init();
#if WITH_SOUND
    presto_audio_init();    // its DMA interrupt runs on this core
#endif
    for (;;) {
        if (vs_available(&s_stream) == 0) {
#if WITH_AMBIENT
            if (presto_ambient_due()) {
                uint32_t t0 = time_us_32();
                presto_ambient_update();
                s_core1_busy_us += time_us_32() - t0;
                continue;
            }
#endif
            __wfe();
            continue;
        }
        uint32_t t0 = time_us_32();
        panel_consume(&s_stream);
        s_core1_busy_us += time_us_32() - t0;
    }
}

// ---- Keys: SDL scancodes to the BBC matrix (row << 4 | column) ----

static uint8_t sdl_to_bbc(uint32_t sc)
{
    static const uint8_t letters[26] = {
        0x41, 0x64, 0x52, 0x32, 0x22, 0x43, 0x53, 0x54, 0x25, 0x45, 0x46, 0x56, 0x65,   // A-M
        0x55, 0x36, 0x37, 0x10, 0x33, 0x51, 0x23, 0x35, 0x63, 0x21, 0x42, 0x44, 0x61,   // N-Z
    };
    static const uint8_t digits[10] = {0x30, 0x31, 0x11, 0x12, 0x13, 0x34, 0x24, 0x15, 0x26, 0x27};   // 1-9, 0
    static const uint8_t fkeys[10] = {0x71, 0x72, 0x73, 0x14, 0x74, 0x75, 0x16, 0x76, 0x77, 0x20};   // F1-F9, F10 = f0
    if (sc >= 4 && sc <= 29) return letters[sc - 4];
    if (sc >= 30 && sc <= 39) return digits[sc - 30];
    if (sc >= 58 && sc <= 67) return fkeys[sc - 58];
    switch (sc) {
    case 40: return 0x49;   // RETURN
    case 41: return 0x70;   // ESCAPE
    case 42: return 0x59;   // DELETE
    case 43: return 0x60;   // TAB
    case 44: return 0x62;   // SPACE
    case 79: return 0x79;   // RIGHT
    case 80: return 0x19;   // LEFT
    case 81: return 0x29;   // DOWN
    case 82: return 0x39;   // UP
    case 224: return 0x01;  // CTRL
    case 225: return 0x00;  // SHIFT
    case 229: return 0x00;  // right SHIFT
    default: return 0xff;
    }
}

// ---- Stats ----

static uint32_t s_core0_pct, s_core1_pct, s_speed_permille;
static uint32_t s_frames;
static bool s_turbo;
static bool s_turbo_ended;
static bool s_was_reset;
static struct machine_hook_io s_hook_io;     // what a program run alongside sees (frame_hook)

// For the web panel (presto_machine.h).
bool presto_turbo_get(void)
{
    return s_turbo;
}

void presto_turbo_set(bool on)
{
    if (s_turbo && !on) {
        s_turbo_ended = true;
    }
    s_turbo = on;
}

bool presto_overlay_get(void)
{
    return false;   // no load display on the panel in this build
}

void presto_overlay_set(bool on)
{
    (void)on;
}

void presto_perf_get(uint *core0, uint *core1, uint *speed)
{
    *core0 = s_core0_pct;
    *core1 = s_core1_pct;
    *speed = (s_speed_permille + 5) / 10;
}

void main_machine_was_reset(void)
{
    s_hook_io.resets++;
    panel_machine_reset(&s_stream);
    s_was_reset = true;
}

static void print_stats(void)
{
    printf("STATS frames=%lu late=0 lcd_frames=%lu avg_busy_us=0 core0=%lu%% core1=%lu%% speed=%lu.%lu%%\n",
           (unsigned long)s_frames, (unsigned long)presto_lcd_frame_count(), (unsigned long)s_core0_pct,
           (unsigned long)s_core1_pct, (unsigned long)(s_speed_permille / 10),
           (unsigned long)(s_speed_permille % 10));
}

static void run_command(const char *cmd)
{
    if (!strcmp(cmd, "stats")) {
        print_stats();
    } else if (!strcmp(cmd, "turbo")) {
        presto_turbo_set(!s_turbo);
        printf("TURBO %s\n", s_turbo ? "on" : "off");
    } else if (!strncmp(cmd, "wifi ", 5)) {
        presto_net_set_wifi(cmd + 5);
    } else if (!strcmp(cmd, "net")) {
        presto_net_print_status();
    } else if (!strcmp(cmd, "discs")) {
        presto_sd_list_discs();
    } else if (!strcmp(cmd, "disc restore")) {
        presto_disc_restore();
    } else if (!strcmp(cmd, "disc")) {
        presto_sd_eject_disc();
    } else if (!strncmp(cmd, "disc ", 5)) {
        if (!presto_sd_load_disc(cmd + 5)) {
            printf("DISC error: %s not loaded\n", cmd + 5);
        }
    } else if (!strncmp(cmd, "put ", 4)) {
        presto_sd_put(cmd + 4);
    } else if (!strcmp(cmd, "shot")) {
        panel_shot();
#if WITH_SOUND
    } else if (!strncmp(cmd, "audio", 5)) {
        presto_audio_command(cmd + 5);
    } else if (!strncmp(cmd, "beep", 4)) {
        presto_audio_test(cmd + 4);
#endif
#if WITH_AMBIENT
    } else if (!strncmp(cmd, "leds", 4)) {
        presto_ambient_command(cmd + 4);
#endif
    } else if (!strcmp(cmd, "bootsel")) {
        printf("BOOTSEL\n");
        sleep_ms(50);
        reset_usb_boot(0, 0);
    } else {
        char reply[256] = "";
        if (!machine_command_hook(cmd, reply, sizeof reply)) printf("? %s\n", cmd);
        else if (reply[0]) printf("%s\n", reply);
    }
}

// a program run alongside the machine (src/common/machine_hook.h) supplies these; without one they do nothing
__attribute__((weak)) void machine_frame_hook(const struct machine_hook_io *io) { (void) io; }
__attribute__((weak)) bool machine_command_hook(const char *cmd, char *reply, uint32_t reply_size) { (void) cmd; (void) reply; (void) reply_size; return false; }
__attribute__((weak)) const char *machine_program_json(void) { return NULL; }
__attribute__((weak)) const struct machine_boot_image *machine_boot_image(void) { return NULL; }

static struct machine *s_hook_m;
static void hook_key(int row, int col, bool down)
{
    machine_queue_key(s_hook_m, s_hook_m->synced + 2000, row, col, down);    // as key_bbc
}
static void hook_analogue(int channel, uint16_t value)
{
    machine_set_analogue(s_hook_m, channel, value);
}
static void frame_hook(struct machine *m)
{
    struct machine_hook_io *io = &s_hook_io;
    if (!io->version) {
        s_hook_m = m;
        *io = (struct machine_hook_io) {
            .version = MACHINE_HOOK_VERSION, .model = MACHINE_MODEL_MASTER_128, .mos = 320,
            .ram = m->ram, .ram_size = sizeof m->ram, .shadow = m->lynne, .shadow_size = sizeof m->lynne,
            .andy = m->andy, .andy_size = sizeof m->andy, .hazel = m->hazel, .hazel_size = sizeof m->hazel,
            .key = hook_key, .analogue = hook_analogue, .resets = io->resets,
        };
    }
    io->frame++;
    machine_frame_hook(io);
}

static void usb_poll(struct machine *m)
{
    static char s_cmd[128];
    static int s_cmd_len = -1;
    static int s_key_state;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (s_key_state == 1) {
            s_key_state = (c == 0) ? 2 : ((c == 1) ? 3 : 0);
        } else if (s_key_state >= 2) {
            uint8_t k = sdl_to_bbc((uint32_t)c);
            if (k != 0xff) {
                machine_queue_key(m, m->synced + 2000, k >> 4, k & 15, s_key_state == 2);
            }
            s_key_state = 0;
        } else if (c == 0x1a) {
            s_key_state = 1;
        } else if (c == '!') {
            s_cmd_len = 0;
        } else if (s_cmd_len >= 0) {
            if (c == '\n' || c == '\r') {
                s_cmd[s_cmd_len] = 0;
                s_cmd_len = -1;
                run_command(s_cmd);
            } else if (s_cmd_len < (int)sizeof s_cmd - 1) {
                s_cmd[s_cmd_len++] = (char)c;
            }
        }
    }
}

// ---- Core 0: the machine, paced to real time ----

// the disc going into the drive, on the loading screen
static void splash_disc_progress(int done, int total)
{
    char what[48];
    snprintf(what, sizeof what, "DISC INTO PSRAM: %s", presto_disc_source());
    panel_splash_status(what, 300 + 700 * done / total);
}

static void run(void)
{
    struct machine *m = &s_machine;
    // a program's loading screen, if it has one, until core 1 starts drawing the machine: what's
    // loading and how far (the ROMs into PSRAM the first 30%, the disc's tracks going in the rest)
    uint64_t boot_t0 = time_us_64();
    bool splash = panel_splash_picture();
    if (splash) panel_splash_status("ROMS INTO PSRAM", 0);
    machine_init(m);
    for (int i = 0; i < MACHINE_SWRAM_SLOTS; i++) {
        m->swram[i] = psram_alloc(0x4000);
        memset(m->swram[i], 0, 0x4000);
    }
    // The MOS and the DFS (the ones a running demo calls) in SRAM; the rest
    // read through the XIP cache from PSRAM.
    uint8_t *mos = malloc(16384), *dfs = malloc(16384);
    memcpy(mos, rom_mos, 16384);
    memcpy(dfs, rom_dfs, 16384);
    if (splash) panel_splash_status("ROMS INTO PSRAM", 2 * 300 / 8);
    m->mos = mos;
    m->rom[0x9] = dfs;
    const uint8_t *const others[6] = {rom_viewsht, rom_edit, rom_basic4, rom_adfs, rom_view, rom_terminal};
    for (int i = 0; i < 6; i++) {
        uint8_t *p = psram_alloc(16384);
        memcpy(p, others[i], 16384);
        m->rom[0xa + i] = p;
        if (splash) panel_splash_status("ROMS INTO PSRAM", (3 + i) * 300 / 8);
    }
    m->vs = &s_stream;
#if WITH_SOUND
    presto_audio_set_ring(psram_alloc(presto_audio_ring_size()));
#endif
    panel_init();
    machine_reset(m);
    // Drive 0's track data (two sides, for .dsd images) and where disc
    // images from the card or the web arrive.
    uint32_t *pulses = psram_alloc(2 * (size_t)DISC_TRACKS * DISC_BYTES_PER_TRACK * sizeof(uint32_t));
    uint8_t *disc_ram = psram_alloc(PRESTO_DISC_MAX);
    presto_disc_progress(splash ? splash_disc_progress : NULL);
    presto_machine_init(m, pulses, disc_ram, disc_image, disc_image_size);
    presto_disc_progress(NULL);
    if (splash) {
        panel_splash_status("READY", 1000);
        while (time_us_64() - boot_t0 < 3000000) sleep_ms(20);     // (it's quick: held a moment so it's seen)
    }
    multicore_launch_core1(core1_main);
    machine_autoboot(m);
    s_was_reset = false;
    printf("# BBC Master on Presto: %lu MHz, machine %u bytes, free heap from %p, PSRAM to %p\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000), (unsigned)sizeof *m, malloc(4), s_psram_next);

    uint64_t t0 = time_us_64();
    uint64_t next_frame = 40000;
    uint64_t stat_t = t0, stat_busy = 0, stat_cycles = 0, stat_core1 = 0;
    uint64_t busy = 0;
    for (;;) {
        uint64_t now = time_us_64();
        if (s_was_reset) {
            // The cycle count started again: real time from here.
            s_was_reset = false;
            t0 = now;
            next_frame = 40000;
            stat_t = now;
            stat_busy = busy;
            stat_cycles = 0;
            stat_core1 = s_core1_busy_us;
        }
        if (s_turbo_ended) {
            // Real time from here: no catching up the time turbo ran ahead.
            s_turbo_ended = false;
            t0 = now - m->cpu.cycles / 2;
        }
        uint64_t due = s_turbo ? (m->cpu.cycles + 40000) : (now - t0) * 2;   // 2 MHz
        if (m->cpu.cycles >= due) {
            usb_poll(m);
            presto_machine_poll();
            sleep_us(100);
            continue;
        }
        uint64_t target = due + 4000;
        if (target > m->cpu.cycles + 40000) {
            target = m->cpu.cycles + 40000;
        }
        // Never lap the renderer: at most a word a cycle into the ring.
        int32_t room = (int32_t)VS_RING - vs_available(&s_stream) - 512;
        if (room < 2000) {
            __sev();
            continue;
        }
        if (target > m->cpu.cycles + (uint64_t)room) {
            target = m->cpu.cycles + (uint64_t)room;
        }
        uint64_t b0 = time_us_64();
        machine_run(m, target);
        busy += time_us_64() - b0;
        __sev();
        bool new_frame = m->cpu.cycles >= next_frame;
        while (m->cpu.cycles >= next_frame) {
            next_frame += 40000;
            s_frames++;
            if (s_frames % 250 == 0) {
                uint64_t t = time_us_64();
                uint64_t el = t - stat_t;
                uint64_t c1 = s_core1_busy_us;
                s_core0_pct = (uint32_t)((busy - stat_busy) * 100 / el);
                s_core1_pct = (uint32_t)((c1 - stat_core1) * 100 / el);
                // 2 MHz is 100.0%, rounded to the nearest 0.1% (truncating made a
                // window a microsecond over 5 s read 99%)
                s_speed_permille = (uint32_t)(((m->cpu.cycles - stat_cycles) * 500 + el / 2) / el);
                stat_t = t;
                stat_busy = busy;
                stat_core1 = c1;
                stat_cycles = m->cpu.cycles;
                print_stats();
            }
        }
        usb_poll(m);
        presto_machine_poll();
        if (new_frame) {
#if WITH_SOUND
            presto_audio_frame(m->cpu.cycles, s_turbo);
#endif
#if WITH_AMBIENT
            // CAPS LOCK and SHIFT LOCK are lit when their IC32 latch bits (6, 7) are low
            presto_ambient_frame((uint8_t)(((m->ic32 & 0x40) ? 0 : 1) | ((m->ic32 & 0x80) ? 0 : 2)));
#endif
            presto_machine_frame();   // may reset the machine (s_was_reset)
            frame_hook(m);
        }
    }
}

int main(void)
{
    slow_flash_timing();
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    set_sys_clock_khz(250000, true);
    stdio_init_all();
    // The piezo's MOSFET (GPIO 43): driven off. Left floating it picks up
    // the WiFi chip's activity and buzzes.
    gpio_init(43);
    gpio_set_dir(43, GPIO_OUT);
    gpio_put(43, 0);
    presto_psram_init();
#if WITH_AMBIENT
    presto_ambient_init();    // first: it moves PIO0 to GPIO 16-47, only possible before PIO0 has a program
#endif
    panel_lcd_init();
    run();
    return 0;
}
