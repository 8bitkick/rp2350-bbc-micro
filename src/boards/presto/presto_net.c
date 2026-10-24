/*
 * The Presto's network (net_if.h): WiFi through its RM2 module (CYW43439), DHCP, mDNS
 * (presto-xxxx.local), the WiFi network saved in flash, WiFi setup, and a small HTTP server: the web
 * page and /api/<name>, each request carried out by the command layer (src/common/api), so the page
 * and the USB channel (tools/presto.py) share every command.
 *
 * The SDK's "poll" mode: no interrupt handler and no background work, lwIP only ever runs inside
 * net_poll / net_frame on core 0. A request is read in as packets arrive, then left until the gap
 * between frames to act on it, where it may press keys, load a disc or reset the machine without
 * any locking.
 *
 *   GET  /                      the page (src/common/api/web/index.html, gzipped into flash)
 *   GET|POST /api/<name>        a command (api.c's table: status, keys, turbo, overlay, boot,
 *                               eject, discs, sd, disc/restore, wifi, ambient, program, command,
 *                               machine, ...), its JSON reply; the body is its arguments. POST
 *                               /api/wifi ("<ssid>\n<password>") joins that network once someone
 *                               at the Presto says so on its screen.
 * and three that move an image in, so are the transport's own:
 *   POST /api/disc?name=&save=&boot=  body: a disc image to boot (save=1 also writes it to the card;
 *                               boot=0 only writes it to the card)
 *   POST /api/fetch             body: a URL for the Presto to download a disc image from and boot
 *   POST /api/app               body: an app's image (its .bin, src/common/app_api.h), written into
 *                               the app slot as it arrives (app.c: app_install_*); checked, then the
 *                               board restarts into it. (DELETE /api/app is POST /api/app/remove.)
 *
 * At power on the machine waits (frontend_start: net_boot_ready) while WiFi joins, behind a loading
 * screen; once it has, the screen shows the web panel's address for 20 seconds (the address card),
 * and then the machine starts, so it's seen booting from the start.
 *
 * WiFi setup. With no network saved (or when asked for: a finger held on the screen at power on,
 * or !wifisetup), the Presto becomes an access point, "Presto-XXXX" (the end of the board's
 * unique ID), WPA2 with a random password made the first time and kept in flash. The password is
 * only ever shown on the screen, with a QR code to join. A DHCP server hands out 192.168.4.16-23,
 * and a DNS server answers every name with 192.168.4.1, so phones see a captive portal and open
 * the setup page, plain HTML made here:
 *   GET  http://192.168.4.1/    the setup page: the networks nearby, and a form (?rescan: look again)
 *   POST /join                  the form: s, the network, or o, another's name; p, its password
 *   POST /api/wifi              "<ssid>\n<password>": join it
 *   GET  /api/status            as ever
 *   anything else               redirected to http://192.168.4.1/
 * It joins with the access point still up, and only then saves the network; failing, the setup
 * screen comes back with why. Skip on the screen, or 10 minutes, ends setup with WiFi off. The
 * machine starts once setup is done with; until then, setup's memory is the machine's framebuffer
 * (presto_screen.h: presto_alloc), idle until it starts. Passwords never go out: not in any reply,
 * not to USB (the screen showing one isn't in a !shot either).
 */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <malloc.h>
#include <unistd.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/apps/mdns.h"
#include "net_if.h"
#include "mem.h"
#include "api.h"
#include "board.h"
#include "drive.h"
#include "boot_guard.h"
#include "presto_net.h"
#include "presto_screen.h"
#include "presto_touch.h"
#include "lib/dhcpserver.h"
#include "lib/dnsserver.h"

// The board's names, from the end of its unique ID (so they're its own, whatever it runs): its
// host name, presto-xxxx (presto-xxxx.local), and, as the setup access point, Presto-XXXX
static char host_name[16], board_name[16];

static void make_names(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(host_name, sizeof(host_name), "presto-%02x%02x", id.id[6], id.id[7]);
    snprintf(board_name, sizeof(board_name), "Presto-%02X%02X", id.id[6], id.id[7]);
}

extern const uint8_t presto_web_page[];     // src/common/api/web/index.html, gzipped (presto_web_page.c, generated)
extern const uint32_t presto_web_page_len;

// downloads and uploads land here in the disc RAM, clear of the disc in drive 0 at its start
// (a machine that copies a disc in when it's inserted can use 0)
#ifndef PRESTO_DISC_STAGE_OFFSET
#define PRESTO_DISC_STAGE_OFFSET (1024 * 1024)
#endif
#define STAGE_OFFSET PRESTO_DISC_STAGE_OFFSET

// ---------------------------------------------------------------------------------------
// The saved settings: the flash layout's WiFi sector (mem.h). The network to join in its first
// page, the setup access point's password in its second (the Acorn Archimedes for the Presto keeps
// them the same way, in the same sector).

#define CRED_MAGIC 0x49464957u      // "WIFI"
#define AP_MAGIC 0x57504150u        // "PAPW"
typedef struct {
    uint32_t magic;
    char ssid[33];
    char pass[65];
} cred_t;

#define AP_PASS_LEN 10
typedef struct {
    uint32_t magic;
    char pass[AP_PASS_LEN + 1];
} ap_cred_t;

static const uint8_t *settings(void) {
    uint32_t off, size;
    if (!mem_flash_region(MEM_FLASH_WIFI, &off, &size)) return NULL;
    return mem_flash_ptr(off);
}

static const cred_t *saved_cred(void) {
    const cred_t *c = (const cred_t *) settings();
    if (!c || c->magic != CRED_MAGIC || !memchr(c->ssid, 0, sizeof(c->ssid)) || !memchr(c->pass, 0, sizeof(c->pass)) || !c->ssid[0]) {
        return NULL;
    }
    return c;
}

static const ap_cred_t *saved_ap_cred(void) {
    const ap_cred_t *a = settings() ? (const ap_cred_t *) (settings() + FLASH_PAGE_SIZE) : NULL;
    if (!a || a->magic != AP_MAGIC || strnlen(a->pass, sizeof(a->pass)) != AP_PASS_LEN) return NULL;
    return a;
}

// a network (ssid non-NULL), and the access point's password (ap_pass non-NULL); what isn't given
// stays as it was
static bool save_settings(const char *ssid, const char *pass, const char *ap_pass) {
    uint32_t off, size;
    if (ssid && (!*ssid || strlen(ssid) > 32 || strlen(pass) > 64)) return false;
    if (!mem_flash_region(MEM_FLASH_WIFI, &off, &size)) return false;
    // (in SRAM: the flash is out of reach while it's written)
    uint8_t *pages = presto_alloc(2 * FLASH_PAGE_SIZE);
    if (!pages) return false;
    memset(pages, 0xff, 2 * FLASH_PAGE_SIZE);
    if (ssid) {
        cred_t c = {.magic = CRED_MAGIC};
        strcpy(c.ssid, ssid);
        strcpy(c.pass, pass);
        memcpy(pages, &c, sizeof(c));
        memset(&c, 0, sizeof(c));
    } else if (saved_cred()) {
        memcpy(pages, saved_cred(), sizeof(cred_t));
    }
    if (ap_pass) {
        ap_cred_t a = {.magic = AP_MAGIC};
        snprintf(a.pass, sizeof(a.pass), "%s", ap_pass);
        memcpy(pages + FLASH_PAGE_SIZE, &a, sizeof(a));
    } else if (saved_ap_cred()) {
        memcpy(pages + FLASH_PAGE_SIZE, saved_ap_cred(), sizeof(ap_cred_t));
    }
    // core 1 is parked (the machine pauses for a moment; the panel's picture stays) while the flash is busy
    bool ok = mem_flash_write_sector(off, pages, 2 * FLASH_PAGE_SIZE);
    if (!ok) printf("NET couldn't write the flash\n");
    memset(pages, 0, 2 * FLASH_PAGE_SIZE);
    presto_free(pages);
    return ok;
}

bool net_wifi_save(const char *ssid, const char *pass) {
    return save_settings(ssid, pass, NULL);
}

static void restart_soon(void) {
    boot_guard_ok();    // (a restart asked for, before the machine had run long enough to say it's up)
    printf("NET restarting\n");
    fflush(stdout);
    sleep_ms(100);
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

static bool restart_after_reply;

void net_restart(bool after_reply) {
    if (after_reply) restart_after_reply = true;    // (net_frame, once the reply has gone)
    else restart_soon();
}

// Setup on the next start (!wifisetup): asked for through a watchdog scratch register, which a
// watchdog reset (and the launcher, which chains rather than resets) leaves alone
#define NET_SETUP_RESET 0x5e7a0000u

void net_setup_restart(void) {
    boot_guard_ok();
    printf("NET restarting into WiFi setup\n");
    fflush(stdout);
    sleep_ms(100);
    watchdog_hw->scratch[0] = NET_SETUP_RESET;
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

// ---------------------------------------------------------------------------------------
// The link

static bool wifi_on;            // the chip is up and being polled
static int link_status;
static bool announced;
static uint32_t last_poll_us, last_link_check_us, joined_at_us;
static char ssid_ram[33], pass_ram[65];

static void join(void) {
    cyw43_arch_wifi_connect_async(ssid_ram, pass_ram, pass_ram[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    joined_at_us = time_us_32();
}

static void start_server(void);
static void setup_begin(void);
static void setup_poll(void);
static bool net_wanted, net_started, setup_wanted;

// started from net_frame a second after the machine is set up, so it doesn't wait on the WiFi
// chip (its firmware takes a moment to load)
void net_init(void) {
    make_names();
    touch_init();
    net_wanted = true;
    if (watchdog_hw->scratch[0] == NET_SETUP_RESET) {
        watchdog_hw->scratch[0] = 0;
        setup_wanted = true;
    }
}

static void net_start(bool setup) {
    const cred_t *c = saved_cred();
    if (!c) {
        printf("NET no WiFi network saved: WiFi setup\n");
        setup = true;
    }
    boot_guard_step(40);
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_UK)) {
        printf("NET WiFi chip didn't start\n");
        return;
    }
    boot_guard_step(41);
    cyw43_arch_enable_sta_mode();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], host_name);   // (for DHCP)
    boot_guard_step(42);
    // no power saving: it adds a lot of latency to every key press from the web page
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    boot_guard_step(43);
    start_server();
    boot_guard_step(44);
    wifi_on = true;
    if (setup) {
        setup_begin();
        return;
    }
    snprintf(ssid_ram, sizeof(ssid_ram), "%s", c->ssid);
    snprintf(pass_ram, sizeof(pass_ram), "%s", c->pass);
    join();
    boot_guard_step(45);
}

static void check_link(void) {
    int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    if (s == CYW43_LINK_UP && !announced) {
        announced = true;
        static bool mdns_started;
        if (!mdns_started) {
            mdns_started = true;
            mdns_resp_init();
            static char service[40];
            snprintf(service, sizeof(service), "BBC Micro (%s)", host_name);
            mdns_resp_add_netif(nif, host_name);
            mdns_resp_add_service(nif, service, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
        } else {
            mdns_resp_announce(nif);
        }
        printf("NET up http://%s/ (http://%s.local/)\n", ip4addr_ntoa(netif_ip4_addr(nif)), host_name);
    } else if (s != CYW43_LINK_UP && ssid_ram[0]) {
        announced = false;
        // failed, or stuck getting nowhere: try again every 15 seconds
        if ((s < 0 || s == CYW43_LINK_DOWN || time_us_32() - joined_at_us > 30000000u) &&
            time_us_32() - joined_at_us > 15000000u) {
            join();
        }
    }
    link_status = s;
}

// ---------------------------------------------------------------------------------------
// Setup, and the confirmation of a network asked for from the web page: the screen (presto_screen.h)

enum { S_OFF, S_AP, S_JOINING, S_JOINED, S_CONFIRM };

#define SCAN_MAX 12
#define SETUP_US (10 * 60 * 1000000u)   // the access point goes after this long
#define JOIN_US (30 * 1000000u)
#define JOINED_US (15 * 1000000u)     // on the screen
#define AP_AFTER_US (30 * 1000000u)   // the access point stays this long after joining
#define CONFIRM_US (30 * 1000000u)

typedef struct {
    uint8_t state;
    bool ap_up, scanning, from_ap;
    char ap_ssid[20], ap_pass[AP_PASS_LEN + 1];
    char ssid[33], pass[65];    // the network being joined, or to be confirmed
    char error[32];
    uint32_t began_us, state_us;
    dhcp_server_t dhcp;
    dns_server_t dns;
    uint8_t nscan;
    struct {
        char ssid[33];
        int8_t rssi;
        bool secure;
    } scan[SCAN_MAX];
} setup_t;

static setup_t *setup;          // allocated while the screen's up

#define C_BG SCREEN_BLACK
#define C_TEXT SCREEN_WHITE
#define C_NAME SCREEN_YELLOW
#define C_ERROR SCREEN_RED
#define C_GOOD SCREEN_GREEN
#define C_LINK SCREEN_CYAN
#define C_BUTTON SCREEN_BLUE
#define C_BUTTON2 SCREEN_MAGENTA
#define C_RULE SCREEN_BLUE

// the screen's buttons
#define SKIP_X 140
#define SKIP_Y 404
#define SKIP_W 200
#define SKIP_H 64
#define YES_X 40
#define NO_X 260
#define YESNO_Y 300
#define YESNO_W 180
#define YESNO_H 80

static void button(int x, int y, int w, int h, int colour, const char *label, int scale) {
    overlay_box(x, y, w, h, colour);
    overlay_text(x + w / 2, y + (h - 8 * scale) / 2, scale, C_TEXT, label);
}

// the top of every setup screen: "BBC Micro for Presto" and this Presto's name, the setup
// network's name, to check the page on the phone against
static void header(void) {
    overlay_text(240, 19, 2, C_TEXT, "BBC Micro for Presto");
    overlay_box(40, 44, 400, 2, C_RULE);
    overlay_text(240, 56, 3, C_NAME, setup->ap_ssid);
}

static void draw_setup(void) {
    if (!overlay_open(C_BG)) return;
    char line[96];
    header();
    switch (setup->state) {
        case S_AP: {
            overlay_private();
            snprintf(line, sizeof(line), "Password %s", setup->ap_pass);
            overlay_text(240, 90, 2, C_NAME, line);
            // the WiFi QR code phones' cameras know, to join the access point; the setup page then
            // opens by itself (neither the name nor the password has a character that would need
            // escaping in it)
            snprintf(line, sizeof(line), "WIFI:T:WPA;S:%s;P:%s;;", setup->ap_ssid, setup->ap_pass);
            int y = 116 + overlay_qr(240, 116, 204, line) + 10;
            memset(line, 0, sizeof(line));
            overlay_text(240, y, 2, C_TEXT, "Scan to join WiFi setup");
            overlay_text(240, y + 22, 1, C_TEXT, "Then open 192.168.4.1 if it doesn't pop up");
            if (setup->error[0]) {
                snprintf(line, sizeof(line), "Couldn't join %s: %s", setup->ssid, setup->error);
                overlay_text(240, y + 38, strlen(line) > 29 ? 1 : 2, C_ERROR, line);
            }
            else if (!setup->ap_up) overlay_text(240, y + 38, 2, C_TEXT, "Starting...");
            button(SKIP_X, SKIP_Y, SKIP_W, SKIP_H, C_BUTTON, "Skip", 4);
            break;
        }
        case S_JOINING:
            overlay_text(240, 150, 3, C_TEXT, "Joining");
            overlay_text(240, 200, 2, C_NAME, setup->ssid);
            overlay_text(240, 250, 2, C_TEXT, "This takes a few seconds");
            break;
        case S_JOINED: {
            struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
            // and a QR code for the web panel, for the phone once it's back on that network
            snprintf(line, sizeof(line), "Joined %s", setup->ssid);
            overlay_text(240, 90, strlen(line) > 29 ? 1 : 2, C_GOOD, line);
            snprintf(line, sizeof(line), "http://%s/", ip4addr_ntoa(netif_ip4_addr(nif)));
            int y = 114 + overlay_qr(240, 114, 196, line) + 8;
            overlay_text(240, y, 2, C_TEXT, "Scan for the web panel");
            overlay_text(240, y + 24, 2, C_TEXT, line);
            snprintf(line, sizeof(line), "http://%s.local/", host_name);
            overlay_text(240, y + 64, 2, C_LINK, line);
            break;
        }
        case S_CONFIRM:
            overlay_text(240, 130, 2, C_TEXT, "Join this network?");
            overlay_text(240, 170, 2, C_NAME, setup->ssid);
            overlay_text(240, 206, 2, C_TEXT, "(asked for on the web)");
            button(YES_X, YESNO_Y, YESNO_W, YESNO_H, C_BUTTON, "Join", 3);
            button(NO_X, YESNO_Y, YESNO_W, YESNO_H, C_BUTTON2, "Cancel", 3);
            break;
    }
}

static void set_state(uint8_t s) {
    setup->state = s;
    setup->state_us = time_us_32();
    draw_setup();
}

static int scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    if (!setup || !r->ssid_len || r->ssid_len > 32) return 0;
    char name[33];
    memcpy(name, r->ssid, r->ssid_len);
    name[r->ssid_len] = 0;
    int i;
    for (i = 0; i < setup->nscan && strcmp(setup->scan[i].ssid, name); i++) {}
    if (i == setup->nscan) {
        if (i == SCAN_MAX) {
            // full: replace the weakest, if this is stronger
            int weakest = 0;
            for (int k = 1; k < SCAN_MAX; k++)
                if (setup->scan[k].rssi < setup->scan[weakest].rssi) weakest = k;
            if (setup->scan[weakest].rssi >= r->rssi) return 0;
            i = weakest;
        } else {
            setup->nscan++;
        }
        strcpy(setup->scan[i].ssid, name);
        setup->scan[i].rssi = -128;
    }
    if (r->rssi > setup->scan[i].rssi) setup->scan[i].rssi = (int8_t) (r->rssi < -127 ? -127 : r->rssi > 0 ? 0 : r->rssi);
    setup->scan[i].secure = r->auth_mode != 0;
    return 0;
}

static bool start_scan(void) {
    if (cyw43_wifi_scan_active(&cyw43_state)) return true;
    cyw43_wifi_scan_options_t opts = {0};
    setup->nscan = 0;
    setup->scanning = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) == 0;
    return setup->scanning;
}

static void start_ap(void) {
    // WPA2 (not WPA3, which not every phone joins), which every phone joins from the QR code
    cyw43_arch_enable_ap_mode(setup->ap_ssid, setup->ap_pass, CYW43_AUTH_WPA2_AES_PSK);
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_AP];
    ip_addr_t ip, mask;
    IP4_ADDR(ip_2_ip4(&ip), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);
    dhcp_server_init(&setup->dhcp, nif, &ip, &mask);
    dns_server_init(&setup->dns, nif, &ip);
    setup->ap_up = true;
    printf("NET setup: access point %s up\n", setup->ap_ssid);
}

static void stop_ap(void) {
    if (!setup->ap_up) return;
    dns_server_deinit(&setup->dns);
    dhcp_server_deinit(&setup->dhcp);
    cyw43_arch_disable_ap_mode();
    setup->ap_up = false;
}

static void setup_free(void) {
    overlay_close();
    memset(setup, 0, sizeof(*setup));   // (the passwords)
    presto_free(setup);
    setup = NULL;
}

static bool setup_alloc(void) {
    if (setup) return true;
    setup = presto_calloc(sizeof(setup_t));
    if (!setup) return false;
    snprintf(setup->ap_ssid, sizeof(setup->ap_ssid), "%s", board_name);
    return true;
}

static void setup_begin(void) {
    // (the screen first: before the machine starts, it brings the framebuffer's memory, presto_alloc)
    overlay_open(C_BG);
    if (!setup_alloc()) {
        printf("NET setup: no memory\n");
        return;
    }
    setup->from_ap = true;
    setup->began_us = time_us_32();
    const ap_cred_t *a = saved_ap_cred();
    if (a) {
        memcpy(setup->ap_pass, a->pass, sizeof(setup->ap_pass));
    } else {
        // from the hardware random number generator, in letters and digits that can't be mistaken
        // for others (no 0/o, 1/l/i); kept, so it's the same every time
        static const char letters[] = "abcdefghjkmnpqrstuvwxyz23456789";
        for (int i = 0; i < AP_PASS_LEN; i++) {
            uint32_t r;
            do r = get_rand_32() & 31; while (r >= sizeof(letters) - 1);
            setup->ap_pass[i] = letters[r];
        }
        setup->ap_pass[AP_PASS_LEN] = 0;
        save_settings(NULL, NULL, setup->ap_pass);
    }
    // look for networks before the access point starts (the radio's then free to)
    start_scan();
    set_state(S_AP);
    printf("NET setup: WiFi setup on the screen\n");
}

// setup over: joined (the network's saved), skipped or timed out (WiFi off until restarted)
static void setup_end(bool joined) {
    stop_ap();
    setup_free();
    if (joined) {
        announced = false;
        joined_at_us = time_us_32();
        return;
    }
    printf("NET setup skipped: WiFi off until restarted\n");
    cyw43_arch_deinit();
    wifi_on = false;
}

static const char *join_error(int s) {
    switch (s) {
        case CYW43_LINK_BADAUTH: return "wrong password";
        case CYW43_LINK_NONET: return "network not found";
        default: return "it failed";
    }
}

static void setup_join(const char *ssid, const char *pass) {
    snprintf(setup->ssid, sizeof(setup->ssid), "%s", ssid);
    snprintf(setup->pass, sizeof(setup->pass), "%s", pass);
    setup->error[0] = 0;
    cyw43_arch_wifi_connect_async(setup->ssid, setup->pass, setup->pass[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    set_state(S_JOINING);
    printf("NET setup: joining a network (%u character name)\n", (uint) strlen(setup->ssid));
}

// every 200 ms while the screen's up
static void setup_poll(void) {
    uint32_t now = time_us_32();
    if (setup->scanning && !cyw43_wifi_scan_active(&cyw43_state)) setup->scanning = false;
    switch (setup->state) {
        case S_AP:
            if (!setup->ap_up && (!setup->scanning || now - setup->began_us > 8000000u)) {
                start_ap();
                draw_setup();
            }
            if (now - setup->began_us > SETUP_US) setup_end(false);
            break;
        case S_JOINING: {
            int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (s == CYW43_LINK_UP) {
                if (!net_wifi_save(setup->ssid, setup->pass)) printf("NET setup: couldn't save the network\n");
                snprintf(ssid_ram, sizeof(ssid_ram), "%s", setup->ssid);
                snprintf(pass_ram, sizeof(pass_ram), "%s", setup->pass);
                memset(setup->pass, 0, sizeof(setup->pass));
                set_state(S_JOINED);
                printf("NET setup: joined, http://%s/\n", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
            } else if (s == CYW43_LINK_BADAUTH || s == CYW43_LINK_NONET || s == CYW43_LINK_FAIL || now - setup->state_us > JOIN_US) {
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                memset(setup->pass, 0, sizeof(setup->pass));
                snprintf(setup->error, sizeof(setup->error), "%s", s < 0 ? join_error(s) : "no answer from it");
                printf("NET setup: couldn't join (%s)\n", setup->error);
                // the access point may have moved to the network's channel: it's still up
                set_state(S_AP);
            }
            break;
        }
        case S_JOINED:
            // the screen for a while, the access point (and the setup page, saying where the
            // web panel is) a little longer
            if (now - setup->state_us > JOINED_US) overlay_close();
            if (now - setup->state_us > AP_AFTER_US) setup_end(true);
            break;
        case S_CONFIRM:
            if (now - setup->state_us > CONFIRM_US) setup_free();
            break;
    }
}

// The address card: where the web panel is (a QR code, the IP and the .local name), shown for
// 20 seconds once WiFi has joined at power on, and whenever asked for (presto_touch.c: a finger
// held in the top-left corner)
static bool card_up, card_waiting;      // on screen; WAIT tapped (it stays until OK)
static uint32_t card_until;             // then, unless waiting (time_us_32)
static int card_shown;                  // the seconds left its SKIP button shows
#define SKIP_CARD_X 70
#define WAIT_X 250
#define CARD_BTN_Y 410
#define CARD_BTN_W 160
#define CARD_BTN_H 48

static void draw_card(int left) {
    if (!overlay_open(C_BG)) return;
    char line[64];
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    overlay_text(240, 19, 2, C_TEXT, "BBC Micro Web Control");
    overlay_box(40, 44, 400, 2, C_RULE);
    snprintf(line, sizeof(line), "http://%s/", ip4addr_ntoa(netif_ip4_addr(nif)));
    int y = 66 + overlay_qr(240, 66, 240, line) + 14;
    overlay_text(240, y, 2, C_TEXT, line);
    snprintf(line, sizeof(line), "http://%s.local/", host_name);
    overlay_text(240, y + 36, 2, C_LINK, line);
    if (card_waiting) {
        button(240 - CARD_BTN_W / 2, CARD_BTN_Y, CARD_BTN_W, CARD_BTN_H, C_BUTTON, "OK", 3);
    } else {
        snprintf(line, sizeof(line), "SKIP %d", left);
        button(SKIP_CARD_X, CARD_BTN_Y, CARD_BTN_W, CARD_BTN_H, C_BUTTON, line, 3);
        button(WAIT_X, CARD_BTN_Y, CARD_BTN_W, CARD_BTN_H, C_BUTTON2, "WAIT", 3);
    }
    card_shown = left;
}

void net_show_address(uint32_t seconds) {
    if (!wifi_on || link_status != CYW43_LINK_UP || (setup && setup->state != S_OFF)) return;
    card_up = true;
    card_waiting = false;
    card_until = time_us_32() + seconds * 1000000u;
    draw_card((int) seconds);
}

static void card_close(void) {
    card_up = false;
    overlay_close();
}

// WiFi setup is wanted or under way (its access point up): the machine waits until it's done
static bool setup_pending(void) {
    return (setup_wanted && !net_started) || (setup != NULL && setup->from_ap && setup->state != S_OFF);
}

bool net_screen_up(void) {
    if (card_up) return true;
    return setup != NULL && setup->state != S_OFF && overlay_is_open();
}

static bool in(int x, int y, int bx, int by, int bw, int bh) {
    // (a little larger than drawn)
    return x >= bx - 16 && x < bx + bw + 16 && y >= by - 16 && y < by + bh + 16;
}

void net_screen_tap(int x, int y) {
    if (card_up) {
        // WAIT keeps it up until OK; anything else closes it
        if (!card_waiting && in(x, y, WAIT_X, CARD_BTN_Y, CARD_BTN_W, CARD_BTN_H)) {
            card_waiting = true;
            draw_card(0);
        } else {
            card_close();
        }
        return;
    }
    if (!setup) return;
    switch (setup->state) {
        case S_AP:
            if (in(x, y, SKIP_X, SKIP_Y, SKIP_W, SKIP_H)) setup_end(false);
            break;
        case S_JOINED:
            overlay_close();
            break;
        case S_CONFIRM:
            if (in(x, y, YES_X, YESNO_Y, YESNO_W, YESNO_H)) {
                bool ok = net_wifi_save(setup->ssid, setup->pass);
                setup_free();
                if (ok) {
                    drive0_sync();      // (a game saved a moment ago: kept)
                    restart_soon();
                }
                printf("WIFI error: couldn't save\n");
            } else if (in(x, y, NO_X, YESNO_Y, YESNO_W, YESNO_H)) {
                setup_free();
            }
            break;
    }
}

void net_poll(void) {
    if (!wifi_on) return;
    uint32_t now = time_us_32();
    if (now - last_poll_us < 500) return;
    last_poll_us = now;
    cyw43_arch_poll();
    if (now - last_link_check_us > 200000) {
        last_link_check_us = now;
        if (setup && setup->from_ap) setup_poll();
        else check_link();
        if (setup && setup->state == S_CONFIRM) setup_poll();
    }
}

static const char *link_name(void) {
    if (setup && setup->from_ap) return setup->state == S_JOINING ? "setup: joining" : "setup";
    if (!wifi_on) return saved_cred() ? "off" : "no network saved";
    switch (link_status) {
        case CYW43_LINK_UP: return "up";
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP: return "getting an address";
        case CYW43_LINK_FAIL: return "failed";
        case CYW43_LINK_NONET: return "network not found";
        case CYW43_LINK_BADAUTH: return "wrong password";
        default: return "joining";
    }
}

// WiFi is up, or the setup screen is, or it won't come up at all (its failures to join the saved
// network: net_boot_ready)
static bool settled(void) {
    if (net_screen_up()) return true;
    if (setup && setup->from_ap) return true;
    if (!wifi_on) return net_started || !net_wanted;
    return link_status == CYW43_LINK_UP;
}

void net_print_status(void) {
    if (!wifi_on || (setup && setup->from_ap)) {
        printf("NET %s\n", link_name());
        return;
    }
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    printf("NET %s ip=%s rssi=%d host=%s.local\n", link_name(), ip4addr_ntoa(netif_ip4_addr(nif)), (int) rssi, host_name);
}

// a network to join, from the setup page or the web panel (POST /api/wifi)
int net_wifi_request(const char *ssid, const char *pass) {
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) return NET_WIFI_REFUSED;
    if (setup && setup->from_ap) {
        // in setup: whoever's on the access point has its password from the screen
        if (setup->state != S_JOINING) setup_join(ssid, pass);
        return NET_WIFI_JOINING;
    }
    // from the network: only once someone at the Presto says so on the screen
    if (card_up) card_close();
    if (!setup_alloc()) return NET_WIFI_REFUSED;
    snprintf(setup->ssid, sizeof(setup->ssid), "%s", ssid);
    snprintf(setup->pass, sizeof(setup->pass), "%s", pass);
    set_state(S_CONFIRM);
    printf("NET a network asked for from the web: Join or Cancel on the screen\n");
    return NET_WIFI_CONFIRM;
}

// ---------------------------------------------------------------------------------------
// Fetching a disc image from a URL

enum { F_IDLE, F_BUSY, F_READY, F_ERROR };
static struct {
    int state;
    char host[96], path[256], name[64];
    uint16_t port;
    int redirects;
    bool save;
    struct tcp_pcb *pcb;
    char head[1024];
    uint head_len;
    bool in_body;
    int status;
    uint32_t got, length;   // length 0: not given
    uint32_t last_progress_us;
    char error[96];
} fetch;

static void fetch_fail(const char *why) {
    if (fetch.pcb) {
        tcp_arg(fetch.pcb, NULL);
        tcp_recv(fetch.pcb, NULL);
        tcp_err(fetch.pcb, NULL);
        tcp_poll(fetch.pcb, NULL, 0);
        tcp_abort(fetch.pcb);
        fetch.pcb = NULL;
    }
    snprintf(fetch.error, sizeof(fetch.error), "%s", why);
    fetch.state = F_ERROR;
}

static bool is_disc_name(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".ssd") || !strcasecmp(dot, ".dsd"));
}

static int from_hex(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// a disc name (for the card, and the page) from the end of a URL path or a query value
static void name_from(const char *s, size_t len, char *out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < out_size; i++) {
        int c = (unsigned char) s[i];
        if (c == '%' && i + 2 < len && from_hex(s[i + 1]) >= 0 && from_hex(s[i + 2]) >= 0) {
            c = from_hex(s[i + 1]) * 16 + from_hex(s[i + 2]);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (isalnum(c) || strchr(" ._-()", c)) out[o++] = (char) c;
    }
    out[o] = 0;
    if (!is_disc_name(out)) strcpy(out + o, ".ssd");
}

static bool parse_url(const char *url) {
    if (strncasecmp(url, "http://", 7)) return false;
    const char *h = url + 7, *slash = strchr(h, '/');
    size_t hl = slash ? (size_t) (slash - h) : strlen(h);
    const char *colon = memchr(h, ':', hl);
    fetch.port = 80;
    if (colon) {
        fetch.port = (uint16_t) atoi(colon + 1);
        hl = (size_t) (colon - h);
    }
    if (!hl || hl >= sizeof(fetch.host)) return false;
    memcpy(fetch.host, h, hl);
    fetch.host[hl] = 0;
    snprintf(fetch.path, sizeof(fetch.path), "%s", slash ? slash : "/");
    const char *end = fetch.path + strcspn(fetch.path, "?#");
    const char *last = end;
    while (last > fetch.path && last[-1] != '/') last--;
    name_from(last, (size_t) (end - last), fetch.name, sizeof(fetch.name));
    return true;
}

static void fetch_connect(const ip_addr_t *addr);

static err_t fetch_header_done(void) {
    // "HTTP/1.x 200 ...", then headers
    fetch.head[fetch.head_len] = 0;
    fetch.status = atoi(strchr(fetch.head, ' ') ? strchr(fetch.head, ' ') + 1 : "0");
    for (char *line = strstr(fetch.head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        char *l = line + 2;
        if (!strncasecmp(l, "content-length:", 15)) fetch.length = (uint32_t) strtoul(l + 15, NULL, 10);
        if (!strncasecmp(l, "location:", 9) && fetch.status / 100 == 3) {
            char loc[300];
            char *v = l + 9;
            while (*v == ' ') v++;
            size_t n = strcspn(v, "\r\n");
            if (n >= sizeof(loc)) n = sizeof(loc) - 1;
            memcpy(loc, v, n);
            loc[n] = 0;
            if (++fetch.redirects > 5) return ERR_VAL;
            if (loc[0] == '/') {
                snprintf(fetch.path, sizeof(fetch.path), "%s", loc);
            } else if (!parse_url(loc)) {
                fetch_fail(strncasecmp(loc, "https:", 6) ? "redirected somewhere odd" : "redirected to https (not supported yet)");
                return ERR_ABRT;
            }
            return ERR_INPROGRESS;   // the caller follows it
        }
    }
    if (fetch.status != 200) return ERR_VAL;
    if (fetch.length > DRIVE_IMAGE_MAX) return ERR_MEM;
    return ERR_OK;
}

static void fetch_restart(void);

static err_t fetch_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) {
        // the server closed: done, if we have the whole thing
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
        fetch.pcb = NULL;
        if (!fetch.in_body) fetch_fail("no reply");
        else if (!fetch.got || (fetch.length && fetch.got != fetch.length)) fetch_fail("download cut short");
        else fetch.state = F_READY;
        return pcb ? ERR_OK : ERR_OK;
    }
    uint8_t *stage = drive0_staging() + STAGE_OFFSET;
    fetch.last_progress_us = time_us_32();
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *d = q->payload;
        uint len = q->len;
        while (len) {
            if (!fetch.in_body) {
                uint n = 0;
                // header bytes one at a time, until the blank line
                while (n < len && fetch.head_len < sizeof(fetch.head) - 1) {
                    fetch.head[fetch.head_len++] = (char) d[n++];
                    if (fetch.head_len >= 4 && !memcmp(fetch.head + fetch.head_len - 4, "\r\n\r\n", 4)) break;
                }
                d += n;
                len -= n;
                if (fetch.head_len >= 4 && !memcmp(fetch.head + fetch.head_len - 4, "\r\n\r\n", 4)) {
                    err_t e = fetch_header_done();
                    if (e == ERR_INPROGRESS) {
                        tcp_recved(pcb, p->tot_len);
                        pbuf_free(p);
                        fetch_restart();
                        return ERR_ABRT;
                    }
                    if (e != ERR_OK) {
                        tcp_recved(pcb, p->tot_len);
                        pbuf_free(p);
                        if (fetch.state == F_BUSY) {
                            char why[64];
                            if (e == ERR_MEM) snprintf(why, sizeof(why), "too big for a disc");
                            else snprintf(why, sizeof(why), "server said %d", fetch.status);
                            fetch_fail(why);
                        }
                        return ERR_ABRT;
                    }
                    fetch.in_body = true;
                } else if (fetch.head_len >= sizeof(fetch.head) - 1) {
                    tcp_recved(pcb, p->tot_len);
                    pbuf_free(p);
                    fetch_fail("reply headers too long");
                    return ERR_ABRT;
                }
            } else {
                if (fetch.got + len > DRIVE_IMAGE_MAX) {
                    tcp_recved(pcb, p->tot_len);
                    pbuf_free(p);
                    fetch_fail("too big for a disc");
                    return ERR_ABRT;
                }
                if (!fetch.got && len >= 2 && d[0] == 'P' && d[1] == 'K') {
                    tcp_recved(pcb, p->tot_len);
                    pbuf_free(p);
                    fetch_fail("that's a zip archive: link to the .ssd inside it");
                    return ERR_ABRT;
                }
                memcpy(stage + fetch.got, d, len);
                fetch.got += len;
                len = 0;
            }
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void fetch_err(void *arg, err_t err) {
    fetch.pcb = NULL;     // already freed by lwIP
    if (fetch.state == F_BUSY) fetch_fail("connection failed");
}

static err_t fetch_poll(void *arg, struct tcp_pcb *pcb) {
    if (time_us_32() - fetch.last_progress_us > 15000000u) {
        fetch_fail("timed out");
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t fetch_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    char req[400];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: presto-beeb\r\nAccept: */*\r\n\r\n",
                     fetch.path, fetch.host);
    if (n >= (int) sizeof(req) || tcp_write(pcb, req, (u16_t) n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        fetch_fail("couldn't send the request");
        return ERR_ABRT;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static void fetch_dns(const char *name, const ip_addr_t *addr, void *arg) {
    if (fetch.state != F_BUSY) return;
    if (!addr) fetch_fail("couldn't find that host");
    else fetch_connect(addr);
}

static void fetch_connect(const ip_addr_t *addr) {
    fetch.pcb = tcp_new_ip_type(IP_GET_TYPE(addr));
    if (!fetch.pcb) {
        fetch_fail("out of connections");
        return;
    }
    tcp_arg(fetch.pcb, NULL);
    tcp_recv(fetch.pcb, fetch_recv);
    tcp_err(fetch.pcb, fetch_err);
    tcp_poll(fetch.pcb, fetch_poll, 4);
    fetch.last_progress_us = time_us_32();
    if (tcp_connect(fetch.pcb, addr, fetch.port, fetch_connected) != ERR_OK) fetch_fail("couldn't connect");
}

static void fetch_begin(void) {
    fetch.head_len = 0;
    fetch.in_body = false;
    fetch.got = fetch.length = 0;
    fetch.status = 0;
    fetch.state = F_BUSY;
    fetch.last_progress_us = time_us_32();
    ip_addr_t addr;
    err_t e = dns_gethostbyname(fetch.host, &addr, fetch_dns, NULL);
    if (e == ERR_OK) fetch_connect(&addr);
    else if (e != ERR_INPROGRESS) fetch_fail("couldn't look up that host");
}

// following a redirect: from inside fetch_recv, so the old connection is aborted by returning ERR_ABRT
static void fetch_restart(void) {
    struct tcp_pcb *old = fetch.pcb;
    tcp_arg(old, NULL);
    tcp_recv(old, NULL);
    tcp_err(old, NULL);
    tcp_poll(old, NULL, 0);
    tcp_abort(old);
    fetch.pcb = NULL;
    fetch_begin();
}

static const char *fetch_start(const char *url, bool save) {
    if (fetch.state == F_BUSY) return "already downloading";
    if (!drive0_staging()) return "no PSRAM";
    while (*url == ' ') url++;
    if (!strncasecmp(url, "https://", 8)) return "https links aren't supported yet: use http, or download it and drop the file";
    if (!parse_url(url)) return "not an http:// link";
    fetch.redirects = 0;
    fetch.save = save;
    fetch.error[0] = 0;
    fetch_begin();
    return NULL;
}

// ---------------------------------------------------------------------------------------
// The web server

enum { C_HEAD, C_BODY, C_WAIT, C_SEND };
enum { R_404, R_PAGE, R_API, R_DISC, R_FETCH, R_APP, R_TOOBIG, R_SETUP_PAGE, R_REDIRECT, R_JOIN };

typedef struct conn {
    struct tcp_pcb *pcb;
    struct conn *next;
    uint8_t state, route;
    const api_cmd_t *cmd;   // R_API: the command
    bool post;
    char head[1024];
    uint16_t head_len;
    char query[128];
    uint32_t length, got;
    char body[512];
    uint16_t body_len;
    // the response: a header and body copied out of out[], then perhaps a body straight from flash
    char *out;
    uint32_t out_len, out_sent;
    const uint8_t *flash_body;
    uint32_t flash_len, flash_sent;
    // GET /api/discs: the card's discs made as they go out, a chunk at a time into out[] (any number of
    // them: a 2 KB reply held ~50, and dropped the rest without a word), from one walk of the card's
    // directory (list_dir, open across the chunks: walking it again for each, the machine stalled)
    bool listing;
    uint32_t list_n;
    void *list_dir;
    uint32_t idle_polls;
    uint32_t accepted_us;   // when it connected: the oldest unused one is evicted first
} conn_t;

#define MAX_CONNS 6
static conn_t *conns;           // all of them
static int conn_count;
static bool upload_busy;        // one disc upload at a time (into the PSRAM's staging area)
static struct {
    bool pending, save, boot, import;   // (import: as the built-in disc's save disc, drive0_import)
    uint32_t size;
    char name[64];
    const char *from;           // what the page shows: "upload" or "link"
} disc_in;

static void conn_free(conn_t *c) {
    for (conn_t **pp = &conns; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    if (c->route == R_DISC && c->state != C_SEND && upload_busy) upload_busy = false;
    if (c->route == R_APP && c->state <= C_BODY) app_install_abort("the upload stopped");
    board_sd_discs_end(c->list_dir);        // (a disc list dropped halfway)
    memset(c->body, 0, sizeof(c->body));    // (perhaps a password)
    presto_free(c->out);
    presto_free(c);
    conn_count--;
}

static void conn_close(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    conn_free(c);
}

// the disc list's next chunk into out[]: the next discs, while one more surely fits (an entry is at most ~120 bytes:
// the name quoted in 80), then the end once there are no more
#define LIST_CHUNK 1024
static void list_refill(conn_t *c) {
    uint32_t used = 0;
    char name[256], q[80];
    uint32_t size;
    bool more = c->list_dir != NULL;
    while (more && used + 160 < LIST_CHUNK && (more = board_sd_discs_next(c->list_dir, name, sizeof name, &size))) {
        api_json_str(q, sizeof q, name);
        used += (uint32_t) snprintf(c->out + used, LIST_CHUNK - used, "%s{\"name\":%s,\"size\":%u}", c->list_n++ ? "," : "", q, (unsigned) size);
    }
    if (!more) {
        used += (uint32_t) snprintf(c->out + used, LIST_CHUNK - used, "],\"card\":%s}", c->list_dir ? "true" : "false");
        board_sd_discs_end(c->list_dir);
        c->list_dir = NULL;
        c->listing = false;
    }
    c->out_len = used, c->out_sent = 0;
}

// queue as much of the response as lwIP will take; close once it's all queued (lwIP sends what's
// left before the FIN)
static void send_more(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    for (;;) {
        while (c->out_sent < c->out_len) {
            uint32_t n = MIN(c->out_len - c->out_sent, tcp_sndbuf(pcb));
            if (!n || tcp_write(pcb, c->out + c->out_sent, (u16_t) MIN(n, 0xffff), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) != ERR_OK) break;
            c->out_sent += MIN(n, 0xffff);
        }
        if (c->out_sent < c->out_len || !c->listing) break;
        list_refill(c);         // (all of that queued: the next chunk, while lwIP has room)
    }
    if (c->out_sent == c->out_len) {
        while (c->flash_sent < c->flash_len) {
            uint32_t n = MIN(c->flash_len - c->flash_sent, tcp_sndbuf(pcb));
            if (!n) break;
            n = MIN(n, 0xffff);
            if (tcp_write(pcb, c->flash_body + c->flash_sent, (u16_t) n, 0) != ERR_OK) break;   // no copy: it's in flash
            c->flash_sent += n;
        }
    }
    tcp_output(pcb);
    if (c->out_sent == c->out_len && c->flash_sent == c->flash_len && !c->listing) conn_close(c);
}

// GET /api/discs: the header and the list's start; send_more makes the rest as it goes
static void respond_disc_list(conn_t *c) {
    presto_free(c->out);
    c->out = presto_alloc(LIST_CHUNK);
    if (!c->out) {
        conn_close(c);
        return;
    }
    c->out_len = (uint32_t) snprintf(c->out, LIST_CHUNK, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n{\"discs\":[");
    c->out_sent = c->flash_sent = c->flash_len = 0;
    c->listing = true, c->list_n = 0;
    c->list_dir = board_sd_discs_begin();
    c->state = C_SEND;
    send_more(c);
}

static void respond(conn_t *c, int code, const char *type, const char *body, uint32_t body_len, bool gzip) {
    const char *status = code == 200 ? "OK" : code == 302 ? "Found" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                         code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : "Error";
    presto_free(c->out);
    uint32_t cap = 300 + (gzip ? 0 : body_len);
    c->out = presto_alloc(cap);
    if (!c->out) {
        conn_close(c);
        return;
    }
    int n = snprintf(c->out, cap, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, status, type, (uint) body_len, gzip ? "Content-Encoding: gzip\r\n" : "",
                     code == 302 ? "Location: http://192.168.4.1/\r\n" : "");
    if (gzip) {
        c->flash_body = (const uint8_t *) body;
        c->flash_len = body_len;
    } else {
        memcpy(c->out + n, body, body_len);
        n += (int) body_len;
    }
    c->out_len = (uint32_t) n;
    c->out_sent = c->flash_sent = 0;
    c->state = C_SEND;
    send_more(c);
}

static void respond_json(conn_t *c, int code, const char *json) {
    respond(c, code, "application/json", json, (uint32_t) strlen(json), false);
}

static void respond_error(conn_t *c, int code, const char *why) {
    char j[200];
    snprintf(j, sizeof(j), "{\"error\":\"%s\"}", why);
    respond_json(c, code, j);
}

static bool query_value(const char *query, const char *key, char *out, size_t out_size) {
    size_t kl = strlen(key);
    for (const char *p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, "&");
            if (n >= out_size) n = out_size - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

// /api/status's part: the WiFi link and a download's progress
void net_status_json(char *j, size_t size) {
    char err[120];
    api_json_str(err, sizeof(err), fetch.error);
    const char *fs = fetch.state == F_BUSY ? "downloading" : fetch.state == F_ERROR ? "error" : fetch.state == F_READY ? "loading" : "idle";
    snprintf(j, size, ",\"fetch\":{\"state\":\"%s\",\"got\":%u,\"length\":%u,\"error\":%s},\"wifi\":\"%s\"",
             fs, (uint) fetch.got, (uint) fetch.length, err, link_name());
}

// The setup page: plain HTML and a form, made here as it's asked for (no script needed: a phone's
// captive portal sheet is a limited browser)
#define PAGE_HEAD 200       // room before the page for its headers
#define PAGE_MAX 2600

typedef struct {
    char *at;
    size_t used, size;
} page_t;

static void put(page_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->at + p->used, p->size - p->used, fmt, ap);
    va_end(ap);
    if (n > 0) p->used = MIN(p->used + (size_t) n, p->size - 1);
}

// text, made safe to put in HTML
static void put_text(page_t *p, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&': put(p, "&amp;"); break;
            case '<': put(p, "&lt;"); break;
            case '>': put(p, "&gt;"); break;
            case '"': put(p, "&quot;"); break;
            case '\'': put(p, "&#39;"); break;
            default: put(p, "%c", *s); break;
        }
    }
}

static void setup_page(conn_t *c) {
    presto_free(c->out);
    c->out = presto_alloc(PAGE_HEAD + PAGE_MAX);
    if (!c->out) {
        conn_close(c);
        return;
    }
    page_t p = {c->out + PAGE_HEAD, 0, PAGE_MAX};
    bool busy = setup->scanning || setup->state == S_JOINING;
    put(&p, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "%s<title>BBC Micro WiFi setup</title><style>body{font:17px system-ui,sans-serif;margin:20px;background:#10182a;color:#e8ecf4}"
            "select,input,button{display:block;font-size:17px;width:100%%;padding:10px;margin:6px 0 16px;box-sizing:border-box}"
            "p{color:#aab4c8}.e{color:#ff7a6c}.o{color:#6cd890}a{color:#8cb4ff}.c{color:#0ff;font-size:20px}.n{color:#feea55;font-size:28px;font-weight:bold;margin:12px 0 2px}</style></head><body>"
            // the Presto screen's header: the title, and this Presto's name
            "<h3>BBC Micro for Presto</h3><div class=n>%s</div>"
            "<p>Check this matches the name on your Presto's screen.</p>",
        busy ? "<meta http-equiv=refresh content=\"2;url=/\">" : "", setup->ap_ssid);
    if (setup->state == S_JOINING) {
        put(&p, "<p>Joining ");
        put_text(&p, setup->ssid);
        put(&p, "&hellip;</p><p>The Presto's screen shows how it goes.</p>");
    } else if (setup->state == S_JOINED) {
        put(&p, "<p class=o>Joined ");
        put_text(&p, setup->ssid);
        const char *ip = ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
        put(&p, ".</p><p>Back on that network, open the web panel:</p><p><a class=c href=\"http://%s.local/\">http://%s.local/</a>"
                "</p><p>(or <a href=\"http://%s/\">http://%s/</a>). This setup network closes in half a minute.</p>", host_name, host_name, ip, ip);
    } else {
        if (setup->error[0]) {
            put(&p, "<p class=e>Couldn't join ");
            put_text(&p, setup->ssid);
            put(&p, ": ");
            put_text(&p, setup->error);
            put(&p, "</p>");
        }
        put(&p, "<form method=post action=/join><label>Network<select name=s>");
        // strongest first
        bool done[SCAN_MAX] = {0};
        for (int k = 0; k < setup->nscan; k++) {
            int best = -1;
            for (int i = 0; i < setup->nscan; i++)
                if (!done[i] && (best < 0 || setup->scan[i].rssi > setup->scan[best].rssi)) best = i;
            done[best] = true;
            put(&p, "<option value=\"");
            put_text(&p, setup->scan[best].ssid);
            put(&p, "\">");
            put_text(&p, setup->scan[best].ssid);
            put(&p, "%s</option>", setup->scan[best].secure ? "" : " (open)");
        }
        put(&p, "<option value=\"\">Another (give its name)</option></select></label>"
                "<input name=o placeholder=\"Another network's name\" maxlength=32 autocapitalize=none autocorrect=off>"
                // (a text field drawn as dots: as a password field, phones offer to make one up)
                "<label>Password<input id=p name=p style=\"-webkit-text-security:disc\" maxlength=64 autocomplete=off "
                "autocapitalize=off autocorrect=off spellcheck=false></label>"
                "<label><input type=checkbox style=\"display:inline;width:auto\" "
                "onchange=\"document.getElementById('p').style.webkitTextSecurity=this.checked?'none':'disc'\"> Show password</label>"
                "<button>Join</button></form>"
                "<p>%s<a href=\"/?rescan\">Look again</a></p>", setup->scanning ? "Looking for networks&hellip; " : "");
    }
    put(&p, "</body></html>");
    char head[PAGE_HEAD];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (uint) p.used);
    memcpy(c->out + PAGE_HEAD - n, head, (size_t) n);
    c->out_sent = (uint32_t) (PAGE_HEAD - n);
    c->out_len = (uint32_t) (PAGE_HEAD + p.used);
    c->flash_len = c->flash_sent = 0;
    c->state = C_SEND;
    send_more(c);
}

// %XX and + in a form's value, in place
static void form_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char) s[1]) && isxdigit((unsigned char) s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char) strtoul(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

// between frames: act on a request whose body has all arrived
static void act(conn_t *c) {
    switch (c->route) {
        case R_JOIN: {
            if (!setup) {
                respond(c, 302, "text/plain", "", 0, false);
                break;
            }
            // the setup page's form: s, the network chosen, or o, another's name; p, its password
            char ssid[100] = "", pass[200];
            if (!query_value(c->body, "o", ssid, sizeof(ssid)) || !ssid[0]) query_value(c->body, "s", ssid, sizeof(ssid));
            if (!query_value(c->body, "p", pass, sizeof(pass))) pass[0] = 0;
            form_decode(ssid);
            form_decode(pass);
            if (net_wifi_request(ssid, pass) == NET_WIFI_REFUSED) snprintf(setup->error, sizeof(setup->error), "no network given");
            memset(pass, 0, sizeof(pass));
            setup_page(c);
            break;
        }
        case R_SETUP_PAGE:
            if (!setup) {
                respond(c, 302, "text/plain", "", 0, false);
                break;
            }
            if (!strcmp(c->query, "rescan") && setup->state != S_JOINING) start_scan();
            setup_page(c);
            break;
        case R_API: {
            if (!strcmp(c->cmd->name, "discs")) {       // (the card's discs, however many: made as they go out)
                respond_disc_list(c);
                break;
            }
            size_t size;
            char *j = api_buffer(&size);
            char from[20];
            snprintf(from, sizeof from, "%s", c->pcb ? ipaddr_ntoa(&c->pcb->remote_ip) : "?");
            api_req_t r = {.args = c->body, .query = c->query, .from = from, .json = true, .out = j, .size = size};
            api_run(c->cmd, &r);
            respond_json(c, r.code, j);
            break;
        }
        case R_DISC: {
            char v[8] = "", b[8] = "1", im[8] = "", name[64];
            query_value(c->query, "save", v, sizeof(v));
            query_value(c->query, "import", im, sizeof(im));
            query_value(c->query, "boot", b, sizeof(b));
            char raw[96] = "";
            query_value(c->query, "name", raw, sizeof(raw));
            name_from(raw, strlen(raw), name, sizeof(name));
            snprintf(disc_in.name, sizeof(disc_in.name), "%s", name);
            disc_in.size = c->got;
            disc_in.save = v[0] == '1';
            disc_in.boot = b[0] != '0';
            disc_in.import = im[0] == '1';
            disc_in.pending = true;
            disc_in.from = "upload";
            upload_busy = false;
            respond_json(c, 200, "{}");
            break;
        }
        case R_APP: {
            char why[96], j[400];
            if (!app_install_end(why, sizeof why)) {
                respond_error(c, 400, why);
                break;
            }
            int n = snprintf(j, sizeof j, "{\"installed\":true,\"restart\":true,\"app\":");
            app_json(j + n, sizeof j - (uint32_t) n - 1, false);
            strcat(j, "}");
            respond_json(c, 200, j);
            drive0_sync();              // (a game saved a moment ago: kept)
            net_restart(true);
            break;
        }
        case R_FETCH: {
            // "<url>", and "\nsave" to keep it on the card too
            char *nl = strchr(c->body, '\n');
            bool save = nl && !strncmp(nl + 1, "save", 4);
            if (nl) *nl = 0;
            const char *why = fetch_start(c->body, save);
            if (why) respond_error(c, 400, why);
            else respond_json(c, 200, "{}");
            break;
        }
        default:
            respond_error(c, 404, "no such thing");
            break;
    }
}

// a request's headers have all arrived
static void route(conn_t *c) {
    c->head[c->head_len] = 0;
    char method[8] = "", target[192] = "";
    sscanf(c->head, "%7s %191s", method, target);
    c->post = !strcmp(method, "POST");
    bool del = !strcmp(method, "DELETE");
    char *q = strchr(target, '?');
    if (q) {
        *q++ = 0;
        snprintf(c->query, sizeof(c->query), "%s", q);
    }
    c->length = 0;
    for (const char *l = strchr(c->head, '\n'); l; l = strchr(l + 1, '\n')) {
        if (!strncasecmp(l + 1, "content-length:", 15)) c->length = (uint32_t) strtoul(l + 16, NULL, 10);
    }
    c->route = R_404;
    if (setup && setup->from_ap) {
        // The setup page. Anything else, or for another host (a phone checking for a captive
        // portal: captive.apple.com, connectivitycheck.gstatic.com, www.msftconnecttest.com, ...,
        // which the DNS server sends here) is redirected to it, so the phone opens it.
        char host[64] = "";
        for (const char *l = strchr(c->head, '\n'); l; l = strchr(l + 1, '\n')) {
            if (!strncasecmp(l + 1, "host:", 5)) sscanf(l + 6, " %63[^:\r\n ]", host);
        }
        c->route = R_REDIRECT;
        if (!host[0] || !strcmp(host, "192.168.4.1")) {
            if (!strcmp(target, "/")) {
                if (!c->post) c->route = R_SETUP_PAGE;
            } else if (!strcmp(target, "/join")) {
                if (c->post) c->route = R_JOIN;
            } else if ((!strcmp(target, "/api/status") && !c->post) || (!strcmp(target, "/api/wifi") && c->post)) {
                if ((c->cmd = api_find_http(target, c->post)) != NULL) c->route = R_API;
            }
        }
    } else if (!strcmp(target, "/") || !strcmp(target, "/index.html")) {
        if (!c->post) c->route = R_PAGE;
    } else if (!strcmp(target, "/api/disc")) {
        if (c->post) c->route = R_DISC;
    } else if (!strcmp(target, "/api/fetch")) {
        if (c->post) c->route = R_FETCH;
    } else if (!strcmp(target, "/api/app") && (c->post || del)) {
        if (c->post) c->route = R_APP;
        else if ((c->cmd = api_find_http("/api/app/remove", true)) != NULL) c->route = R_API;
    } else if ((c->cmd = api_find_http(target, c->post)) != NULL) {
        c->route = R_API;
    }
    if (c->route == R_DISC) {
        if (upload_busy || disc_in.pending) {
            c->route = R_TOOBIG;
            respond_error(c, 409, "already loading a disc");
            return;
        }
        if (!drive0_staging() || !c->length || c->length > DRIVE_IMAGE_MAX) {
            c->route = R_TOOBIG;
            respond_error(c, 413, "that's not the size of a disc image");
            return;
        }
        upload_busy = true;
    } else if (c->route == R_APP) {
        char why[96];
        if (!app_install_begin(c->length, why, sizeof why)) {
            c->route = R_TOOBIG;
            respond_error(c, 409, why);
            return;
        }
    } else if (c->length >= sizeof(c->body)) {
        c->route = R_TOOBIG;
        respond_error(c, 413, "too long");
        return;
    }
    c->state = C_BODY;
}

static void body_done(conn_t *c) {
    if (c->route == R_PAGE) {
        respond(c, 200, "text/html; charset=utf-8", (const char *) presto_web_page, presto_web_page_len, true);
    } else if (c->route == R_REDIRECT) {
        respond(c, 302, "text/plain", "", 0, false);
    } else {
        c->body[c->body_len] = 0;
        c->state = C_WAIT;      // net_frame acts on it
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    if (!p) {
        conn_close(c);
        return ERR_OK;
    }
    c->idle_polls = 0;
    // taken in now: a reply sent from here (the page, a redirect) may close the connection, and
    // closing with data not yet taken in makes lwIP reset it (a short reply never arrived)
    tcp_recved(pcb, p->tot_len);
    for (struct pbuf *b = p; b && c->state <= C_BODY; b = b->next) {
        const char *d = b->payload;
        uint len = b->len;
        while (len && c->state <= C_BODY) {
            if (c->state == C_HEAD) {
                uint n = 0;
                while (n < len && c->head_len < sizeof(c->head) - 1) {
                    c->head[c->head_len++] = d[n++];
                    if (c->head_len >= 4 && !memcmp(c->head + c->head_len - 4, "\r\n\r\n", 4)) break;
                }
                d += n;
                len -= n;
                if (c->head_len >= 4 && !memcmp(c->head + c->head_len - 4, "\r\n\r\n", 4)) {
                    route(c);
                    if (c->state == C_BODY && !c->length) body_done(c);
                } else if (c->head_len >= sizeof(c->head) - 1) {
                    c->route = R_TOOBIG;
                    respond_error(c, 400, "request headers too long");
                }
            } else {
                uint n = MIN(len, c->length - c->got);
                if (c->route == R_DISC) {
                    memcpy(drive0_staging() + STAGE_OFFSET + c->got, d, n);
                } else if (c->route == R_APP) {
                    // (into the flash a sector at a time: the machine pauses for each)
                    if (!app_install_data((const uint8_t *) d, n)) {
                        app_install_abort("the flash couldn't be written");
                        c->route = R_TOOBIG;
                        respond_error(c, 500, "the flash couldn't be written");
                        break;
                    }
                } else {
                    memcpy(c->body + c->body_len, d, n);
                    c->body_len = (uint16_t) (c->body_len + n);
                }
                c->got += n;
                d += n;
                len -= n;
                if (c->got == c->length) body_done(c);
            }
        }
    }
    // (respond may have closed and freed c)
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    conn_t *c = arg;
    c->idle_polls = 0;
    if (c->state == C_SEND) send_more(c);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    conn_t *c = arg;
    if (c) {
        c->pcb = NULL;      // lwIP has freed it
        conn_free(c);
    }
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    conn_t *c = arg;
    // a response that couldn't be queued at all (lwIP out of buffers, with several connections
    // sending at once) gets no sent callback to carry it on: try again here
    // every 2s; give up on a connection that's gone quiet for 20s
    if (c->state == C_SEND && c->idle_polls < 10) {
        c->idle_polls++;
        send_more(c);      // (which may close and free c)
        return ERR_OK;
    }
    if (c->state != C_WAIT && ++c->idle_polls > 10) {
        tcp_arg(pcb, NULL);
        c->pcb = NULL;
        tcp_abort(pcb);
        conn_free(c);
        return ERR_ABRT;
    }
    return ERR_OK;
}

// Browsers open spare connections ahead of time that may never send a request. When every slot
// is taken, close the oldest of those rather than refusing the new one (which the page took as
// the Presto being unreachable).
static bool evict_unused(void) {
    conn_t *oldest = NULL;
    for (conn_t *c = conns; c; c = c->next) {
        if (c->state == C_HEAD && !c->head_len && (!oldest || (int32_t) (c->accepted_us - oldest->accepted_us) < 0)) oldest = c;
    }
    if (!oldest) return false;
    struct tcp_pcb *pcb = oldest->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_abort(pcb);
    }
    oldest->pcb = NULL;
    conn_free(oldest);
    return true;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK || !pcb) return ERR_VAL;
    if (conn_count >= MAX_CONNS) evict_unused();
    conn_t *c = conn_count < MAX_CONNS ? presto_calloc(sizeof(conn_t)) : NULL;
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    conn_count++;
    c->pcb = pcb;
    c->accepted_us = time_us_32();
    c->next = conns;
    conns = c;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 4);
    tcp_nagle_disable(pcb);
    return ERR_OK;
}

static void start_server(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb || tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
        printf("NET couldn't start the web server\n");
        return;
    }
    pcb = tcp_listen_with_backlog(pcb, MAX_CONNS);
    tcp_accept(pcb, on_accept);
}


// ---------------------------------------------------------------------------------------

// Setup at power on: a finger held on the screen as it starts, for 2 seconds
#define HOLD_FOR_SETUP_US 2000000u

void net_frame(void) {
    static uint frames;
    if (frames < 1000) frames++;
    touch_poll();
    if (card_up && !card_waiting) {
        // counting down on its SKIP button, a second at a time
        int32_t left = (int32_t) (card_until - time_us_32());
        if (left <= 0) card_close();
        else if ((left + 999999) / 1000000 != card_shown) draw_card((left + 999999) / 1000000);
    }
    overlay_frame_done();
    if (net_wanted && !net_started && frames >= 50) {
        uint32_t held = touch_held_us();
        if (held >= HOLD_FOR_SETUP_US || !held || frames >= 1000) {
            net_started = true;
            net_start(setup_wanted || held >= HOLD_FOR_SETUP_US);
        }
    }
    if (!wifi_on) return;
    last_poll_us -= 1000;   // (always poll here: in turbo core 0 seldom waits for core 1)
    net_poll();
    // a disc that's arrived: from its staging area to the start of the disc RAM, where drive 0 reads it
    if (fetch.state == F_READY) {
        fetch.state = F_IDLE;
        snprintf(disc_in.name, sizeof(disc_in.name), "%s", fetch.name);
        disc_in.size = fetch.got;
        disc_in.save = fetch.save;
        disc_in.boot = true;
        disc_in.import = false;
        disc_in.pending = true;
        disc_in.from = "link";
    }
    if (disc_in.pending) {
        disc_in.pending = false;
        uint8_t *ram = drive0_staging();
        if (disc_in.import) {
            const char *why = "";
            memmove(ram, ram + STAGE_OFFSET, disc_in.size);
            if (!drive0_import(disc_in.size, &why)) {
                snprintf(fetch.error, sizeof(fetch.error), "not imported: %s", why);
                fetch.state = F_ERROR;
                printf("DISC not imported: %s\n", why);
            }
        } else if (!disc_in.boot) {
            // just onto the card
            if (board_sd_save(disc_in.name, ram + STAGE_OFFSET, disc_in.size)) {
                printf("DISC saved %s to the card (%u bytes)\n", disc_in.name, (uint) disc_in.size);
            } else {
                snprintf(fetch.error, sizeof(fetch.error), "couldn't save that to the card");
                fetch.state = F_ERROR;
            }
        } else {
            memmove(ram, ram + STAGE_OFFSET, disc_in.size);
            if (!drive0_insert(disc_in.name, disc_in.size)) {
                snprintf(fetch.error, sizeof(fetch.error), "that's not the size of a disc image");
                fetch.state = F_ERROR;
            } else {
                printf("DISC loaded %s from %s (%u bytes)\n", disc_in.name, disc_in.from, (uint) disc_in.size);
                if (disc_in.save && !board_sd_save(disc_in.name, ram, disc_in.size)) {
                    snprintf(fetch.error, sizeof(fetch.error), "booted, but couldn't save it to the card");
                    fetch.state = F_ERROR;
                }
            }
        }
    }
    for (conn_t *c = conns, *next; c; c = next) {
        next = c->next;     // (acting may free it)
        if (c->state == C_WAIT) act(c);
    }
    if (restart_after_reply) {
        // let the reply go before restarting
        for (int i = 0; i < 50; i++) {
            cyw43_arch_poll();
            sleep_ms(2);
        }
        restart_soon();
    }
}

// ---------------------------------------------------------------------------------------
// Before the machine starts (frontend_start): the loading screen while WiFi joins, then the address
// card, or WiFi setup; the machine starts once they're done with and the framebuffer's given back.
// 30 s at most for WiFi to join.

static uint32_t boot_us, note_until;
static bool boot_loading, boot_noting;

void net_boot_begin(uint32_t *fb, int rows) {
    screen_lend(fb, rows);
    boot_us = time_us_32();
    boot_loading = true;
}

bool net_boot_ready(void) {
    net_frame();
    uint32_t waited = time_us_32() - boot_us;
    if (boot_loading && wifi_on && ssid_ram[0] && !setup && link_status == CYW43_LINK_BADAUTH) {
        // the saved network's password is wrong: trying it again is no use, so WiFi setup, saying why
        // (the network stays saved until another is joined)
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s", ssid_ram);
        printf("NET couldn't join the saved network (wrong password): WiFi setup\n");
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        memset(ssid_ram, 0, sizeof(ssid_ram));
        memset(pass_ram, 0, sizeof(pass_ram));
        boot_loading = false;
        setup_begin();
        if (setup) {
            snprintf(setup->ssid, sizeof(setup->ssid), "%s", ssid);
            snprintf(setup->error, sizeof(setup->error), "wrong password");
            draw_setup();
        }
    } else if (boot_loading && wifi_on && ssid_ram[0] && !setup &&
               (link_status == CYW43_LINK_NONET || link_status == CYW43_LINK_FAIL || waited > 30000000u)) {
        // not there, or not answering: say so for a moment, then the machine starts; WiFi goes on
        // trying every 15 seconds behind it
        char line[80];
        snprintf(line, sizeof(line), link_status == CYW43_LINK_NONET ? "%s not found" : "Couldn't join %s", ssid_ram);
        printf("NET %s: starting without WiFi\n", link_status == CYW43_LINK_NONET ? "the saved network wasn't found" : "couldn't join the saved network");
        screen_loading_note(line, "Starting without WiFi");
        boot_loading = false;
        boot_noting = true;
        note_until = time_us_32() + 3000000u;
    }
    if (boot_noting) {
        if ((int32_t) (time_us_32() - note_until) < 0) return false;
        boot_noting = false;
        screen_loading(NULL, -1);
    }
    if (boot_loading) {
        if (settled() || waited > 30000000u) {
            boot_loading = false;
            screen_loading(NULL, -1);
            net_show_address(20);   // (if WiFi joined: where the web panel is, before the machine starts)
        } else {
            // (how long joining takes isn't known: most of the way in 10 s, then creeping)
            int permille = waited < 10000000u ? (int) (waited / 12500) : 800 + (int) ((waited - 10000000u) / 100000);
            screen_loading("Joining WiFi", permille > 980 ? 980 : permille);
        }
    }
    if (boot_loading || setup_pending() || net_screen_up()) return false;
    return screen_give_back();
}
