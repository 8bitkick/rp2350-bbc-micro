/*
 * The web control panel (see presto_net.h).
 *
 * WiFi through the Presto's RM2 module (CYW43439) in the SDK's "poll" mode: no interrupt handler
 * and no background work, lwIP only ever runs inside presto_net_poll / presto_net_frame on core 0.
 * A request is read in as packets arrive, then left until the gap between frames to act on it,
 * where it may press keys, load a disc or reset the machine without any locking.
 *
 * The page (web/index.html, gzipped into flash at build time) talks to a small JSON API:
 *   GET  /                      the page
 *   GET  /api/status            turbo, overlay, speed and load, the disc, a fetch's progress
 *   POST /api/keys              "d41 u41 b": keys down/up by BBC matrix code (hex, row << 4 | column);
 *                               b is BREAK
 *   POST /api/turbo, /overlay   "1" on, "0" off, empty toggles
 *   POST /api/boot              boot the disc in the drive (SHIFT+BREAK)
 *   POST /api/eject             back to the built-in disc
 *   GET  /api/discs             the .ssd/.dsd images on the SD card
 *   POST /api/sd                body: a name on the card to load and boot
 *   POST /api/disc?name=&save=&boot=  body: a disc image to boot (save=1 also writes it to the card;
 *                               boot=0 only writes it to the card)
 *   POST /api/fetch             body: a URL for the Presto to download a disc image from and boot
 *   POST /api/wifi              body: "<ssid>\n<password>": save, and restart to join it
 *   POST /api/ambient           body: any of "on=0|1", "bright=<percent>", "locks=0|1", joined by
 *                               "&" (builds with -DAMBIENT=ON; /api/status then has
 *                               "ambient": the LEDs on the back)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>
#include <unistd.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/structs/qmi.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/apps/mdns.h"
#include "presto_net.h"
#include "presto_machine.h"
#include "presto_sd.h"
#include "presto_psram.h"
#include "presto_boot.h"
#if WITH_AMBIENT
#include "presto_ambient.h"
#endif

#define HOSTNAME "beeb"

extern const uint8_t presto_web_page[];     // web/index.html, gzipped (presto_web_page.c, generated)
extern const uint32_t presto_web_page_len;

// downloads and uploads land here in the disc RAM, clear of the disc in drive 0 at its start
// (a machine that copies a disc in when it's inserted can use 0)
#ifndef PRESTO_DISC_STAGE_OFFSET
#define PRESTO_DISC_STAGE_OFFSET (1024 * 1024)
#endif
#define STAGE_OFFSET PRESTO_DISC_STAGE_OFFSET

// ---------------------------------------------------------------------------------------
// WiFi credentials: the last sector of the flash

#define CRED_MAGIC 0x49464957u      // "WIFI"
#define CRED_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
typedef struct {
    uint32_t magic;
    char ssid[33];
    char pass[65];
} cred_t;

static const cred_t *saved_cred(void) {
    const cred_t *c = (const cred_t *) (XIP_BASE + CRED_OFFSET);
    if (c->magic != CRED_MAGIC || !memchr(c->ssid, 0, sizeof(c->ssid)) || !memchr(c->pass, 0, sizeof(c->pass)) || !c->ssid[0]) {
        return NULL;
    }
    return c;
}

static void __no_inline_not_in_flash_func(write_cred)(void *page) {
    // the SDK goes back to XIP through boot2, which sets the flash's own timing, too fast at
    // our clock: put back the slower timing the firmware runs with before touching the flash
    uint32_t timing = qmi_hw->m[0].timing;
    flash_range_erase(CRED_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CRED_OFFSET, page, FLASH_PAGE_SIZE);
    qmi_hw->m[0].timing = timing;
}

static bool save_cred(const char *ssid, const char *pass) {
    static uint8_t page[FLASH_PAGE_SIZE];
    if (!*ssid || strlen(ssid) > 32 || strlen(pass) > 64) return false;
    memset(page, 0xff, sizeof(page));
    cred_t c = {.magic = CRED_MAGIC};
    strcpy(c.ssid, ssid);
    strcpy(c.pass, pass);
    memcpy(page, &c, sizeof(c));
    // core 1 is parked (the panel freezes for a moment) while the flash is busy
    return flash_safe_execute(write_cred, page, 1000) == PICO_OK;
}

static void restart_soon(void) {
    printf("NET restarting to join the network\n");
    fflush(stdout);
    sleep_ms(100);
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

void presto_net_set_wifi(const char *args) {
    char ssid[34], pass[66];
    const char *p = args;
    size_t n;
    while (*p == ' ') p++;
    if (*p == '"') {
        const char *q = strchr(p + 1, '"');
        if (!q) {
            printf("WIFI error: usage wifi <ssid> <password>\n");
            return;
        }
        n = (size_t) (q - p - 1);
        p++;
        if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
        memcpy(ssid, p, n);
        p = q + 1;
    } else {
        const char *q = strchr(p, ' ');
        n = q ? (size_t) (q - p) : strlen(p);
        if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
        memcpy(ssid, p, n);
        p += n;
    }
    ssid[n] = 0;
    while (*p == ' ') p++;
    snprintf(pass, sizeof(pass), "%s", p);
    if (!save_cred(ssid, pass)) {
        printf("WIFI error: couldn't save\n");
        return;
    }
    printf("WIFI saved (%u character network name)\n", (uint) strlen(ssid));
    restart_soon();
}

// ---------------------------------------------------------------------------------------
// The link

static bool wifi_on;            // the chip is up and there's a network to join
static int link_status;
static bool announced;
static uint32_t last_poll_us, last_link_check_us, joined_at_us;
static char ssid_ram[33], pass_ram[65];

static void join(void) {
    cyw43_arch_wifi_connect_async(ssid_ram, pass_ram, pass_ram[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    joined_at_us = time_us_32();
}

static void start_server(void);
static bool net_wanted, net_started;

// started from presto_net_frame once the machine is running, so it boots without waiting on the
// WiFi chip (its firmware takes a moment to load)
void presto_net_init(void) {
    net_wanted = true;
}

static void net_start(void) {
    const cred_t *c = saved_cred();
    if (!c) {
        printf("NET no WiFi network saved (!wifi <ssid> <password>)\n");
        return;
    }
    snprintf(ssid_ram, sizeof(ssid_ram), "%s", c->ssid);
    snprintf(pass_ram, sizeof(pass_ram), "%s", c->pass);
    presto_boot_step(40);
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_UK)) {
        printf("NET WiFi chip didn't start\n");
        return;
    }
    presto_boot_step(41);
    cyw43_arch_enable_sta_mode();
    presto_boot_step(42);
    // no power saving: it adds a lot of latency to every key press from the web page
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    presto_boot_step(43);
    start_server();
    presto_boot_step(44);
    join();
    presto_boot_step(45);
    wifi_on = true;
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
            mdns_resp_add_netif(nif, HOSTNAME);
            mdns_resp_add_service(nif, "BBC Micro", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
        } else {
            mdns_resp_announce(nif);
        }
        printf("NET up http://%s/ (http://" HOSTNAME ".local/)\n", ip4addr_ntoa(netif_ip4_addr(nif)));
    } else if (s != CYW43_LINK_UP) {
        announced = false;
        // failed, or stuck getting nowhere: try again every 15 seconds
        if ((s < 0 || s == CYW43_LINK_DOWN || time_us_32() - joined_at_us > 30000000u) &&
            time_us_32() - joined_at_us > 15000000u) {
            join();
        }
    }
    link_status = s;
}

void presto_net_poll(void) {
    if (!wifi_on) return;
    uint32_t now = time_us_32();
    if (now - last_poll_us < 500) return;
    last_poll_us = now;
    cyw43_arch_poll();
    if (now - last_link_check_us > 200000) {
        last_link_check_us = now;
        check_link();
    }
}

static const char *link_name(void) {
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

void presto_net_print_status(void) {
    if (!wifi_on) {
        printf("NET %s\n", link_name());
        return;
    }
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    printf("NET %s ip=%s rssi=%d\n", link_name(), ip4addr_ntoa(netif_ip4_addr(nif)), (int) rssi);
}

// ---------------------------------------------------------------------------------------
// Keys from the page: by their place in the BBC's keyboard matrix (row << 4 | column), straight
// into the emulated keyboard. Queued, and let out between frames with each key held down for a
// few frames, so the MOS's keyboard scan sees it however quickly the page sends down and up.

#define KEYQ 64
#define KEY_BREAK 0x80
static uint16_t keyq[KEYQ];
static uint8_t keyq_head, keyq_tail;

static void queue_key(bool down, uint code) {
    uint8_t next = (uint8_t) ((keyq_head + 1) % KEYQ);
    if (next == keyq_tail || (code > 0x7f && code != KEY_BREAK)) return;
    keyq[keyq_head] = (uint16_t) (code | (down ? 0x100 : 0));
    keyq_head = next;
}

static void release_keys(void) {
    // a key stays down for 3 frames, and up for 2 before anything else goes down (or the MOS
    // misses the gap between two presses of the same key)
    static uint32_t frames, last_down, last_up;
    frames++;
    while (keyq_head != keyq_tail) {
        uint16_t e = keyq[keyq_tail];
        bool down = e & 0x100;
        if (frames - last_down < 3 || (down && frames - last_up < 2)) break;
        keyq_tail = (uint8_t) ((keyq_tail + 1) % KEYQ);
        uint code = e & 0xff;
        if (code == KEY_BREAK) {
            if (down) presto_break();
        } else {
            key_bbc((int) code, down);
        }
        if (down) last_down = frames;
        else last_up = frames;
    }
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
    if (fetch.length > PRESTO_DISC_MAX) return ERR_MEM;
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
    uint8_t *stage = presto_disc_ram() + STAGE_OFFSET;
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
                if (fetch.got + len > PRESTO_DISC_MAX) {
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
    if (!presto_disc_ram()) return "no PSRAM";
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
enum { R_404, R_PAGE, R_STATUS, R_KEYS, R_TURBO, R_OVERLAY, R_BOOT, R_EJECT, R_DISCS, R_SD, R_DISC, R_FETCH, R_WIFI, R_AMBIENT, R_TOOBIG };

typedef struct conn {
    struct tcp_pcb *pcb;
    struct conn *next;
    uint8_t state, route;
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
    uint32_t idle_polls;
    uint32_t accepted_us;   // when it connected: the oldest unused one is evicted first
} conn_t;

#define MAX_CONNS 6
static conn_t *conns;           // all of them
static int conn_count;
static bool upload_busy;        // one disc upload at a time (into the PSRAM's staging area)
static struct {
    bool pending, save, boot;
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
    free(c->out);
    free(c);
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

// queue as much of the response as lwIP will take; close once it's all queued (lwIP sends what's
// left before the FIN)
static void send_more(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    while (c->out_sent < c->out_len) {
        uint32_t n = MIN(c->out_len - c->out_sent, tcp_sndbuf(pcb));
        if (!n || tcp_write(pcb, c->out + c->out_sent, (u16_t) MIN(n, 0xffff), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) != ERR_OK) break;
        c->out_sent += MIN(n, 0xffff);
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
    if (c->out_sent == c->out_len && c->flash_sent == c->flash_len) conn_close(c);
}

static void respond(conn_t *c, int code, const char *type, const char *body, uint32_t body_len, bool gzip) {
    const char *status = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                         code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : "Error";
    free(c->out);
    uint32_t cap = 256 + (gzip ? 0 : body_len);
    c->out = malloc(cap);
    if (!c->out) {
        conn_close(c);
        return;
    }
    int n = snprintf(c->out, cap, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, status, type, (uint) body_len, gzip ? "Content-Encoding: gzip\r\n" : "");
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

// a JSON string, quoted, into out
static int json_str(char *out, size_t size, const char *s) {
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

// the keyboard's LEDs: bit 0 CAPS LOCK, 1 SHIFT LOCK (lit when their IC32 latch bits are low),
// 2 cassette motor
static uint leds(void) {
    return (IC32 & 0x40 ? 0 : 1) | (IC32 & 0x80 ? 0 : 2) | (serial_get_reg() & 0x80 ? 4 : 0);
}

static void status_json(char *j, size_t size) {
    uint c0, c1, speed;
    presto_perf_get(&c0, &c1, &speed);
    struct mallinfo m = mallinfo();
    extern char __StackLimit;
    uint free_ram = (uint) (m.fordblks + (uint) (&__StackLimit - (char *) sbrk(0)));
    char disc[80], err[120];
    const char *dn = presto_disc_name();
    if (dn) json_str(disc, sizeof(disc), dn);
    else strcpy(disc, "null");
    json_str(err, sizeof(err), fetch.error);
    const char *fs = fetch.state == F_BUSY ? "downloading" : fetch.state == F_ERROR ? "error" : fetch.state == F_READY ? "loading" : "idle";
    char ambient[80] = "";
#if WITH_AMBIENT
    if (presto_ambient_ready()) {
        bool on, locks;
        unsigned bright;
        presto_ambient_get(&on, &bright, &locks);
        snprintf(ambient, sizeof(ambient), ",\"ambient\":{\"on\":%s,\"bright\":%u,\"locks\":%s}",
                 on ? "true" : "false", bright, locks ? "true" : "false");
    }
#endif
    snprintf(j, size, "{\"turbo\":%s,\"overlay\":%s,\"speed\":%u,\"core0\":%u,\"core1\":%u,\"disc\":%s,"
                      "\"fetch\":{\"state\":\"%s\",\"got\":%u,\"length\":%u,\"error\":%s},\"free\":%u,\"wifi\":\"%s\",\"mhz\":%u,\"temp\":%d,\"leds\":%u%s}",
             presto_turbo_get() ? "true" : "false", presto_overlay_get() ? "true" : "false", speed, c0, c1, disc,
             fs, (uint) fetch.got, (uint) fetch.length, err, free_ram, link_name(),
             (uint) (clock_get_hz(clk_sys) / 1000000), (presto_temp_centi() + 50) / 100, leds(), ambient);
}

typedef struct {
    char *j;
    size_t size, used;
    int n;
} list_ctx_t;

static void list_disc(const char *name, uint32_t size, void *ctx) {
    list_ctx_t *l = ctx;
    char q[80];
    json_str(q, sizeof(q), name);
    if (l->used + strlen(q) + 40 >= l->size) return;
    l->used += (size_t) snprintf(l->j + l->used, l->size - l->used, "%s{\"name\":%s,\"size\":%u}", l->n++ ? "," : "", q, (uint) size);
}

static bool set_flag(const char *body, bool now) {
    return body[0] == '1' ? true : body[0] == '0' ? false : !now;
}

// between frames: act on a request whose body has all arrived
static void act(conn_t *c) {
    char j[640];
    switch (c->route) {
        case R_STATUS:
            status_json(j, sizeof(j));
            respond_json(c, 200, j);
            break;
        case R_KEYS: {
            // "d<hex>" / "u<hex>" / "b", space separated
            for (char *p = c->body; *p;) {
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
                ulong code = strtoul(p, &end, 16);
                if (end == p) break;
                queue_key(down, (uint) code);
                p = end;
            }
            respond_json(c, 200, "{}");
            break;
        }
        case R_TURBO:
            presto_turbo_set(set_flag(c->body, presto_turbo_get()));
            status_json(j, sizeof(j));
            respond_json(c, 200, j);
            break;
#if WITH_AMBIENT
        case R_AMBIENT: {
            // "on=1&bright=40&locks=0", any of them
            int on = -1, bright = -1, locks = -1;
            for (char *p = c->body; *p; ) {
                char *amp = strchr(p, '&');
                if (amp) *amp = 0;
                if (!strncmp(p, "on=", 3)) on = atoi(p + 3) ? 1 : 0;
                else if (!strncmp(p, "bright=", 7)) bright = atoi(p + 7);
                else if (!strncmp(p, "locks=", 6)) locks = atoi(p + 6) ? 1 : 0;
                if (!amp) break;
                p = amp + 1;
            }
            presto_ambient_set(on, bright, locks);
            status_json(j, sizeof(j));
            respond_json(c, 200, j);
            break;
        }
#endif
        case R_OVERLAY:
            presto_overlay_set(set_flag(c->body, presto_overlay_get()));
            status_json(j, sizeof(j));
            respond_json(c, 200, j);
            break;
        case R_BOOT:
            // SHIFT+BREAK: SHIFT held for the first second after the reset, as at power-on
            presto_boot_shift();
            presto_break();
            respond_json(c, 200, "{}");
            break;
        case R_EJECT:
            presto_sd_eject_disc();
            respond_json(c, 200, "{}");
            break;
        case R_DISCS: {
            list_ctx_t l = {.size = 8192};
            l.j = malloc(l.size);
            if (!l.j) {
                respond_error(c, 500, "out of memory");
                break;
            }
            l.used = (size_t) snprintf(l.j, l.size, "{\"discs\":[");
            int n = presto_sd_each_disc(list_disc, &l);
            snprintf(l.j + l.used, l.size - l.used, "],\"card\":%s}", n < 0 ? "false" : "true");
            respond_json(c, 200, l.j);
            free(l.j);
            break;
        }
        case R_SD:
            if (presto_sd_load_disc(c->body)) respond_json(c, 200, "{}");
            else respond_error(c, 404, "couldn't load that from the card");
            break;
        case R_DISC: {
            char v[8] = "", b[8] = "1", name[64];
            query_value(c->query, "save", v, sizeof(v));
            query_value(c->query, "boot", b, sizeof(b));
            char raw[96] = "";
            query_value(c->query, "name", raw, sizeof(raw));
            name_from(raw, strlen(raw), name, sizeof(name));
            snprintf(disc_in.name, sizeof(disc_in.name), "%s", name);
            disc_in.size = c->got;
            disc_in.save = v[0] == '1';
            disc_in.boot = b[0] != '0';
            disc_in.pending = true;
            disc_in.from = "upload";
            upload_busy = false;
            respond_json(c, 200, "{}");
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
        case R_WIFI: {
            char *nl = strchr(c->body, '\n');
            if (!nl) {
                respond_error(c, 400, "expected ssid, newline, password");
                break;
            }
            *nl = 0;
            char *pass = nl + 1;
            pass[strcspn(pass, "\r\n")] = 0;
            if (!save_cred(c->body, pass)) {
                respond_error(c, 400, "couldn't save that");
                break;
            }
            respond_json(c, 200, "{}");
            // let the reply go before restarting
            for (int i = 0; i < 50; i++) {
                cyw43_arch_poll();
                sleep_ms(2);
            }
            restart_soon();
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
    char *q = strchr(target, '?');
    if (q) {
        *q++ = 0;
        snprintf(c->query, sizeof(c->query), "%s", q);
    }
    c->length = 0;
    for (const char *l = strchr(c->head, '\n'); l; l = strchr(l + 1, '\n')) {
        if (!strncasecmp(l + 1, "content-length:", 15)) c->length = (uint32_t) strtoul(l + 16, NULL, 10);
    }
    static const struct {
        const char *path;
        bool post;
        uint8_t route;
    } routes[] = {
        {"/", false, R_PAGE}, {"/index.html", false, R_PAGE}, {"/api/status", false, R_STATUS},
        {"/api/keys", true, R_KEYS}, {"/api/turbo", true, R_TURBO}, {"/api/overlay", true, R_OVERLAY},
        {"/api/boot", true, R_BOOT}, {"/api/eject", true, R_EJECT}, {"/api/discs", false, R_DISCS}, {"/api/sd", true, R_SD},
        {"/api/disc", true, R_DISC}, {"/api/fetch", true, R_FETCH}, {"/api/wifi", true, R_WIFI},
        {"/api/ambient", true, R_AMBIENT},
    };
    c->route = R_404;
    for (uint i = 0; i < count_of(routes); i++) {
        if (!strcmp(target, routes[i].path) && c->post == routes[i].post) c->route = routes[i].route;
    }
    if (c->route == R_DISC) {
        if (upload_busy || disc_in.pending) {
            c->route = R_TOOBIG;
            respond_error(c, 409, "already loading a disc");
            return;
        }
        if (!presto_disc_ram() || !c->length || c->length > PRESTO_DISC_MAX) {
            c->route = R_TOOBIG;
            respond_error(c, 413, "that's not the size of a disc image");
            return;
        }
        upload_busy = true;
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
    } else {
        c->body[c->body_len] = 0;
        c->state = C_WAIT;      // presto_net_frame acts on it
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    if (!p) {
        conn_close(c);
        return ERR_OK;
    }
    c->idle_polls = 0;
    uint16_t total = p->tot_len;
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
                    memcpy(presto_disc_ram() + STAGE_OFFSET + c->got, d, n);
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
    // (respond may have closed and freed c: only touch pcb now)
    tcp_recved(pcb, total);
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
    conn_t *c = conn_count < MAX_CONNS ? calloc(1, sizeof(conn_t)) : NULL;
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

void presto_net_frame(void) {
    static uint frames;
    if (frames < 100) frames++;
    if (frames == 100) presto_boot_ok();     // running: disarm the boot guard (see presto_boot.c)
    if (net_wanted && !net_started && frames == 50) {
        net_started = true;
        net_start();
    }
    if (!wifi_on) return;
    release_keys();
    last_poll_us -= 1000;   // (always poll here: in turbo core 0 seldom waits for core 1)
    presto_net_poll();
    // a disc that's arrived: from its staging area to the start of the disc RAM, where drive 0 reads it
    if (fetch.state == F_READY) {
        fetch.state = F_IDLE;
        snprintf(disc_in.name, sizeof(disc_in.name), "%s", fetch.name);
        disc_in.size = fetch.got;
        disc_in.save = fetch.save;
        disc_in.boot = true;
        disc_in.pending = true;
        disc_in.from = "link";
    }
    if (disc_in.pending) {
        disc_in.pending = false;
        uint8_t *ram = presto_disc_ram();
        if (!disc_in.boot) {
            // just onto the card
            if (presto_sd_save(disc_in.name, ram + STAGE_OFFSET, disc_in.size)) {
                printf("DISC saved %s to the card (%u bytes)\n", disc_in.name, (uint) disc_in.size);
            } else {
                snprintf(fetch.error, sizeof(fetch.error), "couldn't save that to the card");
                fetch.state = F_ERROR;
            }
        } else {
            memmove(ram, ram + STAGE_OFFSET, disc_in.size);
            if (!presto_disc_insert(disc_in.name, disc_in.size)) {
                snprintf(fetch.error, sizeof(fetch.error), "that's not the size of a disc image");
                fetch.state = F_ERROR;
            } else {
                printf("DISC loaded %s from %s (%u bytes)\n", disc_in.name, disc_in.from, (uint) disc_in.size);
                if (disc_in.save && !presto_sd_save(disc_in.name, ram, disc_in.size)) {
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
}
