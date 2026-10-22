// The command layer (see api.h): every operation once, for the USB channel and the HTTP API.
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"

#include "api.h"
#include "frontend.h"
#include "machine_if.h"
#include "app.h"
#include "drive.h"
#include "mem.h"
#include "system.h"
#include "board.h"
#if BOARD_HAS_NET
#include "net_if.h"
#endif
#if WITH_AMBIENT
#endif
#if WITH_SOUND
#include "sound.h"
#endif

// ---- Replies

char *api_buffer(size_t *size) {
    static char s_buf[8192];
    *size = sizeof s_buf;
    return s_buf;
}

void api_reply(api_req_t *r, int code, const char *fmt, ...) {
    r->code = code;
    if (!r->json || !r->out) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->out, r->size, fmt, ap);
    va_end(ap);
}

void api_error(api_req_t *r, int code, const char *why) {
    if (r->json) {
        char q[160];
        api_json_str(q, sizeof q, why);
        api_reply(r, code, "{\"error\":%s}", q);
    } else {
        r->code = code;
        printf("ERR %s\n", why);
    }
}

int api_json_str(char *out, size_t size, const char *s) {
    size_t o = 0;
    if (size < 3) return 0;
    out[o++] = '"';
    for (; *s && o + 3 < size; s++) {
        unsigned char ch = (unsigned char) *s;
        if (ch == '"' || ch == '\\') out[o++] = '\\';
        out[o++] = ch < 0x20 ? ' ' : (char) ch;
    }
    out[o++] = '"';
    out[o] = 0;
    return (int) o;
}

// "1" on, "0" off, anything else toggles
static bool flag(const char *args, bool now) {
    while (*args == ' ') args++;
    return args[0] == '1' ? true : args[0] == '0' ? false : !now;
}

// ---- Keys from the page: by their place in the BBC's keyboard matrix (row << 4 | column).
// Queued, and let out between frames with each key held down for a few frames, so the MOS's
// keyboard scan sees it however quickly the page sends down and up.

#define KEYQ 64
#define KEY_BREAK 0x80
static uint16_t s_keyq[KEYQ];
static uint8_t s_keyq_head, s_keyq_tail;

static void queue_key(bool down, unsigned code) {
    uint8_t next = (uint8_t) ((s_keyq_head + 1) % KEYQ);
    if (next == s_keyq_tail || (code > 0x7f && code != KEY_BREAK)) return;
    s_keyq[s_keyq_head] = (uint16_t) (code | (down ? 0x100 : 0));
    s_keyq_head = next;
}

void api_frame(void) {
    // a key stays down for 3 frames, and up for 2 before anything else goes down (or the MOS
    // misses the gap between two presses of the same key)
    static uint32_t frames, last_down, last_up;
    frames++;
    while (s_keyq_head != s_keyq_tail) {
        uint16_t e = s_keyq[s_keyq_tail];
        bool down = e & 0x100;
        if (frames - last_down < 3 || (down && frames - last_up < 2)) break;
        s_keyq_tail = (uint8_t) ((s_keyq_tail + 1) % KEYQ);
        unsigned code = e & 0xff;
        if (code == KEY_BREAK) {
            if (down) machine_break();
        } else {
            machine_key((int) code, down);
        }
        if (down) last_down = frames;
        else last_up = frames;
    }
}

// "d<hex>" / "u<hex>" / "b", space separated: keys down and up by matrix code, b is BREAK
static void cmd_keys(api_req_t *r) {
    for (const char *p = r->args; *p;) {
        while (*p == ' ' || *p == '\n') p++;
        if (*p == 'b') {
            queue_key(true, KEY_BREAK);
            queue_key(false, KEY_BREAK);
            p++;
            continue;
        }
        if (*p != 'd' && *p != 'u') break;
        bool down = *p++ == 'd';
        char *end;
        unsigned long code = strtoul(p, &end, 16);
        if (end == p) break;
        queue_key(down, (unsigned) code);
        p = end;
    }
    api_reply(r, 200, "{}");
    if (!r->json) printf("KEYS ok\n");
}

// ---- Status

static void status_json(char *j, size_t size) {
    unsigned c0, c1, speed;
    machine_perf_get(&c0, &c1, &speed);
    struct mallinfo m = mallinfo();
    extern char __StackLimit;
    unsigned free_ram = (unsigned) (m.fordblks + (unsigned) (&__StackLimit - (char *) sbrk(0)));
    char disc[80];
    const char *dn = drive0_name();
    if (dn) api_json_str(disc, sizeof disc, dn);
    else strcpy(disc, "null");
    char builtin[40];
    api_json_str(builtin, sizeof builtin, drive0_builtin_title());
    char overlay[8];
    snprintf(overlay, sizeof overlay, "%s", !machine_overlay_has() ? "null" : machine_overlay_get() ? "true" : "false");
    int n = snprintf(j, size, "{\"machine\":\"%s\",\"board\":\"%s\",\"turbo\":%s,\"overlay\":%s,\"speed\":%u,"
                              "\"core0\":%u,\"core1\":%u,\"disc\":%s,\"builtin\":%s,\"saves\":%d,\"free\":%u,\"mhz\":%u,"
                              "\"temp\":%d,\"leds\":%u,\"program\":%s",
                     machine_name(), BOARD_NAME, machine_turbo_get() ? "true" : "false", overlay, speed, c0, c1,
                     disc, builtin, drive0_saves(), free_ram, (unsigned) (clock_get_hz(clk_sys) / 1000000),
                     (frontend_temp_centi() + 50) / 100, machine_leds(),
                     app_program_json() ? "true" : "false");
#if BOARD_HAS_NET
    if (n > 0 && (size_t) n < size) {
        net_status_json(j + n, size - (size_t) n);
        n += (int) strlen(j + n);
    }
#endif
#if WITH_SOUND
    if (n > 0 && (size_t) n < size) {
        bool on;
        unsigned vol;
        audio_get(&on, &vol);
        n += snprintf(j + n, size - (size_t) n, ",\"sound\":{\"on\":%s,\"vol\":%u}", on ? "true" : "false", vol);
    }
#endif
#if WITH_AMBIENT
    if (board_leds_ready() && n > 0 && (size_t) n < size) {
        bool on, locks;
        unsigned bright;
        board_leds_get(&on, &bright, &locks);
        n += snprintf(j + n, size - (size_t) n, ",\"ambient\":{\"on\":%s,\"bright\":%u,\"locks\":%s}",
                      on ? "true" : "false", bright, locks ? "true" : "false");
    }
#endif
    if (n > 0 && (size_t) n < size) {
        // the firmware and the regions the board keeps in the flash; the PSRAM the machine has
        // taken; the SRAM not free (the heap's free blocks and the gap below the stack are)
        extern char __flash_binary_end;
        uint32_t flash_used = (uint32_t) ((uintptr_t) &__flash_binary_end - XIP_BASE), off, sz;
        for (int i = 0; i < MEM_FLASH_REGIONS; i++)
            if (mem_flash_region(i, &off, &sz)) flash_used += sz;
        flash_used += app_flash_used();     // (the app slot's app; the other machine's slot not counted)
        const uint32_t ram_total = SRAM_END - SRAM_BASE;
        n += snprintf(j + n, size - (size_t) n,
                      ",\"mem\":{\"flash\":[%lu,%lu],\"psram\":[%lu,%lu],\"ram\":[%lu,%lu]}",
                      (unsigned long) flash_used, (unsigned long) PICO_FLASH_SIZE_BYTES,
                      (unsigned long) machine_psram_used(), (unsigned long) board_psram_size(),
                      (unsigned long) (ram_total - free_ram), (unsigned long) ram_total);
    }
    if (n > 0 && (size_t) n < size) {
        system_status_json(j + n, size - (size_t) n);
        n += (int) strlen(j + n);
    }
    if (n > 0 && (size_t) n + 8 < size) {
        n += snprintf(j + n, size - (size_t) n, ",\"app\":");
        app_json(j + n, (uint32_t) (size - (size_t) n), false);
        n += (int) strlen(j + n);
    }
    if (n > 0 && (size_t) n + 2 <= size) strcpy(j + n, "}");
}

static void cmd_status(api_req_t *r) {
    if (r->json) {
        status_json(r->out, r->size);
        r->code = 200;
    } else {
        size_t size;
        char *j = api_buffer(&size);
        status_json(j, size);
        printf("STATUS %s\n", j);
    }
}

static void cmd_stats(api_req_t *r) {
    if (r->json) {
        unsigned c0, c1, speed;
        machine_perf_get(&c0, &c1, &speed);
        api_reply(r, 200, "{\"speed\":%u,\"core0\":%u,\"core1\":%u,\"turbo\":%s}", speed, c0, c1,
                  machine_turbo_get() ? "true" : "false");
    } else {
        machine_print_stats();
    }
}

static void cmd_turbo(api_req_t *r) {
    machine_turbo_set(flag(r->args, machine_turbo_get()));
    if (r->json) cmd_status(r);
    else printf("TURBO %s\n", machine_turbo_get() ? "on" : "off");
}

static void cmd_overlay(api_req_t *r) {
    if (!machine_overlay_has()) {
        api_error(r, 404, "no overlay in this build");
        return;
    }
    machine_overlay_set(flag(r->args, machine_overlay_get()));
    if (r->json) cmd_status(r);
    else printf("PERF overlay %s\n", machine_overlay_get() ? "on" : "off");
}

// SHIFT+BREAK: SHIFT held for the first second after the reset, as at power-on
static void cmd_boot(api_req_t *r) {
    machine_boot_shift();
    machine_break();
    api_reply(r, 200, "{}");
    if (!r->json) printf("BOOT\n");
}

static void cmd_break(api_req_t *r) {
    machine_break();
    api_reply(r, 200, "{}");
    if (!r->json) printf("BREAK\n");
}

// ---- The machine: with both in the flash (system.h), the one to boot

// "" (which there are, and which is chosen) or "master" / "beeb": choose it and restart into it
static void cmd_machine(api_req_t *r) {
    char want[16];
    snprintf(want, sizeof want, "%s", r->args);
    want[strcspn(want, " \r\n")] = 0;
    if (!system_launched()) {
        if (want[0]) api_error(r, 404, "one machine in this build (no launcher)");
        else if (r->json) api_reply(r, 200, "{\"machine\":\"%s\"}", machine_name());
        else printf("MACHINE %s (no launcher)\n", machine_name());
        return;
    }
    if (!want[0]) {
        if (r->json) {
            char sys[96];
            system_status_json(sys, sizeof sys);
            api_reply(r, 200, "{\"machine\":\"%s\"%s}", machine_name(), sys);
        } else {
            printf("MACHINE %s (choice %s; %s)\n", machine_name(), system_choice(), system_machines());
        }
        return;
    }
    if (system_slot(want) < 0) {
        api_error(r, 400, "no such machine (master or beeb)");
        return;
    }
    if (!system_choose(want)) {
        api_error(r, 500, "couldn't save the choice");
        return;
    }
    if (!strcmp(want, machine_name())) {
        api_reply(r, 200, "{\"restart\":false}");
        if (!r->json) printf("MACHINE %s (running)\n", want);
        return;
    }
    drive0_sync();              // a game saved a moment ago: keep it
    api_reply(r, 200, "{\"restart\":true}");
    printf("MACHINE %s: restarting (asked by %s)\n", want, r->from ? r->from : "USB");
#if BOARD_HAS_NET
    if (r->json) {
        net_restart(true);      // once the reply has gone
        return;
    }
#endif
    fflush(stdout);
    sleep_ms(100);
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

// ---- Discs

static void cmd_eject(api_req_t *r) {
    drive0_eject();             // ("DISC built-in")
    api_reply(r, 200, "{}");
}

typedef struct {
    char *j;
    size_t size, used;
    int n;
} list_ctx_t;

static void list_disc(const char *name, uint32_t size, void *ctx) {
    list_ctx_t *l = ctx;
    char q[80];
    api_json_str(q, sizeof q, name);
    if (l->used + strlen(q) + 40 >= l->size) return;
    l->used += (size_t) snprintf(l->j + l->used, l->size - l->used, "%s{\"name\":%s,\"size\":%u}",
                                 l->n++ ? "," : "", q, (unsigned) size);
}

static void cmd_discs(api_req_t *r) {
    if (!r->json) {
        board_sd_list_discs();
        return;
    }
    list_ctx_t l = {.j = r->out, .size = r->size};
    l.used = (size_t) snprintf(l.j, l.size, "{\"discs\":[");
    int n = board_sd_each_disc(list_disc, &l);
    snprintf(l.j + l.used, l.size - l.used, "],\"card\":%s}", n < 0 ? "false" : "true");
    r->code = 200;
}

// a disc on the card into drive 0, booted
static void cmd_sd(api_req_t *r) {
    const char *name = r->args;
    while (*name == ' ') name++;
    if (board_sd_load_disc(name)) {
        api_reply(r, 200, "{}");
    } else {
        api_error(r, 404, "couldn't load that from the card");
    }
}

// the built-in disc as it came (what games saved to it forgotten), booted
static void cmd_restore(api_req_t *r) {
    if (drive0_saves() < 0) {
        api_error(r, 404, "no save disc in this build");
        return;
    }
    drive0_restore();           // ("DISC golden disc restored")
    api_reply(r, 200, "{}");
}

// ---- The network

#if BOARD_HAS_NET
static void cmd_net(api_req_t *r) {
    if (!r->json) {
        net_print_status();
        return;
    }
    char frag[400];
    net_status_json(frag, sizeof frag);
    api_reply(r, 200, "{%s}", frag[0] == ',' ? frag + 1 : frag);
}

// "<ssid>\n<password>" (HTTP), or "<ssid> <password>" (USB: a quoted ssid may have spaces)
static void cmd_wifi(api_req_t *r) {
    char ssid[34], pass[66];
    const char *p = r->args, *nl = strchr(p, '\n');
    size_t n;
    if (nl) {
        n = (size_t) (nl - p);
        if (n >= sizeof ssid) n = sizeof ssid - 1;
        memcpy(ssid, p, n);
        p = nl + 1;
    } else {
        while (*p == ' ') p++;
        if (*p == '"') {
            const char *q = strchr(p + 1, '"');
            if (!q) {
                api_error(r, 400, "usage: wifi <ssid> <password>");
                return;
            }
            n = (size_t) (q - p - 1);
            p++;
            if (n >= sizeof ssid) n = sizeof ssid - 1;
            memcpy(ssid, p, n);
            p = q + 1;
        } else {
            const char *q = strchr(p, ' ');
            n = q ? (size_t) (q - p) : strlen(p);
            if (n >= sizeof ssid) n = sizeof ssid - 1;
            memcpy(ssid, p, n);
            p += n;
        }
        while (*p == ' ') p++;
    }
    ssid[n] = 0;
    snprintf(pass, sizeof pass, "%s", p);
    pass[strcspn(pass, "\r\n")] = 0;
    if (!net_wifi_save(ssid, pass)) {
        api_error(r, 400, "couldn't save that");
        return;
    }
    api_reply(r, 200, "{}");
    if (!r->json) printf("WIFI saved (%u character network name)\n", (unsigned) strlen(ssid));
    net_restart(r->json);
}
#endif

// ---- The LEDs on the back

#if WITH_AMBIENT
// USB: "leds [off|screen|test|bright <percent>|locks on|locks off]"; HTTP: any of "on=0|1",
// "bright=<percent>", "locks=0|1", joined by "&"
static void cmd_ambient(api_req_t *r) {
    if (!r->json) {
        board_leds_command(r->args);
        return;
    }
    int on = -1, bright = -1, locks = -1;
    char body[128];
    snprintf(body, sizeof body, "%s", r->args);
    for (char *p = body; *p;) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;
        if (!strncmp(p, "on=", 3)) on = atoi(p + 3) ? 1 : 0;
        else if (!strncmp(p, "bright=", 7)) bright = atoi(p + 7);
        else if (!strncmp(p, "locks=", 6)) locks = atoi(p + 6) ? 1 : 0;
        if (!amp) break;
        p = amp + 1;
    }
    board_leds_set(on, bright, locks);
    cmd_status(r);
}
#endif

// ---- The sound

#if WITH_SOUND
// USB: "sound [on|off|vol <percent>]"; HTTP: any of "on=0|1", "vol=<percent>", joined by "&"
static void cmd_sound(api_req_t *r) {
    int on = -1, vol = -1;
    char body[64];
    snprintf(body, sizeof body, "%s", r->args);
    if (!r->json) {
        char *p = body;
        while (*p == ' ') p++;
        if (!strncmp(p, "on", 2) && (p[2] == 0 || p[2] == ' ')) on = 1;
        else if (!strncmp(p, "off", 3)) on = 0;
        else if (!strncmp(p, "vol ", 4)) vol = atoi(p + 4);
        audio_set(on, vol);
        bool o;
        unsigned v;
        audio_get(&o, &v);
        printf("SOUND %s vol=%u%%\n", o ? "on" : "off", v);
        return;
    }
    for (char *p = body; *p;) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;
        if (!strncmp(p, "on=", 3)) on = atoi(p + 3) ? 1 : 0;
        else if (!strncmp(p, "vol=", 4)) vol = atoi(p + 4);
        if (!amp) break;
        p = amp + 1;
    }
    audio_set(on, vol);
    cmd_status(r);
}
#endif

// ---- The app run alongside the machine (app.c)

// what it is (GET /api/app, "!app"); "on" (POST /api/app/on, "!app on"): let one the app guard
// turned off run again, from the next boot; "remove" (POST /api/app/remove, DELETE /api/app,
// "!app remove"): erase the slot's. (Installing: POST /api/app with the image, presto_net.c, or
// "!app install <size>" over USB, frontend.c: both app.c's app_install_*.)
static void cmd_app(api_req_t *r) {
    if (!strcmp(r->args, "on")) {
        if (!app_turn_on()) {
            api_error(r, 500, "the setting couldn't be written");
            return;
        }
        if (!r->json) printf("APP on: it runs from the next boot\n");
    } else if (!strcmp(r->args, "remove")) {
        if (!app_remove()) {
            api_error(r, 409, "no app to remove (or one built in, or installing)");
            return;
        }
    } else if (r->args[0]) {
        api_error(r, 400, "app [on|remove|install <size>]");
        return;
    }
    if (r->json) {
        app_json(r->out, (uint32_t) r->size, true);
        r->code = 200;
    } else {
        size_t size;
        char *j = api_buffer(&size);
        app_text(j, (uint32_t) size);
        printf("%s\n", j);
    }
}

static void cmd_app_on(api_req_t *r) {
    r->args = "on";
    cmd_app(r);
}

static void cmd_app_remove(api_req_t *r) {
    r->args = "remove";
    cmd_app(r);
}

static void cmd_program(api_req_t *r) {
    const char *pj = app_program_json();
    if (!pj) {
        api_error(r, 404, "no program built in");
        return;
    }
    if (r->json) api_reply(r, 200, "%s", pj);
    else printf("PROGRAM %s\n", pj);
}

// a command for the program: {"reply": "..."}
static void cmd_command(api_req_t *r) {
    char cmd[256], reply[256] = "";
    snprintf(cmd, sizeof cmd, "%s", r->args);
    for (char *e = cmd + strlen(cmd); e > cmd && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ');) *--e = 0;
    if (!app_command(cmd, reply, sizeof reply)) {
        api_error(r, 404, "command not taken");
        return;
    }
    if (r->json) {
        char q[300];
        api_json_str(q, sizeof q, reply);
        api_reply(r, 200, "{\"reply\":%s}", q);
    } else if (reply[0]) {
        printf("%s\n", reply);
    }
}

// ---- The commands

static const api_cmd_t s_cmds[] = {
    {"status", NULL, false, cmd_status},
    {"machine", NULL, true, cmd_machine},
    {"stats", NULL, false, cmd_stats},
    {"keys", NULL, true, cmd_keys},
    {"turbo", NULL, true, cmd_turbo},
    {"overlay", NULL, true, cmd_overlay},
    {"boot", NULL, true, cmd_boot},
    {"break", NULL, true, cmd_break},
    {"eject", NULL, true, cmd_eject},
    {"discs", NULL, false, cmd_discs},
    {"sd", NULL, true, cmd_sd},
    {"restore", "/api/disc/restore", true, cmd_restore},
#if BOARD_HAS_NET
    {"net", NULL, false, cmd_net},
    {"wifi", NULL, true, cmd_wifi},
#endif
#if WITH_AMBIENT
    {"ambient", NULL, true, cmd_ambient},
#endif
#if WITH_SOUND
    {"sound", NULL, true, cmd_sound},
#endif
    {"app", NULL, false, cmd_app},
    {"app on", "/api/app/on", true, cmd_app_on},
    {"app remove", "/api/app/remove", true, cmd_app_remove},
    {"program", NULL, false, cmd_program},
    {"command", NULL, true, cmd_command},
};

const api_cmd_t *api_find(const char *name) {
    for (size_t i = 0; i < sizeof s_cmds / sizeof s_cmds[0]; i++) {
        if (!strcmp(name, s_cmds[i].name)) return &s_cmds[i];
    }
    return NULL;
}

const api_cmd_t *api_find_http(const char *path, bool post) {
    for (size_t i = 0; i < sizeof s_cmds / sizeof s_cmds[0]; i++) {
        const api_cmd_t *c = &s_cmds[i];
        bool match = c->http ? !strcmp(path, c->http) : (!strncmp(path, "/api/", 5) && !strcmp(path + 5, c->name));
        if (match && c->post == post) return c;
    }
    return NULL;
}

void api_run(const api_cmd_t *c, api_req_t *r) {
    if (!r->args) r->args = "";
    if (!r->query) r->query = "";
    r->code = 200;
    if (r->json && r->out && r->size) r->out[0] = 0;
    c->fn(r);
    if (r->json && r->out && r->size && !r->out[0]) snprintf(r->out, r->size, "{}");
}

bool api_call(const char *name, api_req_t *r) {
    const api_cmd_t *c = api_find(name);
    if (!c) return false;
    api_run(c, r);
    return true;
}
