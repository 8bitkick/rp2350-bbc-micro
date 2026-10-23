// The app run alongside the machine (app_api.h): the loader, and what's common to the machines.
//
// At power on the machine calls app_load. It finds the app: one compiled in (its header,
// app_header, linked into the firmware), or else the one in the app slot, which it checks
// (the magic, the API version, the size and CRC, the machine, the RAM and stack it needs, the
// disc, and that it didn't stop the machine at the last boot) and refuses with the reason if
// anything's wrong. An app it takes gets its RAM set up (.data copied in, .bss zeroed), and its
// disc, if it carries one, becomes the machine's built-in disc. Its init is called at the first
// frame, with the app_api; then its frame each frame (app_frame), its command for any command
// the firmware doesn't know (api.c: command; frontend.c: the USB channel's), and its
// program_json for the web panel.
//
// A program built in the first way, with machine_hook.h's functions (machine_frame_hook and the
// rest), is an app too: a header made from those, if the build has them.
//
// Installing: the running firmware writes an app into the slot (app_install_*: POST /api/app, and
// "!app install" over USB), sector by sector as it arrives, the header's sector last, once the
// whole image has been checked against it (size, CRC, API version, a machine there is): an
// upload that fails or stops halfway leaves no app, never a broken one. The board restarts into
// it. "!app remove" (DELETE /api/app) erases the header.
//
// The app guard: every call into the app is marked (boot_guard.h), and for its first minute the
// watchdog is armed (unless the machine runs its own). If the board resets while the app is
// running (it hung, or faulted and the watchdog came), twice in a row, the next boot turns it off,
// in the settings sector (system.h), until another app is installed or "!app on".
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/qmi.h"

#include "app.h"
#include "app_api.h"
#include "boot_guard.h"
#include "machine_if.h"
#include "mem.h"
#include "system.h"

// ---- Built in

// an app compiled in (APP_HEADER, in a build with -DAPP_SOURCES=...)
extern const struct app_header app_header __attribute__((weak));
// or a program built in with machine_hook.h's functions
extern void machine_frame_hook(const struct machine_hook_io *io) __attribute__((weak));
extern bool machine_command_hook(const char *cmd, char *reply, uint32_t reply_size) __attribute__((weak));
extern const char *machine_program_json(void) __attribute__((weak));
extern const struct machine_boot_image *machine_boot_image(void) __attribute__((weak));
static const struct app_header s_hooks = {
    .magic = APP_MAGIC, .api_version = APP_API_VERSION, .header_size = sizeof (struct app_header),
    .name = "built-in program", .version = "-",
    .boot_image = machine_boot_image, .frame = machine_frame_hook, .command = machine_command_hook,
    .program_json = machine_program_json,
};

// ---- The state

static const struct app_header *s_app;      // the app taken (NULL: none)
static uint32_t s_id;                       // its id: its CRC (1: built in)
static bool s_builtin, s_started;
static char s_state[16] = "none";           // none, loaded, running, refused, off, stopped,
                                            // installing, installed
static char s_why[120];                     // why it's not running
static bool s_overflowed;                   // (turned off for a stack overflow at the last boot)
// (the Master's stack guard, stack_guard.c; none elsewhere)
__attribute__((weak)) uint32_t stack_guard_app_overflow(void) { return 0; }
static uint32_t s_probation;                // frames left with the watchdog armed for it (0: not)
static struct app_api s_api;
#define APP_PROBATION (60 * 50)

// its log: the last lines it wrote (api->log), for the panel
#define LOG_LINES 8
static char s_log[LOG_LINES][96];
static uint32_t s_log_n;

static void refuse(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void refuse(const char *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_why, sizeof s_why, fmt, ap);
    va_end(ap);
    snprintf(s_state, sizeof s_state, "%s", state);
    s_app = NULL;
    printf("APP %s: %s\n", state, s_why);
}

static const char *model_name(uint32_t m) {
    switch (m) {
    case MACHINE_MODEL_BBC_B: return "BBC Model B";
    case MACHINE_MODEL_BBC_B_PLUS: return "BBC Model B+";
    case MACHINE_MODEL_MASTER_128: return "BBC Master 128";
    case 0: return "any machine";
    default: return "an unknown machine";
    }
}
static const char *model_id(uint32_t m) {
    return m == MACHINE_MODEL_MASTER_128 ? "master" : m == MACHINE_MODEL_BBC_B ? "beeb" : m == 0 ? "any" : "other";
}

// ---- The services (app_api)

static void api_log(const char *line) {
    printf("%s\n", line);
    snprintf(s_log[s_log_n++ % LOG_LINES], sizeof s_log[0], "%s", line);
}
static uint32_t api_time_us(void) { return time_us_32(); }

// ---- Loading

// CRC-32 (IEEE, as zlib's), a nibble at a time
static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint32_t n) {
    static const uint32_t t[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 15];
        c = (c >> 4) ^ t[c & 15];
    }
    return c;
}
static uint32_t fnv(const uint8_t *p, uint32_t n) {       // (drive.c's disc hash)
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static bool in_slot(uint32_t at, uint32_t size, uint32_t image_size) {
    return at >= APP_SLOT_XIP && at - APP_SLOT_XIP <= image_size && size <= image_size - (at - APP_SLOT_XIP);
}

// ---- The app's RAM: the top of the SRAM, kept from the heap. The heap's limit is set at the first
// allocation (before the loader runs) from the slot's header, if it looks like an app with RAM; the
// loader gives it back to the heap if it refuses the app (app_ram_release)

#if APP_RAM
static char *s_heap_end, *s_heap_limit;

// where the slot's app wants its RAM (APP_RAM_TOP: nowhere); only its header looked at
static uint32_t slot_ram_base(void) {
    qmi_hw->atrans[3] = (0x400u << QMI_ATRANS3_SIZE_LSB) | (0xc00u << QMI_ATRANS3_BASE_LSB);    // (app_load's)
    const struct app_header *h = (const struct app_header *) APP_SLOT_XIP;
    uint32_t need = h->data_size + h->bss_size;
    if (h->magic != APP_MAGIC || h->api_version != APP_API_VERSION || !need || need > APP_RAM_MAX ||
        h->ram_base < APP_RAM_TOP - APP_RAM_MAX || h->ram_base + need > APP_RAM_TOP || (h->ram_base & 7))
        return APP_RAM_TOP;
    return h->ram_base;
}

// the heap, up to the app's RAM (the SDK's own _sbrk goes up to the stacks, __StackLimit)
void *_sbrk(int incr) {
    extern char end;
    if (!s_heap_end) {
        s_heap_end = &end;
        s_heap_limit = (char *) slot_ram_base();
    }
    char *prev = s_heap_end;
    if (s_heap_end + incr > s_heap_limit) return (char *) -1;
    s_heap_end += incr;
    return prev;
}

char *app_heap_limit(void) {
    if (!s_heap_end) free(malloc(1));       // (the limit is set by the first allocation)
    return s_heap_limit;
}

static void app_ram_release(void) {
    s_heap_limit = (char *) APP_RAM_TOP;
}

static bool ram_fits(const struct app_header *h) {
    return h->ram_base >= APP_RAM_TOP - APP_RAM_MAX && h->ram_base + h->data_size + h->bss_size <= APP_RAM_TOP &&
           !(h->ram_base & 7) && (char *) h->ram_base >= s_heap_end && (char *) h->ram_base >= app_heap_limit();
}
#else
char *app_heap_limit(void) {
    extern char __StackLimit;
    return &__StackLimit;
}

static void app_ram_release(void) {}
static bool ram_fits(const struct app_header *h) { (void) h; return false; }
#endif

// the slot's app checked (why not in s_why): true to take it
static bool check_slot(const struct app_header *h, uint32_t model, const uint8_t *disc, uint32_t disc_size) {
    if (h->magic == 0xffffffffu) {
        snprintf(s_why, sizeof s_why, "the app slot is empty");
        return false;
    }
    if (h->magic != APP_MAGIC) {
        refuse("refused", "the app slot holds no app (no header: %08lx)", (unsigned long) h->magic);
        return false;
    }
    if (h->api_version != APP_API_VERSION) {
        refuse("refused", "built for app API %u, this firmware has %u", h->api_version, APP_API_VERSION);
        return false;
    }
    if (h->header_size < sizeof (struct app_header) || h->image_size < h->header_size || h->image_size > APP_SLOT_SIZE) {
        refuse("refused", "its header is damaged (header %u bytes, image %lu)", h->header_size, (unsigned long) h->image_size);
        return false;
    }
    // (read through the uncached alias: the check doesn't fill the cache with the whole slot)
    const uint8_t *raw = (const uint8_t *) (XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + APP_SLOT_OFFSET);
    uint32_t at = offsetof(struct app_header, crc), zero = 0;
    uint32_t c = crc32_add(0xffffffffu, raw, at);
    c = crc32_add(c, (const uint8_t *) &zero, 4);
    c = ~crc32_add(c, raw + at + 4, h->image_size - at - 4);
    if (c != h->crc) {
        refuse("refused", "the image is damaged (CRC %08lx, its header says %08lx)", (unsigned long) c, (unsigned long) h->crc);
        return false;
    }
    if (h->machine && h->machine != model) {
        refuse("refused", "built for the %s, this is the %s", model_name(h->machine), model_name(model));
        return false;
    }
    if (h->crc == system_app_off()) {
        if (s_overflowed) refuse("off", "it overflowed core 0's stack at the last boot (\"!app on\" to try again)");
        else refuse("off", "it stopped the machine at the last boot (\"!app on\" to try again)");
        return false;
    }
#if APP_RAM
    const uint32_t ram = APP_RAM_MAX;
#else
    const uint32_t ram = 0;
#endif
    uint32_t need = h->data_size + h->bss_size;
    if (need > ram || (h->data_size && !in_slot(h->data_load, h->data_size, h->image_size))) {
        refuse("refused", "it needs %lu KB of app RAM, this machine has %lu KB",
               (unsigned long) (need + 1023) / 1024, (unsigned long) ram / 1024);
        return false;
    }
    if (need && !ram_fits(h)) {
        refuse("refused", "its RAM isn't where it can be (%08lx, %lu bytes: the firmware's heap there already?)",
               (unsigned long) h->ram_base, (unsigned long) need);
        return false;
    }
    if (h->stack_size > APP_STACK_SIZE) {
        refuse("refused", "it needs %lu bytes of stack, apps get %u", (unsigned long) h->stack_size, APP_STACK_SIZE);
        return false;
    }
    if (h->disc_size ? !in_slot(h->disc_load, h->disc_size, h->image_size) || fnv((const uint8_t *) h->disc_load, h->disc_size) != h->disc_hash
                     : h->disc_hash && (!disc || fnv(disc, disc_size) != h->disc_hash)) {
        refuse("refused", "it needs the disc \"%.32s\", %s", h->disc,
               h->disc_size ? "and the one it carries is damaged" : "which this firmware hasn't got");
        return false;
    }
    return true;
}

static void load(uint32_t model, const uint8_t **disc, uint32_t *disc_size) {
    // the slot at its own address: a machine booted into its slot by the launcher sees the flash
    // through the boot ROM's address translation, its slot at 0x10000000; the top 4 MB window
    // goes back to the flash's own top 4 MB (its reset value), so the app runs where it was
    // linked, through the cache
    qmi_hw->atrans[3] = (0x400u << QMI_ATRANS3_SIZE_LSB) | (0xc00u << QMI_ATRANS3_BASE_LSB);
    uint32_t died = boot_guard_app_died();
    if (&app_header || machine_frame_hook || machine_command_hook || machine_program_json || machine_boot_image) {
        s_app = &app_header ? &app_header : &s_hooks;
        s_builtin = true;
        s_id = 1;
        if (died == s_id) system_set_app_off(s_id);
        if (system_app_off() == s_id) {
            refuse("off", "it stopped the machine at the last boot (\"!app on\" to try again)");
            return;
        }
        if (s_app->machine && s_app->machine != model) {
            refuse("refused", "built for the %s, this is the %s", model_name(s_app->machine), model_name(model));
            return;
        }
    } else {
        const struct app_header *h = (const struct app_header *) APP_SLOT_XIP;
        if (died && died == h->crc) {
            printf("APP it stopped the machine at the last boot: turned off\n");
            system_set_app_off(died);
        }
        // (core 0's stack overflowed while it ran: turned off at once, stack_guard.c; a hang takes two boots)
        uint32_t ovf = stack_guard_app_overflow();
        if (ovf && ovf == h->crc) {
            printf("APP it overflowed core 0's stack at the last boot: turned off\n");
            system_set_app_off(ovf);
            s_overflowed = true;
        }
        if (!check_slot(h, model, disc ? *disc : NULL, disc_size ? *disc_size : 0)) return;
        s_app = h;
        s_id = h->crc;
        // its RAM
        memcpy((void *) h->ram_base, (const void *) h->data_load, h->data_size);
        memset((uint8_t *) h->ram_base + h->data_size, 0, h->bss_size);
        if (h->disc_size && disc && disc_size) {
            *disc = (const uint8_t *) h->disc_load;
            *disc_size = h->disc_size;
        }
    }
    snprintf(s_state, sizeof s_state, "loaded");
    s_why[0] = 0;
    printf("APP %.24s %.24s (%s)%s%.32s\n", s_app->name, s_app->version, s_builtin ? "built in" : "from the app slot",
           s_app->disc[0] ? ", disc " : "", s_app->disc);
}

void app_load(uint32_t model, const uint8_t **disc, uint32_t *disc_size) {
    load(model, disc, disc_size);
    if (!s_app || s_builtin) app_ram_release();     // (no app from the slot: the heap has its RAM)
}

// ---- Calling it

static inline void enter(void) { boot_guard_app_enter(s_id); }
static inline void leave(void) { boot_guard_app_leave(); }

static void start(const struct machine_hook_io *io) {
    s_started = true;
    s_api = (struct app_api) {
        .version = APP_API_VERSION, .size = sizeof s_api, .io = io, .log = api_log, .time_us = api_time_us,
        .turbo_get = machine_turbo_get, .turbo_set = machine_turbo_set,
#if APP_RAM
        .ram = s_builtin ? NULL : (void *) s_app->ram_base, .ram_size = s_builtin ? 0 : APP_RAM_TOP - s_app->ram_base,
#endif
    };
    // the watchdog for its first minute, unless the machine runs one of its own (the boot guard's
    // builds: the Model B). (Not by its enable bit: a watchdog reboot leaves that set, its tick
    // stopped.)
#if !BOOT_GUARD
    watchdog_enable(8000, true);
    s_probation = APP_PROBATION;
#endif
    enter();
    bool ok = !s_app->init || s_app->init(&s_api);
    leave();
    if (!ok) {
        refuse("stopped", "it wouldn't start (its log says why)");
        return;
    }
    snprintf(s_state, sizeof s_state, "running");
}

void app_frame(struct machine_hook_io *io) {
    io->frame++;
    if (!s_app) return;
    if (!s_started) {
        start(io);
        if (!s_app) return;
    }
    if (s_probation) {
        watchdog_update();
        if (!--s_probation) watchdog_disable();
    }
    if (s_app->frame) {
        enter();
        s_app->frame(io);
        leave();
    }
}

bool app_command(const char *cmd, char *reply, uint32_t reply_size) {
    if (!s_app || !s_started || !s_app->command) return false;
    enter();
    bool took = s_app->command(cmd, reply, reply_size);
    leave();
    return took;
}

const char *app_program_json(void) {
    if (!s_app || !s_started || !s_app->program_json) return NULL;
    enter();
    const char *j = s_app->program_json();
    leave();
    return j;
}

const struct machine_boot_image *app_boot_image(void) {
    if (!s_app || !s_app->boot_image) return NULL;
    enter();
    const struct machine_boot_image *im = s_app->boot_image();
    leave();
    return im;
}

// ---- Installing

static struct {
    bool busy;
    uint32_t size, got;
    uint8_t *sector, *first;    // the sector filling up; the header's, written last
} s_in;

// the app stopped, before its slot is written over: nothing more is called there, and the keys it
// held let go (they stayed down: the game flew on, a roll held, through the upload)
static void stop_app(const char *state) {
    if (s_probation) {
        watchdog_disable();
        s_probation = 0;
    }
    if (s_app)
        for (int row = 0; row < 8; row++)           // (the BBC's matrix: 8 rows of 10)
            for (int col = 0; col < 10; col++) machine_key(row << 4 | col, false);
    s_app = NULL;
    snprintf(s_state, sizeof s_state, "%s", state);
}

static void install_free(void) {
    free(s_in.sector);
    free(s_in.first);
    s_in.sector = s_in.first = NULL;
    s_in.busy = false;
}

bool app_install_begin(uint32_t size, char *why, uint32_t why_size) {
    const char *no = s_in.busy ? "already installing one" : s_builtin ? "this firmware has an app built in"
                   : size < sizeof (struct app_header) || size > APP_SLOT_SIZE ? "that's not the size of an app (at most 1 MB)" : NULL;
    if (!no) {
        s_in.sector = malloc(FLASH_SECTOR_SIZE);
        s_in.first = malloc(FLASH_SECTOR_SIZE);
        if (!s_in.sector || !s_in.first) no = "no RAM for it";
        else memset(s_in.first, 0, FLASH_SECTOR_SIZE);      // (no header yet: the install screen asks)
    }
    if (no) {
        if (!s_in.busy) install_free();
        snprintf(why, why_size, "%s", no);
        return false;
    }
    s_in.busy = true;
    s_in.size = size;
    s_in.got = 0;
    stop_app("installing");
    s_why[0] = 0;
    machine_install_screen("INSTALLING A PROGRAM", 0);     // (the machine paused till the restart)
    // no app from here until it's all there and checked: the header's sector goes first
    mem_flash_write_sector(APP_SLOT_OFFSET, NULL, 0);
    printf("APP installing %lu bytes\n", (unsigned long) size);
    return true;
}

// the sector filled up so far (the last one padded): into the flash, or kept (the header's)
static bool install_sector(void) {
    uint32_t at = (s_in.got - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE, n = s_in.got - at;
    memset(s_in.sector + n, 0xff, FLASH_SECTOR_SIZE - n);
    if (!at) {
        memcpy(s_in.first, s_in.sector, FLASH_SECTOR_SIZE);
        return true;
    }
    return mem_flash_write_sector(APP_SLOT_OFFSET + at, s_in.sector, FLASH_SECTOR_SIZE);
}

bool app_install_data(const uint8_t *p, uint32_t n) {
    if (!s_in.busy) return false;
    while (n) {
        uint32_t k = FLASH_SECTOR_SIZE - s_in.got % FLASH_SECTOR_SIZE;
        if (k > n) k = n;
        if (k > s_in.size - s_in.got) return false;     // (more than it said)
        memcpy(s_in.sector + s_in.got % FLASH_SECTOR_SIZE, p, k);
        s_in.got += k;
        p += k;
        n -= k;
        if ((s_in.got % FLASH_SECTOR_SIZE == 0 || s_in.got == s_in.size)) {
            if (!install_sector()) return false;
            if (s_in.got % (64 * 1024) < FLASH_SECTOR_SIZE) printf("APP installing: %lu of %lu bytes\n", (unsigned long) s_in.got, (unsigned long) s_in.size);
            // on the screen: its name and version once its header's here, and how far
            const struct app_header *h = (const struct app_header *) s_in.first;
            char what[40];
            if (h->magic == APP_MAGIC) snprintf(what, sizeof what, "INSTALLING %.12s %.8s", h->name, h->version);
            else snprintf(what, sizeof what, "INSTALLING A PROGRAM");
            for (char *c = what; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
            machine_install_screen(what, (int) ((uint64_t) s_in.got * 1000 / s_in.size));
        }
    }
    return true;
}

void app_install_abort(const char *why) {
    if (!s_in.busy) return;
    install_free();
    machine_install_screen(NULL, -1);       // (the machine carries on, with no app until the next boot)
    snprintf(s_state, sizeof s_state, "none");
    snprintf(s_why, sizeof s_why, "the last install failed: %s", why);
    printf("APP install failed: %s\n", why);
}

bool app_install_end(char *why, uint32_t why_size) {
    if (!s_in.busy) {
        snprintf(why, why_size, "not installing");
        return false;
    }
    const struct app_header *h = (const struct app_header *) s_in.first;
    const char *no = NULL;
    if (s_in.got != s_in.size) no = "it stopped short";
    else if (h->magic != APP_MAGIC) no = "it's not an app (no header)";
    else if (h->api_version != APP_API_VERSION) no = "it's for another app API version";
    else if (h->header_size < sizeof (struct app_header) || h->image_size != s_in.size) no = "its size isn't its header's";
    else if (h->machine != 0 && h->machine != MACHINE_MODEL_MASTER_128 && h->machine != MACHINE_MODEL_BBC_B) no = "it's for a machine there isn't";
    else {
        // the CRC: the header's sector as it came, the rest as the flash has it now
        const uint8_t *raw = (const uint8_t *) (XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + APP_SLOT_OFFSET);
        uint32_t at = offsetof(struct app_header, crc), zero = 0, first = s_in.size < FLASH_SECTOR_SIZE ? s_in.size : FLASH_SECTOR_SIZE;
        uint32_t c = crc32_add(0xffffffffu, s_in.first, at);
        c = crc32_add(c, (const uint8_t *) &zero, 4);
        c = crc32_add(c, s_in.first + at + 4, first - at - 4);
        c = ~crc32_add(c, raw + first, s_in.size - first);
        if (c != h->crc) no = "its CRC is wrong (damaged on the way?)";
    }
    if (!no && !mem_flash_write_sector(APP_SLOT_OFFSET, s_in.first, FLASH_SECTOR_SIZE)) no = "the flash couldn't be written";
    if (no) {
        snprintf(why, why_size, "%s", no);
        app_install_abort(no);
        return false;
    }
    printf("APP installed %.24s %.24s (for the %s): restarting\n", h->name, h->version, model_name(h->machine));
    machine_install_screen("RESTARTING", 1000);
    install_free();
    // (an app the guard turned off: this one's another, or the same again, wanted on)
    if (system_app_off()) system_set_app_off(0);
    snprintf(s_state, sizeof s_state, "installed");
    snprintf(s_why, sizeof s_why, "restarting into it");
    return true;
}

void app_usb_install(const char *args) {
    // "<size>", then exactly <size> bytes of the image (presto.py app install)
    char why[80];
    uint32_t size = (uint32_t) strtoul(args, NULL, 10);
    if (!app_install_begin(size, why, sizeof why)) {
        printf("APP install error: %s\n", why);
        return;
    }
    printf("APP ready\n");
    fflush(stdout);
    static uint8_t buf[256];
    while (s_in.got < size) {
        uint32_t n = 0, want = size - s_in.got < sizeof buf ? size - s_in.got : sizeof buf;
        while (n < want) {
            int c = getchar_timeout_us(2000000);
            if (c == PICO_ERROR_TIMEOUT) break;
            buf[n++] = (uint8_t) c;
        }
        watchdog_update();      // (a machine with a running watchdog: this takes a while)
        if (n < want || !app_install_data(buf, n)) {
            app_install_abort("it stopped short");
            printf("APP install error: stopped after %lu bytes\n", (unsigned long) s_in.got);
            return;
        }
    }
    if (!app_install_end(why, sizeof why)) {
        printf("APP install error: %s\n", why);
        return;
    }
    fflush(stdout);
    sleep_ms(100);
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

bool app_remove(void) {
    if (s_in.busy || s_builtin) return false;
    stop_app("none");
    snprintf(s_why, sizeof s_why, "removed");
    printf("APP removed\n");
    return mem_flash_write_sector(APP_SLOT_OFFSET, NULL, 0);
}

// ---- What it is

uint32_t app_flash_used(void) {
    const struct app_header *h = (const struct app_header *) APP_SLOT_XIP;
    return h->magic == APP_MAGIC && h->image_size <= APP_SLOT_SIZE ? h->image_size : 0;
}

bool app_turn_on(void) {
    return system_set_app_off(0);
}

static int json_str(char *o, uint32_t n, const char *s, uint32_t max) {
    uint32_t k = 0;
    if (n < 3) return 0;
    o[k++] = '"';
    for (uint32_t i = 0; i < max && s[i] && k + 3 < n; i++) {
        if (s[i] == '"' || s[i] == '\\') o[k++] = '\\';
        o[k++] = (unsigned char) s[i] < 0x20 ? ' ' : s[i];
    }
    o[k++] = '"';
    o[k] = 0;
    return (int) k;
}

// the header to describe: the app's, or a refused one's (what it says it is), or none
static const struct app_header *described(void) {
    if (s_app) return s_app;
    if (s_builtin) return &app_header ? &app_header : &s_hooks;
    const struct app_header *h = (const struct app_header *) APP_SLOT_XIP;
    return h->magic == APP_MAGIC ? h : NULL;
}

void app_json(char *o, uint32_t size, bool full) {
    const struct app_header *h = described();
    char name[64] = "null", version[64] = "null", why[200] = "null", disc[80] = "null";
    if (h) {
        json_str(name, sizeof name, h->name, sizeof h->name);
        json_str(version, sizeof version, h->version, sizeof h->version);
        if (h->disc[0]) json_str(disc, sizeof disc, h->disc, sizeof h->disc);
    }
    if (s_why[0]) json_str(why, sizeof why, s_why, sizeof s_why);
    int n = snprintf(o, size, "{\"state\":\"%s\",\"why\":%s,\"name\":%s,\"version\":%s,\"machine\":\"%s\",\"from\":\"%s\"",
                     s_state, why, name, version, h ? model_id(h->machine) : "none", s_builtin ? "built in" : "slot");
    if (full && n > 0 && (uint32_t) n < size) {
        uint32_t ram = s_app && !s_builtin ? s_app->data_size + s_app->bss_size : 0;
#if APP_RAM
        const uint32_t ram_size = APP_RAM_MAX;
#else
        const uint32_t ram_size = 0;
#endif
        n += snprintf(o + n, size - n, ",\"disc\":%s,\"ram\":[%lu,%lu],\"flash\":[%lu,%lu],\"log\":[", disc,
                      (unsigned long) ram, (unsigned long) ram_size, (unsigned long) app_flash_used(),
                      (unsigned long) APP_SLOT_SIZE);
        uint32_t first = s_log_n > LOG_LINES ? s_log_n - LOG_LINES : 0;
        for (uint32_t i = first; i < s_log_n && n > 0 && (uint32_t) n + 200 < size; i++) {
            if (i > first) o[n++] = ',';
            n += json_str(o + n, size - n, s_log[i % LOG_LINES], sizeof s_log[0]);
        }
        if (n > 0 && (uint32_t) n + 2 < size) o[n++] = ']';
    }
    if (n > 0 && (uint32_t) n + 2 < size) {
        o[n++] = '}';
        o[n] = 0;
    }
}

void app_text(char *o, uint32_t size) {
    const struct app_header *h = described();
    int n = snprintf(o, size, "APP %s", s_state);
    if (h && n > 0 && (uint32_t) n < size)
        n += snprintf(o + n, size - n, " %.24s %.24s (for the %s, %s)", h->name, h->version, model_name(h->machine),
                      s_builtin ? "built in" : "in the app slot");
    if (s_why[0] && n > 0 && (uint32_t) n < size) snprintf(o + n, size - n, ": %s", s_why);
}
