// A bounded power-on banner. The upload staging area is idle until the HTTP
// listener is enabled after this function returns. No permanent framebuffer RAM.
#include "boot_banner.h"
#include "board.h"
#include "drive.h"
#include "machine_if.h"
#include "net_if.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if NIMBUS_TEST
// Optional companion PR: these hooks do not write pairing flash during startup.
extern void presto_bluetooth_boot_poll(void);
extern const char *presto_bluetooth_boot_status(void);
extern const char *presto_keyboard_boot_status(void);
#endif
#include "boot_logo.h"
#if WITH_SOUND
#include "sound.h"
#endif
#ifndef BOOT_BUILD_ID
#define BOOT_BUILD_ID "development"
#endif

// Original compact 5x7 uppercase bitmap alphabet, one row per byte.
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .:-/";
static const uint8_t glyphs[][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14},{0,0,0,0,0,0,0},{0,0,0,0,0,12,12},
 {0,12,12,0,12,12,0},{0,0,0,31,0,0,0},{1,2,2,4,8,8,16}
};
#define ROW_BYTES (BOARD_DISPLAY_ROW_WORDS * 4)
#define BANNER_ROWS 240
_Static_assert(2 * BANNER_ROWS * ROW_BYTES <= DRIVE_IMAGE_MAX, "banner exceeds upload staging");

static void pixel(uint32_t *fb, int x, int y, unsigned colour) {
    if (x < 0 || x >= 480 || y < 0 || y >= BANNER_ROWS) return;
    uint32_t *p = fb + y * BOARD_DISPLAY_ROW_WORDS + x / 8;
    unsigned shift = (x % 8) * 4;
    *p = (*p & ~(15u << shift)) | ((colour & 7u) << shift);

}
static void text(uint32_t *fb, int x, int y, const char *s) {
    for (; *s && x + 10 <= 464; s++, x += 12) {
        const char *g = strchr(alphabet, toupper((unsigned char)*s));
        if (!g) continue;
        for (int yy = 0; yy < 7; yy++) for (int xx = 0; xx < 5; xx++)
            if (glyphs[g-alphabet][yy] & (16 >> xx)) {
                pixel(fb, x+xx*2, y+yy, 7); pixel(fb, x+xx*2+1, y+yy, 7);
            }
    }
}
static void read_status(char *value, size_t size, char *line, size_t line_size) {
#if BOARD_HAS_NET
    net_boot_status(value, size);
#else
    snprintf(value, size, "UNAVAILABLE");
#endif
#if NIMBUS_TEST
    #if BT_KEYBOARD_CLASSIC
    snprintf(line, line_size, "JOYSTICK: %s\nKEYBOARD: %s", presto_bluetooth_boot_status(), presto_keyboard_boot_status());
#else
    snprintf(line, line_size, "JOYSTICK: %s\nKEYBOARD: DISABLED IN BUILD", presto_bluetooth_boot_status());
#endif
#else
    snprintf(line, line_size, "JOYSTICK: DISABLED IN BUILD\nKEYBOARD: DISABLED IN BUILD");
#endif
}
static void status(uint32_t *fb, const char *value, const char *line) {
    // Only rewrite status rows; the logo and build text stay stable.
    for (int y = 110; y < 180; y++) memset(fb + y * BOARD_DISPLAY_ROW_WORDS, 0, ROW_BYTES - 4);
    text(fb, 20, 114, "DHCP IP ADDRESS");
    text(fb, 20, 127, value);
    const char *second = strchr(line, '\n');
    char first[64];
    snprintf(first, sizeof first, "%.*s", second ? (int)(second-line) : (int)strlen(line), line);
    text(fb, 20, 155, first);
    if(second)text(fb, 20, 168, second+1);
}
static void retire_display_table(void) {
    uint32_t frame = board_display_frames();
    while ((uint32_t)(board_display_frames() - frame) < 2) {
        watchdog_update();
        sleep_ms(1);
    }
}
void boot_banner_show(void) {
    // Web machine switches use a watchdog restart: leave their network and
    // controller startup path untouched, and skip recovery restarts too.
    if (watchdog_caused_reboot()) return;
    // Recovery paths without an initialized display/staging area safely skip it.
    uint32_t *fb = (uint32_t *)drive0_staging();
    const uint16_t *const *previous = board_display_get_rows();
    if (!fb || !previous) return;
    const uint16_t **rows = malloc(2 * (BOARD_DISPLAY_HEIGHT + 1) * sizeof *rows);
    if (!rows) return; // Failure must never prevent the emulator from booting.
    const unsigned words = BANNER_ROWS * BOARD_DISPLAY_ROW_WORDS;
    uint32_t *buffers[2] = {fb, fb + words};
    const uint16_t **tables[2] = {rows, rows + BOARD_DISPLAY_HEIGHT + 1};
    memset(fb, 0, 2 * BANNER_ROWS * ROW_BYTES);
    for (int y = 0; y < BANNER_ROWS; y++) {
        fb[(y + 1) * BOARD_DISPLAY_ROW_WORDS - 1] = BOARD_DISPLAY_ROW_END;
        rows[y*2] = rows[y*2+1] = (const uint16_t *)(fb + y * BOARD_DISPLAY_ROW_WORDS);
    }
    rows[480] = NULL;
    for (int y = 0; y < 480; y++) tables[1][y] = (const uint16_t *)(buffers[1] + (y/2) * BOARD_DISPLAY_ROW_WORDS);
    tables[1][480] = NULL;
    for (int y=0; y<48; y++) for (int x=0; x<48; x++) {
        pixel(fb, 16+x*2, 8+y, boot_logo[y*48+x]);
        pixel(fb, 17+x*2, 8+y, boot_logo[y*48+x]);
    }
    text(fb, 132, 20, "RP2350");
    text(fb, 132, 34, "BBC MICRO");
    text(fb, 20, 72, "BUILD " BOOT_BUILD_ID);
    text(fb, 20, 86, __DATE__ " " __TIME__);
    text(fb, 20, 196, "WEB: BEEB.LOCAL");
    text(fb, 20, 216, "STARTING EMULATOR...");
    char value[48], line[128], last_value[48], last_line[128];
    read_status(value, sizeof value, line, sizeof line);
    status(fb, value, line);
    strcpy(last_value, value); strcpy(last_line, line);
    // Never expose an old emulator scanline/cursor while changing row tables.
    board_display_backlight(0);
    board_display_set_rows(rows);
    retire_display_table();
    board_display_backlight(255);
    unsigned active = 0;
    const uint64_t started = time_us_64();
    const uint64_t minimum_until = started + 3000000;
    const uint64_t network_timeout = started + 30000000;
    uint64_t address_until = 0;
    bool wait_for_address = false;
#if BOARD_HAS_NET
    wait_for_address = net_boot_configured();
#endif
    do {
        watchdog_update();
#if BOARD_HAS_NET
        net_boot_poll();
#endif
#if NIMBUS_TEST
        presto_bluetooth_boot_poll();
#endif
        read_status(value, sizeof value, line, sizeof line);
        if (strcmp(value, last_value) || strcmp(line, last_line)) {
            unsigned next = active ^ 1u;
            memcpy(buffers[next], buffers[active], BANNER_ROWS * ROW_BYTES);
            status(buffers[next], value, line);
            board_display_set_rows(tables[next]);
            // The previous buffer must leave DMA before it can be drawn into.
            retire_display_table();
            active = next;
            strcpy(last_value, value); strcpy(last_line, line);
        }
#if BOARD_HAS_NET
        // Start the reading interval only after the IP text has reached scanout.
        if (wait_for_address && !address_until && net_boot_address_ready())
            address_until = time_us_64() + 2000000;
#endif
        uint64_t now = time_us_64();
        bool network_done = !wait_for_address ||
            (address_until ? now >= address_until : now >= network_timeout);
        if (now >= minimum_until && network_done) break;
        sleep_ms(20);
    } while (true);
    board_display_backlight(0);
    board_display_set_rows(previous);
    retire_display_table();
    board_display_backlight(255);
    free(rows);
}


#if WITH_BOOT_FILE_SELECTOR
#include "boot_button.h"

#if NIMBUS_TEST
#include "controller.h"
#endif
// Runtime buffers must not borrow the upload staging area. Every machine reserves
// the top 256 KiB of PSRAM in this build, including Beeb's disc and big allocator.
_Static_assert(2 * BANNER_ROWS * ROW_BYTES <= BOARD_STATUS_BYTES, "status reservation too small");
static const uint16_t **runtime_rows;
static const uint16_t *const *emulator_rows;
static bool status_visible;
static uint32_t *status_sram;
#define STATUS_BANK_ROWS 96
static unsigned status_active;
static uint32_t status_frame, status_updated_ms;

// Root-directory titles use the existing machine-specific SD extension filter.
// The last 60 KiB of the reserved area is separate from both status buffers.
#define TITLE_LIMIT 240
#define TITLE_NAME_BYTES 256
_Static_assert(2 * BANNER_ROWS * ROW_BYTES + TITLE_LIMIT * TITLE_NAME_BYTES <= BOARD_STATUS_BYTES,
               "title list overlaps status buffers");
static unsigned title_count, title_selected;
static bool chooser, chooser_dirty, title_limit, fire_armed;
static int held_direction;
static uint32_t repeat_at;
static void *title_iterator;
static const char *load_error;
static char (*titles(void))[TITLE_NAME_BYTES] {
    return (char (*)[TITLE_NAME_BYTES])(board_psram()+board_psram_size()-TITLE_LIMIT*TITLE_NAME_BYTES);
}
static void titles_end(void) {
    if(title_iterator)board_sd_discs_end(title_iterator);
    title_iterator=NULL;
}
static bool joystick_state(unsigned *directions,bool *fire) {
#if NIMBUS_TEST
    bool joystick=controller_menu_state(directions,fire);
    unsigned kd;bool kf;controller_keyboard_menu_state(&kd,&kf);
    *directions|=kd;*fire|=kf;
    if((*directions & 3u)==3u)*directions &= ~3u;
#if BT_KEYBOARD_CLASSIC
    return joystick || !strcmp(presto_keyboard_boot_status(),"CONNECTED");
#else
    return joystick;
#endif
#else
    *directions=0;*fire=false;return false;
#endif
}
static int title_compare(const void *a,const void *b){return strcasecmp(a,b);}
static void titles_begin(void) {
    titles_end();title_count=title_selected=0;title_limit=false;load_error=NULL;
    held_direction=0;fire_armed=false;
    unsigned directions;bool fire;
    chooser=joystick_state(&directions,&fire);
    if(chooser){title_iterator=board_sd_discs_begin();if(!title_iterator)chooser=false;}
}
static void titles_poll(void) {
    if(!title_iterator)return;
    for(unsigned i=0;i<8;i++) {
        char name[TITLE_NAME_BYTES];uint32_t size;
        if(!board_sd_discs_next(title_iterator,name,sizeof name,&size)){
            titles_end();break;
        }
        if(!size || size>DRIVE_IMAGE_MAX)continue;
        if(title_count==TITLE_LIMIT){title_limit=true;titles_end();break;}
        snprintf(titles()[title_count++],TITLE_NAME_BYTES,"%s",name);
    }
    if(!title_iterator){
        qsort(titles(),title_count,TITLE_NAME_BYTES,title_compare);
        if(!title_count)chooser=false;
        chooser_dirty=true;
    }
}

static bool status_allocate(void) {
    if (runtime_rows) return true;
    if (!board_display_get_rows() || !board_psram() || board_psram_size() < BOARD_STATUS_BYTES) return false;
    runtime_rows = malloc(2 * (BOARD_DISPLAY_HEIGHT + 1) * sizeof *runtime_rows);
    if (!runtime_rows) return false;
    // Packed-4 status uses 46 KiB for two compact banks; emulator drawing
    // uses its own buffer. Never scan status pixels from PSRAM.
    status_sram=malloc(2*STATUS_BANK_ROWS*ROW_BYTES);
    if(!status_sram){free(runtime_rows);runtime_rows=NULL;return false;}
    return true;
}

static void status_draw(unsigned buffer) {
    uint32_t *fb = (uint32_t *)(board_psram() + board_psram_size() - BOARD_STATUS_BYTES);
    (void)buffer;
    fb += BANNER_ROWS * BOARD_DISPLAY_ROW_WORDS;
    memset(fb,0,BANNER_ROWS*ROW_BYTES);
    for(unsigned y=0;y<BANNER_ROWS;y++)fb[(y+1)*BOARD_DISPLAY_ROW_WORDS-1]=BOARD_DISPLAY_ROW_END;
    if(chooser) {
        text(fb,20,15,"SD TITLES");
        char line[64];
        if(title_iterator)text(fb,20,40,"READING SD CARD...");
        else {
            unsigned start=(title_selected/8)*8;
            for(unsigned i=start;i<title_count && i<start+8;i++) {
                unsigned y=42+(i-start)*18;
                if(i==title_selected) {
                    for(unsigned yy=y-2;yy<y+9;yy++)for(unsigned x=8;x<14;x++)pixel(fb,x,yy,6);
                }
                snprintf(line,sizeof line,"%.36s",titles()[i]);text(fb,20,y,line);
            }
            snprintf(line,sizeof line,"%u / %u%s",title_selected+1,title_count,title_limit?" - LIST LIMIT":"");
            text(fb,20,190,line);
        }
        text(fb,20,211,"UP/DOWN: CHOOSE  A/ENTER: LOAD");
        text(fb,20,227,"BOOT: STATUS SCREEN");
        return;
    }
    text(fb, 20, 15, "BBC MICRO STATUS");
    char line[64], value[48], joystick[128];
    snprintf(line, sizeof line, "MACHINE: %s", machine_name()); text(fb, 20, 36, line);
    text(fb, 20, 54, "BUILD " BOOT_BUILD_ID);
    unsigned c0, c1, speed; machine_perf_get(&c0, &c1, &speed);
    snprintf(line, sizeof line, "SPEED %u / CPU %u / %u", speed, c0, c1); text(fb, 20, 75, line);
    read_status(value, sizeof value, joystick, sizeof joystick);
    status(fb, value, joystick);
    text(fb, 20, 192, "WEB: BEEB.LOCAL");
    text(fb, 20, 211, "BOOT: RETURN TO EMULATOR");
    text(fb, 20, 227, load_error ? load_error : "EMULATION PAUSED");
}
static void status_present(void) {
    const uint32_t *staged=(const uint32_t *)(board_psram()+board_psram_size()-BOARD_STATUS_BYTES);
    staged+=BANNER_ROWS*BOARD_DISPLAY_ROW_WORDS;
    uint32_t *bank=status_sram+status_active*STATUS_BANK_ROWS*BOARD_DISPLAY_ROW_WORDS;
    const uint16_t **table=runtime_rows+status_active*(BOARD_DISPLAY_HEIGHT+1);
    // Blank scanlines share one immutable SRAM row. Only nonblank text rows
    // consume space, so two complete screens fit without large SRAM buffers.
    memset(bank,0,ROW_BYTES);
    bank[BOARD_DISPLAY_ROW_WORDS-1]=BOARD_DISPLAY_ROW_END;
    unsigned used=1;
    for(unsigned y=0;y<BANNER_ROWS;y++) {
        const uint32_t *src=staged+y*BOARD_DISPLAY_ROW_WORDS;
        bool nonblank=false;
        for(unsigned x=0;x<BOARD_DISPLAY_ROW_WORDS-1;x++)nonblank|=src[x]!=0;
        const uint32_t *row=bank;
        if(nonblank) {
            if(used>=STATUS_BANK_ROWS){status_active^=1u;return;} // Never publish an incomplete table.
            row=bank+used++*BOARD_DISPLAY_ROW_WORDS;
            memcpy((void *)row,src,ROW_BYTES);
        }
        table[y*2]=table[y*2+1]=(const uint16_t *)row;
    }
    table[BOARD_DISPLAY_HEIGHT]=NULL;
    __dmb();
    // Driver publishes the complete immutable bank at vertical blank only.
    board_display_set_rows(table);
}
static void status_close(void) {
    titles_end();chooser=false;
#if NIMBUS_TEST
    controller_menu_capture(false);
#endif
    board_display_set_rows(emulator_rows);
#if WITH_SOUND
    audio_status_mute(false);
#endif
    status_visible = false;

}
static bool status_open(void) {
    if(!status_allocate())return false;
#if NIMBUS_TEST
    controller_menu_capture(true);
#endif
    emulator_rows = board_display_get_rows();
#if WITH_SOUND
    audio_status_mute(true);
#endif
    status_active ^= 1u;
    status_draw(status_active);
    status_present();
    status_visible = true;

    return true;
}
#endif

void boot_status_frame(void) {
#if WITH_BOOT_FILE_SELECTOR
    static bool gpio_ready;
    static boot_button_t button;
    static bool pending_toggle;
    if (!gpio_ready) {
        // Pimoroni Presto schematic: USER_SW on GP46, active low. This reads
        // the dedicated input; it does not manipulate QSPI chip select.
        gpio_init(BOARD_BOOT_BUTTON_PIN);
        gpio_set_dir(BOARD_BOOT_BUTTON_PIN, GPIO_IN);
        gpio_pull_up(BOARD_BOOT_BUTTON_PIN);
        gpio_ready = true;
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (boot_button_press(&button, !gpio_get(BOARD_BOOT_BUTTON_PIN), now))
        pending_toggle = !pending_toggle;
    uint32_t frame = board_display_frames();
    // Allow the previous scanout table/buffer to retire without blocking audio.
    if ((uint32_t)(frame - status_frame) < 2) return;
    if (pending_toggle) {
        pending_toggle = false;
        if(status_visible && chooser) {
            titles_end();chooser=false;chooser_dirty=true;
        } else if(status_visible)status_close();
        else {titles_begin();if(!status_open())titles_end();}
        status_frame = frame; status_updated_ms = now;
    }
    if(status_visible && chooser) {
        titles_poll();
        unsigned directions=0;bool fire=false;
        if(!joystick_state(&directions,&fire)) {
            titles_end();chooser=false;chooser_dirty=true;
        } else if(!title_iterator && title_count) {
            if(!fire)fire_armed=true;
            int direction=(directions&1)?-1:((directions&2)?1:0);
            if(direction && (direction!=held_direction || (int32_t)(now-repeat_at)>=0)) {
                if(direction<0 && title_selected)title_selected--;
                if(direction>0 && title_selected+1<title_count)title_selected++;
                repeat_at=now+(direction!=held_direction?400:140);chooser_dirty=true;
            }
            held_direction=direction;
            if(fire && fire_armed) {
                char name[TITLE_NAME_BYTES];snprintf(name,sizeof name,"%s",titles()[title_selected]);
                fire_armed=false;status_close();
                if(!board_sd_load_disc(name)) {
                    load_error="LOAD FAILED - CHECK SD CARD";chooser=false;status_open();
                }
                status_frame=board_display_frames();status_updated_ms=now;
                return;
            }
        }
    }
    if (status_visible && (chooser_dirty || (!chooser && (uint32_t)(now - status_updated_ms) >= 1000))) {
        chooser_dirty=false;
        status_active ^= 1u;
        status_draw(status_active);
        status_present();
        status_frame = frame; status_updated_ms = now;
    }
#endif
}

bool boot_status_visible(void) {
#if WITH_BOOT_FILE_SELECTOR
    return status_visible;
#else
    return false;
#endif
}
